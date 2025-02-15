//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// ContextVk.h:
//    Defines the class interface for ContextVk, implementing ContextImpl.
//

#ifndef LIBANGLE_RENDERER_VULKAN_CONTEXTVK_H_
#define LIBANGLE_RENDERER_VULKAN_CONTEXTVK_H_

#include <condition_variable>

#include "common/PackedEnums.h"
#include "common/vulkan/vk_headers.h"
#include "libANGLE/renderer/ContextImpl.h"
#include "libANGLE/renderer/renderer_utils.h"
#include "libANGLE/renderer/vulkan/DisplayVk.h"
#include "libANGLE/renderer/vulkan/OverlayVk.h"
#include "libANGLE/renderer/vulkan/PersistentCommandPool.h"
#include "libANGLE/renderer/vulkan/RendererVk.h"
#include "libANGLE/renderer/vulkan/vk_helpers.h"

namespace angle
{
struct FeaturesVk;
}  // namespace angle

namespace rx
{
class ProgramExecutableVk;
class RendererVk;
class WindowSurfaceVk;
class ShareGroupVk;

static constexpr uint32_t kMaxGpuEventNameLen = 32;
using EventName                               = std::array<char, kMaxGpuEventNameLen>;

class ContextVk : public ContextImpl, public vk::Context, public MultisampleTextureInitializer
{
  public:
    ContextVk(const gl::State &state, gl::ErrorSet *errorSet, RendererVk *renderer);
    ~ContextVk() override;

    angle::Result initialize() override;

    void onDestroy(const gl::Context *context) override;

    // Flush and finish.
    angle::Result flush(const gl::Context *context) override;
    angle::Result finish(const gl::Context *context) override;

    // Drawing methods.
    angle::Result drawArrays(const gl::Context *context,
                             gl::PrimitiveMode mode,
                             GLint first,
                             GLsizei count) override;
    angle::Result drawArraysInstanced(const gl::Context *context,
                                      gl::PrimitiveMode mode,
                                      GLint first,
                                      GLsizei count,
                                      GLsizei instanceCount) override;
    angle::Result drawArraysInstancedBaseInstance(const gl::Context *context,
                                                  gl::PrimitiveMode mode,
                                                  GLint first,
                                                  GLsizei count,
                                                  GLsizei instanceCount,
                                                  GLuint baseInstance) override;

    angle::Result drawElements(const gl::Context *context,
                               gl::PrimitiveMode mode,
                               GLsizei count,
                               gl::DrawElementsType type,
                               const void *indices) override;
    angle::Result drawElementsBaseVertex(const gl::Context *context,
                                         gl::PrimitiveMode mode,
                                         GLsizei count,
                                         gl::DrawElementsType type,
                                         const void *indices,
                                         GLint baseVertex) override;
    angle::Result drawElementsInstanced(const gl::Context *context,
                                        gl::PrimitiveMode mode,
                                        GLsizei count,
                                        gl::DrawElementsType type,
                                        const void *indices,
                                        GLsizei instanceCount) override;
    angle::Result drawElementsInstancedBaseVertex(const gl::Context *context,
                                                  gl::PrimitiveMode mode,
                                                  GLsizei count,
                                                  gl::DrawElementsType type,
                                                  const void *indices,
                                                  GLsizei instanceCount,
                                                  GLint baseVertex) override;
    angle::Result drawElementsInstancedBaseVertexBaseInstance(const gl::Context *context,
                                                              gl::PrimitiveMode mode,
                                                              GLsizei count,
                                                              gl::DrawElementsType type,
                                                              const void *indices,
                                                              GLsizei instances,
                                                              GLint baseVertex,
                                                              GLuint baseInstance) override;
    angle::Result drawRangeElements(const gl::Context *context,
                                    gl::PrimitiveMode mode,
                                    GLuint start,
                                    GLuint end,
                                    GLsizei count,
                                    gl::DrawElementsType type,
                                    const void *indices) override;
    angle::Result drawRangeElementsBaseVertex(const gl::Context *context,
                                              gl::PrimitiveMode mode,
                                              GLuint start,
                                              GLuint end,
                                              GLsizei count,
                                              gl::DrawElementsType type,
                                              const void *indices,
                                              GLint baseVertex) override;
    angle::Result drawArraysIndirect(const gl::Context *context,
                                     gl::PrimitiveMode mode,
                                     const void *indirect) override;
    angle::Result drawElementsIndirect(const gl::Context *context,
                                       gl::PrimitiveMode mode,
                                       gl::DrawElementsType type,
                                       const void *indirect) override;

    angle::Result multiDrawArrays(const gl::Context *context,
                                  gl::PrimitiveMode mode,
                                  const GLint *firsts,
                                  const GLsizei *counts,
                                  GLsizei drawcount) override;
    angle::Result multiDrawArraysInstanced(const gl::Context *context,
                                           gl::PrimitiveMode mode,
                                           const GLint *firsts,
                                           const GLsizei *counts,
                                           const GLsizei *instanceCounts,
                                           GLsizei drawcount) override;
    angle::Result multiDrawElements(const gl::Context *context,
                                    gl::PrimitiveMode mode,
                                    const GLsizei *counts,
                                    gl::DrawElementsType type,
                                    const GLvoid *const *indices,
                                    GLsizei drawcount) override;
    angle::Result multiDrawElementsInstanced(const gl::Context *context,
                                             gl::PrimitiveMode mode,
                                             const GLsizei *counts,
                                             gl::DrawElementsType type,
                                             const GLvoid *const *indices,
                                             const GLsizei *instanceCounts,
                                             GLsizei drawcount) override;
    angle::Result multiDrawArraysInstancedBaseInstance(const gl::Context *context,
                                                       gl::PrimitiveMode mode,
                                                       const GLint *firsts,
                                                       const GLsizei *counts,
                                                       const GLsizei *instanceCounts,
                                                       const GLuint *baseInstances,
                                                       GLsizei drawcount) override;
    angle::Result multiDrawElementsInstancedBaseVertexBaseInstance(const gl::Context *context,
                                                                   gl::PrimitiveMode mode,
                                                                   const GLsizei *counts,
                                                                   gl::DrawElementsType type,
                                                                   const GLvoid *const *indices,
                                                                   const GLsizei *instanceCounts,
                                                                   const GLint *baseVertices,
                                                                   const GLuint *baseInstances,
                                                                   GLsizei drawcount) override;

    // ShareGroup
    ShareGroupVk *getShareGroupVk() { return mShareGroupVk; }
    PipelineLayoutCache &getPipelineLayoutCache()
    {
        return mShareGroupVk->getPipelineLayoutCache();
    }
    DescriptorSetLayoutCache &getDescriptorSetLayoutCache()
    {
        return mShareGroupVk->getDescriptorSetLayoutCache();
    }

    // Device loss
    gl::GraphicsResetStatus getResetStatus() override;

    // Vendor and description strings.
    std::string getVendorString() const override;
    std::string getRendererDescription() const override;

    // EXT_debug_marker
    angle::Result insertEventMarker(GLsizei length, const char *marker) override;
    angle::Result pushGroupMarker(GLsizei length, const char *marker) override;
    angle::Result popGroupMarker() override;

    // KHR_debug
    angle::Result pushDebugGroup(const gl::Context *context,
                                 GLenum source,
                                 GLuint id,
                                 const std::string &message) override;
    angle::Result popDebugGroup(const gl::Context *context) override;

    // Record GL API calls for debuggers
    void logEvent(const char *eventString);
    void endEventLog(angle::EntryPoint entryPoint);

    bool isViewportFlipEnabledForDrawFBO() const;
    bool isViewportFlipEnabledForReadFBO() const;
    // When the device/surface is rotated such that the surface's aspect ratio is different than
    // the native device (e.g. 90 degrees), the width and height of the viewport, scissor, and
    // render area must be swapped.
    bool isRotatedAspectRatioForDrawFBO() const;
    bool isRotatedAspectRatioForReadFBO() const;
    SurfaceRotation getRotationDrawFramebuffer() const;
    SurfaceRotation getRotationReadFramebuffer() const;

    void invalidateProgramBindingHelper(const gl::State &glState);
    angle::Result invalidateProgramExecutableHelper(const gl::Context *context);

    // State sync with dirty bits.
    angle::Result syncState(const gl::Context *context,
                            const gl::State::DirtyBits &dirtyBits,
                            const gl::State::DirtyBits &bitMask) override;

    // Disjoint timer queries
    GLint getGPUDisjoint() override;
    GLint64 getTimestamp() override;

    // Context switching
    angle::Result onMakeCurrent(const gl::Context *context) override;
    angle::Result onUnMakeCurrent(const gl::Context *context) override;

    // Native capabilities, unmodified by gl::Context.
    gl::Caps getNativeCaps() const override;
    const gl::TextureCapsMap &getNativeTextureCaps() const override;
    const gl::Extensions &getNativeExtensions() const override;
    const gl::Limitations &getNativeLimitations() const override;

    // Shader creation
    CompilerImpl *createCompiler() override;
    ShaderImpl *createShader(const gl::ShaderState &state) override;
    ProgramImpl *createProgram(const gl::ProgramState &state) override;

    // Framebuffer creation
    FramebufferImpl *createFramebuffer(const gl::FramebufferState &state) override;

    // Texture creation
    TextureImpl *createTexture(const gl::TextureState &state) override;

    // Renderbuffer creation
    RenderbufferImpl *createRenderbuffer(const gl::RenderbufferState &state) override;

    // Buffer creation
    BufferImpl *createBuffer(const gl::BufferState &state) override;

    // Vertex Array creation
    VertexArrayImpl *createVertexArray(const gl::VertexArrayState &state) override;

    // Query and Fence creation
    QueryImpl *createQuery(gl::QueryType type) override;
    FenceNVImpl *createFenceNV() override;
    SyncImpl *createSync() override;

    // Transform Feedback creation
    TransformFeedbackImpl *createTransformFeedback(
        const gl::TransformFeedbackState &state) override;

    // Sampler object creation
    SamplerImpl *createSampler(const gl::SamplerState &state) override;

    // Program Pipeline object creation
    ProgramPipelineImpl *createProgramPipeline(const gl::ProgramPipelineState &data) override;

    // Memory object creation.
    MemoryObjectImpl *createMemoryObject() override;

    // Semaphore creation.
    SemaphoreImpl *createSemaphore() override;

    // Overlay creation.
    OverlayImpl *createOverlay(const gl::OverlayState &state) override;

    angle::Result dispatchCompute(const gl::Context *context,
                                  GLuint numGroupsX,
                                  GLuint numGroupsY,
                                  GLuint numGroupsZ) override;
    angle::Result dispatchComputeIndirect(const gl::Context *context, GLintptr indirect) override;

    angle::Result memoryBarrier(const gl::Context *context, GLbitfield barriers) override;
    angle::Result memoryBarrierByRegion(const gl::Context *context, GLbitfield barriers) override;

    ANGLE_INLINE void invalidateTexture(gl::TextureType target) override {}

    VkDevice getDevice() const;
    egl::ContextPriority getPriority() const { return mContextPriority; }

    ANGLE_INLINE const angle::FeaturesVk &getFeatures() const { return mRenderer->getFeatures(); }

    ANGLE_INLINE void invalidateVertexAndIndexBuffers()
    {
        invalidateCurrentGraphicsPipeline();
        mGraphicsDirtyBits.set(DIRTY_BIT_VERTEX_BUFFERS);
        mGraphicsDirtyBits.set(DIRTY_BIT_INDEX_BUFFER);
    }

    angle::Result onVertexBufferChange(const vk::BufferHelper *vertexBuffer);

    angle::Result onVertexAttributeChange(size_t attribIndex,
                                          GLuint stride,
                                          GLuint divisor,
                                          angle::FormatID format,
                                          bool compressed,
                                          GLuint relativeOffset,
                                          const vk::BufferHelper *vertexBuffer);

    void invalidateDefaultAttribute(size_t attribIndex);
    void invalidateDefaultAttributes(const gl::AttributesMask &dirtyMask);
    void onFramebufferChange(FramebufferVk *framebufferVk);
    void onDrawFramebufferRenderPassDescChange(FramebufferVk *framebufferVk);
    void onHostVisibleBufferWrite() { mIsAnyHostVisibleBufferWritten = true; }

    void invalidateCurrentTransformFeedbackBuffers();
    void onTransformFeedbackStateChanged();
    angle::Result onBeginTransformFeedback(
        size_t bufferCount,
        const gl::TransformFeedbackBuffersArray<vk::BufferHelper *> &buffers);
    void onEndTransformFeedback();
    angle::Result onPauseTransformFeedback();
    void pauseTransformFeedbackIfStartedAndRebindBuffersOnResume();

    // When UtilsVk issues draw or dispatch calls, it binds a new pipeline and descriptor sets that
    // the context is not aware of.  These functions are called to make sure the pipeline and
    // affected descriptor set bindings are dirtied for the next application draw/dispatch call.
    void invalidateGraphicsPipeline();
    void invalidateComputePipeline();
    void invalidateGraphicsDescriptorSet(DescriptorSetIndex usedDescriptorSet);
    void invalidateComputeDescriptorSet(DescriptorSetIndex usedDescriptorSet);

    void optimizeRenderPassForPresent(VkFramebuffer framebufferHandle);

    vk::DynamicQueryPool *getQueryPool(gl::QueryType queryType);

    const VkClearValue &getClearColorValue() const;
    const VkClearValue &getClearDepthStencilValue() const;
    gl::BlendStateExt::ColorMaskStorage::Type getClearColorMasks() const;
    angle::Result getIncompleteTexture(const gl::Context *context,
                                       gl::TextureType type,
                                       gl::Texture **textureOut);
    void updateColorMasks(const gl::BlendStateExt &blendStateExt);
    void updateSampleMaskWithRasterizationSamples(const uint32_t rasterizationSamples);

    void handleError(VkResult errorCode,
                     const char *file,
                     const char *function,
                     unsigned int line) override;
    const gl::ActiveTextureArray<vk::TextureUnit> &getActiveTextures() const
    {
        return mActiveTextures;
    }
    const gl::ActiveTextureArray<TextureVk *> &getActiveImages() const { return mActiveImages; }

    angle::Result onIndexBufferChange(const vk::BufferHelper *currentIndexBuffer);

    angle::Result flushImpl(const vk::Semaphore *semaphore);
    angle::Result finishImpl();

    void addWaitSemaphore(VkSemaphore semaphore, VkPipelineStageFlags stageMask);

    const vk::CommandPool &getCommandPool() const;

    Serial getCurrentQueueSerial() const { return mRenderer->getCurrentQueueSerial(); }
    Serial getLastSubmittedQueueSerial() const { return mRenderer->getLastSubmittedQueueSerial(); }
    Serial getLastCompletedQueueSerial() const { return mRenderer->getLastCompletedQueueSerial(); }

    bool isSerialInUse(Serial serial) const;

    template <typename T>
    void addGarbage(T *object)
    {
        if (object->valid())
        {
            mCurrentGarbage.emplace_back(vk::GetGarbage(object));
        }
    }

    // It would be nice if we didn't have to expose this for QueryVk::getResult.
    angle::Result checkCompletedCommands();

    // Wait for completion of batches until (at least) batch with given serial is finished.
    angle::Result finishToSerial(Serial serial);

    angle::Result getCompatibleRenderPass(const vk::RenderPassDesc &desc,
                                          vk::RenderPass **renderPassOut);
    angle::Result getRenderPassWithOps(const vk::RenderPassDesc &desc,
                                       const vk::AttachmentOpsArray &ops,
                                       vk::RenderPass **renderPassOut);

    vk::ShaderLibrary &getShaderLibrary() { return mShaderLibrary; }
    UtilsVk &getUtils() { return mUtils; }

    angle::Result getTimestamp(uint64_t *timestampOut);

    // Create Begin/End/Instant GPU trace events, which take their timestamps from GPU queries.
    // The events are queued until the query results are available.  Possible values for `phase`
    // are TRACE_EVENT_PHASE_*
    ANGLE_INLINE angle::Result traceGpuEvent(vk::CommandBuffer *commandBuffer,
                                             char phase,
                                             const EventName &name)
    {
        if (mGpuEventsEnabled)
            return traceGpuEventImpl(commandBuffer, phase, name);
        return angle::Result::Continue;
    }

    RenderPassCache &getRenderPassCache() { return mRenderPassCache; }

    vk::DescriptorSetLayoutDesc getDriverUniformsDescriptorSetDesc(
        VkShaderStageFlags shaderStages) const;

    // We use textureSerial to optimize texture binding updates. Each permutation of a
    // {VkImage/VkSampler} generates a unique serial. These object ids are combined to form a unique
    // signature for each descriptor set. This allows us to keep a cache of descriptor sets and
    // avoid calling vkAllocateDesctiporSets each texture update.
    const vk::TextureDescriptorDesc &getActiveTexturesDesc() const { return mActiveTexturesDesc; }

    void updateScissor(const gl::State &glState);

    bool emulateSeamfulCubeMapSampling() const { return mEmulateSeamfulCubeMapSampling; }

    bool useOldRewriteStructSamplers() const { return mUseOldRewriteStructSamplers; }

    const gl::Debug &getDebug() const { return mState.getDebug(); }
    const gl::OverlayType *getOverlay() const { return mState.getOverlay(); }

    vk::ResourceUseList &getResourceUseList() { return mResourceUseList; }

    angle::Result onBufferReleaseToExternal(const vk::BufferHelper &buffer);
    angle::Result onImageReleaseToExternal(const vk::ImageHelper &image);

    void onImageRenderPassRead(VkImageAspectFlags aspectFlags,
                               vk::ImageLayout imageLayout,
                               vk::ImageHelper *image)
    {
        ASSERT(mRenderPassCommands->started());
        mRenderPassCommands->imageRead(&mResourceUseList, aspectFlags, imageLayout, image);
    }

    void onImageRenderPassWrite(gl::LevelIndex level,
                                uint32_t layerStart,
                                uint32_t layerCount,
                                VkImageAspectFlags aspectFlags,
                                vk::ImageLayout imageLayout,
                                vk::ImageHelper *image)
    {
        ASSERT(mRenderPassCommands->started());
        mRenderPassCommands->imageWrite(&mResourceUseList, level, layerStart, layerCount,
                                        aspectFlags, imageLayout, vk::AliasingMode::Allowed, image);
    }

    void onDepthStencilDraw(gl::LevelIndex level,
                            uint32_t layerStart,
                            uint32_t layerCount,
                            vk::ImageHelper *image,
                            vk::ImageHelper *resolveImage)
    {
        ASSERT(mRenderPassCommands->started());
        mRenderPassCommands->depthStencilImagesDraw(&mResourceUseList, level, layerStart,
                                                    layerCount, image, resolveImage);
    }

    void onImageHelperRelease(const vk::ImageHelper *image)
    {
        if (mRenderPassCommands->started())
        {
            mRenderPassCommands->onImageHelperRelease(image);
        }
    }

    angle::Result getOutsideRenderPassCommandBuffer(const vk::CommandBufferAccess &access,
                                                    vk::CommandBuffer **commandBufferOut)
    {
        ASSERT(!mOutsideRenderPassCommands->hasRenderPass());
        ANGLE_TRY(onResourceAccess(access));
        *commandBufferOut = &mOutsideRenderPassCommands->getCommandBuffer();
        return angle::Result::Continue;
    }

    angle::Result beginNewRenderPass(const vk::Framebuffer &framebuffer,
                                     const gl::Rectangle &renderArea,
                                     const vk::RenderPassDesc &renderPassDesc,
                                     const vk::AttachmentOpsArray &renderPassAttachmentOps,
                                     const vk::PackedAttachmentIndex depthStencilAttachmentIndex,
                                     const vk::PackedClearValuesArray &clearValues,
                                     vk::CommandBuffer **commandBufferOut);

    // Only returns true if we have a started RP and we've run setupDraw.
    bool hasStartedRenderPass() const
    {
        // Checking mRenderPassCommandBuffer ensures we've called setupDraw.
        return mRenderPassCommandBuffer && mRenderPassCommands->started();
    }

    bool hasStartedRenderPassWithFramebuffer(vk::Framebuffer *framebuffer)
    {
        return hasStartedRenderPass() &&
               mRenderPassCommands->getFramebufferHandle() == framebuffer->getHandle();
    }

    bool hasStartedRenderPassWithCommands() const
    {
        return hasStartedRenderPass() && !mRenderPassCommands->getCommandBuffer().empty();
    }

    vk::CommandBufferHelper &getStartedRenderPassCommands()
    {
        ASSERT(mRenderPassCommands->started());
        return *mRenderPassCommands;
    }

    // TODO(https://anglebug.com/4968): Support multiple open render passes.
    void restoreFinishedRenderPass(vk::Framebuffer *framebuffer);

    uint32_t getCurrentSubpassIndex() const;

    egl::ContextPriority getContextPriority() const override { return mContextPriority; }
    angle::Result startRenderPass(gl::Rectangle renderArea, vk::CommandBuffer **commandBufferOut);
    void startNextSubpass();
    angle::Result flushCommandsAndEndRenderPass();

    angle::Result syncExternalMemory();

    void addCommandBufferDiagnostics(const std::string &commandBufferDiagnostics);

    VkIndexType getVkIndexType(gl::DrawElementsType glIndexType) const;
    size_t getVkIndexTypeSize(gl::DrawElementsType glIndexType) const;
    bool shouldConvertUint8VkIndexType(gl::DrawElementsType glIndexType) const;

    ANGLE_INLINE bool isBresenhamEmulationEnabled(const gl::PrimitiveMode mode)
    {
        return getFeatures().basicGLLineRasterization.enabled && gl::IsLineMode(mode);
    }

    const ProgramExecutableVk *getExecutable() const { return mExecutable; }
    ProgramExecutableVk *getExecutable() { return mExecutable; }

    bool isRobustResourceInitEnabled() const;

    // Queries that begin and end automatically with render pass start and end
    angle::Result beginRenderPassQuery(QueryVk *queryVk);
    void endRenderPassQuery(QueryVk *queryVk);
    void pauseRenderPassQueriesIfActive();
    angle::Result resumeRenderPassQueriesIfActive();

    // Used by QueryVk to share query helpers between transform feedback queries.
    QueryVk *getActiveRenderPassQuery(gl::QueryType queryType) const;

    void updateOverlayOnPresent();
    void addOverlayUsedBuffersCount(vk::CommandBufferHelper *commandBuffer);

    // DescriptorSet writes
    VkDescriptorBufferInfo *allocDescriptorBufferInfos(size_t count);
    VkDescriptorImageInfo *allocDescriptorImageInfos(size_t count);
    VkWriteDescriptorSet *allocWriteDescriptorSets(size_t count);

    VkDescriptorBufferInfo &allocDescriptorBufferInfo() { return *allocDescriptorBufferInfos(1); }
    VkDescriptorImageInfo &allocDescriptorImageInfo() { return *allocDescriptorImageInfos(1); }
    VkWriteDescriptorSet &allocWriteDescriptorSet() { return *allocWriteDescriptorSets(1); }

    vk::DynamicBuffer *getDefaultUniformStorage() { return &mDefaultUniformStorage; }
    // For testing only.
    void setDefaultUniformBlocksMinSizeForTesting(size_t minSize);

    vk::BufferHelper &getEmptyBuffer() { return mEmptyBuffer; }
    vk::DynamicBuffer *getStagingBuffer() { return &mStagingBuffer; }

    const vk::PerfCounters &getPerfCounters() const { return mPerfCounters; }
    vk::PerfCounters &getPerfCounters() { return mPerfCounters; }

    void onSyncHelperInitialize() { mSyncObjectPendingFlush = true; }
    void onEGLSyncHelperInitialize() { mEGLSyncObjectPendingFlush = true; }

    // Implementation of MultisampleTextureInitializer
    angle::Result initializeMultisampleTextureToBlack(const gl::Context *context,
                                                      gl::Texture *glTexture) override;

  private:
    // Dirty bits.
    enum DirtyBitType : size_t
    {
        DIRTY_BIT_EVENT_LOG,
        DIRTY_BIT_DEFAULT_ATTRIBS,
        DIRTY_BIT_PIPELINE,
        DIRTY_BIT_TEXTURES,
        DIRTY_BIT_VERTEX_BUFFERS,
        DIRTY_BIT_INDEX_BUFFER,
        DIRTY_BIT_DRIVER_UNIFORMS,
        DIRTY_BIT_DRIVER_UNIFORMS_BINDING,
        DIRTY_BIT_SHADER_RESOURCES,  // excluding textures, which are handled separately.
        DIRTY_BIT_TRANSFORM_FEEDBACK_BUFFERS,
        DIRTY_BIT_TRANSFORM_FEEDBACK_STATE,
        DIRTY_BIT_TRANSFORM_FEEDBACK_RESUME,
        DIRTY_BIT_DESCRIPTOR_SETS,
        DIRTY_BIT_MAX,
    };

    using DirtyBits = angle::BitSet<DIRTY_BIT_MAX>;

    using DirtyBitHandler = angle::Result (ContextVk::*)(const gl::Context *,
                                                         vk::CommandBuffer *commandBuffer);

    struct DriverUniformsDescriptorSet
    {
        vk::DynamicBuffer dynamicBuffer;
        VkDescriptorSet descriptorSet;
        uint32_t dynamicOffset;
        vk::BindingPointer<vk::DescriptorSetLayout> descriptorSetLayout;
        vk::RefCountedDescriptorPoolBinding descriptorPoolBinding;
        DriverUniformsDescriptorSetCache descriptorSetCache;

        DriverUniformsDescriptorSet();
        ~DriverUniformsDescriptorSet();

        void init(RendererVk *rendererVk);
        void destroy(RendererVk *rendererVk);
    };

    enum class PipelineType
    {
        Graphics = 0,
        Compute  = 1,

        InvalidEnum = 2,
        EnumCount   = 2,
    };

    // The GpuEventQuery struct holds together a timestamp query and enough data to create a
    // trace event based on that. Use traceGpuEvent to insert such queries.  They will be readback
    // when the results are available, without inserting a GPU bubble.
    //
    // - eventName will be the reported name of the event
    // - phase is either 'B' (duration begin), 'E' (duration end) or 'i' (instant // event).
    //   See Google's "Trace Event Format":
    //   https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU
    // - serial is the serial of the batch the query was submitted on.  Until the batch is
    //   submitted, the query is not checked to avoid incuring a flush.
    struct GpuEventQuery final
    {
        EventName name;
        char phase;
        vk::QueryHelper queryHelper;
    };

    // Once a query result is available, the timestamp is read and a GpuEvent object is kept until
    // the next clock sync, at which point the clock drift is compensated in the results before
    // handing them off to the application.
    struct GpuEvent final
    {
        uint64_t gpuTimestampCycles;
        std::array<char, kMaxGpuEventNameLen> name;
        char phase;
    };

    struct GpuClockSyncInfo
    {
        double gpuTimestampS;
        double cpuTimestampS;
    };

    // Performance Counters specific to this object type
    using DescriptorSetList =
        std::array<uint32_t, ToUnderlying(ContextVk::PipelineType::EnumCount)>;
    struct PerfCounters
    {
        DescriptorSetList descriptorSetsAllocated;
    };

    class ScopedDescriptorSetUpdates;

    angle::Result setupDraw(const gl::Context *context,
                            gl::PrimitiveMode mode,
                            GLint firstVertexOrInvalid,
                            GLsizei vertexOrIndexCount,
                            GLsizei instanceCount,
                            gl::DrawElementsType indexTypeOrInvalid,
                            const void *indices,
                            DirtyBits dirtyBitMask,
                            vk::CommandBuffer **commandBufferOut);

    angle::Result setupIndexedDraw(const gl::Context *context,
                                   gl::PrimitiveMode mode,
                                   GLsizei indexCount,
                                   GLsizei instanceCount,
                                   gl::DrawElementsType indexType,
                                   const void *indices,
                                   vk::CommandBuffer **commandBufferOut);
    angle::Result setupIndirectDraw(const gl::Context *context,
                                    gl::PrimitiveMode mode,
                                    DirtyBits dirtyBitMask,
                                    vk::BufferHelper *indirectBuffer,
                                    VkDeviceSize indirectBufferOffset,
                                    vk::CommandBuffer **commandBufferOut);
    angle::Result setupIndexedIndirectDraw(const gl::Context *context,
                                           gl::PrimitiveMode mode,
                                           gl::DrawElementsType indexType,
                                           vk::BufferHelper *indirectBuffer,
                                           VkDeviceSize indirectBufferOffset,
                                           vk::CommandBuffer **commandBufferOut);

    angle::Result setupLineLoopIndexedIndirectDraw(const gl::Context *context,
                                                   gl::PrimitiveMode mode,
                                                   gl::DrawElementsType indexType,
                                                   vk::BufferHelper *srcIndirectBuf,
                                                   VkDeviceSize indirectBufferOffset,
                                                   vk::CommandBuffer **commandBufferOut,
                                                   vk::BufferHelper **indirectBufferOut,
                                                   VkDeviceSize *indirectBufferOffsetOut);
    angle::Result setupLineLoopIndirectDraw(const gl::Context *context,
                                            gl::PrimitiveMode mode,
                                            vk::BufferHelper *indirectBuffer,
                                            VkDeviceSize indirectBufferOffset,
                                            vk::CommandBuffer **commandBufferOut,
                                            vk::BufferHelper **indirectBufferOut,
                                            VkDeviceSize *indirectBufferOffsetOut);

    angle::Result setupLineLoopDraw(const gl::Context *context,
                                    gl::PrimitiveMode mode,
                                    GLint firstVertex,
                                    GLsizei vertexOrIndexCount,
                                    gl::DrawElementsType indexTypeOrInvalid,
                                    const void *indices,
                                    vk::CommandBuffer **commandBufferOut,
                                    uint32_t *numIndicesOut);
    angle::Result setupDispatch(const gl::Context *context, vk::CommandBuffer **commandBufferOut);

    gl::Rectangle getCorrectedViewport(const gl::Rectangle &viewport) const;
    void updateViewport(FramebufferVk *framebufferVk,
                        const gl::Rectangle &viewport,
                        float nearPlane,
                        float farPlane,
                        bool invertViewport);
    void updateDepthRange(float nearPlane, float farPlane);
    void updateFlipViewportDrawFramebuffer(const gl::State &glState);
    void updateFlipViewportReadFramebuffer(const gl::State &glState);
    void updateSurfaceRotationDrawFramebuffer(const gl::State &glState);
    void updateSurfaceRotationReadFramebuffer(const gl::State &glState);

    angle::Result updateActiveTextures(const gl::Context *context);
    angle::Result updateActiveImages(const gl::Context *context,
                                     vk::CommandBufferHelper *commandBufferHelper);
    angle::Result updateDefaultAttribute(size_t attribIndex);

    ANGLE_INLINE void invalidateCurrentGraphicsPipeline()
    {
        mGraphicsDirtyBits.set(DIRTY_BIT_PIPELINE);
    }

    ANGLE_INLINE void invalidateCurrentComputePipeline()
    {
        mComputeDirtyBits.set(DIRTY_BIT_PIPELINE);
        mCurrentComputePipeline = nullptr;
    }

    void invalidateCurrentDefaultUniforms();
    angle::Result invalidateCurrentTextures(const gl::Context *context);
    void invalidateCurrentShaderResources();
    void invalidateGraphicsDriverUniforms();
    void invalidateDriverUniforms();

    // Handlers for graphics pipeline dirty bits.
    angle::Result handleDirtyGraphicsEventLog(const gl::Context *context,
                                              vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsDefaultAttribs(const gl::Context *context,
                                                    vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsPipeline(const gl::Context *context,
                                              vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsTextures(const gl::Context *context,
                                              vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsVertexBuffers(const gl::Context *context,
                                                   vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsIndexBuffer(const gl::Context *context,
                                                 vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsDriverUniforms(const gl::Context *context,
                                                    vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsDriverUniformsBinding(const gl::Context *context,
                                                           vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsShaderResources(const gl::Context *context,
                                                     vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsTransformFeedbackBuffersEmulation(
        const gl::Context *context,
        vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsTransformFeedbackBuffersExtension(
        const gl::Context *context,
        vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsTransformFeedbackState(const gl::Context *context,
                                                            vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyGraphicsTransformFeedbackResume(const gl::Context *context,
                                                             vk::CommandBuffer *commandBuffer);

    // Handlers for compute pipeline dirty bits.
    angle::Result handleDirtyComputePipeline(const gl::Context *context,
                                             vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyComputeTextures(const gl::Context *context,
                                             vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyComputeDriverUniforms(const gl::Context *context,
                                                   vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyComputeDriverUniformsBinding(const gl::Context *context,
                                                          vk::CommandBuffer *commandBuffer);
    angle::Result handleDirtyComputeShaderResources(const gl::Context *context,
                                                    vk::CommandBuffer *commandBuffer);

    // Common parts of the common dirty bit handlers.
    angle::Result handleDirtyTexturesImpl(vk::CommandBufferHelper *commandBufferHelper);
    angle::Result handleDirtyShaderResourcesImpl(const gl::Context *context,
                                                 vk::CommandBufferHelper *commandBufferHelper);
    void handleDirtyDriverUniformsBindingImpl(vk::CommandBuffer *commandBuffer,
                                              VkPipelineBindPoint bindPoint,
                                              DriverUniformsDescriptorSet *driverUniforms);
    angle::Result handleDirtyDescriptorSets(const gl::Context *context,
                                            vk::CommandBuffer *commandBuffer);
    angle::Result allocateDriverUniforms(size_t driverUniformsSize,
                                         DriverUniformsDescriptorSet *driverUniforms,
                                         uint8_t **ptrOut,
                                         bool *newBufferOut);
    angle::Result updateDriverUniformsDescriptorSet(bool newBuffer,
                                                    size_t driverUniformsSize,
                                                    DriverUniformsDescriptorSet *driverUniforms);

    void writeAtomicCounterBufferDriverUniformOffsets(uint32_t *offsetsOut, size_t offsetsSize);

    angle::Result submitFrame(const vk::Semaphore *signalSemaphore);
    angle::Result memoryBarrierImpl(GLbitfield barriers, VkPipelineStageFlags stageMask);

    angle::Result synchronizeCpuGpuTime();
    angle::Result traceGpuEventImpl(vk::CommandBuffer *commandBuffer,
                                    char phase,
                                    const EventName &name);
    angle::Result checkCompletedGpuEvents();
    void flushGpuEvents(double nextSyncGpuTimestampS, double nextSyncCpuTimestampS);
    void handleDeviceLost();
    bool shouldEmulateSeamfulCubeMapSampling() const;
    bool shouldUseOldRewriteStructSamplers() const;
    void clearAllGarbage();
    bool hasRecordedCommands();
    void dumpCommandStreamDiagnostics();
    angle::Result flushOutsideRenderPassCommands();
    void flushDescriptorSetUpdates();

    void onRenderPassFinished();

    void initIndexTypeMap();

    angle::Result endRenderPassIfTransformFeedbackBuffer(const vk::BufferHelper *buffer);

    void populateTransformFeedbackBufferSet(
        size_t bufferCount,
        const gl::TransformFeedbackBuffersArray<vk::BufferHelper *> &buffers);
    void pauseTransformFeedbackIfStarted(DirtyBits onResumeOps);

    // DescriptorSet writes
    template <typename T, const T *VkWriteDescriptorSet::*pInfo>
    T *allocDescriptorInfos(std::vector<T> *descriptorVector, size_t count);
    template <typename T, const T *VkWriteDescriptorSet::*pInfo>
    void growDesciptorCapacity(std::vector<T> *descriptorVector, size_t newSize);

    angle::Result updateRenderPassDepthStencilAccess();
    bool shouldSwitchToReadOnlyDepthFeedbackLoopMode(const gl::Context *context,
                                                     gl::Texture *texture) const;

    angle::Result onResourceAccess(const vk::CommandBufferAccess &access);
    angle::Result flushCommandBuffersIfNecessary(const vk::CommandBufferAccess &access);

    void outputCumulativePerfCounters();

    void updateSampleShadingWithRasterizationSamples(const uint32_t rasterizationSamples);
    void updateRasterizationSamples(const uint32_t rasterizationSamples);

    SpecConstUsageBits getCurrentProgramSpecConstUsageBits() const;
    void updateGraphicsPipelineDescWithSpecConstUsageBits(SpecConstUsageBits usageBits);

    std::array<DirtyBitHandler, DIRTY_BIT_MAX> mGraphicsDirtyBitHandlers;
    std::array<DirtyBitHandler, DIRTY_BIT_MAX> mComputeDirtyBitHandlers;

    vk::CommandBuffer *mRenderPassCommandBuffer;

    vk::PipelineHelper *mCurrentGraphicsPipeline;
    vk::PipelineAndSerial *mCurrentComputePipeline;
    gl::PrimitiveMode mCurrentDrawMode;

    WindowSurfaceVk *mCurrentWindowSurface;
    // Records the current rotation of the surface (draw/read) framebuffer, derived from
    // mCurrentWindowSurface->getPreTransform().
    SurfaceRotation mCurrentRotationDrawFramebuffer;
    SurfaceRotation mCurrentRotationReadFramebuffer;

    // Keep a cached pipeline description structure that can be used to query the pipeline cache.
    // Kept in a pointer so allocations can be aligned, and structs can be portably packed.
    std::unique_ptr<vk::GraphicsPipelineDesc> mGraphicsPipelineDesc;
    vk::GraphicsPipelineTransitionBits mGraphicsPipelineTransition;

    // These pools are externally sychronized, so cannot be accessed from different
    // threads simultaneously. Hence, we keep them in the ContextVk instead of the RendererVk.
    // Note that this implementation would need to change in shared resource scenarios. Likely
    // we'd instead share a single set of pools between the share groups.
    angle::PackedEnumMap<PipelineType, vk::DynamicDescriptorPool> mDriverUniformsDescriptorPools;
    gl::QueryTypeMap<vk::DynamicQueryPool> mQueryPools;

    // Queries that need to be closed and reopened with the render pass:
    //
    // - Occlusion queries
    // - Transform feedback queries, if not emulated
    gl::QueryTypeMap<QueryVk *> mActiveRenderPassQueries;

    // Dirty bits.
    DirtyBits mGraphicsDirtyBits;
    DirtyBits mComputeDirtyBits;
    DirtyBits mNonIndexedDirtyBitsMask;
    DirtyBits mIndexedDirtyBitsMask;
    DirtyBits mNewGraphicsCommandBufferDirtyBits;
    DirtyBits mNewComputeCommandBufferDirtyBits;

    // Cached back-end objects.
    VertexArrayVk *mVertexArray;
    FramebufferVk *mDrawFramebuffer;
    ProgramVk *mProgram;
    ProgramPipelineVk *mProgramPipeline;
    ProgramExecutableVk *mExecutable;

    // The offset we had the last time we bound the index buffer.
    const GLvoid *mLastIndexBufferOffset;
    gl::DrawElementsType mCurrentDrawElementsType;
    angle::PackedEnumMap<gl::DrawElementsType, VkIndexType> mIndexTypeMap;

    // Cache the current draw call's firstVertex to be passed to
    // TransformFeedbackVk::getBufferOffsets.  Unfortunately, gl_BaseVertex support in Vulkan is
    // not yet ubiquitous, which would have otherwise removed the need for this value to be passed
    // as a uniform.
    GLint mXfbBaseVertex;
    // Cache the current draw call's vertex count as well to support instanced draw calls
    GLuint mXfbVertexCountPerInstance;

    // Cached clear value/mask for color and depth/stencil.
    VkClearValue mClearColorValue;
    VkClearValue mClearDepthStencilValue;
    gl::BlendStateExt::ColorMaskStorage::Type mClearColorMasks;

    IncompleteTextureSet mIncompleteTextures;

    // If the current surface bound to this context wants to have all rendering flipped vertically.
    // Updated on calls to onMakeCurrent.
    bool mFlipYForCurrentSurface;
    bool mFlipViewportForDrawFramebuffer;
    bool mFlipViewportForReadFramebuffer;

    // If any host-visible buffer is written by the GPU since last submission, a barrier is inserted
    // at the end of the command buffer to make that write available to the host.
    bool mIsAnyHostVisibleBufferWritten;

    // Whether this context should do seamful cube map sampling emulation.
    bool mEmulateSeamfulCubeMapSampling;

    // Whether this context should use the old version of the
    // RewriteStructSamplers pass.
    bool mUseOldRewriteStructSamplers;

    angle::PackedEnumMap<PipelineType, DriverUniformsDescriptorSet> mDriverUniforms;

    // This cache should also probably include the texture index (shader location) and array
    // index (also in the shader). This info is used in the descriptor update step.
    gl::ActiveTextureArray<vk::TextureUnit> mActiveTextures;
    vk::TextureDescriptorDesc mActiveTexturesDesc;

    gl::ActiveTextureArray<TextureVk *> mActiveImages;

    // "Current Value" aka default vertex attribute state.
    gl::AttributesMask mDirtyDefaultAttribsMask;
    gl::AttribArray<vk::DynamicBuffer> mDefaultAttribBuffers;

    // We use a single pool for recording commands. We also keep a free list for pool recycling.
    vk::CommandPool mCommandPool;

    vk::GarbageList mCurrentGarbage;

    RenderPassCache mRenderPassCache;

    vk::CommandBufferHelper *mOutsideRenderPassCommands;
    vk::CommandBufferHelper *mRenderPassCommands;

    // Transform feedback buffers.
    angle::FastUnorderedSet<const vk::BufferHelper *,
                            gl::IMPLEMENTATION_MAX_TRANSFORM_FEEDBACK_BUFFERS>
        mCurrentTransformFeedbackBuffers;

    // Internal shader library.
    vk::ShaderLibrary mShaderLibrary;
    UtilsVk mUtils;

    bool mGpuEventsEnabled;
    vk::DynamicQueryPool mGpuEventQueryPool;
    // A list of queries that have yet to be turned into an event (their result is not yet
    // available).
    std::vector<GpuEventQuery> mInFlightGpuEventQueries;
    // A list of gpu events since the last clock sync.
    std::vector<GpuEvent> mGpuEvents;

    // Track SyncHelper object been added into secondary command buffer that has not been flushed to
    // vulkan.
    bool mSyncObjectPendingFlush;
    bool mEGLSyncObjectPendingFlush;
    bool mHasDeferredFlush;

    // Semaphores that must be waited on in the next submission.
    std::vector<VkSemaphore> mWaitSemaphores;
    std::vector<VkPipelineStageFlags> mWaitSemaphoreStageMasks;

    // Hold information from the last gpu clock sync for future gpu-to-cpu timestamp conversions.
    GpuClockSyncInfo mGpuClockSync;

    // The very first timestamp queried for a GPU event is used as origin, so event timestamps would
    // have a value close to zero, to avoid losing 12 bits when converting these 64 bit values to
    // double.
    uint64_t mGpuEventTimestampOrigin;

    // A mix of per-frame and per-run counters.
    vk::PerfCounters mPerfCounters;
    PerfCounters mObjectPerfCounters;

    gl::State::DirtyBits mPipelineDirtyBitsMask;

    // List of all resources currently being used by this ContextVk's recorded commands.
    vk::ResourceUseList mResourceUseList;

    egl::ContextPriority mContextPriority;

    const vk::BufferHelper *mCurrentIndirectBuffer;

    // Storage for vkUpdateDescriptorSets
    std::vector<VkDescriptorBufferInfo> mDescriptorBufferInfos;
    std::vector<VkDescriptorImageInfo> mDescriptorImageInfos;
    std::vector<VkWriteDescriptorSet> mWriteDescriptorSets;

    ShareGroupVk *mShareGroupVk;

    // This is a special "empty" placeholder buffer for use when we just need a placeholder buffer
    // but not the data. Examples are shader that has no uniform or doesn't use all slots in the
    // atomic counter buffer array, or places where there is no vertex buffer since Vulkan does not
    // allow binding a null vertex buffer.
    vk::BufferHelper mEmptyBuffer;

    // Storage for default uniforms of ProgramVks and ProgramPipelineVks.
    vk::DynamicBuffer mDefaultUniformStorage;

    // All staging buffer support is provided by a DynamicBuffer.
    vk::DynamicBuffer mStagingBuffer;

    std::vector<std::string> mCommandBufferDiagnostics;

    // Record GL API calls for debuggers
    std::vector<std::string> mEventLog;
};

ANGLE_INLINE angle::Result ContextVk::endRenderPassIfTransformFeedbackBuffer(
    const vk::BufferHelper *buffer)
{
    if (!buffer || !mCurrentTransformFeedbackBuffers.contains(buffer))
    {
        return angle::Result::Continue;
    }

    return flushCommandsAndEndRenderPass();
}

ANGLE_INLINE angle::Result ContextVk::onIndexBufferChange(
    const vk::BufferHelper *currentIndexBuffer)
{
    mGraphicsDirtyBits.set(DIRTY_BIT_INDEX_BUFFER);
    mLastIndexBufferOffset = reinterpret_cast<const void *>(angle::DirtyPointer);
    return endRenderPassIfTransformFeedbackBuffer(currentIndexBuffer);
}

ANGLE_INLINE angle::Result ContextVk::onVertexBufferChange(const vk::BufferHelper *vertexBuffer)
{
    mGraphicsDirtyBits.set(DIRTY_BIT_VERTEX_BUFFERS);
    return endRenderPassIfTransformFeedbackBuffer(vertexBuffer);
}

ANGLE_INLINE angle::Result ContextVk::onVertexAttributeChange(size_t attribIndex,
                                                              GLuint stride,
                                                              GLuint divisor,
                                                              angle::FormatID format,
                                                              bool compressed,
                                                              GLuint relativeOffset,
                                                              const vk::BufferHelper *vertexBuffer)
{
    invalidateCurrentGraphicsPipeline();
    // Set divisor to 1 for attribs with emulated divisor
    mGraphicsPipelineDesc->updateVertexInput(
        &mGraphicsPipelineTransition, static_cast<uint32_t>(attribIndex), stride,
        divisor > mRenderer->getMaxVertexAttribDivisor() ? 1 : divisor, format, compressed,
        relativeOffset);
    return onVertexBufferChange(vertexBuffer);
}
}  // namespace rx

#endif  // LIBANGLE_RENDERER_VULKAN_CONTEXTVK_H_
