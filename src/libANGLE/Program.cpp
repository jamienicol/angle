//
// Copyright 2002 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// Program.cpp: Implements the gl::Program class. Implements GL program objects
// and related functionality. [OpenGL ES 2.0.24] section 2.10.3 page 28.

#include "libANGLE/Program.h"

#include <algorithm>
#include <utility>

#include "common/angle_version.h"
#include "common/bitset_utils.h"
#include "common/debug.h"
#include "common/platform.h"
#include "common/string_utils.h"
#include "common/utilities.h"
#include "compiler/translator/blocklayout.h"
#include "libANGLE/Context.h"
#include "libANGLE/ErrorStrings.h"
#include "libANGLE/MemoryProgramCache.h"
#include "libANGLE/ProgramLinkedResources.h"
#include "libANGLE/ResourceManager.h"
#include "libANGLE/Uniform.h"
#include "libANGLE/VaryingPacking.h"
#include "libANGLE/Version.h"
#include "libANGLE/features.h"
#include "libANGLE/histogram_macros.h"
#include "libANGLE/queryconversions.h"
#include "libANGLE/renderer/GLImplFactory.h"
#include "libANGLE/renderer/ProgramImpl.h"
#include "platform/FrontendFeatures.h"
#include "platform/PlatformMethods.h"

namespace gl
{

namespace
{

// This simplified cast function doesn't need to worry about advanced concepts like
// depth range values, or casting to bool.
template <typename DestT, typename SrcT>
DestT UniformStateQueryCast(SrcT value);

// From-Float-To-Integer Casts
template <>
GLint UniformStateQueryCast(GLfloat value)
{
    return clampCast<GLint>(roundf(value));
}

template <>
GLuint UniformStateQueryCast(GLfloat value)
{
    return clampCast<GLuint>(roundf(value));
}

// From-Integer-to-Integer Casts
template <>
GLint UniformStateQueryCast(GLuint value)
{
    return clampCast<GLint>(value);
}

template <>
GLuint UniformStateQueryCast(GLint value)
{
    return clampCast<GLuint>(value);
}

// From-Boolean-to-Anything Casts
template <>
GLfloat UniformStateQueryCast(GLboolean value)
{
    return (ConvertToBool(value) ? 1.0f : 0.0f);
}

template <>
GLint UniformStateQueryCast(GLboolean value)
{
    return (ConvertToBool(value) ? 1 : 0);
}

template <>
GLuint UniformStateQueryCast(GLboolean value)
{
    return (ConvertToBool(value) ? 1u : 0u);
}

// Default to static_cast
template <typename DestT, typename SrcT>
DestT UniformStateQueryCast(SrcT value)
{
    return static_cast<DestT>(value);
}

template <typename SrcT, typename DestT>
void UniformStateQueryCastLoop(DestT *dataOut, const uint8_t *srcPointer, int components)
{
    for (int comp = 0; comp < components; ++comp)
    {
        // We only work with strides of 4 bytes for uniform components. (GLfloat/GLint)
        // Don't use SrcT stride directly since GLboolean has a stride of 1 byte.
        size_t offset               = comp * 4;
        const SrcT *typedSrcPointer = reinterpret_cast<const SrcT *>(&srcPointer[offset]);
        dataOut[comp]               = UniformStateQueryCast<DestT>(*typedSrcPointer);
    }
}

template <typename VarT>
GLuint GetResourceIndexFromName(const std::vector<VarT> &list, const std::string &name)
{
    std::string nameAsArrayName = name + "[0]";
    for (size_t index = 0; index < list.size(); index++)
    {
        const VarT &resource = list[index];
        if (resource.name == name || (resource.isArray() && resource.name == nameAsArrayName))
        {
            return static_cast<GLuint>(index);
        }
    }

    return GL_INVALID_INDEX;
}

GLint GetVariableLocation(const std::vector<sh::ShaderVariable> &list,
                          const std::vector<VariableLocation> &locationList,
                          const std::string &name)
{
    size_t nameLengthWithoutArrayIndex;
    unsigned int arrayIndex = ParseArrayIndex(name, &nameLengthWithoutArrayIndex);

    for (size_t location = 0u; location < locationList.size(); ++location)
    {
        const VariableLocation &variableLocation = locationList[location];
        if (!variableLocation.used())
        {
            continue;
        }

        const sh::ShaderVariable &variable = list[variableLocation.index];

        // Array output variables may be bound out of order, so we need to ensure we only pick the
        // first element if given the base name.
        if ((variable.name == name) && (variableLocation.arrayIndex == 0))
        {
            return static_cast<GLint>(location);
        }
        if (variable.isArray() && variableLocation.arrayIndex == arrayIndex &&
            angle::BeginsWith(variable.name, name, nameLengthWithoutArrayIndex))
        {
            return static_cast<GLint>(location);
        }
    }

    return -1;
}

GLint GetVariableLocation(const std::vector<LinkedUniform> &list,
                          const std::vector<VariableLocation> &locationList,
                          const std::string &name)
{
    size_t nameLengthWithoutArrayIndex;
    unsigned int arrayIndex = ParseArrayIndex(name, &nameLengthWithoutArrayIndex);

    for (size_t location = 0u; location < locationList.size(); ++location)
    {
        const VariableLocation &variableLocation = locationList[location];
        if (!variableLocation.used())
        {
            continue;
        }

        const LinkedUniform &variable = list[variableLocation.index];

        // Array output variables may be bound out of order, so we need to ensure we only pick the
        // first element if given the base name. Uniforms don't allow this behavior and some code
        // seemingly depends on the opposite behavior, so only enable it for output variables.
        if (angle::BeginsWith(variable.name, name) && (variableLocation.arrayIndex == 0))
        {
            if (name.length() == variable.name.length())
            {
                ASSERT(name == variable.name);
                // GLES 3.1 November 2016 page 87.
                // The string exactly matches the name of the active variable.
                return static_cast<GLint>(location);
            }
            if (name.length() + 3u == variable.name.length() && variable.isArray())
            {
                ASSERT(name + "[0]" == variable.name);
                // The string identifies the base name of an active array, where the string would
                // exactly match the name of the variable if the suffix "[0]" were appended to the
                // string.
                return static_cast<GLint>(location);
            }
        }
        if (variable.isArray() && variableLocation.arrayIndex == arrayIndex &&
            nameLengthWithoutArrayIndex + 3u == variable.name.length() &&
            angle::BeginsWith(variable.name, name, nameLengthWithoutArrayIndex))
        {
            ASSERT(name.substr(0u, nameLengthWithoutArrayIndex) + "[0]" == variable.name);
            // The string identifies an active element of the array, where the string ends with the
            // concatenation of the "[" character, an integer (with no "+" sign, extra leading
            // zeroes, or whitespace) identifying an array element, and the "]" character, the
            // integer is less than the number of active elements of the array variable, and where
            // the string would exactly match the enumerated name of the array if the decimal
            // integer were replaced with zero.
            return static_cast<GLint>(location);
        }
    }

    return -1;
}

void CopyStringToBuffer(GLchar *buffer,
                        const std::string &string,
                        GLsizei bufSize,
                        GLsizei *lengthOut)
{
    ASSERT(bufSize > 0);
    size_t length = std::min<size_t>(bufSize - 1, string.length());
    memcpy(buffer, string.c_str(), length);
    buffer[length] = '\0';

    if (lengthOut)
    {
        *lengthOut = static_cast<GLsizei>(length);
    }
}

std::string GetInterfaceBlockLimitName(ShaderType shaderType, sh::BlockType blockType)
{
    std::ostringstream stream;
    stream << "GL_MAX_" << GetShaderTypeString(shaderType) << "_";

    switch (blockType)
    {
        case sh::BlockType::BLOCK_UNIFORM:
            stream << "UNIFORM_BUFFERS";
            break;
        case sh::BlockType::BLOCK_BUFFER:
            stream << "SHADER_STORAGE_BLOCKS";
            break;
        default:
            UNREACHABLE();
            return "";
    }

    if (shaderType == ShaderType::Geometry)
    {
        stream << "_EXT";
    }

    return stream.str();
}

const char *GetInterfaceBlockTypeString(sh::BlockType blockType)
{
    switch (blockType)
    {
        case sh::BlockType::BLOCK_UNIFORM:
            return "uniform block";
        case sh::BlockType::BLOCK_BUFFER:
            return "shader storage block";
        default:
            UNREACHABLE();
            return "";
    }
}

void LogInterfaceBlocksExceedLimit(InfoLog &infoLog,
                                   ShaderType shaderType,
                                   sh::BlockType blockType,
                                   GLuint limit)
{
    infoLog << GetShaderTypeString(shaderType) << " shader "
            << GetInterfaceBlockTypeString(blockType) << " count exceeds "
            << GetInterfaceBlockLimitName(shaderType, blockType) << " (" << limit << ")";
}

bool ValidateInterfaceBlocksCount(GLuint maxInterfaceBlocks,
                                  const std::vector<sh::InterfaceBlock> &interfaceBlocks,
                                  ShaderType shaderType,
                                  sh::BlockType blockType,
                                  GLuint *combinedInterfaceBlocksCount,
                                  InfoLog &infoLog)
{
    GLuint blockCount = 0;
    for (const sh::InterfaceBlock &block : interfaceBlocks)
    {
        if (IsActiveInterfaceBlock(block))
        {
            blockCount += std::max(block.arraySize, 1u);
            if (blockCount > maxInterfaceBlocks)
            {
                LogInterfaceBlocksExceedLimit(infoLog, shaderType, blockType, maxInterfaceBlocks);
                return false;
            }
        }
    }

    // [OpenGL ES 3.1] Chapter 7.6.2 Page 105:
    // If a uniform block is used by multiple shader stages, each such use counts separately
    // against this combined limit.
    // [OpenGL ES 3.1] Chapter 7.8 Page 111:
    // If a shader storage block in a program is referenced by multiple shaders, each such
    // reference counts separately against this combined limit.
    if (combinedInterfaceBlocksCount)
    {
        *combinedInterfaceBlocksCount += blockCount;
    }

    return true;
}

GLuint GetInterfaceBlockIndex(const std::vector<InterfaceBlock> &list, const std::string &name)
{
    std::vector<unsigned int> subscripts;
    std::string baseName = ParseResourceName(name, &subscripts);

    unsigned int numBlocks = static_cast<unsigned int>(list.size());
    for (unsigned int blockIndex = 0; blockIndex < numBlocks; blockIndex++)
    {
        const auto &block = list[blockIndex];
        if (block.name == baseName)
        {
            const bool arrayElementZero =
                (subscripts.empty() && (!block.isArray || block.arrayElement == 0));
            const bool arrayElementMatches =
                (subscripts.size() == 1 && subscripts[0] == block.arrayElement);
            if (arrayElementMatches || arrayElementZero)
            {
                return blockIndex;
            }
        }
    }

    return GL_INVALID_INDEX;
}

void GetInterfaceBlockName(const GLuint index,
                           const std::vector<InterfaceBlock> &list,
                           GLsizei bufSize,
                           GLsizei *length,
                           GLchar *name)
{
    ASSERT(index < list.size());

    const auto &block = list[index];

    if (bufSize > 0)
    {
        std::string blockName = block.name;

        if (block.isArray)
        {
            blockName += ArrayString(block.arrayElement);
        }
        CopyStringToBuffer(name, blockName, bufSize, length);
    }
}

void InitUniformBlockLinker(const ProgramState &state, UniformBlockLinker *blockLinker)
{
    for (ShaderType shaderType : AllShaderTypes())
    {
        Shader *shader = state.getAttachedShader(shaderType);
        if (shader)
        {
            blockLinker->addShaderBlocks(shaderType, &shader->getUniformBlocks());
        }
    }
}

void InitShaderStorageBlockLinker(const ProgramState &state, ShaderStorageBlockLinker *blockLinker)
{
    for (ShaderType shaderType : AllShaderTypes())
    {
        Shader *shader = state.getAttachedShader(shaderType);
        if (shader != nullptr)
        {
            blockLinker->addShaderBlocks(shaderType, &shader->getShaderStorageBlocks());
        }
    }
}
}  // anonymous namespace

const char *GetLinkMismatchErrorString(LinkMismatchError linkError)
{
    switch (linkError)
    {
        case LinkMismatchError::TYPE_MISMATCH:
            return "Type";
        case LinkMismatchError::ARRAY_SIZE_MISMATCH:
            return "Array size";
        case LinkMismatchError::PRECISION_MISMATCH:
            return "Precision";
        case LinkMismatchError::STRUCT_NAME_MISMATCH:
            return "Structure name";
        case LinkMismatchError::FIELD_NUMBER_MISMATCH:
            return "Field number";
        case LinkMismatchError::FIELD_NAME_MISMATCH:
            return "Field name";

        case LinkMismatchError::INTERPOLATION_TYPE_MISMATCH:
            return "Interpolation type";
        case LinkMismatchError::INVARIANCE_MISMATCH:
            return "Invariance";

        case LinkMismatchError::BINDING_MISMATCH:
            return "Binding layout qualifier";
        case LinkMismatchError::LOCATION_MISMATCH:
            return "Location layout qualifier";
        case LinkMismatchError::OFFSET_MISMATCH:
            return "Offset layout qualifier";
        case LinkMismatchError::INSTANCE_NAME_MISMATCH:
            return "Instance name qualifier";
        case LinkMismatchError::FORMAT_MISMATCH:
            return "Format qualifier";

        case LinkMismatchError::LAYOUT_QUALIFIER_MISMATCH:
            return "Layout qualifier";
        case LinkMismatchError::MATRIX_PACKING_MISMATCH:
            return "Matrix Packing";

        case LinkMismatchError::FIELD_LOCATION_MISMATCH:
            return "Field location";
        case LinkMismatchError::FIELD_STRUCT_NAME_MISMATCH:
            return "Field structure name";
        default:
            UNREACHABLE();
            return "";
    }
}

LinkMismatchError LinkValidateInterfaceBlockFields(const sh::ShaderVariable &blockField1,
                                                   const sh::ShaderVariable &blockField2,
                                                   bool webglCompatibility,
                                                   std::string *mismatchedBlockFieldName)
{
    if (blockField1.name != blockField2.name)
    {
        return LinkMismatchError::FIELD_NAME_MISMATCH;
    }

    // If webgl, validate precision of UBO fields, otherwise don't.  See Khronos bug 10287.
    LinkMismatchError linkError = LinkValidateProgramVariables(
        blockField1, blockField2, webglCompatibility, true, mismatchedBlockFieldName);
    if (linkError != LinkMismatchError::NO_MISMATCH)
    {
        AddProgramVariableParentPrefix(blockField1.name, mismatchedBlockFieldName);
        return linkError;
    }

    if (blockField1.isRowMajorLayout != blockField2.isRowMajorLayout)
    {
        AddProgramVariableParentPrefix(blockField1.name, mismatchedBlockFieldName);
        return LinkMismatchError::MATRIX_PACKING_MISMATCH;
    }

    return LinkMismatchError::NO_MISMATCH;
}

LinkMismatchError AreMatchingInterfaceBlocks(const sh::InterfaceBlock &interfaceBlock1,
                                             const sh::InterfaceBlock &interfaceBlock2,
                                             bool webglCompatibility,
                                             std::string *mismatchedBlockFieldName)
{
    // validate blocks for the same member types
    if (interfaceBlock1.fields.size() != interfaceBlock2.fields.size())
    {
        return LinkMismatchError::FIELD_NUMBER_MISMATCH;
    }
    if (interfaceBlock1.arraySize != interfaceBlock2.arraySize)
    {
        return LinkMismatchError::ARRAY_SIZE_MISMATCH;
    }
    if (interfaceBlock1.layout != interfaceBlock2.layout ||
        interfaceBlock1.binding != interfaceBlock2.binding)
    {
        return LinkMismatchError::LAYOUT_QUALIFIER_MISMATCH;
    }
    if (interfaceBlock1.instanceName.empty() != interfaceBlock2.instanceName.empty())
    {
        return LinkMismatchError::INSTANCE_NAME_MISMATCH;
    }
    const unsigned int numBlockMembers = static_cast<unsigned int>(interfaceBlock1.fields.size());
    for (unsigned int blockMemberIndex = 0; blockMemberIndex < numBlockMembers; blockMemberIndex++)
    {
        const sh::ShaderVariable &member1 = interfaceBlock1.fields[blockMemberIndex];
        const sh::ShaderVariable &member2 = interfaceBlock2.fields[blockMemberIndex];

        LinkMismatchError linkError = LinkValidateInterfaceBlockFields(
            member1, member2, webglCompatibility, mismatchedBlockFieldName);
        if (linkError != LinkMismatchError::NO_MISMATCH)
        {
            return linkError;
        }
    }
    return LinkMismatchError::NO_MISMATCH;
}

using ShaderInterfaceBlock = std::pair<ShaderType, const sh::InterfaceBlock *>;
using InterfaceBlockMap    = std::map<std::string, ShaderInterfaceBlock>;

void InitializeInterfaceBlockMap(const std::vector<sh::InterfaceBlock> &interfaceBlocks,
                                 ShaderType shaderType,
                                 InterfaceBlockMap *linkedInterfaceBlocks)
{
    ASSERT(linkedInterfaceBlocks);

    for (const sh::InterfaceBlock &interfaceBlock : interfaceBlocks)
    {
        (*linkedInterfaceBlocks)[interfaceBlock.name] = std::make_pair(shaderType, &interfaceBlock);
    }
}

bool ValidateGraphicsInterfaceBlocksPerShader(
    const std::vector<sh::InterfaceBlock> &interfaceBlocksToLink,
    ShaderType shaderType,
    bool webglCompatibility,
    InterfaceBlockMap *linkedBlocks,
    InfoLog &infoLog)
{
    ASSERT(linkedBlocks);

    for (const sh::InterfaceBlock &block : interfaceBlocksToLink)
    {
        const auto &entry = linkedBlocks->find(block.name);
        if (entry != linkedBlocks->end())
        {
            const sh::InterfaceBlock &linkedBlock = *(entry->second.second);
            std::string mismatchedStructFieldName;
            LinkMismatchError linkError = AreMatchingInterfaceBlocks(
                block, linkedBlock, webglCompatibility, &mismatchedStructFieldName);
            if (linkError != LinkMismatchError::NO_MISMATCH)
            {
                LogLinkMismatch(infoLog, block.name, GetInterfaceBlockTypeString(block.blockType),
                                linkError, mismatchedStructFieldName, entry->second.first,
                                shaderType);
                return false;
            }
        }
        else
        {
            (*linkedBlocks)[block.name] = std::make_pair(shaderType, &block);
        }
    }

    return true;
}

void LogAmbiguousFieldLinkMismatch(InfoLog &infoLog,
                                   const std::string &blockName1,
                                   const std::string &blockName2,
                                   const std::string &fieldName,
                                   ShaderType shaderType1,
                                   ShaderType shaderType2)
{
    infoLog << "Ambiguous field '" << fieldName << "' in blocks '" << blockName1 << "' ("
            << GetShaderTypeString(shaderType1) << " shader) and '" << blockName2 << "' ("
            << GetShaderTypeString(shaderType2) << " shader) which don't have instance names.";
}

bool ValidateInstancelessGraphicsInterfaceBlocksPerShader(
    const std::vector<sh::InterfaceBlock> &interfaceBlocks,
    ShaderType shaderType,
    InterfaceBlockMap *instancelessBlocksFields,
    InfoLog &infoLog)
{
    ASSERT(instancelessBlocksFields);

    for (const sh::InterfaceBlock &block : interfaceBlocks)
    {
        if (!block.instanceName.empty())
        {
            continue;
        }

        for (const sh::ShaderVariable &field : block.fields)
        {
            const auto &entry = instancelessBlocksFields->find(field.name);
            if (entry != instancelessBlocksFields->end())
            {
                const sh::InterfaceBlock &linkedBlock = *(entry->second.second);
                if (block.name != linkedBlock.name)
                {
                    LogAmbiguousFieldLinkMismatch(infoLog, block.name, linkedBlock.name, field.name,
                                                  entry->second.first, shaderType);
                    return false;
                }
            }
            else
            {
                (*instancelessBlocksFields)[field.name] = std::make_pair(shaderType, &block);
            }
        }
    }

    return true;
}

bool ValidateInterfaceBlocksMatch(
    GLuint numShadersHasInterfaceBlocks,
    const ShaderMap<const std::vector<sh::InterfaceBlock> *> &shaderInterfaceBlocks,
    InfoLog &infoLog,
    bool webglCompatibility,
    InterfaceBlockMap *instancelessInterfaceBlocksFields)
{
    for (ShaderType shaderType : kAllGraphicsShaderTypes)
    {
        // Validate that instanceless blocks of different names don't have fields of the same name.
        if (shaderInterfaceBlocks[shaderType] &&
            !ValidateInstancelessGraphicsInterfaceBlocksPerShader(
                *shaderInterfaceBlocks[shaderType], shaderType, instancelessInterfaceBlocksFields,
                infoLog))
        {
            return false;
        }
    }

    if (numShadersHasInterfaceBlocks < 2u)
    {
        return true;
    }

    ASSERT(!shaderInterfaceBlocks[ShaderType::Compute]);

    // Check that interface blocks defined in the graphics shaders are identical

    InterfaceBlockMap linkedInterfaceBlocks;

    bool interfaceBlockMapInitialized = false;
    for (ShaderType shaderType : kAllGraphicsShaderTypes)
    {
        if (!shaderInterfaceBlocks[shaderType])
        {
            continue;
        }

        if (!interfaceBlockMapInitialized)
        {
            InitializeInterfaceBlockMap(*shaderInterfaceBlocks[shaderType], shaderType,
                                        &linkedInterfaceBlocks);
            interfaceBlockMapInitialized = true;
        }
        else if (!ValidateGraphicsInterfaceBlocksPerShader(*shaderInterfaceBlocks[shaderType],
                                                           shaderType, webglCompatibility,
                                                           &linkedInterfaceBlocks, infoLog))
        {
            return false;
        }
    }

    return true;
}

void UpdateInterfaceVariable(std::vector<sh::ShaderVariable> &block, sh::ShaderVariable &var)
{
    if (!var.isStruct())
    {
        var.resetEffectiveLocation();
        block.emplace_back(var);
    }

    for (sh::ShaderVariable &field : var.fields)
    {
        ASSERT(!var.name.empty() || var.isShaderIOBlock);

        // Shader I/O block naming is similar to UBOs and SSBOs:
        //
        //     in Block
        //     {
        //         type field;  // produces "field"
        //     };
        //
        //     in Block2
        //     {
        //         type field;  // produces "Block2.field"
        //     } block2;
        //
        const std::string &baseName = var.isShaderIOBlock ? var.structName : var.name;
        const std::string prefix    = var.name.empty() ? "" : baseName + ".";

        if (!field.isStruct())
        {
            field.updateEffectiveLocation(var);
            field.name = prefix + field.name;
            block.emplace_back(field);
        }

        for (sh::ShaderVariable &nested : field.fields)
        {
            nested.updateEffectiveLocation(field);
            nested.name = prefix + field.name + "." + nested.name;
            block.emplace_back(nested);
        }
    }
}

void WriteShaderVariableBuffer(BinaryOutputStream *stream, const ShaderVariableBuffer &var)
{
    stream->writeInt(var.binding);
    stream->writeInt(var.dataSize);

    for (ShaderType shaderType : AllShaderTypes())
    {
        stream->writeBool(var.isActive(shaderType));
    }

    stream->writeInt(var.memberIndexes.size());
    for (unsigned int memberCounterIndex : var.memberIndexes)
    {
        stream->writeInt(memberCounterIndex);
    }
}

void LoadShaderVariableBuffer(BinaryInputStream *stream, ShaderVariableBuffer *var)
{
    var->binding  = stream->readInt<int>();
    var->dataSize = stream->readInt<unsigned int>();

    for (ShaderType shaderType : AllShaderTypes())
    {
        var->setActive(shaderType, stream->readBool());
    }

    size_t numMembers = stream->readInt<size_t>();
    for (size_t blockMemberIndex = 0; blockMemberIndex < numMembers; blockMemberIndex++)
    {
        var->memberIndexes.push_back(stream->readInt<unsigned int>());
    }
}

void WriteBufferVariable(BinaryOutputStream *stream, const BufferVariable &var)
{
    WriteShaderVar(stream, var);

    stream->writeInt(var.bufferIndex);
    WriteBlockMemberInfo(stream, var.blockInfo);
    stream->writeInt(var.topLevelArraySize);

    for (ShaderType shaderType : AllShaderTypes())
    {
        stream->writeBool(var.isActive(shaderType));
    }
}

void LoadBufferVariable(BinaryInputStream *stream, BufferVariable *var)
{
    LoadShaderVar(stream, var);

    var->bufferIndex = stream->readInt<int>();
    LoadBlockMemberInfo(stream, &var->blockInfo);
    var->topLevelArraySize = stream->readInt<int>();

    for (ShaderType shaderType : AllShaderTypes())
    {
        var->setActive(shaderType, stream->readBool());
    }
}

void WriteInterfaceBlock(BinaryOutputStream *stream, const InterfaceBlock &block)
{
    stream->writeString(block.name);
    stream->writeString(block.mappedName);
    stream->writeBool(block.isArray);
    stream->writeInt(block.arrayElement);

    WriteShaderVariableBuffer(stream, block);
}

void LoadInterfaceBlock(BinaryInputStream *stream, InterfaceBlock *block)
{
    block->name         = stream->readString();
    block->mappedName   = stream->readString();
    block->isArray      = stream->readBool();
    block->arrayElement = stream->readInt<unsigned int>();

    LoadShaderVariableBuffer(stream, block);
}

// Saves the linking context for later use in resolveLink().
struct Program::LinkingState
{
    std::shared_ptr<ProgramExecutable> linkedExecutable;
    ProgramLinkedResources resources;
    egl::BlobCache::Key programHash;
    std::unique_ptr<rx::LinkEvent> linkEvent;
    bool linkingFromBinary;
};

const char *const g_fakepath = "C:\\fakepath";

// InfoLog implementation.
InfoLog::InfoLog() : mLazyStream(nullptr) {}

InfoLog::~InfoLog() {}

size_t InfoLog::getLength() const
{
    if (!mLazyStream)
    {
        return 0;
    }

    const std::string &logString = mLazyStream->str();
    return logString.empty() ? 0 : logString.length() + 1;
}

void InfoLog::getLog(GLsizei bufSize, GLsizei *length, char *infoLog) const
{
    size_t index = 0;

    if (bufSize > 0)
    {
        const std::string logString(str());

        if (!logString.empty())
        {
            index = std::min(static_cast<size_t>(bufSize) - 1, logString.length());
            memcpy(infoLog, logString.c_str(), index);
        }

        infoLog[index] = '\0';
    }

    if (length)
    {
        *length = static_cast<GLsizei>(index);
    }
}

// append a santized message to the program info log.
// The D3D compiler includes a fake file path in some of the warning or error
// messages, so lets remove all occurrences of this fake file path from the log.
void InfoLog::appendSanitized(const char *message)
{
    ensureInitialized();

    std::string msg(message);

    size_t found;
    do
    {
        found = msg.find(g_fakepath);
        if (found != std::string::npos)
        {
            msg.erase(found, strlen(g_fakepath));
        }
    } while (found != std::string::npos);

    *mLazyStream << message << std::endl;
}

void InfoLog::reset()
{
    if (mLazyStream)
    {
        mLazyStream.reset(nullptr);
    }
}

bool InfoLog::empty() const
{
    if (!mLazyStream)
    {
        return true;
    }

    return mLazyStream->rdbuf()->in_avail() == 0;
}

void LogLinkMismatch(InfoLog &infoLog,
                     const std::string &variableName,
                     const char *variableType,
                     LinkMismatchError linkError,
                     const std::string &mismatchedStructOrBlockFieldName,
                     ShaderType shaderType1,
                     ShaderType shaderType2)
{
    std::ostringstream stream;
    stream << GetLinkMismatchErrorString(linkError) << "s of " << variableType << " '"
           << variableName;

    if (!mismatchedStructOrBlockFieldName.empty())
    {
        stream << "' member '" << variableName << "." << mismatchedStructOrBlockFieldName;
    }

    stream << "' differ between " << GetShaderTypeString(shaderType1) << " and "
           << GetShaderTypeString(shaderType2) << " shaders.";

    infoLog << stream.str();
}

bool IsActiveInterfaceBlock(const sh::InterfaceBlock &interfaceBlock)
{
    // Only 'packed' blocks are allowed to be considered inactive.
    return interfaceBlock.active || interfaceBlock.layout != sh::BLOCKLAYOUT_PACKED;
}

void WriteBlockMemberInfo(BinaryOutputStream *stream, const sh::BlockMemberInfo &var)
{
    stream->writeInt(var.arrayStride);
    stream->writeBool(var.isRowMajorMatrix);
    stream->writeInt(var.matrixStride);
    stream->writeInt(var.offset);
    stream->writeInt(var.topLevelArrayStride);
}

void LoadBlockMemberInfo(BinaryInputStream *stream, sh::BlockMemberInfo *var)
{
    var->arrayStride         = stream->readInt<int>();
    var->isRowMajorMatrix    = stream->readBool();
    var->matrixStride        = stream->readInt<int>();
    var->offset              = stream->readInt<int>();
    var->topLevelArrayStride = stream->readInt<int>();
}

void WriteShaderVar(BinaryOutputStream *stream, const sh::ShaderVariable &var)
{
    stream->writeInt(var.type);
    stream->writeInt(var.precision);
    stream->writeString(var.name);
    stream->writeString(var.mappedName);
    stream->writeIntVector(var.arraySizes);
    stream->writeBool(var.staticUse);
    stream->writeBool(var.active);
    stream->writeInt(var.binding);
    stream->writeString(var.structName);
    stream->writeString(var.mappedStructName);
    stream->writeInt(var.hasParentArrayIndex() ? var.parentArrayIndex() : -1);

    stream->writeInt(var.imageUnitFormat);
    stream->writeInt(var.offset);
    stream->writeBool(var.readonly);
    stream->writeBool(var.writeonly);
    stream->writeBool(var.texelFetchStaticUse);

    ASSERT(var.fields.empty());
}

void LoadShaderVar(BinaryInputStream *stream, sh::ShaderVariable *var)
{
    var->type       = stream->readInt<GLenum>();
    var->precision  = stream->readInt<GLenum>();
    var->name       = stream->readString();
    var->mappedName = stream->readString();
    stream->readIntVector<unsigned int>(&var->arraySizes);
    var->staticUse        = stream->readBool();
    var->active           = stream->readBool();
    var->binding          = stream->readInt<int>();
    var->structName       = stream->readString();
    var->mappedStructName = stream->readString();
    var->setParentArrayIndex(stream->readInt<int>());

    var->imageUnitFormat     = stream->readInt<GLenum>();
    var->offset              = stream->readInt<int>();
    var->readonly            = stream->readBool();
    var->writeonly           = stream->readBool();
    var->texelFetchStaticUse = stream->readBool();
}

// VariableLocation implementation.
VariableLocation::VariableLocation() : arrayIndex(0), index(kUnused), ignored(false) {}

VariableLocation::VariableLocation(unsigned int arrayIndex, unsigned int index)
    : arrayIndex(arrayIndex), index(index), ignored(false)
{
    ASSERT(arrayIndex != GL_INVALID_INDEX);
}

// SamplerBindings implementation.
SamplerBinding::SamplerBinding(TextureType textureTypeIn,
                               GLenum samplerTypeIn,
                               SamplerFormat formatIn,
                               size_t elementCount)
    : textureType(textureTypeIn),
      samplerType(samplerTypeIn),
      format(formatIn),
      boundTextureUnits(elementCount, 0)
{}

SamplerBinding::SamplerBinding(const SamplerBinding &other) = default;

SamplerBinding::~SamplerBinding() = default;

// ProgramBindings implementation.
ProgramBindings::ProgramBindings() {}

ProgramBindings::~ProgramBindings() {}

void ProgramBindings::bindLocation(GLuint index, const std::string &name)
{
    mBindings[name] = index;
}

int ProgramBindings::getBindingByName(const std::string &name) const
{
    auto iter = mBindings.find(name);
    return (iter != mBindings.end()) ? iter->second : -1;
}

int ProgramBindings::getBinding(const sh::ShaderVariable &variable) const
{
    return getBindingByName(variable.name);
}

ProgramBindings::const_iterator ProgramBindings::begin() const
{
    return mBindings.begin();
}

ProgramBindings::const_iterator ProgramBindings::end() const
{
    return mBindings.end();
}

// ProgramAliasedBindings implementation.
ProgramAliasedBindings::ProgramAliasedBindings() {}

ProgramAliasedBindings::~ProgramAliasedBindings() {}

void ProgramAliasedBindings::bindLocation(GLuint index, const std::string &name)
{
    mBindings[name] = ProgramBinding(index);

    // EXT_blend_func_extended spec: "If it specifies the base name of an array,
    // it identifies the resources associated with the first element of the array."
    //
    // Normalize array bindings so that "name" and "name[0]" map to the same entry.
    // If this binding is of the form "name[0]", then mark the "name" binding as
    // aliased but do not update it yet in case "name" is not actually an array.
    size_t nameLengthWithoutArrayIndex;
    unsigned int arrayIndex = ParseArrayIndex(name, &nameLengthWithoutArrayIndex);
    if (arrayIndex == 0)
    {
        std::string baseName = name.substr(0u, nameLengthWithoutArrayIndex);
        auto iter            = mBindings.find(baseName);
        if (iter != mBindings.end())
        {
            iter->second.aliased = true;
        }
    }
}

int ProgramAliasedBindings::getBindingByName(const std::string &name) const
{
    auto iter = mBindings.find(name);
    return (iter != mBindings.end()) ? iter->second.location : -1;
}

int ProgramAliasedBindings::getBindingByLocation(GLuint location) const
{
    for (const auto &iter : mBindings)
    {
        if (iter.second.location == location)
        {
            return iter.second.location;
        }
    }
    return -1;
}

int ProgramAliasedBindings::getBinding(const sh::ShaderVariable &variable) const
{
    const std::string &name = variable.name;

    // Check with the normalized array name if applicable.
    if (variable.isArray())
    {
        size_t nameLengthWithoutArrayIndex;
        unsigned int arrayIndex = ParseArrayIndex(name, &nameLengthWithoutArrayIndex);
        if (arrayIndex == 0)
        {
            std::string baseName = name.substr(0u, nameLengthWithoutArrayIndex);
            auto iter            = mBindings.find(baseName);
            // If "name" exists and is not aliased, that means it was modified more
            // recently than its "name[0]" form and should be used instead of that.
            if (iter != mBindings.end() && !iter->second.aliased)
            {
                return iter->second.location;
            }
        }
        else if (arrayIndex == GL_INVALID_INDEX)
        {
            auto iter = mBindings.find(variable.name);
            // If "name" exists and is not aliased, that means it was modified more
            // recently than its "name[0]" form and should be used instead of that.
            if (iter != mBindings.end() && !iter->second.aliased)
            {
                return iter->second.location;
            }
            // The base name was aliased, so use the name with the array notation.
            return getBindingByName(name + "[0]");
        }
    }

    return getBindingByName(name);
}

ProgramAliasedBindings::const_iterator ProgramAliasedBindings::begin() const
{
    return mBindings.begin();
}

ProgramAliasedBindings::const_iterator ProgramAliasedBindings::end() const
{
    return mBindings.end();
}

// ImageBinding implementation.
ImageBinding::ImageBinding(size_t count, TextureType textureTypeIn)
    : textureType(textureTypeIn), boundImageUnits(count, 0)
{}
ImageBinding::ImageBinding(GLuint imageUnit, size_t count, TextureType textureTypeIn)
    : textureType(textureTypeIn)
{
    for (size_t index = 0; index < count; ++index)
    {
        boundImageUnits.push_back(imageUnit + static_cast<GLuint>(index));
    }
}

ImageBinding::ImageBinding(const ImageBinding &other) = default;

ImageBinding::~ImageBinding() = default;

// ProgramState implementation.
ProgramState::ProgramState()
    : mLabel(),
      mAttachedShaders{},
      mAttachedShadersMarkedForDetach{},
      mLocationsUsedForXfbExtension(0),
      mAtomicCounterUniformRange(0, 0),
      mYUVOutput(false),
      mBinaryRetrieveableHint(false),
      mSeparable(false),
      mNumViews(-1),
      mDrawIDLocation(-1),
      mBaseVertexLocation(-1),
      mBaseInstanceLocation(-1),
      mCachedBaseVertex(0),
      mCachedBaseInstance(0),
      mExecutable(new ProgramExecutable())
{
    mComputeShaderLocalSize.fill(1);
}

ProgramState::~ProgramState()
{
    ASSERT(!hasAttachedShader());
}

const std::string &ProgramState::getLabel()
{
    return mLabel;
}

Shader *ProgramState::getAttachedShader(ShaderType shaderType) const
{
    ASSERT(shaderType != ShaderType::InvalidEnum);
    return mAttachedShaders[shaderType];
}

GLuint ProgramState::getUniformIndexFromName(const std::string &name) const
{
    return GetResourceIndexFromName(mExecutable->mUniforms, name);
}

GLuint ProgramState::getBufferVariableIndexFromName(const std::string &name) const
{
    return GetResourceIndexFromName(mBufferVariables, name);
}

GLuint ProgramState::getUniformIndexFromLocation(UniformLocation location) const
{
    ASSERT(location.value >= 0 && static_cast<size_t>(location.value) < mUniformLocations.size());
    return mUniformLocations[location.value].index;
}

Optional<GLuint> ProgramState::getSamplerIndex(UniformLocation location) const
{
    GLuint index = getUniformIndexFromLocation(location);
    if (!isSamplerUniformIndex(index))
    {
        return Optional<GLuint>::Invalid();
    }

    return getSamplerIndexFromUniformIndex(index);
}

bool ProgramState::isSamplerUniformIndex(GLuint index) const
{
    return mExecutable->mSamplerUniformRange.contains(index);
}

GLuint ProgramState::getSamplerIndexFromUniformIndex(GLuint uniformIndex) const
{
    ASSERT(isSamplerUniformIndex(uniformIndex));
    return uniformIndex - mExecutable->mSamplerUniformRange.low();
}

GLuint ProgramState::getUniformIndexFromSamplerIndex(GLuint samplerIndex) const
{
    ASSERT(samplerIndex < mExecutable->mSamplerUniformRange.length());
    return samplerIndex + mExecutable->mSamplerUniformRange.low();
}

bool ProgramState::isImageUniformIndex(GLuint index) const
{
    return mExecutable->mImageUniformRange.contains(index);
}

GLuint ProgramState::getImageIndexFromUniformIndex(GLuint uniformIndex) const
{
    ASSERT(isImageUniformIndex(uniformIndex));
    return uniformIndex - mExecutable->mImageUniformRange.low();
}

GLuint ProgramState::getAttributeLocation(const std::string &name) const
{
    for (const sh::ShaderVariable &attribute : mExecutable->mProgramInputs)
    {
        if (attribute.name == name)
        {
            return attribute.location;
        }
    }

    return static_cast<GLuint>(-1);
}

bool ProgramState::hasAttachedShader() const
{
    for (const Shader *shader : mAttachedShaders)
    {
        if (shader)
        {
            return true;
        }
    }
    return false;
}

ShaderType ProgramState::getFirstAttachedShaderStageType() const
{
    if (mExecutable->getLinkedShaderStages().none())
    {
        return ShaderType::InvalidEnum;
    }

    return *mExecutable->getLinkedShaderStages().begin();
}

ShaderType ProgramState::getLastAttachedShaderStageType() const
{
    for (int i = gl::kAllGraphicsShaderTypes.size() - 1; i >= 0; --i)
    {
        const gl::ShaderType shaderType = gl::kAllGraphicsShaderTypes[i];

        if (mExecutable->hasLinkedShaderStage(shaderType))
        {
            return shaderType;
        }
    }

    if (mExecutable->hasLinkedShaderStage(ShaderType::Compute))
    {
        return ShaderType::Compute;
    }

    return ShaderType::InvalidEnum;
}

ShaderType ProgramState::getAttachedTransformFeedbackStage() const
{
    if (mAttachedShaders[ShaderType::Geometry])
    {
        return ShaderType::Geometry;
    }
    if (mAttachedShaders[ShaderType::TessEvaluation])
    {
        return ShaderType::TessEvaluation;
    }
    return ShaderType::Vertex;
}

Program::Program(rx::GLImplFactory *factory, ShaderProgramManager *manager, ShaderProgramID handle)
    : mSerial(factory->generateSerial()),
      mProgram(factory->createProgram(mState)),
      mValidated(false),
      mLinked(false),
      mDeleteStatus(false),
      mRefCount(0),
      mResourceManager(manager),
      mHandle(handle)
{
    ASSERT(mProgram);

    unlink();
}

Program::~Program()
{
    ASSERT(!mProgram);
}

void Program::onDestroy(const Context *context)
{
    resolveLink(context);
    for (ShaderType shaderType : AllShaderTypes())
    {
        if (mState.mAttachedShaders[shaderType])
        {
            mState.mAttachedShaders[shaderType]->release(context);
            mState.mAttachedShaders[shaderType]                = nullptr;
            mState.mAttachedShadersMarkedForDetach[shaderType] = false;
        }
    }

    mProgram->destroy(context);

    ASSERT(!mState.hasAttachedShader());
    SafeDelete(mProgram);

    delete this;
}
ShaderProgramID Program::id() const
{
    ASSERT(!mLinkingState);
    return mHandle;
}

void Program::setLabel(const Context *context, const std::string &label)
{
    ASSERT(!mLinkingState);
    mState.mLabel = label;
}

const std::string &Program::getLabel() const
{
    ASSERT(!mLinkingState);
    return mState.mLabel;
}

void Program::attachShader(const Context *context, Shader *shader)
{
    ASSERT(!mLinkingState);
    ShaderType shaderType = shader->getType();
    ASSERT(shaderType != ShaderType::InvalidEnum);

    // Since detachShader doesn't actually detach anymore, we need to do that work when attaching a
    // new shader to make sure we don't lose track of it and free the resources.
    if (mState.mAttachedShaders[shaderType])
    {
        mState.mAttachedShaders[shaderType]->release(context);
        mState.mAttachedShaders[shaderType]                = nullptr;
        mState.mAttachedShadersMarkedForDetach[shaderType] = false;
    }

    mState.mAttachedShaders[shaderType] = shader;
    mState.mAttachedShaders[shaderType]->addRef();
}

void Program::detachShader(const Context *context, Shader *shader)
{
    ASSERT(!mLinkingState);
    ShaderType shaderType = shader->getType();
    ASSERT(shaderType != ShaderType::InvalidEnum);

    ASSERT(mState.mAttachedShaders[shaderType] == shader);

    if (isSeparable())
    {
        // Don't actually detach the shader since we still need it in case this
        // Program is part of a Program Pipeline Object. Instead, leave a mark
        // that indicates we intended to.
        mState.mAttachedShadersMarkedForDetach[shaderType] = true;
        return;
    }

    shader->release(context);
    mState.mAttachedShaders[shaderType]                = nullptr;
    mState.mAttachedShadersMarkedForDetach[shaderType] = false;
}

int Program::getAttachedShadersCount() const
{
    ASSERT(!mLinkingState);
    int numAttachedShaders = 0;
    for (const Shader *shader : mState.mAttachedShaders)
    {
        if (shader)
        {
            ++numAttachedShaders;
        }
    }

    return numAttachedShaders;
}

Shader *Program::getAttachedShader(ShaderType shaderType) const
{
    ASSERT(!mLinkingState);
    return mState.getAttachedShader(shaderType);
}

void Program::bindAttributeLocation(GLuint index, const char *name)
{
    ASSERT(!mLinkingState);
    mAttributeBindings.bindLocation(index, name);
}

void Program::bindUniformLocation(UniformLocation location, const char *name)
{
    ASSERT(!mLinkingState);
    mState.mUniformLocationBindings.bindLocation(location.value, name);
}

void Program::bindFragmentOutputLocation(GLuint index, const char *name)
{
    mFragmentOutputLocations.bindLocation(index, name);
}

void Program::bindFragmentOutputIndex(GLuint index, const char *name)
{
    mFragmentOutputIndexes.bindLocation(index, name);
}

angle::Result Program::link(const Context *context)
{
    angle::Result result = linkImpl(context);

    // Avoid having two ProgramExecutables if the link failed and the Program had successfully
    // linked previously.
    if (mLinkingState && mLinkingState->linkedExecutable)
    {
        mState.mExecutable = mLinkingState->linkedExecutable;
    }

    return result;
}

// The attached shaders are checked for linking errors by matching up their variables.
// Uniform, input and output variables get collected.
// The code gets compiled into binaries.
angle::Result Program::linkImpl(const Context *context)
{
    ASSERT(!mLinkingState);
    // Don't make any local variables pointing to anything within the ProgramExecutable, since
    // unlink() could make a new ProgramExecutable making any references/pointers invalid.
    auto *platform   = ANGLEPlatformCurrent();
    double startTime = platform->currentTime(platform);

    // Unlink the program, but do not clear the validation-related caching yet, since we can still
    // use the previously linked program if linking the shaders fails.
    mLinked = false;

    mState.mExecutable->getInfoLog().reset();

    // Validate we have properly attached shaders before checking the cache.
    if (!linkValidateShaders(mState.mExecutable->getInfoLog()))
    {
        return angle::Result::Continue;
    }

    egl::BlobCache::Key programHash = {0};
    MemoryProgramCache *cache       = context->getMemoryProgramCache();

    // TODO: http://anglebug.com/4530: Enable program caching for separable programs
    if (cache && !isSeparable())
    {
        std::lock_guard<std::mutex> cacheLock(context->getProgramCacheMutex());
        angle::Result cacheResult = cache->getProgram(context, this, &programHash);
        ANGLE_TRY(cacheResult);

        // Check explicitly for Continue, Incomplete means a cache miss
        if (cacheResult == angle::Result::Continue)
        {
            // Succeeded in loading the binaries in the front-end, back end may still be loading
            // asynchronously
            double delta = platform->currentTime(platform) - startTime;
            int us       = static_cast<int>(delta * 1000000.0);
            ANGLE_HISTOGRAM_COUNTS("GPU.ANGLE.ProgramCache.ProgramCacheHitTimeUS", us);
            return angle::Result::Continue;
        }
    }

    // Cache load failed, fall through to normal linking.
    unlink();
    InfoLog &infoLog = mState.mExecutable->getInfoLog();

    // Re-link shaders after the unlink call.
    bool result = linkValidateShaders(infoLog);
    ASSERT(result);

    std::unique_ptr<LinkingState> linkingState(new LinkingState());
    ProgramMergedVaryings mergedVaryings;
    ProgramLinkedResources &resources = linkingState->resources;

    if (mState.mAttachedShaders[ShaderType::Compute])
    {
        resources.init(&mState.mExecutable->mUniformBlocks, &mState.mExecutable->mUniforms,
                       &mState.mExecutable->mComputeShaderStorageBlocks, &mState.mBufferVariables,
                       &mState.mExecutable->mAtomicCounterBuffers);

        GLuint combinedImageUniforms = 0u;
        if (!linkUniforms(context->getCaps(), context->getClientVersion(), infoLog,
                          mState.mUniformLocationBindings, &combinedImageUniforms,
                          &resources.unusedUniforms))
        {
            return angle::Result::Continue;
        }

        GLuint combinedShaderStorageBlocks = 0u;
        if (!linkInterfaceBlocks(context->getCaps(), context->getClientVersion(),
                                 context->getExtensions().webglCompatibility, infoLog,
                                 &combinedShaderStorageBlocks))
        {
            return angle::Result::Continue;
        }

        // [OpenGL ES 3.1] Chapter 8.22 Page 203:
        // A link error will be generated if the sum of the number of active image uniforms used in
        // all shaders, the number of active shader storage blocks, and the number of active
        // fragment shader outputs exceeds the implementation-dependent value of
        // MAX_COMBINED_SHADER_OUTPUT_RESOURCES.
        if (combinedImageUniforms + combinedShaderStorageBlocks >
            static_cast<GLuint>(context->getCaps().maxCombinedShaderOutputResources))
        {
            infoLog
                << "The sum of the number of active image uniforms, active shader storage blocks "
                   "and active fragment shader outputs exceeds "
                   "MAX_COMBINED_SHADER_OUTPUT_RESOURCES ("
                << context->getCaps().maxCombinedShaderOutputResources << ")";
            return angle::Result::Continue;
        }

        InitUniformBlockLinker(mState, &resources.uniformBlockLinker);
        InitShaderStorageBlockLinker(mState, &resources.shaderStorageBlockLinker);
    }
    else
    {
        resources.init(&mState.mExecutable->mUniformBlocks, &mState.mExecutable->mUniforms,
                       &mState.mExecutable->mGraphicsShaderStorageBlocks, &mState.mBufferVariables,
                       &mState.mExecutable->mAtomicCounterBuffers);

        if (!linkAttributes(context, infoLog))
        {
            return angle::Result::Continue;
        }

        if (!linkVaryings(infoLog))
        {
            return angle::Result::Continue;
        }

        GLuint combinedImageUniforms = 0u;
        if (!linkUniforms(context->getCaps(), context->getClientVersion(), infoLog,
                          mState.mUniformLocationBindings, &combinedImageUniforms,
                          &resources.unusedUniforms))
        {
            return angle::Result::Continue;
        }

        GLuint combinedShaderStorageBlocks = 0u;
        if (!linkInterfaceBlocks(context->getCaps(), context->getClientVersion(),
                                 context->getExtensions().webglCompatibility, infoLog,
                                 &combinedShaderStorageBlocks))
        {
            return angle::Result::Continue;
        }

        if (!LinkValidateProgramGlobalNames(infoLog, *this))
        {
            return angle::Result::Continue;
        }

        if (!linkOutputVariables(context->getCaps(), context->getExtensions(),
                                 context->getClientVersion(), combinedImageUniforms,
                                 combinedShaderStorageBlocks))
        {
            return angle::Result::Continue;
        }

        gl::Shader *vertexShader = mState.mAttachedShaders[ShaderType::Vertex];
        if (vertexShader)
        {
            mState.mNumViews = vertexShader->getNumViews();
            mState.mSpecConstUsageBits |= vertexShader->getSpecConstUsageBits();
        }

        gl::Shader *fragmentShader = mState.mAttachedShaders[ShaderType::Fragment];
        if (fragmentShader)
        {
            mState.mEarlyFramentTestsOptimization =
                fragmentShader->hasEarlyFragmentTestsOptimization();
            mState.mSpecConstUsageBits |= fragmentShader->getSpecConstUsageBits();
        }

        InitUniformBlockLinker(mState, &resources.uniformBlockLinker);
        InitShaderStorageBlockLinker(mState, &resources.shaderStorageBlockLinker);

        mergedVaryings = GetMergedVaryingsFromShaders(*this);
        if (!mState.mExecutable->linkMergedVaryings(context, *this, mergedVaryings,
                                                    mState.mTransformFeedbackVaryingNames,
                                                    isSeparable(), &resources.varyingPacking))
        {
            return angle::Result::Continue;
        }
    }

    updateLinkedShaderStages();

    mLinkingState                    = std::move(linkingState);
    mLinkingState->linkingFromBinary = false;
    mLinkingState->programHash       = programHash;
    mLinkingState->linkEvent         = mProgram->link(context, resources, infoLog, mergedVaryings);

    // Must be after mProgram->link() to avoid misleading the linker about output variables.
    mState.updateProgramInterfaceInputs();
    mState.updateProgramInterfaceOutputs();

    // Linking has succeeded, so we need to save some information that may get overwritten by a
    // later linkProgram() that could fail.
    if (mState.mSeparable)
    {
        mState.mExecutable->saveLinkedStateInfo(mState);
        mLinkingState->linkedExecutable = mState.mExecutable;
    }

    return angle::Result::Continue;
}

bool Program::isLinking() const
{
    return (mLinkingState.get() && mLinkingState->linkEvent &&
            mLinkingState->linkEvent->isLinking());
}

void Program::resolveLinkImpl(const Context *context)
{
    ASSERT(mLinkingState.get());

    angle::Result result = mLinkingState->linkEvent->wait(context);

    mLinked                                    = result == angle::Result::Continue;
    std::unique_ptr<LinkingState> linkingState = std::move(mLinkingState);
    if (!mLinked)
    {
        return;
    }

    if (linkingState->linkingFromBinary)
    {
        // All internal Program state is already loaded from the binary.
        return;
    }

    initInterfaceBlockBindings();

    // According to GLES 3.0/3.1 spec for LinkProgram and UseProgram,
    // Only successfully linked program can replace the executables.
    ASSERT(mLinked);

    // Mark implementation-specific unreferenced uniforms as ignored.
    std::vector<ImageBinding> *imageBindings = getExecutable().getImageBindings();
    mProgram->markUnusedUniformLocations(&mState.mUniformLocations,
                                         &mState.mExecutable->mSamplerBindings, imageBindings);

    // Must be called after markUnusedUniformLocations.
    postResolveLink(context);

    // Save to the program cache.
    std::lock_guard<std::mutex> cacheLock(context->getProgramCacheMutex());
    MemoryProgramCache *cache = context->getMemoryProgramCache();
    // TODO: http://anglebug.com/4530: Enable program caching for separable programs
    if (cache && !isSeparable() &&
        (mState.mExecutable->mLinkedTransformFeedbackVaryings.empty() ||
         !context->getFrontendFeatures().disableProgramCachingForTransformFeedback.enabled))
    {
        if (cache->putProgram(linkingState->programHash, context, this) == angle::Result::Stop)
        {
            // Don't fail linking if putting the program binary into the cache fails, the program is
            // still usable.
            WARN() << "Failed to save linked program to memory program cache.";
        }
    }
}

void Program::updateLinkedShaderStages()
{
    mState.mExecutable->resetLinkedShaderStages();

    for (const Shader *shader : mState.mAttachedShaders)
    {
        if (shader)
        {
            mState.mExecutable->setLinkedShaderStages(shader->getType());
        }
    }

    if (mState.mExecutable->hasLinkedShaderStage(ShaderType::Compute))
    {
        mState.mExecutable->setIsCompute(true);
    }
    else
    {
        mState.mExecutable->setIsCompute(false);
    }
}

void ProgramState::updateActiveSamplers()
{
    mExecutable->mActiveSamplerRefCounts.fill(0);
    mExecutable->updateActiveSamplers(*this);
}

void ProgramState::updateProgramInterfaceInputs()
{
    const ShaderType firstAttachedShaderType = getFirstAttachedShaderStageType();

    if (firstAttachedShaderType == ShaderType::Vertex)
    {
        // Vertex attributes are already what we need, so nothing to do
        return;
    }

    Shader *shader = getAttachedShader(firstAttachedShaderType);
    ASSERT(shader);

    // Copy over each input varying, since the Shader could go away
    if (shader->getType() == ShaderType::Compute)
    {
        for (const sh::ShaderVariable &attribute : shader->getAllAttributes())
        {
            // Compute Shaders have the following built-in input variables.
            //
            // in uvec3 gl_NumWorkGroups;
            // in uvec3 gl_WorkGroupID;
            // in uvec3 gl_LocalInvocationID;
            // in uvec3 gl_GlobalInvocationID;
            // in uint  gl_LocalInvocationIndex;
            // They are all vecs or uints, so no special handling is required.
            mExecutable->mProgramInputs.emplace_back(attribute);
        }
    }
    else
    {
        for (const sh::ShaderVariable &varying : shader->getInputVaryings())
        {
            sh::ShaderVariable var = sh::ShaderVariable(varying);
            UpdateInterfaceVariable(mExecutable->mProgramInputs, var);
        }
    }
}

void ProgramState::updateProgramInterfaceOutputs()
{
    const ShaderType lastAttachedShaderType = getLastAttachedShaderStageType();

    if (lastAttachedShaderType == ShaderType::Fragment)
    {
        // Fragment outputs are already what we need, so nothing to do
        return;
    }
    if (lastAttachedShaderType == ShaderType::Compute)
    {
        // If the program only contains a Compute Shader, then there are no user-defined outputs.
        return;
    }

    Shader *shader = getAttachedShader(lastAttachedShaderType);
    ASSERT(shader);

    // Copy over each output varying, since the Shader could go away
    for (const sh::ShaderVariable &varying : shader->getOutputVaryings())
    {
        sh::ShaderVariable var = sh::ShaderVariable(varying);
        UpdateInterfaceVariable(mExecutable->mOutputVariables, var);
    }
}

// Returns the program object to an unlinked state, before re-linking, or at destruction
void Program::unlink()
{
    if (mLinkingState && mLinkingState->linkedExecutable)
    {
        // The new ProgramExecutable that we'll attempt to link with needs to start from a copy of
        // the last successfully linked ProgramExecutable, so we don't lose any state information.
        mState.mExecutable.reset(new ProgramExecutable(*mLinkingState->linkedExecutable));
    }
    mState.mExecutable->reset();

    mState.mUniformLocations.clear();
    mState.mBufferVariables.clear();
    mState.mActiveUniformBlockBindings.reset();
    mState.mSecondaryOutputLocations.clear();
    mState.mOutputVariableTypes.clear();
    mState.mDrawBufferTypeMask.reset();
    mState.mYUVOutput = false;
    mState.mActiveOutputVariables.reset();
    mState.mComputeShaderLocalSize.fill(1);
    mState.mNumViews                      = -1;
    mState.mDrawIDLocation                = -1;
    mState.mBaseVertexLocation            = -1;
    mState.mBaseInstanceLocation          = -1;
    mState.mCachedBaseVertex              = 0;
    mState.mCachedBaseInstance            = 0;
    mState.mEarlyFramentTestsOptimization = false;
    mState.mSpecConstUsageBits.reset();

    mValidated = false;

    mLinked = false;
}

angle::Result Program::loadBinary(const Context *context,
                                  GLenum binaryFormat,
                                  const void *binary,
                                  GLsizei length)
{
    ASSERT(!mLinkingState);
    unlink();
    InfoLog &infoLog = mState.mExecutable->getInfoLog();

#if ANGLE_PROGRAM_BINARY_LOAD != ANGLE_ENABLED
    return angle::Result::Continue;
#else
    ASSERT(binaryFormat == GL_PROGRAM_BINARY_ANGLE);
    if (binaryFormat != GL_PROGRAM_BINARY_ANGLE)
    {
        infoLog << "Invalid program binary format.";
        return angle::Result::Continue;
    }

    BinaryInputStream stream(binary, length);
    ANGLE_TRY(deserialize(context, stream, infoLog));

    // Currently we require the full shader text to compute the program hash.
    // We could also store the binary in the internal program cache.

    for (size_t uniformBlockIndex = 0;
         uniformBlockIndex < mState.mExecutable->getActiveUniformBlockCount(); ++uniformBlockIndex)
    {
        mDirtyBits.set(uniformBlockIndex);
    }

    // The rx::LinkEvent returned from ProgramImpl::load is a base class with multiple
    // implementations. In some implementations, a background thread is used to compile the
    // shaders. Any calls to the LinkEvent object, therefore, are racy and may interfere with
    // the operation.

    // We do not want to call LinkEvent::wait because that will cause the background thread
    // to finish its task before returning, thus defeating the purpose of background compilation.
    // We need to defer waiting on background compilation until the very last minute when we
    // absolutely need the results, such as when the developer binds the program or queries
    // for the completion status.

    // If load returns nullptr, we know for sure that the binary is not compatible with the backend.
    // The loaded binary could have been read from the on-disk shader cache and be corrupted or
    // serialized with different revision and subsystem id than the currently loaded backend.
    // Returning 'Incomplete' to the caller results in link happening using the original shader
    // sources.
    angle::Result result;
    std::unique_ptr<LinkingState> linkingState;
    std::unique_ptr<rx::LinkEvent> linkEvent = mProgram->load(context, &stream, infoLog);
    if (linkEvent)
    {
        linkingState                    = std::make_unique<LinkingState>();
        linkingState->linkingFromBinary = true;
        linkingState->linkEvent         = std::move(linkEvent);
        result                          = angle::Result::Continue;
    }
    else
    {
        result = angle::Result::Incomplete;
    }
    mLinkingState = std::move(linkingState);

    return result;
#endif  // #if ANGLE_PROGRAM_BINARY_LOAD == ANGLE_ENABLED
}

angle::Result Program::saveBinary(Context *context,
                                  GLenum *binaryFormat,
                                  void *binary,
                                  GLsizei bufSize,
                                  GLsizei *length) const
{
    ASSERT(!mLinkingState);
    if (binaryFormat)
    {
        *binaryFormat = GL_PROGRAM_BINARY_ANGLE;
    }

    angle::MemoryBuffer memoryBuf;
    ANGLE_TRY(serialize(context, &memoryBuf));

    GLsizei streamLength       = static_cast<GLsizei>(memoryBuf.size());
    const uint8_t *streamState = memoryBuf.data();

    if (streamLength > bufSize)
    {
        if (length)
        {
            *length = 0;
        }

        // TODO: This should be moved to the validation layer but computing the size of the binary
        // before saving it causes the save to happen twice.  It may be possible to write the binary
        // to a separate buffer, validate sizes and then copy it.
        ANGLE_CHECK(context, false, "Insufficient buffer size", GL_INVALID_OPERATION);
    }

    if (binary)
    {
        char *ptr = reinterpret_cast<char *>(binary);

        memcpy(ptr, streamState, streamLength);
        ptr += streamLength;

        ASSERT(ptr - streamLength == binary);
    }

    if (length)
    {
        *length = streamLength;
    }

    return angle::Result::Continue;
}

GLint Program::getBinaryLength(Context *context) const
{
    ASSERT(!mLinkingState);
    if (!mLinked)
    {
        return 0;
    }

    GLint length;
    angle::Result result =
        saveBinary(context, nullptr, nullptr, std::numeric_limits<GLint>::max(), &length);
    if (result != angle::Result::Continue)
    {
        return 0;
    }

    return length;
}

void Program::setBinaryRetrievableHint(bool retrievable)
{
    ASSERT(!mLinkingState);
    // TODO(jmadill) : replace with dirty bits
    mProgram->setBinaryRetrievableHint(retrievable);
    mState.mBinaryRetrieveableHint = retrievable;
}

bool Program::getBinaryRetrievableHint() const
{
    ASSERT(!mLinkingState);
    return mState.mBinaryRetrieveableHint;
}

void Program::setSeparable(bool separable)
{
    ASSERT(!mLinkingState);
    // TODO(yunchao) : replace with dirty bits
    if (mState.mSeparable != separable)
    {
        mProgram->setSeparable(separable);
        mState.mSeparable = separable;
    }
}

bool Program::isSeparable() const
{
    ASSERT(!mLinkingState);
    return mState.mSeparable;
}

void Program::deleteSelf(const Context *context)
{
    ASSERT(mRefCount == 0 && mDeleteStatus);
    mResourceManager->deleteProgram(context, mHandle);
}

unsigned int Program::getRefCount() const
{
    return mRefCount;
}

void Program::getAttachedShaders(GLsizei maxCount, GLsizei *count, ShaderProgramID *shaders) const
{
    ASSERT(!mLinkingState);
    int total = 0;

    for (const Shader *shader : mState.mAttachedShaders)
    {
        if (shader && (total < maxCount))
        {
            shaders[total] = shader->getHandle();
            ++total;
        }
    }

    if (count)
    {
        *count = total;
    }
}

GLuint Program::getAttributeLocation(const std::string &name) const
{
    ASSERT(!mLinkingState);
    return mState.getAttributeLocation(name);
}

void Program::getActiveAttribute(GLuint index,
                                 GLsizei bufsize,
                                 GLsizei *length,
                                 GLint *size,
                                 GLenum *type,
                                 GLchar *name) const
{
    ASSERT(!mLinkingState);
    if (!mLinked)
    {
        if (bufsize > 0)
        {
            name[0] = '\0';
        }

        if (length)
        {
            *length = 0;
        }

        *type = GL_NONE;
        *size = 1;
        return;
    }

    ASSERT(index < mState.mExecutable->getProgramInputs().size());
    const sh::ShaderVariable &attrib = mState.mExecutable->getProgramInputs()[index];

    if (bufsize > 0)
    {
        CopyStringToBuffer(name, attrib.name, bufsize, length);
    }

    // Always a single 'type' instance
    *size = 1;
    *type = attrib.type;
}

GLint Program::getActiveAttributeCount() const
{
    ASSERT(!mLinkingState);
    if (!mLinked)
    {
        return 0;
    }

    return static_cast<GLint>(mState.mExecutable->getProgramInputs().size());
}

GLint Program::getActiveAttributeMaxLength() const
{
    ASSERT(!mLinkingState);
    if (!mLinked)
    {
        return 0;
    }

    size_t maxLength = 0;

    for (const sh::ShaderVariable &attrib : mState.mExecutable->getProgramInputs())
    {
        maxLength = std::max(attrib.name.length() + 1, maxLength);
    }

    return static_cast<GLint>(maxLength);
}

const std::vector<sh::ShaderVariable> &Program::getAttributes() const
{
    ASSERT(!mLinkingState);
    return mState.mExecutable->getProgramInputs();
}

const sh::WorkGroupSize &Program::getComputeShaderLocalSize() const
{
    ASSERT(!mLinkingState);
    return mState.mComputeShaderLocalSize;
}

PrimitiveMode Program::getGeometryShaderInputPrimitiveType() const
{
    ASSERT(!mLinkingState && mState.mExecutable);
    return mState.mExecutable->getGeometryShaderInputPrimitiveType();
}
PrimitiveMode Program::getGeometryShaderOutputPrimitiveType() const
{
    ASSERT(!mLinkingState && mState.mExecutable);
    return mState.mExecutable->getGeometryShaderOutputPrimitiveType();
}
GLint Program::getGeometryShaderInvocations() const
{
    ASSERT(!mLinkingState && mState.mExecutable);
    return mState.mExecutable->getGeometryShaderInvocations();
}
GLint Program::getGeometryShaderMaxVertices() const
{
    ASSERT(!mLinkingState && mState.mExecutable);
    return mState.mExecutable->getGeometryShaderMaxVertices();
}

const sh::ShaderVariable &Program::getInputResource(size_t index) const
{
    ASSERT(!mLinkingState);
    ASSERT(index < mState.mExecutable->getProgramInputs().size());
    return mState.mExecutable->getProgramInputs()[index];
}

GLuint Program::getInputResourceIndex(const GLchar *name) const
{
    ASSERT(!mLinkingState);
    const std::string nameString = StripLastArrayIndex(name);

    for (size_t index = 0; index < mState.mExecutable->getProgramInputs().size(); index++)
    {
        sh::ShaderVariable resource = getInputResource(index);
        if (resource.name == nameString)
        {
            return static_cast<GLuint>(index);
        }
    }

    return GL_INVALID_INDEX;
}

GLuint Program::getResourceMaxNameSize(const sh::ShaderVariable &resource, GLint max) const
{
    if (resource.isArray())
    {
        return std::max(max, clampCast<GLint>((resource.name + "[0]").size()));
    }
    else
    {
        return std::max(max, clampCast<GLint>((resource.name).size()));
    }
}

GLuint Program::getInputResourceMaxNameSize() const
{
    GLint max = 0;

    for (const sh::ShaderVariable &resource : mState.mExecutable->getProgramInputs())
    {
        max = getResourceMaxNameSize(resource, max);
    }

    return max;
}

GLuint Program::getOutputResourceMaxNameSize() const
{
    GLint max = 0;

    for (const sh::ShaderVariable &resource : mState.mExecutable->getOutputVariables())
    {
        max = getResourceMaxNameSize(resource, max);
    }

    return max;
}

GLuint Program::getResourceLocation(const GLchar *name, const sh::ShaderVariable &variable) const
{
    if (variable.isBuiltIn())
    {
        return GL_INVALID_INDEX;
    }

    GLint location = variable.location;
    if (variable.isArray())
    {
        size_t nameLengthWithoutArrayIndexOut;
        size_t arrayIndex = ParseArrayIndex(name, &nameLengthWithoutArrayIndexOut);
        // The 'name' string may not contain the array notation "[0]"
        if (arrayIndex != GL_INVALID_INDEX)
        {
            location += arrayIndex;
        }
    }

    return location;
}

GLuint Program::getInputResourceLocation(const GLchar *name) const
{
    const GLuint index = getInputResourceIndex(name);
    if (index == GL_INVALID_INDEX)
    {
        return index;
    }

    const sh::ShaderVariable &variable = getInputResource(index);

    return getResourceLocation(name, variable);
}

GLuint Program::getOutputResourceLocation(const GLchar *name) const
{
    const GLuint index = getOutputResourceIndex(name);
    if (index == GL_INVALID_INDEX)
    {
        return index;
    }

    const sh::ShaderVariable &variable = getOutputResource(index);

    return getResourceLocation(name, variable);
}

GLuint Program::getOutputResourceIndex(const GLchar *name) const
{
    ASSERT(!mLinkingState);
    const std::string nameString = StripLastArrayIndex(name);

    for (size_t index = 0; index < mState.mExecutable->getOutputVariables().size(); index++)
    {
        sh::ShaderVariable resource = getOutputResource(index);
        if (resource.name == nameString)
        {
            return static_cast<GLuint>(index);
        }
    }

    return GL_INVALID_INDEX;
}

size_t Program::getOutputResourceCount() const
{
    ASSERT(!mLinkingState);
    return (mLinked ? mState.mExecutable->getOutputVariables().size() : 0);
}

const std::vector<GLenum> &Program::getOutputVariableTypes() const
{
    ASSERT(!mLinkingState);
    return mState.mOutputVariableTypes;
}

void Program::getResourceName(const std::string name,
                              GLsizei bufSize,
                              GLsizei *length,
                              GLchar *dest) const
{
    if (length)
    {
        *length = 0;
    }

    if (!mLinked)
    {
        if (bufSize > 0)
        {
            dest[0] = '\0';
        }
        return;
    }

    if (bufSize > 0)
    {
        CopyStringToBuffer(dest, name, bufSize, length);
    }
}

void Program::getInputResourceName(GLuint index,
                                   GLsizei bufSize,
                                   GLsizei *length,
                                   GLchar *name) const
{
    ASSERT(!mLinkingState);
    getResourceName(getInputResourceName(index), bufSize, length, name);
}

void Program::getOutputResourceName(GLuint index,
                                    GLsizei bufSize,
                                    GLsizei *length,
                                    GLchar *name) const
{
    ASSERT(!mLinkingState);
    getResourceName(getOutputResourceName(index), bufSize, length, name);
}

void Program::getUniformResourceName(GLuint index,
                                     GLsizei bufSize,
                                     GLsizei *length,
                                     GLchar *name) const
{
    ASSERT(!mLinkingState);
    ASSERT(index < mState.mExecutable->getUniforms().size());
    getResourceName(mState.mExecutable->getUniforms()[index].name, bufSize, length, name);
}

void Program::getBufferVariableResourceName(GLuint index,
                                            GLsizei bufSize,
                                            GLsizei *length,
                                            GLchar *name) const
{
    ASSERT(!mLinkingState);
    ASSERT(index < mState.mBufferVariables.size());
    getResourceName(mState.mBufferVariables[index].name, bufSize, length, name);
}

const std::string Program::getResourceName(const sh::ShaderVariable &resource) const
{
    std::string resourceName = resource.name;

    if (resource.isArray())
    {
        resourceName += "[0]";
    }

    return resourceName;
}

const std::string Program::getInputResourceName(GLuint index) const
{
    ASSERT(!mLinkingState);
    const sh::ShaderVariable &resource = getInputResource(index);

    return getResourceName(resource);
}

const std::string Program::getOutputResourceName(GLuint index) const
{
    ASSERT(!mLinkingState);
    const sh::ShaderVariable &resource = getOutputResource(index);

    return getResourceName(resource);
}

const sh::ShaderVariable &Program::getOutputResource(size_t index) const
{
    ASSERT(!mLinkingState);
    ASSERT(index < mState.mExecutable->getOutputVariables().size());
    return mState.mExecutable->getOutputVariables()[index];
}

const ProgramBindings &Program::getAttributeBindings() const
{
    ASSERT(!mLinkingState);
    return mAttributeBindings;
}
const ProgramAliasedBindings &Program::getUniformLocationBindings() const
{
    ASSERT(!mLinkingState);
    return mState.mUniformLocationBindings;
}

const gl::ProgramAliasedBindings &Program::getFragmentOutputLocations() const
{
    ASSERT(!mLinkingState);
    return mFragmentOutputLocations;
}

const gl::ProgramAliasedBindings &Program::getFragmentOutputIndexes() const
{
    ASSERT(!mLinkingState);
    return mFragmentOutputIndexes;
}

ComponentTypeMask Program::getDrawBufferTypeMask() const
{
    ASSERT(!mLinkingState);
    return mState.mDrawBufferTypeMask;
}

const std::vector<GLsizei> &Program::getTransformFeedbackStrides() const
{
    ASSERT(!mLinkingState);
    return mState.mExecutable->getTransformFeedbackStrides();
}

GLint Program::getFragDataLocation(const std::string &name) const
{
    ASSERT(!mLinkingState);
    GLint primaryLocation = GetVariableLocation(mState.mExecutable->getOutputVariables(),
                                                mState.mExecutable->getOutputLocations(), name);
    if (primaryLocation != -1)
    {
        return primaryLocation;
    }
    return GetVariableLocation(mState.mExecutable->getOutputVariables(),
                               mState.mSecondaryOutputLocations, name);
}

GLint Program::getFragDataIndex(const std::string &name) const
{
    ASSERT(!mLinkingState);
    if (GetVariableLocation(mState.mExecutable->getOutputVariables(),
                            mState.mExecutable->getOutputLocations(), name) != -1)
    {
        return 0;
    }
    if (GetVariableLocation(mState.mExecutable->getOutputVariables(),
                            mState.mSecondaryOutputLocations, name) != -1)
    {
        return 1;
    }
    return -1;
}

void Program::getActiveUniform(GLuint index,
                               GLsizei bufsize,
                               GLsizei *length,
                               GLint *size,
                               GLenum *type,
                               GLchar *name) const
{
    ASSERT(!mLinkingState);
    if (mLinked)
    {
        // index must be smaller than getActiveUniformCount()
        ASSERT(index < mState.mExecutable->getUniforms().size());
        const LinkedUniform &uniform = mState.mExecutable->getUniforms()[index];

        if (bufsize > 0)
        {
            std::string string = uniform.name;
            CopyStringToBuffer(name, string, bufsize, length);
        }

        *size = clampCast<GLint>(uniform.getBasicTypeElementCount());
        *type = uniform.type;
    }
    else
    {
        if (bufsize > 0)
        {
            name[0] = '\0';
        }

        if (length)
        {
            *length = 0;
        }

        *size = 0;
        *type = GL_NONE;
    }
}

GLint Program::getActiveUniformCount() const
{
    ASSERT(!mLinkingState);
    if (mLinked)
    {
        return static_cast<GLint>(mState.mExecutable->getUniforms().size());
    }
    else
    {
        return 0;
    }
}

size_t Program::getActiveBufferVariableCount() const
{
    ASSERT(!mLinkingState);
    return mLinked ? mState.mBufferVariables.size() : 0;
}

GLint Program::getActiveUniformMaxLength() const
{
    ASSERT(!mLinkingState);
    size_t maxLength = 0;

    if (mLinked)
    {
        for (const LinkedUniform &uniform : mState.mExecutable->getUniforms())
        {
            if (!uniform.name.empty())
            {
                size_t length = uniform.name.length() + 1u;
                if (uniform.isArray())
                {
                    length += 3;  // Counting in "[0]".
                }
                maxLength = std::max(length, maxLength);
            }
        }
    }

    return static_cast<GLint>(maxLength);
}

bool Program::isValidUniformLocation(UniformLocation location) const
{
    ASSERT(!mLinkingState);
    ASSERT(angle::IsValueInRangeForNumericType<GLint>(mState.mUniformLocations.size()));
    return (location.value >= 0 &&
            static_cast<size_t>(location.value) < mState.mUniformLocations.size() &&
            mState.mUniformLocations[static_cast<size_t>(location.value)].used());
}

const LinkedUniform &Program::getUniformByLocation(UniformLocation location) const
{
    ASSERT(!mLinkingState);
    ASSERT(location.value >= 0 &&
           static_cast<size_t>(location.value) < mState.mUniformLocations.size());
    return mState.mExecutable->getUniforms()[mState.getUniformIndexFromLocation(location)];
}

const VariableLocation &Program::getUniformLocation(UniformLocation location) const
{
    ASSERT(!mLinkingState);
    ASSERT(location.value >= 0 &&
           static_cast<size_t>(location.value) < mState.mUniformLocations.size());
    return mState.mUniformLocations[location.value];
}

const BufferVariable &Program::getBufferVariableByIndex(GLuint index) const
{
    ASSERT(!mLinkingState);
    ASSERT(index < static_cast<size_t>(mState.mBufferVariables.size()));
    return mState.mBufferVariables[index];
}

UniformLocation Program::getUniformLocation(const std::string &name) const
{
    ASSERT(!mLinkingState);
    return {GetVariableLocation(mState.mExecutable->getUniforms(), mState.mUniformLocations, name)};
}

GLuint Program::getUniformIndex(const std::string &name) const
{
    ASSERT(!mLinkingState);
    return mState.getUniformIndexFromName(name);
}

bool Program::shouldIgnoreUniform(UniformLocation location) const
{
    if (location.value == -1)
    {
        return true;
    }

    if (mState.mUniformLocations[static_cast<size_t>(location.value)].ignored)
    {
        return true;
    }

    return false;
}

void Program::setUniform1fv(UniformLocation location, GLsizei count, const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 1, v);
    mProgram->setUniform1fv(location.value, clampedCount, v);
}

void Program::setUniform2fv(UniformLocation location, GLsizei count, const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 2, v);
    mProgram->setUniform2fv(location.value, clampedCount, v);
}

void Program::setUniform3fv(UniformLocation location, GLsizei count, const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 3, v);
    mProgram->setUniform3fv(location.value, clampedCount, v);
}

void Program::setUniform4fv(UniformLocation location, GLsizei count, const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 4, v);
    mProgram->setUniform4fv(location.value, clampedCount, v);
}

void Program::setUniform1iv(Context *context,
                            UniformLocation location,
                            GLsizei count,
                            const GLint *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 1, v);

    mProgram->setUniform1iv(location.value, clampedCount, v);

    if (mState.isSamplerUniformIndex(locationInfo.index))
    {
        updateSamplerUniform(context, locationInfo, clampedCount, v);
    }
}

void Program::setUniform2iv(UniformLocation location, GLsizei count, const GLint *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 2, v);
    mProgram->setUniform2iv(location.value, clampedCount, v);
}

void Program::setUniform3iv(UniformLocation location, GLsizei count, const GLint *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 3, v);
    mProgram->setUniform3iv(location.value, clampedCount, v);
}

void Program::setUniform4iv(UniformLocation location, GLsizei count, const GLint *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 4, v);
    mProgram->setUniform4iv(location.value, clampedCount, v);
}

void Program::setUniform1uiv(UniformLocation location, GLsizei count, const GLuint *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 1, v);
    mProgram->setUniform1uiv(location.value, clampedCount, v);
}

void Program::setUniform2uiv(UniformLocation location, GLsizei count, const GLuint *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 2, v);
    mProgram->setUniform2uiv(location.value, clampedCount, v);
}

void Program::setUniform3uiv(UniformLocation location, GLsizei count, const GLuint *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 3, v);
    mProgram->setUniform3uiv(location.value, clampedCount, v);
}

void Program::setUniform4uiv(UniformLocation location, GLsizei count, const GLuint *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];
    GLsizei clampedCount                 = clampUniformCount(locationInfo, count, 4, v);
    mProgram->setUniform4uiv(location.value, clampedCount, v);
}

void Program::setUniformMatrix2fv(UniformLocation location,
                                  GLsizei count,
                                  GLboolean transpose,
                                  const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    GLsizei clampedCount = clampMatrixUniformCount<2, 2>(location, count, transpose, v);
    mProgram->setUniformMatrix2fv(location.value, clampedCount, transpose, v);
}

void Program::setUniformMatrix3fv(UniformLocation location,
                                  GLsizei count,
                                  GLboolean transpose,
                                  const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    GLsizei clampedCount = clampMatrixUniformCount<3, 3>(location, count, transpose, v);
    mProgram->setUniformMatrix3fv(location.value, clampedCount, transpose, v);
}

void Program::setUniformMatrix4fv(UniformLocation location,
                                  GLsizei count,
                                  GLboolean transpose,
                                  const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    GLsizei clampedCount = clampMatrixUniformCount<4, 4>(location, count, transpose, v);
    mProgram->setUniformMatrix4fv(location.value, clampedCount, transpose, v);
}

void Program::setUniformMatrix2x3fv(UniformLocation location,
                                    GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    GLsizei clampedCount = clampMatrixUniformCount<2, 3>(location, count, transpose, v);
    mProgram->setUniformMatrix2x3fv(location.value, clampedCount, transpose, v);
}

void Program::setUniformMatrix2x4fv(UniformLocation location,
                                    GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    GLsizei clampedCount = clampMatrixUniformCount<2, 4>(location, count, transpose, v);
    mProgram->setUniformMatrix2x4fv(location.value, clampedCount, transpose, v);
}

void Program::setUniformMatrix3x2fv(UniformLocation location,
                                    GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    GLsizei clampedCount = clampMatrixUniformCount<3, 2>(location, count, transpose, v);
    mProgram->setUniformMatrix3x2fv(location.value, clampedCount, transpose, v);
}

void Program::setUniformMatrix3x4fv(UniformLocation location,
                                    GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    GLsizei clampedCount = clampMatrixUniformCount<3, 4>(location, count, transpose, v);
    mProgram->setUniformMatrix3x4fv(location.value, clampedCount, transpose, v);
}

void Program::setUniformMatrix4x2fv(UniformLocation location,
                                    GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    GLsizei clampedCount = clampMatrixUniformCount<4, 2>(location, count, transpose, v);
    mProgram->setUniformMatrix4x2fv(location.value, clampedCount, transpose, v);
}

void Program::setUniformMatrix4x3fv(UniformLocation location,
                                    GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *v)
{
    ASSERT(!mLinkingState);
    if (shouldIgnoreUniform(location))
    {
        return;
    }

    GLsizei clampedCount = clampMatrixUniformCount<4, 3>(location, count, transpose, v);
    mProgram->setUniformMatrix4x3fv(location.value, clampedCount, transpose, v);
}

GLuint Program::getSamplerUniformBinding(const VariableLocation &uniformLocation) const
{
    ASSERT(!mLinkingState);
    GLuint samplerIndex = mState.getSamplerIndexFromUniformIndex(uniformLocation.index);
    const std::vector<GLuint> &boundTextureUnits =
        mState.mExecutable->mSamplerBindings[samplerIndex].boundTextureUnits;
    return (uniformLocation.arrayIndex < boundTextureUnits.size())
               ? boundTextureUnits[uniformLocation.arrayIndex]
               : 0;
}

GLuint Program::getImageUniformBinding(const VariableLocation &uniformLocation) const
{
    ASSERT(!mLinkingState);
    GLuint imageIndex = mState.getImageIndexFromUniformIndex(uniformLocation.index);

    const std::vector<ImageBinding> &imageBindings = getExecutable().getImageBindings();
    const std::vector<GLuint> &boundImageUnits     = imageBindings[imageIndex].boundImageUnits;
    return boundImageUnits[uniformLocation.arrayIndex];
}

void Program::getUniformfv(const Context *context, UniformLocation location, GLfloat *v) const
{
    ASSERT(!mLinkingState);
    const VariableLocation &uniformLocation = mState.getUniformLocations()[location.value];
    const LinkedUniform &uniform            = mState.getUniforms()[uniformLocation.index];

    if (uniform.isSampler())
    {
        *v = static_cast<GLfloat>(getSamplerUniformBinding(uniformLocation));
        return;
    }
    else if (uniform.isImage())
    {
        *v = static_cast<GLfloat>(getImageUniformBinding(uniformLocation));
        return;
    }

    const GLenum nativeType = gl::VariableComponentType(uniform.type);
    if (nativeType == GL_FLOAT)
    {
        mProgram->getUniformfv(context, location.value, v);
    }
    else
    {
        getUniformInternal(context, v, location, nativeType, VariableComponentCount(uniform.type));
    }
}

void Program::getUniformiv(const Context *context, UniformLocation location, GLint *v) const
{
    ASSERT(!mLinkingState);
    const VariableLocation &uniformLocation = mState.getUniformLocations()[location.value];
    const LinkedUniform &uniform            = mState.getUniforms()[uniformLocation.index];

    if (uniform.isSampler())
    {
        *v = static_cast<GLint>(getSamplerUniformBinding(uniformLocation));
        return;
    }
    else if (uniform.isImage())
    {
        *v = static_cast<GLint>(getImageUniformBinding(uniformLocation));
        return;
    }

    const GLenum nativeType = gl::VariableComponentType(uniform.type);
    if (nativeType == GL_INT || nativeType == GL_BOOL)
    {
        mProgram->getUniformiv(context, location.value, v);
    }
    else
    {
        getUniformInternal(context, v, location, nativeType, VariableComponentCount(uniform.type));
    }
}

void Program::getUniformuiv(const Context *context, UniformLocation location, GLuint *v) const
{
    ASSERT(!mLinkingState);
    const VariableLocation &uniformLocation = mState.getUniformLocations()[location.value];
    const LinkedUniform &uniform            = mState.getUniforms()[uniformLocation.index];

    if (uniform.isSampler())
    {
        *v = getSamplerUniformBinding(uniformLocation);
        return;
    }
    else if (uniform.isImage())
    {
        *v = getImageUniformBinding(uniformLocation);
        return;
    }

    const GLenum nativeType = VariableComponentType(uniform.type);
    if (nativeType == GL_UNSIGNED_INT)
    {
        mProgram->getUniformuiv(context, location.value, v);
    }
    else
    {
        getUniformInternal(context, v, location, nativeType, VariableComponentCount(uniform.type));
    }
}

void Program::flagForDeletion()
{
    ASSERT(!mLinkingState);
    mDeleteStatus = true;
}

bool Program::isFlaggedForDeletion() const
{
    ASSERT(!mLinkingState);
    return mDeleteStatus;
}

void Program::validate(const Caps &caps)
{
    ASSERT(!mLinkingState);
    mState.mExecutable->resetInfoLog();
    InfoLog &infoLog = mState.mExecutable->getInfoLog();

    if (mLinked)
    {
        mValidated = ConvertToBool(mProgram->validate(caps, &infoLog));
    }
    else
    {
        infoLog << "Program has not been successfully linked.";
    }
}

bool Program::validateSamplersImpl(InfoLog *infoLog, const Caps &caps)
{
    const ProgramExecutable *executable = mState.mExecutable.get();
    ASSERT(!mLinkingState);

    // if any two active samplers in a program are of different types, but refer to the same
    // texture image unit, and this is the current program, then ValidateProgram will fail, and
    // DrawArrays and DrawElements will issue the INVALID_OPERATION error.
    for (size_t textureUnit : executable->mActiveSamplersMask)
    {
        if (executable->mActiveSamplerTypes[textureUnit] == TextureType::InvalidEnum)
        {
            if (infoLog)
            {
                (*infoLog) << "Samplers of conflicting types refer to the same texture "
                              "image unit ("
                           << textureUnit << ").";
            }

            mCachedValidateSamplersResult = false;
            return false;
        }
    }

    mCachedValidateSamplersResult = true;
    return true;
}

bool Program::isValidated() const
{
    ASSERT(!mLinkingState);
    return mValidated;
}

void Program::getActiveUniformBlockName(const GLuint blockIndex,
                                        GLsizei bufSize,
                                        GLsizei *length,
                                        GLchar *blockName) const
{
    ASSERT(!mLinkingState);
    GetInterfaceBlockName(blockIndex, mState.mExecutable->getUniformBlocks(), bufSize, length,
                          blockName);
}

void Program::getActiveShaderStorageBlockName(const GLuint blockIndex,
                                              GLsizei bufSize,
                                              GLsizei *length,
                                              GLchar *blockName) const
{
    ASSERT(!mLinkingState);
    GetInterfaceBlockName(blockIndex, mState.mExecutable->getShaderStorageBlocks(), bufSize, length,
                          blockName);
}

template <typename T>
GLint Program::getActiveInterfaceBlockMaxNameLength(const std::vector<T> &resources) const
{
    int maxLength = 0;

    if (mLinked)
    {
        for (const T &resource : resources)
        {
            if (!resource.name.empty())
            {
                int length = static_cast<int>(resource.nameWithArrayIndex().length());
                maxLength  = std::max(length + 1, maxLength);
            }
        }
    }

    return maxLength;
}

GLint Program::getActiveUniformBlockMaxNameLength() const
{
    ASSERT(!mLinkingState);
    return getActiveInterfaceBlockMaxNameLength(mState.mExecutable->getUniformBlocks());
}

GLint Program::getActiveShaderStorageBlockMaxNameLength() const
{
    ASSERT(!mLinkingState);
    return getActiveInterfaceBlockMaxNameLength(mState.mExecutable->getShaderStorageBlocks());
}

GLuint Program::getUniformBlockIndex(const std::string &name) const
{
    ASSERT(!mLinkingState);
    return GetInterfaceBlockIndex(mState.mExecutable->getUniformBlocks(), name);
}

GLuint Program::getShaderStorageBlockIndex(const std::string &name) const
{
    ASSERT(!mLinkingState);
    return GetInterfaceBlockIndex(mState.mExecutable->getShaderStorageBlocks(), name);
}

const InterfaceBlock &Program::getUniformBlockByIndex(GLuint index) const
{
    ASSERT(!mLinkingState);
    ASSERT(index < static_cast<GLuint>(mState.mExecutable->getActiveUniformBlockCount()));
    return mState.mExecutable->getUniformBlocks()[index];
}

const InterfaceBlock &Program::getShaderStorageBlockByIndex(GLuint index) const
{
    ASSERT(!mLinkingState);
    ASSERT(index < static_cast<GLuint>(mState.mExecutable->getActiveShaderStorageBlockCount()));
    return mState.mExecutable->getShaderStorageBlocks()[index];
}

void Program::bindUniformBlock(GLuint uniformBlockIndex, GLuint uniformBlockBinding)
{
    ASSERT(!mLinkingState);
    mState.mExecutable->mUniformBlocks[uniformBlockIndex].binding = uniformBlockBinding;
    mState.mActiveUniformBlockBindings.set(uniformBlockIndex, uniformBlockBinding != 0);
    mDirtyBits.set(DIRTY_BIT_UNIFORM_BLOCK_BINDING_0 + uniformBlockIndex);
}

GLuint Program::getUniformBlockBinding(GLuint uniformBlockIndex) const
{
    ASSERT(!mLinkingState);
    return mState.getUniformBlockBinding(uniformBlockIndex);
}

GLuint Program::getShaderStorageBlockBinding(GLuint shaderStorageBlockIndex) const
{
    ASSERT(!mLinkingState);
    return mState.getShaderStorageBlockBinding(shaderStorageBlockIndex);
}

void Program::setTransformFeedbackVaryings(GLsizei count,
                                           const GLchar *const *varyings,
                                           GLenum bufferMode)
{
    ASSERT(!mLinkingState);
    mState.mTransformFeedbackVaryingNames.resize(count);
    for (GLsizei i = 0; i < count; i++)
    {
        mState.mTransformFeedbackVaryingNames[i] = varyings[i];
    }

    mState.mExecutable->mTransformFeedbackBufferMode = bufferMode;
}

void Program::getTransformFeedbackVarying(GLuint index,
                                          GLsizei bufSize,
                                          GLsizei *length,
                                          GLsizei *size,
                                          GLenum *type,
                                          GLchar *name) const
{
    ASSERT(!mLinkingState);
    if (mLinked)
    {
        ASSERT(index < mState.mExecutable->mLinkedTransformFeedbackVaryings.size());
        const auto &var     = mState.mExecutable->mLinkedTransformFeedbackVaryings[index];
        std::string varName = var.nameWithArrayIndex();
        GLsizei lastNameIdx = std::min(bufSize - 1, static_cast<GLsizei>(varName.length()));
        if (length)
        {
            *length = lastNameIdx;
        }
        if (size)
        {
            *size = var.size();
        }
        if (type)
        {
            *type = var.type;
        }
        if (name)
        {
            memcpy(name, varName.c_str(), lastNameIdx);
            name[lastNameIdx] = '\0';
        }
    }
}

GLsizei Program::getTransformFeedbackVaryingCount() const
{
    ASSERT(!mLinkingState);
    if (mLinked)
    {
        return static_cast<GLsizei>(mState.mExecutable->mLinkedTransformFeedbackVaryings.size());
    }
    else
    {
        return 0;
    }
}

GLsizei Program::getTransformFeedbackVaryingMaxLength() const
{
    ASSERT(!mLinkingState);
    if (mLinked)
    {
        GLsizei maxSize = 0;
        for (const auto &var : mState.mExecutable->mLinkedTransformFeedbackVaryings)
        {
            maxSize =
                std::max(maxSize, static_cast<GLsizei>(var.nameWithArrayIndex().length() + 1));
        }

        return maxSize;
    }
    else
    {
        return 0;
    }
}

GLenum Program::getTransformFeedbackBufferMode() const
{
    ASSERT(!mLinkingState);
    return mState.mExecutable->getTransformFeedbackBufferMode();
}

bool Program::linkValidateShaders(InfoLog &infoLog)
{
    Shader *vertexShader   = mState.mAttachedShaders[ShaderType::Vertex];
    Shader *fragmentShader = mState.mAttachedShaders[ShaderType::Fragment];
    Shader *computeShader  = mState.mAttachedShaders[ShaderType::Compute];
    Shader *geometryShader = mState.mAttachedShaders[ShaderType::Geometry];

    bool isComputeShaderAttached = (computeShader != nullptr);
    bool isGraphicsShaderAttached =
        (vertexShader != nullptr || fragmentShader != nullptr || geometryShader != nullptr);
    // Check whether we both have a compute and non-compute shaders attached.
    // If there are of both types attached, then linking should fail.
    // OpenGL ES 3.10, 7.3 Program Objects, under LinkProgram
    if (isComputeShaderAttached == true && isGraphicsShaderAttached == true)
    {
        infoLog << "Both compute and graphics shaders are attached to the same program.";
        return false;
    }

    if (computeShader)
    {
        if (!computeShader->isCompiled())
        {
            infoLog << "Attached compute shader is not compiled.";
            return false;
        }
        ASSERT(computeShader->getType() == ShaderType::Compute);

        mState.mComputeShaderLocalSize = computeShader->getWorkGroupSize();

        // GLSL ES 3.10, 4.4.1.1 Compute Shader Inputs
        // If the work group size is not specified, a link time error should occur.
        if (!mState.mComputeShaderLocalSize.isDeclared())
        {
            infoLog << "Work group size is not specified.";
            return false;
        }
    }
    else
    {
        if (isSeparable())
        {
            if (!fragmentShader && !vertexShader && !geometryShader)
            {
                infoLog << "No compiled shaders.";
                return false;
            }

            ASSERT(!fragmentShader || fragmentShader->getType() == ShaderType::Fragment);
            if (fragmentShader && !fragmentShader->isCompiled())
            {
                infoLog << "Fragment shader is not compiled.";
                return false;
            }

            ASSERT(!vertexShader || vertexShader->getType() == ShaderType::Vertex);
            if (vertexShader && !vertexShader->isCompiled())
            {
                infoLog << "Vertex shader is not compiled.";
                return false;
            }

            ASSERT(!geometryShader || geometryShader->getType() == ShaderType::Geometry);
            if (geometryShader && !geometryShader->isCompiled())
            {
                infoLog << "Geometry shader is not compiled.";
                return false;
            }
        }
        else
        {
            if (!fragmentShader || !fragmentShader->isCompiled())
            {
                infoLog
                    << "No compiled fragment shader when at least one graphics shader is attached.";
                return false;
            }
            ASSERT(fragmentShader->getType() == ShaderType::Fragment);

            if (!vertexShader || !vertexShader->isCompiled())
            {
                infoLog
                    << "No compiled vertex shader when at least one graphics shader is attached.";
                return false;
            }
            ASSERT(vertexShader->getType() == ShaderType::Vertex);
        }

        if (vertexShader && fragmentShader)
        {
            int vertexShaderVersion   = vertexShader->getShaderVersion();
            int fragmentShaderVersion = fragmentShader->getShaderVersion();

            if (fragmentShaderVersion != vertexShaderVersion)
            {
                infoLog << "Fragment shader version does not match vertex shader version.";
                return false;
            }
        }

        if (geometryShader)
        {
            // [GL_EXT_geometry_shader] Chapter 7
            // Linking can fail for a variety of reasons as specified in the OpenGL ES Shading
            // Language Specification, as well as any of the following reasons:
            // * One or more of the shader objects attached to <program> are not compiled
            //   successfully.
            // * The shaders do not use the same shader language version.
            // * <program> contains objects to form a geometry shader, and
            //   - <program> is not separable and contains no objects to form a vertex shader; or
            //   - the input primitive type, output primitive type, or maximum output vertex count
            //     is not specified in the compiled geometry shader object.
            if (!geometryShader->isCompiled())
            {
                infoLog << "The attached geometry shader isn't compiled.";
                return false;
            }

            if (vertexShader &&
                (geometryShader->getShaderVersion() != vertexShader->getShaderVersion()))
            {
                infoLog << "Geometry shader version does not match vertex shader version.";
                return false;
            }
            ASSERT(geometryShader->getType() == ShaderType::Geometry);

            Optional<PrimitiveMode> inputPrimitive =
                geometryShader->getGeometryShaderInputPrimitiveType();
            if (!inputPrimitive.valid())
            {
                infoLog << "Input primitive type is not specified in the geometry shader.";
                return false;
            }

            Optional<PrimitiveMode> outputPrimitive =
                geometryShader->getGeometryShaderOutputPrimitiveType();
            if (!outputPrimitive.valid())
            {
                infoLog << "Output primitive type is not specified in the geometry shader.";
                return false;
            }

            Optional<GLint> maxVertices = geometryShader->getGeometryShaderMaxVertices();
            if (!maxVertices.valid())
            {
                infoLog << "'max_vertices' is not specified in the geometry shader.";
                return false;
            }

            mState.mExecutable->mGeometryShaderInputPrimitiveType  = inputPrimitive.value();
            mState.mExecutable->mGeometryShaderOutputPrimitiveType = outputPrimitive.value();
            mState.mExecutable->mGeometryShaderMaxVertices         = maxVertices.value();
            mState.mExecutable->mGeometryShaderInvocations =
                geometryShader->getGeometryShaderInvocations();
        }
    }

    return true;
}

GLuint Program::getTransformFeedbackVaryingResourceIndex(const GLchar *name) const
{
    ASSERT(!mLinkingState);
    for (GLuint tfIndex = 0; tfIndex < mState.mExecutable->mLinkedTransformFeedbackVaryings.size();
         ++tfIndex)
    {
        const auto &tf = mState.mExecutable->mLinkedTransformFeedbackVaryings[tfIndex];
        if (tf.nameWithArrayIndex() == name)
        {
            return tfIndex;
        }
    }
    return GL_INVALID_INDEX;
}

const TransformFeedbackVarying &Program::getTransformFeedbackVaryingResource(GLuint index) const
{
    ASSERT(!mLinkingState);
    ASSERT(index < mState.mExecutable->mLinkedTransformFeedbackVaryings.size());
    return mState.mExecutable->mLinkedTransformFeedbackVaryings[index];
}

bool Program::hasDrawIDUniform() const
{
    ASSERT(!mLinkingState);
    return mState.mDrawIDLocation >= 0;
}

void Program::setDrawIDUniform(GLint drawid)
{
    ASSERT(!mLinkingState);
    ASSERT(mState.mDrawIDLocation >= 0);
    mProgram->setUniform1iv(mState.mDrawIDLocation, 1, &drawid);
}

bool Program::hasBaseVertexUniform() const
{
    ASSERT(!mLinkingState);
    return mState.mBaseVertexLocation >= 0;
}

void Program::setBaseVertexUniform(GLint baseVertex)
{
    ASSERT(!mLinkingState);
    ASSERT(mState.mBaseVertexLocation >= 0);
    if (baseVertex == mState.mCachedBaseVertex)
    {
        return;
    }
    mState.mCachedBaseVertex = baseVertex;
    mProgram->setUniform1iv(mState.mBaseVertexLocation, 1, &baseVertex);
}

bool Program::hasBaseInstanceUniform() const
{
    ASSERT(!mLinkingState);
    return mState.mBaseInstanceLocation >= 0;
}

void Program::setBaseInstanceUniform(GLuint baseInstance)
{
    ASSERT(!mLinkingState);
    ASSERT(mState.mBaseInstanceLocation >= 0);
    if (baseInstance == mState.mCachedBaseInstance)
    {
        return;
    }
    mState.mCachedBaseInstance = baseInstance;
    GLint baseInstanceInt      = baseInstance;
    mProgram->setUniform1iv(mState.mBaseInstanceLocation, 1, &baseInstanceInt);
}

bool Program::linkVaryings(InfoLog &infoLog) const
{
    ShaderType previousShaderType = ShaderType::InvalidEnum;
    for (ShaderType shaderType : kAllGraphicsShaderTypes)
    {
        Shader *currentShader = mState.mAttachedShaders[shaderType];
        if (!currentShader)
        {
            continue;
        }

        if (previousShaderType != ShaderType::InvalidEnum)
        {
            Shader *previousShader = mState.mAttachedShaders[previousShaderType];
            const std::vector<sh::ShaderVariable> &outputVaryings =
                previousShader->getOutputVaryings();

            if (!LinkValidateShaderInterfaceMatching(
                    outputVaryings, currentShader->getInputVaryings(), previousShaderType,
                    currentShader->getType(), previousShader->getShaderVersion(),
                    currentShader->getShaderVersion(), isSeparable(), infoLog))
            {
                return false;
            }
        }
        previousShaderType = currentShader->getType();
    }

    Shader *vertexShader   = mState.mAttachedShaders[ShaderType::Vertex];
    Shader *fragmentShader = mState.mAttachedShaders[ShaderType::Fragment];
    if (vertexShader && fragmentShader &&
        !LinkValidateBuiltInVaryings(vertexShader->getOutputVaryings(),
                                     fragmentShader->getInputVaryings(),
                                     vertexShader->getShaderVersion(), infoLog))
    {
        return false;
    }

    return true;
}

bool Program::linkUniforms(const Caps &caps,
                           const Version &version,
                           InfoLog &infoLog,
                           const ProgramAliasedBindings &uniformLocationBindings,
                           GLuint *combinedImageUniformsCount,
                           std::vector<UnusedUniform> *unusedUniforms)
{
    UniformLinker linker(mState);
    if (!linker.link(caps, infoLog, uniformLocationBindings))
    {
        return false;
    }

    linker.getResults(&mState.mExecutable->mUniforms, unusedUniforms, &mState.mUniformLocations);

    linkSamplerAndImageBindings(combinedImageUniformsCount);

    if (!linkAtomicCounterBuffers())
    {
        return false;
    }

    if (version >= Version(3, 1))
    {
        GLint locationSize = static_cast<GLint>(mState.getUniformLocations().size());

        if (locationSize > caps.maxUniformLocations)
        {
            infoLog << "Exceeded maximum uniform location size";
            return false;
        }
    }

    return true;
}

void Program::linkSamplerAndImageBindings(GLuint *combinedImageUniforms)
{
    ASSERT(combinedImageUniforms);

    // Iterate over mExecutable->mUniforms from the back, and find the range of atomic counters,
    // images and samplers in that order.
    auto highIter = mState.mExecutable->getUniforms().rbegin();
    auto lowIter  = highIter;

    unsigned int high = static_cast<unsigned int>(mState.mExecutable->getUniforms().size());
    unsigned int low  = high;

    // Note that uniform block uniforms are not yet appended to this list.
    ASSERT(mState.mExecutable->getUniforms().size() == 0 || highIter->isAtomicCounter() ||
           highIter->isImage() || highIter->isSampler() || highIter->isInDefaultBlock());

    for (; lowIter != mState.mExecutable->getUniforms().rend() && lowIter->isAtomicCounter();
         ++lowIter)
    {
        --low;
    }

    mState.mAtomicCounterUniformRange = RangeUI(low, high);

    highIter = lowIter;
    high     = low;

    for (; lowIter != mState.mExecutable->getUniforms().rend() && lowIter->isImage(); ++lowIter)
    {
        --low;
    }

    mState.mExecutable->mImageUniformRange = RangeUI(low, high);
    *combinedImageUniforms                 = 0u;
    // The Program is still linking, so getExecutable().isCompute() isn't accurate yet.
    bool hasComputeShader = mState.mAttachedShaders[ShaderType::Compute] != nullptr;
    std::vector<ImageBinding> &imageBindings = hasComputeShader
                                                   ? mState.mExecutable->mComputeImageBindings
                                                   : mState.mExecutable->mGraphicsImageBindings;
    // The arrays of arrays are flattened to arrays, it needs to record the array offset for the
    // correct binding image unit.
    uint32_t arrayOffset = 0;
    // If uniform is a image type, insert it into the mImageBindings array.
    for (unsigned int imageIndex : mState.mExecutable->getImageUniformRange())
    {
        // ES3.1 (section 7.6.1) and GLSL ES3.1 (section 4.4.5), Uniform*i{v} commands
        // cannot load values into a uniform defined as an image. if declare without a
        // binding qualifier, any uniform image variable (include all elements of
        // unbound image array) shoud be bound to unit zero.
        auto &imageUniform      = mState.mExecutable->getUniforms()[imageIndex];
        TextureType textureType = ImageTypeToTextureType(imageUniform.type);
        if (imageUniform.binding == -1)
        {
            imageBindings.emplace_back(
                ImageBinding(imageUniform.getBasicTypeElementCount(), textureType));
        }
        else
        {
            imageBindings.emplace_back(ImageBinding(imageUniform.binding + arrayOffset,
                                                    imageUniform.getBasicTypeElementCount(),
                                                    textureType));
        }

        GLuint arraySize = imageUniform.isArray() ? imageUniform.arraySizes[0] : 1u;
        *combinedImageUniforms += imageUniform.activeShaderCount() * arraySize;
        if (imageUniform.hasParentArrayIndex() && imageUniform.isArray())
        {
            arrayOffset += arraySize;
        }
        else
        {
            arrayOffset = 0;
        }
    }

    highIter = lowIter;
    high     = low;

    for (; lowIter != mState.mExecutable->getUniforms().rend() && lowIter->isSampler(); ++lowIter)
    {
        --low;
    }

    mState.mExecutable->mSamplerUniformRange = RangeUI(low, high);

    // If uniform is a sampler type, insert it into the mSamplerBindings array.
    for (unsigned int samplerIndex : mState.mExecutable->getSamplerUniformRange())
    {
        const auto &samplerUniform = mState.mExecutable->getUniforms()[samplerIndex];
        TextureType textureType    = SamplerTypeToTextureType(samplerUniform.type);
        GLenum samplerType         = samplerUniform.typeInfo->type;
        unsigned int elementCount  = samplerUniform.getBasicTypeElementCount();
        SamplerFormat format       = samplerUniform.typeInfo->samplerFormat;
        mState.mExecutable->mSamplerBindings.emplace_back(textureType, samplerType, format,
                                                          elementCount);
    }

    // Whatever is left constitutes the default uniforms.
    mState.mExecutable->mDefaultUniformRange = RangeUI(0, low);
}

bool Program::linkAtomicCounterBuffers()
{
    for (unsigned int index : mState.mAtomicCounterUniformRange)
    {
        auto &uniform                      = mState.mExecutable->mUniforms[index];
        uniform.blockInfo.offset           = uniform.offset;
        uniform.blockInfo.arrayStride      = (uniform.isArray() ? 4 : 0);
        uniform.blockInfo.matrixStride     = 0;
        uniform.blockInfo.isRowMajorMatrix = false;

        bool found = false;
        for (unsigned int bufferIndex = 0;
             bufferIndex < mState.mExecutable->getActiveAtomicCounterBufferCount(); ++bufferIndex)
        {
            auto &buffer = mState.mExecutable->mAtomicCounterBuffers[bufferIndex];
            if (buffer.binding == uniform.binding)
            {
                buffer.memberIndexes.push_back(index);
                uniform.bufferIndex = bufferIndex;
                found               = true;
                buffer.unionReferencesWith(uniform);
                break;
            }
        }
        if (!found)
        {
            AtomicCounterBuffer atomicCounterBuffer;
            atomicCounterBuffer.binding = uniform.binding;
            atomicCounterBuffer.memberIndexes.push_back(index);
            atomicCounterBuffer.unionReferencesWith(uniform);
            mState.mExecutable->mAtomicCounterBuffers.push_back(atomicCounterBuffer);
            uniform.bufferIndex =
                static_cast<int>(mState.mExecutable->getActiveAtomicCounterBufferCount() - 1);
        }
    }
    // TODO(jie.a.chen@intel.com): Count each atomic counter buffer to validate against
    // gl_Max[Vertex|Fragment|Compute|Geometry|Combined]AtomicCounterBuffers.

    return true;
}

// Assigns locations to all attributes (except built-ins) from the bindings and program locations.
bool Program::linkAttributes(const Context *context, InfoLog &infoLog)
{
    const Caps &caps               = context->getCaps();
    const Limitations &limitations = context->getLimitations();
    bool webglCompatibility        = context->getExtensions().webglCompatibility;
    int shaderVersion              = -1;
    unsigned int usedLocations     = 0;

    Shader *vertexShader = mState.getAttachedShader(gl::ShaderType::Vertex);

    if (!vertexShader)
    {
        // No vertex shader, so no attributes, so nothing to do
        return true;
    }

    shaderVersion = vertexShader->getShaderVersion();
    if (shaderVersion >= 300)
    {
        // In GLSL ES 3.00.6, aliasing checks should be done with all declared attributes -
        // see GLSL ES 3.00.6 section 12.46. Inactive attributes will be pruned after
        // aliasing checks.
        mState.mExecutable->mProgramInputs = vertexShader->getAllAttributes();
    }
    else
    {
        // In GLSL ES 1.00.17 we only do aliasing checks for active attributes.
        mState.mExecutable->mProgramInputs = vertexShader->getActiveAttributes();
    }

    GLuint maxAttribs = static_cast<GLuint>(caps.maxVertexAttributes);
    std::vector<sh::ShaderVariable *> usedAttribMap(maxAttribs, nullptr);

    // Assign locations to attributes that have a binding location and check for attribute aliasing.
    for (sh::ShaderVariable &attribute : mState.mExecutable->mProgramInputs)
    {
        // GLSL ES 3.10 January 2016 section 4.3.4: Vertex shader inputs can't be arrays or
        // structures, so we don't need to worry about adjusting their names or generating entries
        // for each member/element (unlike uniforms for example).
        ASSERT(!attribute.isArray() && !attribute.isStruct());

        int bindingLocation = mAttributeBindings.getBinding(attribute);
        if (attribute.location == -1 && bindingLocation != -1)
        {
            attribute.location = bindingLocation;
        }

        if (attribute.location != -1)
        {
            // Location is set by glBindAttribLocation or by location layout qualifier
            const int regs = VariableRegisterCount(attribute.type);

            if (static_cast<GLuint>(regs + attribute.location) > maxAttribs)
            {
                infoLog << "Attribute (" << attribute.name << ") at location " << attribute.location
                        << " is too big to fit";

                return false;
            }

            for (int reg = 0; reg < regs; reg++)
            {
                const int regLocation               = attribute.location + reg;
                sh::ShaderVariable *linkedAttribute = usedAttribMap[regLocation];

                // In GLSL ES 3.00.6 and in WebGL, attribute aliasing produces a link error.
                // In non-WebGL GLSL ES 1.00.17, attribute aliasing is allowed with some
                // restrictions - see GLSL ES 1.00.17 section 2.10.4, but ANGLE currently has a bug.
                // In D3D 9 and 11, aliasing is not supported, so check a limitation.
                if (linkedAttribute)
                {
                    if (shaderVersion >= 300 || webglCompatibility ||
                        limitations.noVertexAttributeAliasing)
                    {
                        infoLog << "Attribute '" << attribute.name << "' aliases attribute '"
                                << linkedAttribute->name << "' at location " << regLocation;
                        return false;
                    }
                }
                else
                {
                    usedAttribMap[regLocation] = &attribute;
                }

                usedLocations |= 1 << regLocation;
            }
        }
    }

    // Assign locations to attributes that don't have a binding location.
    for (sh::ShaderVariable &attribute : mState.mExecutable->mProgramInputs)
    {
        // Not set by glBindAttribLocation or by location layout qualifier
        if (attribute.location == -1)
        {
            int regs           = VariableRegisterCount(attribute.type);
            int availableIndex = AllocateFirstFreeBits(&usedLocations, regs, maxAttribs);

            if (availableIndex == -1 || static_cast<GLuint>(availableIndex + regs) > maxAttribs)
            {
                infoLog << "Too many attributes (" << attribute.name << ")";
                return false;
            }

            attribute.location = availableIndex;
        }
    }

    ASSERT(mState.mExecutable->mAttributesTypeMask.none());
    ASSERT(mState.mExecutable->mAttributesMask.none());

    // Prune inactive attributes. This step is only needed on shaderVersion >= 300 since on earlier
    // shader versions we're only processing active attributes to begin with.
    if (shaderVersion >= 300)
    {
        for (auto attributeIter = mState.mExecutable->getProgramInputs().begin();
             attributeIter != mState.mExecutable->getProgramInputs().end();)
        {
            if (attributeIter->active)
            {
                ++attributeIter;
            }
            else
            {
                attributeIter = mState.mExecutable->mProgramInputs.erase(attributeIter);
            }
        }
    }

    for (const sh::ShaderVariable &attribute : mState.mExecutable->getProgramInputs())
    {
        ASSERT(attribute.active);
        ASSERT(attribute.location != -1);
        unsigned int regs = static_cast<unsigned int>(VariableRegisterCount(attribute.type));

        unsigned int location = static_cast<unsigned int>(attribute.location);
        for (unsigned int r = 0; r < regs; r++)
        {
            // Built-in active program inputs don't have a bound attribute.
            if (!attribute.isBuiltIn())
            {
                mState.mExecutable->mActiveAttribLocationsMask.set(location);
                mState.mExecutable->mMaxActiveAttribLocation =
                    std::max(mState.mExecutable->mMaxActiveAttribLocation, location + 1);

                ComponentType componentType =
                    GLenumToComponentType(VariableComponentType(attribute.type));

                SetComponentTypeMask(componentType, location,
                                     &mState.mExecutable->mAttributesTypeMask);
                mState.mExecutable->mAttributesMask.set(location);

                location++;
            }
        }
    }

    return true;
}

bool Program::linkInterfaceBlocks(const Caps &caps,
                                  const Version &version,
                                  bool webglCompatibility,
                                  InfoLog &infoLog,
                                  GLuint *combinedShaderStorageBlocksCount)
{
    ASSERT(combinedShaderStorageBlocksCount);

    GLuint combinedUniformBlocksCount                                         = 0u;
    GLuint numShadersHasUniformBlocks                                         = 0u;
    ShaderMap<const std::vector<sh::InterfaceBlock> *> allShaderUniformBlocks = {};
    InterfaceBlockMap instancelessInterfaceBlocksFields;

    for (ShaderType shaderType : AllShaderTypes())
    {
        Shader *shader = mState.mAttachedShaders[shaderType];
        if (!shader)
        {
            continue;
        }

        const auto &uniformBlocks = shader->getUniformBlocks();
        if (!uniformBlocks.empty())
        {
            if (!ValidateInterfaceBlocksCount(
                    static_cast<GLuint>(caps.maxShaderUniformBlocks[shaderType]), uniformBlocks,
                    shaderType, sh::BlockType::BLOCK_UNIFORM, &combinedUniformBlocksCount, infoLog))
            {
                return false;
            }

            allShaderUniformBlocks[shaderType] = &uniformBlocks;
            ++numShadersHasUniformBlocks;
        }
    }

    if (combinedUniformBlocksCount > static_cast<GLuint>(caps.maxCombinedUniformBlocks))
    {
        infoLog << "The sum of the number of active uniform blocks exceeds "
                   "MAX_COMBINED_UNIFORM_BLOCKS ("
                << caps.maxCombinedUniformBlocks << ").";
        return false;
    }

    if (!ValidateInterfaceBlocksMatch(numShadersHasUniformBlocks, allShaderUniformBlocks, infoLog,
                                      webglCompatibility, &instancelessInterfaceBlocksFields))
    {
        return false;
    }

    if (version >= Version(3, 1))
    {
        *combinedShaderStorageBlocksCount                                         = 0u;
        GLuint numShadersHasShaderStorageBlocks                                   = 0u;
        ShaderMap<const std::vector<sh::InterfaceBlock> *> allShaderStorageBlocks = {};
        for (ShaderType shaderType : AllShaderTypes())
        {
            Shader *shader = mState.mAttachedShaders[shaderType];
            if (!shader)
            {
                continue;
            }

            const auto &shaderStorageBlocks = shader->getShaderStorageBlocks();
            if (!shaderStorageBlocks.empty())
            {
                if (!ValidateInterfaceBlocksCount(
                        static_cast<GLuint>(caps.maxShaderStorageBlocks[shaderType]),
                        shaderStorageBlocks, shaderType, sh::BlockType::BLOCK_BUFFER,
                        combinedShaderStorageBlocksCount, infoLog))
                {
                    return false;
                }

                allShaderStorageBlocks[shaderType] = &shaderStorageBlocks;
                ++numShadersHasShaderStorageBlocks;
            }
        }

        if (*combinedShaderStorageBlocksCount >
            static_cast<GLuint>(caps.maxCombinedShaderStorageBlocks))
        {
            infoLog << "The sum of the number of active shader storage blocks exceeds "
                       "MAX_COMBINED_SHADER_STORAGE_BLOCKS ("
                    << caps.maxCombinedShaderStorageBlocks << ").";
            return false;
        }

        if (!ValidateInterfaceBlocksMatch(numShadersHasShaderStorageBlocks, allShaderStorageBlocks,
                                          infoLog, webglCompatibility,
                                          &instancelessInterfaceBlocksFields))
        {
            return false;
        }
    }

    return true;
}

int Program::getOutputLocationForLink(const sh::ShaderVariable &outputVariable) const
{
    if (outputVariable.location != -1)
    {
        return outputVariable.location;
    }
    int apiLocation = mFragmentOutputLocations.getBinding(outputVariable);
    if (apiLocation != -1)
    {
        return apiLocation;
    }
    return -1;
}

bool Program::isOutputSecondaryForLink(const sh::ShaderVariable &outputVariable) const
{
    if (outputVariable.index != -1)
    {
        ASSERT(outputVariable.index == 0 || outputVariable.index == 1);
        return (outputVariable.index == 1);
    }
    int apiIndex = mFragmentOutputIndexes.getBinding(outputVariable);
    if (apiIndex != -1)
    {
        // Index layout qualifier from the shader takes precedence, so the index from the API is
        // checked only if the index was not set in the shader. This is not specified in the EXT
        // spec, but is specified in desktop OpenGL specs.
        return (apiIndex == 1);
    }
    // EXT_blend_func_extended: Outputs get index 0 by default.
    return false;
}

namespace
{

bool FindUsedOutputLocation(std::vector<VariableLocation> &outputLocations,
                            unsigned int baseLocation,
                            unsigned int elementCount,
                            const std::vector<VariableLocation> &reservedLocations,
                            unsigned int variableIndex)
{
    if (baseLocation + elementCount > outputLocations.size())
    {
        elementCount = baseLocation < outputLocations.size()
                           ? static_cast<unsigned int>(outputLocations.size() - baseLocation)
                           : 0;
    }
    for (unsigned int elementIndex = 0; elementIndex < elementCount; elementIndex++)
    {
        const unsigned int location = baseLocation + elementIndex;
        if (outputLocations[location].used())
        {
            VariableLocation locationInfo(elementIndex, variableIndex);
            if (std::find(reservedLocations.begin(), reservedLocations.end(), locationInfo) ==
                reservedLocations.end())
            {
                return true;
            }
        }
    }
    return false;
}

void AssignOutputLocations(std::vector<VariableLocation> &outputLocations,
                           unsigned int baseLocation,
                           unsigned int elementCount,
                           const std::vector<VariableLocation> &reservedLocations,
                           unsigned int variableIndex,
                           sh::ShaderVariable &outputVariable)
{
    if (baseLocation + elementCount > outputLocations.size())
    {
        outputLocations.resize(baseLocation + elementCount);
    }
    for (unsigned int elementIndex = 0; elementIndex < elementCount; elementIndex++)
    {
        VariableLocation locationInfo(elementIndex, variableIndex);
        if (std::find(reservedLocations.begin(), reservedLocations.end(), locationInfo) ==
            reservedLocations.end())
        {
            outputVariable.location     = baseLocation;
            const unsigned int location = baseLocation + elementIndex;
            outputLocations[location]   = locationInfo;
        }
    }
}

}  // anonymous namespace

bool Program::linkOutputVariables(const Caps &caps,
                                  const Extensions &extensions,
                                  const Version &version,
                                  GLuint combinedImageUniformsCount,
                                  GLuint combinedShaderStorageBlocksCount)
{
    InfoLog &infoLog       = mState.mExecutable->getInfoLog();
    Shader *fragmentShader = mState.mAttachedShaders[ShaderType::Fragment];

    ASSERT(mState.mOutputVariableTypes.empty());
    ASSERT(mState.mActiveOutputVariables.none());
    ASSERT(mState.mDrawBufferTypeMask.none());
    ASSERT(!mState.mYUVOutput);

    if (!fragmentShader)
    {
        // No fragment shader, so nothing to link
        return true;
    }

    const std::vector<sh::ShaderVariable> &outputVariables =
        fragmentShader->getActiveOutputVariables();

    // Gather output variable types
    for (const sh::ShaderVariable &outputVariable : outputVariables)
    {
        if (outputVariable.isBuiltIn() && outputVariable.name != "gl_FragColor" &&
            outputVariable.name != "gl_FragData")
        {
            continue;
        }

        unsigned int baseLocation =
            (outputVariable.location == -1 ? 0u
                                           : static_cast<unsigned int>(outputVariable.location));

        // GLSL ES 3.10 section 4.3.6: Output variables cannot be arrays of arrays or arrays of
        // structures, so we may use getBasicTypeElementCount().
        unsigned int elementCount = outputVariable.getBasicTypeElementCount();
        for (unsigned int elementIndex = 0; elementIndex < elementCount; elementIndex++)
        {
            const unsigned int location = baseLocation + elementIndex;
            if (location >= mState.mOutputVariableTypes.size())
            {
                mState.mOutputVariableTypes.resize(location + 1, GL_NONE);
            }
            ASSERT(location < mState.mActiveOutputVariables.size());
            mState.mActiveOutputVariables.set(location);
            mState.mOutputVariableTypes[location] = VariableComponentType(outputVariable.type);
            ComponentType componentType =
                GLenumToComponentType(mState.mOutputVariableTypes[location]);
            SetComponentTypeMask(componentType, location, &mState.mDrawBufferTypeMask);
        }

        if (outputVariable.yuv)
        {
            ASSERT(outputVariables.size() == 1);
            mState.mYUVOutput = true;
        }
    }

    if (version >= ES_3_1)
    {
        // [OpenGL ES 3.1] Chapter 8.22 Page 203:
        // A link error will be generated if the sum of the number of active image uniforms used in
        // all shaders, the number of active shader storage blocks, and the number of active
        // fragment shader outputs exceeds the implementation-dependent value of
        // MAX_COMBINED_SHADER_OUTPUT_RESOURCES.
        if (combinedImageUniformsCount + combinedShaderStorageBlocksCount +
                mState.mActiveOutputVariables.count() >
            static_cast<GLuint>(caps.maxCombinedShaderOutputResources))
        {
            infoLog
                << "The sum of the number of active image uniforms, active shader storage blocks "
                   "and active fragment shader outputs exceeds "
                   "MAX_COMBINED_SHADER_OUTPUT_RESOURCES ("
                << caps.maxCombinedShaderOutputResources << ")";
            return false;
        }
    }

    // Skip this step for GLES2 shaders.
    if (fragmentShader && fragmentShader->getShaderVersion() == 100)
        return true;

    mState.mExecutable->mOutputVariables = outputVariables;
    mState.mExecutable->mYUVOutput       = mState.mYUVOutput;
    // TODO(jmadill): any caps validation here?

    // EXT_blend_func_extended doesn't specify anything related to binding specific elements of an
    // output array in explicit terms.
    //
    // Assuming fragData is an output array, you can defend the position that:
    // P1) you must support binding "fragData" because it's specified
    // P2) you must support querying "fragData[x]" because it's specified
    // P3) you must support binding "fragData[0]" because it's a frequently used pattern
    //
    // Then you can make the leap of faith:
    // P4) you must support binding "fragData[x]" because you support "fragData[0]"
    // P5) you must support binding "fragData[x]" because you support querying "fragData[x]"
    //
    // The spec brings in the "world of arrays" when it mentions binding the arrays and the
    // automatic binding. Thus it must be interpreted that the thing is not undefined, rather you
    // must infer the only possible interpretation (?). Note again: this need of interpretation
    // might be completely off of what GL spec logic is.
    //
    // The other complexity is that unless you implement this feature, it's hard to understand what
    // should happen when the client invokes the feature. You cannot add an additional error as it
    // is not specified. One can ignore it, but obviously it creates the discrepancies...

    std::vector<VariableLocation> reservedLocations;

    // Process any output API bindings for arrays that don't alias to the first element.
    for (const auto &binding : mFragmentOutputLocations)
    {
        size_t nameLengthWithoutArrayIndex;
        unsigned int arrayIndex = ParseArrayIndex(binding.first, &nameLengthWithoutArrayIndex);
        if (arrayIndex == 0 || arrayIndex == GL_INVALID_INDEX)
        {
            continue;
        }
        for (unsigned int outputVariableIndex = 0;
             outputVariableIndex < mState.mExecutable->getOutputVariables().size();
             outputVariableIndex++)
        {
            const sh::ShaderVariable &outputVariable =
                mState.mExecutable->getOutputVariables()[outputVariableIndex];
            // Check that the binding corresponds to an output array and its array index fits.
            if (outputVariable.isBuiltIn() || !outputVariable.isArray() ||
                !angle::BeginsWith(outputVariable.name, binding.first,
                                   nameLengthWithoutArrayIndex) ||
                arrayIndex >= outputVariable.getOutermostArraySize())
            {
                continue;
            }

            // Get the API index that corresponds to this exact binding.
            // This index may differ from the index used for the array's base.
            auto &outputLocations = mFragmentOutputIndexes.getBindingByName(binding.first) == 1
                                        ? mState.mSecondaryOutputLocations
                                        : mState.mExecutable->mOutputLocations;
            unsigned int location = binding.second.location;
            VariableLocation locationInfo(arrayIndex, outputVariableIndex);
            if (location >= outputLocations.size())
            {
                outputLocations.resize(location + 1);
            }
            if (outputLocations[location].used())
            {
                infoLog << "Location of variable " << outputVariable.name
                        << " conflicts with another variable.";
                return false;
            }
            outputLocations[location] = locationInfo;

            // Note the array binding location so that it can be skipped later.
            reservedLocations.push_back(locationInfo);
        }
    }

    // Reserve locations for output variables whose location is fixed in the shader or through the
    // API. Otherwise, the remaining unallocated outputs will be processed later.
    for (unsigned int outputVariableIndex = 0;
         outputVariableIndex < mState.mExecutable->getOutputVariables().size();
         outputVariableIndex++)
    {
        const sh::ShaderVariable &outputVariable =
            mState.mExecutable->getOutputVariables()[outputVariableIndex];

        // Don't store outputs for gl_FragDepth, gl_FragColor, etc.
        if (outputVariable.isBuiltIn())
            continue;

        int fixedLocation = getOutputLocationForLink(outputVariable);
        if (fixedLocation == -1)
        {
            // Here we're only reserving locations for variables whose location is fixed.
            continue;
        }
        unsigned int baseLocation = static_cast<unsigned int>(fixedLocation);

        auto &outputLocations = isOutputSecondaryForLink(outputVariable)
                                    ? mState.mSecondaryOutputLocations
                                    : mState.mExecutable->mOutputLocations;

        // GLSL ES 3.10 section 4.3.6: Output variables cannot be arrays of arrays or arrays of
        // structures, so we may use getBasicTypeElementCount().
        unsigned int elementCount = outputVariable.getBasicTypeElementCount();
        if (FindUsedOutputLocation(outputLocations, baseLocation, elementCount, reservedLocations,
                                   outputVariableIndex))
        {
            infoLog << "Location of variable " << outputVariable.name
                    << " conflicts with another variable.";
            return false;
        }
        AssignOutputLocations(outputLocations, baseLocation, elementCount, reservedLocations,
                              outputVariableIndex,
                              mState.mExecutable->mOutputVariables[outputVariableIndex]);
    }

    // Here we assign locations for the output variables that don't yet have them. Note that we're
    // not necessarily able to fit the variables optimally, since then we might have to try
    // different arrangements of output arrays. Now we just assign the locations in the order that
    // we got the output variables. The spec isn't clear on what kind of algorithm is required for
    // finding locations for the output variables, so this should be acceptable at least for now.
    GLuint maxLocation = static_cast<GLuint>(caps.maxDrawBuffers);
    if (!mState.mSecondaryOutputLocations.empty())
    {
        // EXT_blend_func_extended: Program outputs will be validated against
        // MAX_DUAL_SOURCE_DRAW_BUFFERS_EXT if there's even one output with index one.
        maxLocation = extensions.maxDualSourceDrawBuffers;
    }

    for (unsigned int outputVariableIndex = 0;
         outputVariableIndex < mState.mExecutable->getOutputVariables().size();
         outputVariableIndex++)
    {
        const sh::ShaderVariable &outputVariable =
            mState.mExecutable->getOutputVariables()[outputVariableIndex];

        // Don't store outputs for gl_FragDepth, gl_FragColor, etc.
        if (outputVariable.isBuiltIn())
            continue;

        int fixedLocation     = getOutputLocationForLink(outputVariable);
        auto &outputLocations = isOutputSecondaryForLink(outputVariable)
                                    ? mState.mSecondaryOutputLocations
                                    : mState.mExecutable->mOutputLocations;
        unsigned int baseLocation = 0;
        unsigned int elementCount = outputVariable.getBasicTypeElementCount();
        if (fixedLocation != -1)
        {
            // Secondary inputs might have caused the max location to drop below what has already
            // been explicitly assigned locations. Check for any fixed locations above the max
            // that should cause linking to fail.
            baseLocation = static_cast<unsigned int>(fixedLocation);
        }
        else
        {
            // No fixed location, so try to fit the output in unassigned locations.
            // Try baseLocations starting from 0 one at a time and see if the variable fits.
            while (FindUsedOutputLocation(outputLocations, baseLocation, elementCount,
                                          reservedLocations, outputVariableIndex))
            {
                baseLocation++;
            }
            AssignOutputLocations(outputLocations, baseLocation, elementCount, reservedLocations,
                                  outputVariableIndex,
                                  mState.mExecutable->mOutputVariables[outputVariableIndex]);
        }

        // Check for any elements assigned above the max location that are actually used.
        if (baseLocation + elementCount > maxLocation &&
            (baseLocation >= maxLocation ||
             FindUsedOutputLocation(outputLocations, maxLocation,
                                    baseLocation + elementCount - maxLocation, reservedLocations,
                                    outputVariableIndex)))
        {
            // EXT_blend_func_extended: Linking can fail:
            // "if the explicit binding assignments do not leave enough space for the linker to
            // automatically assign a location for a varying out array, which requires multiple
            // contiguous locations."
            infoLog << "Could not fit output variable into available locations: "
                    << outputVariable.name;
            return false;
        }
    }

    return true;
}

void Program::setUniformValuesFromBindingQualifiers()
{
    for (unsigned int samplerIndex : mState.mExecutable->getSamplerUniformRange())
    {
        const auto &samplerUniform = mState.mExecutable->getUniforms()[samplerIndex];
        if (samplerUniform.binding != -1)
        {
            UniformLocation location = getUniformLocation(samplerUniform.name);
            ASSERT(location.value != -1);
            std::vector<GLint> boundTextureUnits;
            for (unsigned int elementIndex = 0;
                 elementIndex < samplerUniform.getBasicTypeElementCount(); ++elementIndex)
            {
                boundTextureUnits.push_back(samplerUniform.binding + elementIndex);
            }

            // Here we pass nullptr to avoid a large chain of calls that need a non-const Context.
            // We know it's safe not to notify the Context because this is only called after link.
            setUniform1iv(nullptr, location, static_cast<GLsizei>(boundTextureUnits.size()),
                          boundTextureUnits.data());
        }
    }
}

void Program::initInterfaceBlockBindings()
{
    // Set initial bindings from shader.
    for (unsigned int blockIndex = 0; blockIndex < mState.mExecutable->getActiveUniformBlockCount();
         blockIndex++)
    {
        InterfaceBlock &uniformBlock = mState.mExecutable->mUniformBlocks[blockIndex];
        bindUniformBlock(blockIndex, uniformBlock.binding);
    }
}

void Program::updateSamplerUniform(Context *context,
                                   const VariableLocation &locationInfo,
                                   GLsizei clampedCount,
                                   const GLint *v)
{
    ASSERT(mState.isSamplerUniformIndex(locationInfo.index));
    GLuint samplerIndex            = mState.getSamplerIndexFromUniformIndex(locationInfo.index);
    SamplerBinding &samplerBinding = mState.mExecutable->mSamplerBindings[samplerIndex];
    std::vector<GLuint> &boundTextureUnits = samplerBinding.boundTextureUnits;

    if (locationInfo.arrayIndex >= boundTextureUnits.size())
    {
        return;
    }
    GLsizei safeUniformCount = std::min(
        clampedCount, static_cast<GLsizei>(boundTextureUnits.size() - locationInfo.arrayIndex));

    // Update the sampler uniforms.
    for (GLsizei arrayIndex = 0; arrayIndex < safeUniformCount; ++arrayIndex)
    {
        GLint oldTextureUnit = boundTextureUnits[arrayIndex + locationInfo.arrayIndex];
        GLint newTextureUnit = v[arrayIndex];

        if (oldTextureUnit == newTextureUnit)
        {
            continue;
        }

        boundTextureUnits[arrayIndex + locationInfo.arrayIndex] = newTextureUnit;

        // Update the reference counts.
        uint32_t &oldRefCount = mState.mExecutable->mActiveSamplerRefCounts[oldTextureUnit];
        uint32_t &newRefCount = mState.mExecutable->mActiveSamplerRefCounts[newTextureUnit];
        ASSERT(oldRefCount > 0);
        ASSERT(newRefCount < std::numeric_limits<uint32_t>::max());
        oldRefCount--;
        newRefCount++;

        // Check for binding type change.
        TextureType &newSamplerType     = mState.mExecutable->mActiveSamplerTypes[newTextureUnit];
        TextureType &oldSamplerType     = mState.mExecutable->mActiveSamplerTypes[oldTextureUnit];
        SamplerFormat &newSamplerFormat = mState.mExecutable->mActiveSamplerFormats[newTextureUnit];
        SamplerFormat &oldSamplerFormat = mState.mExecutable->mActiveSamplerFormats[oldTextureUnit];

        if (newRefCount == 1)
        {
            newSamplerType   = samplerBinding.textureType;
            newSamplerFormat = samplerBinding.format;
            mState.mExecutable->mActiveSamplersMask.set(newTextureUnit);
            mState.mExecutable->mActiveSamplerShaderBits[newTextureUnit] =
                mState.mExecutable->getUniforms()[locationInfo.index].activeShaders();
        }
        else
        {
            if (newSamplerType != samplerBinding.textureType)
            {
                // Conflict detected. Ensure we reset it properly.
                newSamplerType = TextureType::InvalidEnum;
            }
            if (newSamplerFormat != samplerBinding.format)
            {
                newSamplerFormat = SamplerFormat::InvalidEnum;
            }
        }

        // Unset previously active sampler.
        if (oldRefCount == 0)
        {
            oldSamplerType   = TextureType::InvalidEnum;
            oldSamplerFormat = SamplerFormat::InvalidEnum;
            mState.mExecutable->mActiveSamplersMask.reset(oldTextureUnit);
        }
        else
        {
            if (oldSamplerType == TextureType::InvalidEnum ||
                oldSamplerFormat == SamplerFormat::InvalidEnum)
            {
                // Previous conflict. Check if this new change fixed the conflict.
                mState.setSamplerUniformTextureTypeAndFormat(oldTextureUnit);
            }
        }

        // Notify context.
        if (context)
        {
            context->onSamplerUniformChange(newTextureUnit);
            context->onSamplerUniformChange(oldTextureUnit);
        }
    }

    // Invalidate the validation cache.
    mCachedValidateSamplersResult.reset();
}

void ProgramState::setSamplerUniformTextureTypeAndFormat(size_t textureUnitIndex)
{
    mExecutable->setSamplerUniformTextureTypeAndFormat(textureUnitIndex,
                                                       mExecutable->mSamplerBindings);
}

template <typename T>
GLsizei Program::clampUniformCount(const VariableLocation &locationInfo,
                                   GLsizei count,
                                   int vectorSize,
                                   const T *v)
{
    if (count == 1)
        return 1;

    const LinkedUniform &linkedUniform = mState.mExecutable->getUniforms()[locationInfo.index];

    // OpenGL ES 3.0.4 spec pg 67: "Values for any array element that exceeds the highest array
    // element index used, as reported by GetActiveUniform, will be ignored by the GL."
    unsigned int remainingElements =
        linkedUniform.getBasicTypeElementCount() - locationInfo.arrayIndex;
    GLsizei maxElementCount =
        static_cast<GLsizei>(remainingElements * linkedUniform.getElementComponents());

    if (count * vectorSize > maxElementCount)
    {
        return maxElementCount / vectorSize;
    }

    return count;
}

template <size_t cols, size_t rows, typename T>
GLsizei Program::clampMatrixUniformCount(UniformLocation location,
                                         GLsizei count,
                                         GLboolean transpose,
                                         const T *v)
{
    const VariableLocation &locationInfo = mState.mUniformLocations[location.value];

    if (!transpose)
    {
        return clampUniformCount(locationInfo, count, cols * rows, v);
    }

    const LinkedUniform &linkedUniform = mState.mExecutable->getUniforms()[locationInfo.index];

    // OpenGL ES 3.0.4 spec pg 67: "Values for any array element that exceeds the highest array
    // element index used, as reported by GetActiveUniform, will be ignored by the GL."
    unsigned int remainingElements =
        linkedUniform.getBasicTypeElementCount() - locationInfo.arrayIndex;
    return std::min(count, static_cast<GLsizei>(remainingElements));
}

// Driver differences mean that doing the uniform value cast ourselves gives consistent results.
// EG: on NVIDIA drivers, it was observed that getUniformi for MAX_INT+1 returned MIN_INT.
template <typename DestT>
void Program::getUniformInternal(const Context *context,
                                 DestT *dataOut,
                                 UniformLocation location,
                                 GLenum nativeType,
                                 int components) const
{
    switch (nativeType)
    {
        case GL_BOOL:
        {
            GLint tempValue[16] = {0};
            mProgram->getUniformiv(context, location.value, tempValue);
            UniformStateQueryCastLoop<GLboolean>(
                dataOut, reinterpret_cast<const uint8_t *>(tempValue), components);
            break;
        }
        case GL_INT:
        {
            GLint tempValue[16] = {0};
            mProgram->getUniformiv(context, location.value, tempValue);
            UniformStateQueryCastLoop<GLint>(dataOut, reinterpret_cast<const uint8_t *>(tempValue),
                                             components);
            break;
        }
        case GL_UNSIGNED_INT:
        {
            GLuint tempValue[16] = {0};
            mProgram->getUniformuiv(context, location.value, tempValue);
            UniformStateQueryCastLoop<GLuint>(dataOut, reinterpret_cast<const uint8_t *>(tempValue),
                                              components);
            break;
        }
        case GL_FLOAT:
        {
            GLfloat tempValue[16] = {0};
            mProgram->getUniformfv(context, location.value, tempValue);
            UniformStateQueryCastLoop<GLfloat>(
                dataOut, reinterpret_cast<const uint8_t *>(tempValue), components);
            break;
        }
        default:
            UNREACHABLE();
            break;
    }
}

angle::Result Program::syncState(const Context *context)
{
    if (mDirtyBits.any())
    {
        ASSERT(!mLinkingState);
        ANGLE_TRY(mProgram->syncState(context, mDirtyBits));
        mDirtyBits.reset();
    }

    return angle::Result::Continue;
}

angle::Result Program::serialize(const Context *context, angle::MemoryBuffer *binaryOut) const
{
    BinaryOutputStream stream;

    stream.writeBytes(reinterpret_cast<const unsigned char *>(ANGLE_COMMIT_HASH),
                      ANGLE_COMMIT_HASH_SIZE);

    // nullptr context is supported when computing binary length.
    if (context)
    {
        stream.writeInt(context->getClientVersion().major);
        stream.writeInt(context->getClientVersion().minor);
    }
    else
    {
        stream.writeInt(2);
        stream.writeInt(0);
    }

    mState.mExecutable->save(&stream);

    const auto &computeLocalSize = mState.getComputeShaderLocalSize();

    stream.writeInt(computeLocalSize[0]);
    stream.writeInt(computeLocalSize[1]);
    stream.writeInt(computeLocalSize[2]);

    stream.writeInt(mState.mNumViews);
    stream.writeBool(mState.mEarlyFramentTestsOptimization);
    stream.writeInt(mState.mSpecConstUsageBits.bits());

    stream.writeInt(mState.getProgramInputs().size());
    for (const sh::ShaderVariable &attrib : mState.getProgramInputs())
    {
        WriteShaderVar(&stream, attrib);
        stream.writeInt(attrib.location);
    }

    stream.writeInt(mState.getUniforms().size());
    for (const LinkedUniform &uniform : mState.getUniforms())
    {
        WriteShaderVar(&stream, uniform);

        // FIXME: referenced

        stream.writeInt(uniform.bufferIndex);
        WriteBlockMemberInfo(&stream, uniform.blockInfo);

        stream.writeIntVector(uniform.outerArraySizes);

        // Active shader info
        for (ShaderType shaderType : gl::AllShaderTypes())
        {
            stream.writeBool(uniform.isActive(shaderType));
        }
    }

    stream.writeInt(mState.getUniformLocations().size());
    for (const auto &variable : mState.getUniformLocations())
    {
        stream.writeInt(variable.arrayIndex);
        stream.writeIntOrNegOne(variable.index);
        stream.writeBool(variable.ignored);
    }

    stream.writeInt(mState.getUniformBlocks().size());
    for (const InterfaceBlock &uniformBlock : mState.getUniformBlocks())
    {
        WriteInterfaceBlock(&stream, uniformBlock);
    }

    stream.writeInt(mState.getBufferVariables().size());
    for (const BufferVariable &bufferVariable : mState.getBufferVariables())
    {
        WriteBufferVariable(&stream, bufferVariable);
    }

    stream.writeInt(mState.getShaderStorageBlocks().size());
    for (const InterfaceBlock &shaderStorageBlock : mState.getShaderStorageBlocks())
    {
        WriteInterfaceBlock(&stream, shaderStorageBlock);
    }

    stream.writeInt(mState.mExecutable->mAtomicCounterBuffers.size());
    for (const AtomicCounterBuffer &atomicCounterBuffer :
         mState.mExecutable->getAtomicCounterBuffers())
    {
        WriteShaderVariableBuffer(&stream, atomicCounterBuffer);
    }

    // Warn the app layer if saving a binary with unsupported transform feedback.
    if (!mState.getLinkedTransformFeedbackVaryings().empty() &&
        context->getFrontendFeatures().disableProgramCachingForTransformFeedback.enabled)
    {
        WARN() << "Saving program binary with transform feedback, which is not supported on this "
                  "driver.";
    }

    stream.writeInt(mState.getLinkedTransformFeedbackVaryings().size());
    for (const auto &var : mState.getLinkedTransformFeedbackVaryings())
    {
        stream.writeIntVector(var.arraySizes);
        stream.writeInt(var.type);
        stream.writeString(var.name);

        stream.writeIntOrNegOne(var.arrayIndex);
    }

    stream.writeInt(mState.getTransformFeedbackBufferMode());

    stream.writeInt(mState.getOutputVariables().size());
    for (const sh::ShaderVariable &output : mState.getOutputVariables())
    {
        WriteShaderVar(&stream, output);
        stream.writeInt(output.location);
        stream.writeInt(output.index);
    }

    stream.writeInt(mState.getOutputLocations().size());
    for (const auto &outputVar : mState.getOutputLocations())
    {
        stream.writeInt(outputVar.arrayIndex);
        stream.writeIntOrNegOne(outputVar.index);
        stream.writeBool(outputVar.ignored);
    }

    stream.writeInt(mState.getSecondaryOutputLocations().size());
    for (const auto &outputVar : mState.getSecondaryOutputLocations())
    {
        stream.writeInt(outputVar.arrayIndex);
        stream.writeIntOrNegOne(outputVar.index);
        stream.writeBool(outputVar.ignored);
    }

    stream.writeInt(mState.mOutputVariableTypes.size());
    for (const auto &outputVariableType : mState.mOutputVariableTypes)
    {
        stream.writeInt(outputVariableType);
    }

    static_assert(
        IMPLEMENTATION_MAX_DRAW_BUFFERS * 2 <= 8 * sizeof(uint32_t),
        "All bits of mDrawBufferTypeMask and mActiveOutputVariables can be contained in 32 bits");
    stream.writeInt(static_cast<int>(mState.mDrawBufferTypeMask.to_ulong()));
    stream.writeInt(static_cast<int>(mState.mActiveOutputVariables.to_ulong()));

    stream.writeBool(mState.isYUVOutput());

    stream.writeInt(mState.getDefaultUniformRange().low());
    stream.writeInt(mState.getDefaultUniformRange().high());

    stream.writeInt(mState.getSamplerUniformRange().low());
    stream.writeInt(mState.getSamplerUniformRange().high());

    stream.writeInt(mState.getSamplerBindings().size());
    for (const auto &samplerBinding : mState.getSamplerBindings())
    {
        stream.writeEnum(samplerBinding.textureType);
        stream.writeInt(samplerBinding.samplerType);
        stream.writeEnum(samplerBinding.format);
        stream.writeInt(samplerBinding.boundTextureUnits.size());
    }

    stream.writeInt(mState.getImageUniformRange().low());
    stream.writeInt(mState.getImageUniformRange().high());

    stream.writeInt(mState.getImageBindings().size());
    for (const auto &imageBinding : mState.getImageBindings())
    {
        stream.writeInt(imageBinding.boundImageUnits.size());
        stream.writeInt(static_cast<unsigned int>(imageBinding.textureType));
        for (size_t i = 0; i < imageBinding.boundImageUnits.size(); ++i)
        {
            stream.writeInt(imageBinding.boundImageUnits[i]);
        }
    }

    stream.writeInt(mState.getAtomicCounterUniformRange().low());
    stream.writeInt(mState.getAtomicCounterUniformRange().high());

    mProgram->save(context, &stream);

    ASSERT(binaryOut);
    if (!binaryOut->resize(stream.length()))
    {
        WARN() << "Failed to allocate enough memory to serialize a program. (" << stream.length()
               << " bytes )";
        return angle::Result::Incomplete;
    }
    memcpy(binaryOut->data(), stream.data(), stream.length());
    return angle::Result::Continue;
}

angle::Result Program::deserialize(const Context *context,
                                   BinaryInputStream &stream,
                                   InfoLog &infoLog)
{
    unsigned char commitString[ANGLE_COMMIT_HASH_SIZE];
    stream.readBytes(commitString, ANGLE_COMMIT_HASH_SIZE);
    if (memcmp(commitString, ANGLE_COMMIT_HASH, sizeof(unsigned char) * ANGLE_COMMIT_HASH_SIZE) !=
        0)
    {
        infoLog << "Invalid program binary version.";
        return angle::Result::Incomplete;
    }

    int majorVersion = stream.readInt<int>();
    int minorVersion = stream.readInt<int>();
    if (majorVersion != context->getClientMajorVersion() ||
        minorVersion != context->getClientMinorVersion())
    {
        infoLog << "Cannot load program binaries across different ES context versions.";
        return angle::Result::Incomplete;
    }

    mState.mExecutable->load(&stream);

    mState.mComputeShaderLocalSize[0] = stream.readInt<int>();
    mState.mComputeShaderLocalSize[1] = stream.readInt<int>();
    mState.mComputeShaderLocalSize[2] = stream.readInt<int>();

    mState.mNumViews                      = stream.readInt<int>();
    mState.mEarlyFramentTestsOptimization = stream.readBool();
    mState.mSpecConstUsageBits            = rx::SpecConstUsageBits(stream.readInt<uint32_t>());

    size_t attribCount = stream.readInt<size_t>();
    ASSERT(mState.mExecutable->getProgramInputs().empty());
    for (size_t attribIndex = 0; attribIndex < attribCount; ++attribIndex)
    {
        sh::ShaderVariable attrib;
        LoadShaderVar(&stream, &attrib);
        attrib.location = stream.readInt<int>();
        mState.mExecutable->mProgramInputs.push_back(attrib);
    }

    size_t uniformCount = stream.readInt<size_t>();
    ASSERT(mState.mExecutable->getUniforms().empty());
    for (size_t uniformIndex = 0; uniformIndex < uniformCount; ++uniformIndex)
    {
        LinkedUniform uniform;
        LoadShaderVar(&stream, &uniform);

        uniform.bufferIndex = stream.readInt<int>();
        LoadBlockMemberInfo(&stream, &uniform.blockInfo);

        stream.readIntVector<unsigned int>(&uniform.outerArraySizes);

        uniform.typeInfo = &GetUniformTypeInfo(uniform.type);

        // Active shader info
        for (ShaderType shaderType : gl::AllShaderTypes())
        {
            uniform.setActive(shaderType, stream.readBool());
        }

        mState.mExecutable->mUniforms.push_back(uniform);
    }

    const size_t uniformIndexCount = stream.readInt<size_t>();
    ASSERT(mState.mUniformLocations.empty());
    for (size_t uniformIndexIndex = 0; uniformIndexIndex < uniformIndexCount; ++uniformIndexIndex)
    {
        VariableLocation variable;
        stream.readInt(&variable.arrayIndex);
        stream.readInt(&variable.index);
        stream.readBool(&variable.ignored);

        mState.mUniformLocations.push_back(variable);
    }

    size_t uniformBlockCount = stream.readInt<size_t>();
    ASSERT(mState.mExecutable->getUniformBlocks().empty());
    for (size_t uniformBlockIndex = 0; uniformBlockIndex < uniformBlockCount; ++uniformBlockIndex)
    {
        InterfaceBlock uniformBlock;
        LoadInterfaceBlock(&stream, &uniformBlock);
        mState.mExecutable->mUniformBlocks.push_back(uniformBlock);

        mState.mActiveUniformBlockBindings.set(uniformBlockIndex, uniformBlock.binding != 0);
    }

    size_t bufferVariableCount = stream.readInt<size_t>();
    ASSERT(mState.mBufferVariables.empty());
    for (size_t bufferVarIndex = 0; bufferVarIndex < bufferVariableCount; ++bufferVarIndex)
    {
        BufferVariable bufferVariable;
        LoadBufferVariable(&stream, &bufferVariable);
        mState.mBufferVariables.push_back(bufferVariable);
    }

    size_t shaderStorageBlockCount = stream.readInt<size_t>();
    ASSERT(mState.mExecutable->getShaderStorageBlocks().empty());
    for (size_t shaderStorageBlockIndex = 0; shaderStorageBlockIndex < shaderStorageBlockCount;
         ++shaderStorageBlockIndex)
    {
        InterfaceBlock shaderStorageBlock;
        LoadInterfaceBlock(&stream, &shaderStorageBlock);
        if (getExecutable().isCompute())
        {
            mState.mExecutable->mComputeShaderStorageBlocks.push_back(shaderStorageBlock);
        }
        else
        {
            mState.mExecutable->mGraphicsShaderStorageBlocks.push_back(shaderStorageBlock);
        }
    }

    size_t atomicCounterBufferCount = stream.readInt<size_t>();
    ASSERT(mState.mExecutable->getAtomicCounterBuffers().empty());
    for (size_t bufferIndex = 0; bufferIndex < atomicCounterBufferCount; ++bufferIndex)
    {
        AtomicCounterBuffer atomicCounterBuffer;
        LoadShaderVariableBuffer(&stream, &atomicCounterBuffer);

        mState.mExecutable->mAtomicCounterBuffers.push_back(atomicCounterBuffer);
    }

    size_t transformFeedbackVaryingCount = stream.readInt<size_t>();

    // Reject programs that use transform feedback varyings if the hardware cannot support them.
    if (transformFeedbackVaryingCount > 0 &&
        context->getFrontendFeatures().disableProgramCachingForTransformFeedback.enabled)
    {
        infoLog << "Current driver does not support transform feedback in binary programs.";
        return angle::Result::Incomplete;
    }

    ASSERT(mState.mExecutable->mLinkedTransformFeedbackVaryings.empty());
    for (size_t transformFeedbackVaryingIndex = 0;
         transformFeedbackVaryingIndex < transformFeedbackVaryingCount;
         ++transformFeedbackVaryingIndex)
    {
        sh::ShaderVariable varying;
        stream.readIntVector<unsigned int>(&varying.arraySizes);
        stream.readInt(&varying.type);
        stream.readString(&varying.name);

        GLuint arrayIndex = stream.readInt<GLuint>();

        mState.mExecutable->mLinkedTransformFeedbackVaryings.emplace_back(varying, arrayIndex);
    }

    stream.readInt(&mState.mExecutable->mTransformFeedbackBufferMode);

    size_t outputCount = stream.readInt<size_t>();
    ASSERT(mState.mExecutable->getOutputVariables().empty());
    for (size_t outputIndex = 0; outputIndex < outputCount; ++outputIndex)
    {
        sh::ShaderVariable output;
        LoadShaderVar(&stream, &output);
        output.location = stream.readInt<int>();
        output.index    = stream.readInt<int>();
        mState.mExecutable->mOutputVariables.push_back(output);
    }

    size_t outputVarCount = stream.readInt<size_t>();
    ASSERT(mState.mExecutable->getOutputLocations().empty());
    for (size_t outputIndex = 0; outputIndex < outputVarCount; ++outputIndex)
    {
        VariableLocation locationData;
        stream.readInt(&locationData.arrayIndex);
        stream.readInt(&locationData.index);
        stream.readBool(&locationData.ignored);
        mState.mExecutable->mOutputLocations.push_back(locationData);
    }

    size_t secondaryOutputVarCount = stream.readInt<size_t>();
    ASSERT(mState.mSecondaryOutputLocations.empty());
    for (size_t outputIndex = 0; outputIndex < secondaryOutputVarCount; ++outputIndex)
    {
        VariableLocation locationData;
        stream.readInt(&locationData.arrayIndex);
        stream.readInt(&locationData.index);
        stream.readBool(&locationData.ignored);
        mState.mSecondaryOutputLocations.push_back(locationData);
    }

    size_t outputTypeCount = stream.readInt<size_t>();
    for (size_t outputIndex = 0; outputIndex < outputTypeCount; ++outputIndex)
    {
        mState.mOutputVariableTypes.push_back(stream.readInt<GLenum>());
    }

    static_assert(IMPLEMENTATION_MAX_DRAW_BUFFERS * 2 <= 8 * sizeof(uint32_t),
                  "All bits of mDrawBufferTypeMask and mActiveOutputVariables types and mask fit "
                  "into 32 bits each");
    mState.mDrawBufferTypeMask = gl::ComponentTypeMask(stream.readInt<uint32_t>());
    mState.mActiveOutputVariables =
        gl::DrawBufferMask(stream.readInt<gl::DrawBufferMask::value_type>());

    stream.readBool(&mState.mYUVOutput);

    unsigned int defaultUniformRangeLow  = stream.readInt<unsigned int>();
    unsigned int defaultUniformRangeHigh = stream.readInt<unsigned int>();
    mState.mExecutable->mDefaultUniformRange =
        RangeUI(defaultUniformRangeLow, defaultUniformRangeHigh);

    unsigned int samplerRangeLow             = stream.readInt<unsigned int>();
    unsigned int samplerRangeHigh            = stream.readInt<unsigned int>();
    mState.mExecutable->mSamplerUniformRange = RangeUI(samplerRangeLow, samplerRangeHigh);
    size_t samplerCount                      = stream.readInt<size_t>();
    for (size_t samplerIndex = 0; samplerIndex < samplerCount; ++samplerIndex)
    {
        TextureType textureType = stream.readEnum<TextureType>();
        GLenum samplerType      = stream.readInt<GLenum>();
        SamplerFormat format    = stream.readEnum<SamplerFormat>();
        size_t bindingCount     = stream.readInt<size_t>();
        mState.mExecutable->mSamplerBindings.emplace_back(textureType, samplerType, format,
                                                          bindingCount);
    }

    unsigned int imageRangeLow             = stream.readInt<unsigned int>();
    unsigned int imageRangeHigh            = stream.readInt<unsigned int>();
    mState.mExecutable->mImageUniformRange = RangeUI(imageRangeLow, imageRangeHigh);
    size_t imageBindingCount               = stream.readInt<size_t>();
    for (size_t imageIndex = 0; imageIndex < imageBindingCount; ++imageIndex)
    {
        size_t elementCount     = stream.readInt<size_t>();
        TextureType textureType = static_cast<TextureType>(stream.readInt<unsigned int>());
        ImageBinding imageBinding(elementCount, textureType);
        for (size_t elementIndex = 0; elementIndex < elementCount; ++elementIndex)
        {
            imageBinding.boundImageUnits[elementIndex] = stream.readInt<unsigned int>();
        }
        if (getExecutable().isCompute())
        {
            mState.mExecutable->mComputeImageBindings.emplace_back(imageBinding);
        }
        else
        {
            mState.mExecutable->mGraphicsImageBindings.emplace_back(imageBinding);
        }
    }

    unsigned int atomicCounterRangeLow  = stream.readInt<unsigned int>();
    unsigned int atomicCounterRangeHigh = stream.readInt<unsigned int>();
    mState.mAtomicCounterUniformRange   = RangeUI(atomicCounterRangeLow, atomicCounterRangeHigh);

    static_assert(static_cast<unsigned long>(ShaderType::EnumCount) <= sizeof(unsigned long) * 8,
                  "Too many shader types");

    if (!mState.mAttachedShaders[ShaderType::Compute])
    {
        mState.mExecutable->updateTransformFeedbackStrides();
    }

    postResolveLink(context);
    mState.mExecutable->updateCanDrawWith();

    return angle::Result::Continue;
}

void Program::postResolveLink(const gl::Context *context)
{
    mState.updateActiveSamplers();
    mState.mExecutable->updateActiveImages(getExecutable());

    setUniformValuesFromBindingQualifiers();

    if (context->getExtensions().multiDraw)
    {
        mState.mDrawIDLocation = getUniformLocation("gl_DrawID").value;
    }

    if (context->getExtensions().baseVertexBaseInstance)
    {
        mState.mBaseVertexLocation   = getUniformLocation("gl_BaseVertex").value;
        mState.mBaseInstanceLocation = getUniformLocation("gl_BaseInstance").value;
    }
}
}  // namespace gl
