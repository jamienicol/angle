//
// Copyright 2020 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// DriverUniform.h: Add code to support driver uniforms
//

#ifndef COMPILER_TRANSLATOR_TREEUTIL_DRIVERUNIFORM_H_
#define COMPILER_TRANSLATOR_TREEUTIL_DRIVERUNIFORM_H_

#include "common/angleutils.h"
#include "compiler/translator/Types.h"

namespace sh
{

class TCompiler;
class TIntermBlock;
class TIntermNode;
class TSymbolTable;
class TIntermTyped;
class TIntermSwizzle;
class TIntermBinary;

class DriverUniform
{
  public:
    DriverUniform() : mDriverUniforms(nullptr) {}
    virtual ~DriverUniform() = default;

    bool addComputeDriverUniformsToShader(TIntermBlock *root, TSymbolTable *symbolTable);
    bool addGraphicsDriverUniformsToShader(TIntermBlock *root, TSymbolTable *symbolTable);

    TIntermBinary *getViewportRef() const;
    TIntermBinary *getAbcBufferOffsets() const;
    TIntermBinary *getXfbVerticesPerInstance() const;
    TIntermBinary *getXfbBufferOffsets() const;
    TIntermBinary *getClipDistancesEnabled() const;
    TIntermBinary *getDepthRangeRef() const;
    TIntermBinary *getDepthRangeReservedFieldRef() const;
    TIntermBinary *getNumSamplesRef() const;

    virtual TIntermBinary *getFlipXYRef() const { return nullptr; }
    virtual TIntermBinary *getNegFlipXYRef() const { return nullptr; }
    virtual TIntermBinary *getFragRotationMatrixRef() const { return nullptr; }
    virtual TIntermBinary *getPreRotationMatrixRef() const { return nullptr; }
    virtual TIntermBinary *getHalfRenderAreaRef() const { return nullptr; }
    virtual TIntermSwizzle *getNegFlipYRef() const { return nullptr; }

  protected:
    TIntermBinary *createDriverUniformRef(const char *fieldName) const;
    virtual TFieldList *createUniformFields(TSymbolTable *symbolTable) const;
    TType *createEmulatedDepthRangeType(TSymbolTable *symbolTable) const;

    const TVariable *mDriverUniforms;
};

class DriverUniformExtended : public DriverUniform
{
  public:
    DriverUniformExtended() : DriverUniform() {}
    virtual ~DriverUniformExtended() override {}

    TIntermBinary *getFlipXYRef() const override;
    TIntermBinary *getNegFlipXYRef() const override;
    TIntermBinary *getFragRotationMatrixRef() const override;
    TIntermBinary *getPreRotationMatrixRef() const override;
    TIntermBinary *getHalfRenderAreaRef() const override;
    TIntermSwizzle *getNegFlipYRef() const override;

  protected:
    virtual TFieldList *createUniformFields(TSymbolTable *symbolTable) const override;
};

}  // namespace sh

#endif  // COMPILER_TRANSLATOR_TREEUTIL_DRIVERUNIFORM_H_
