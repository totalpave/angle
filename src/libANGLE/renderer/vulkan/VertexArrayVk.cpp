//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// VertexArrayVk.cpp:
//    Implements the class methods for VertexArrayVk.
//

#include "libANGLE/renderer/vulkan/VertexArrayVk.h"

#include "common/debug.h"
#include "common/utilities.h"
#include "libANGLE/Context.h"
#include "libANGLE/renderer/vulkan/BufferVk.h"
#include "libANGLE/renderer/vulkan/CommandGraph.h"
#include "libANGLE/renderer/vulkan/ContextVk.h"
#include "libANGLE/renderer/vulkan/FramebufferVk.h"
#include "libANGLE/renderer/vulkan/RendererVk.h"
#include "libANGLE/renderer/vulkan/vk_format_utils.h"
#include "libANGLE/trace.h"

namespace rx
{
namespace
{
constexpr size_t kDynamicVertexDataSize = 1024 * 1024;
constexpr size_t kDynamicIndexDataSize  = 1024 * 8;

ANGLE_INLINE bool BindingIsAligned(const gl::VertexBinding &binding,
                                   const angle::Format &angleFormat,
                                   unsigned int attribSize,
                                   GLuint relativeOffset)
{
    GLintptr totalOffset = binding.getOffset() + relativeOffset;
    GLuint mask          = angleFormat.componentAlignmentMask;
    if (mask != std::numeric_limits<GLuint>::max())
    {
        return ((totalOffset & mask) == 0 && (binding.getStride() & mask) == 0);
    }
    else
    {
        // To perform the GPU conversion for formats with components that aren't byte-aligned
        // (for example, A2BGR10 or RGB10A2), one element has to be placed in 4 bytes to perform
        // the compute shader. So, binding offset and stride has to be aligned to formatSize.
        unsigned int formatSize = angleFormat.pixelBytes;
        return (totalOffset % formatSize == 0) && (binding.getStride() % formatSize == 0);
    }
}

angle::Result StreamVertexData(ContextVk *contextVk,
                               vk::DynamicBuffer *dynamicBuffer,
                               const uint8_t *sourceData,
                               size_t bytesToAllocate,
                               size_t destOffset,
                               size_t vertexCount,
                               size_t stride,
                               VertexCopyFunction vertexLoadFunction,
                               vk::BufferHelper **bufferOut,
                               VkDeviceSize *bufferOffsetOut)
{
    uint8_t *dst = nullptr;
    ANGLE_TRY(dynamicBuffer->allocate(contextVk, bytesToAllocate, &dst, nullptr, bufferOffsetOut,
                                      nullptr));
    *bufferOut = dynamicBuffer->getCurrentBuffer();
    dst += destOffset;
    vertexLoadFunction(sourceData, stride, vertexCount, dst);

    ANGLE_TRY(dynamicBuffer->flush(contextVk));
    return angle::Result::Continue;
}

size_t GetVertexCount(BufferVk *srcBuffer, const gl::VertexBinding &binding, uint32_t srcFormatSize)
{
    // Bytes usable for vertex data.
    GLint64 bytes = srcBuffer->getSize() - binding.getOffset();
    if (bytes < srcFormatSize)
        return 0;

    // Count the last vertex.  It may occupy less than a full stride.
    size_t numVertices = 1;
    bytes -= srcFormatSize;

    // Count how many strides fit remaining space.
    if (bytes > 0)
        numVertices += static_cast<size_t>(bytes) / binding.getStride();

    return numVertices;
}
}  // anonymous namespace

VertexArrayVk::VertexArrayVk(ContextVk *contextVk, const gl::VertexArrayState &state)
    : VertexArrayImpl(state),
      mCurrentArrayBufferHandles{},
      mCurrentArrayBufferOffsets{},
      mCurrentArrayBuffers{},
      mCurrentElementArrayBufferOffset(0),
      mCurrentElementArrayBuffer(nullptr),
      mLineLoopHelper(contextVk->getRenderer()),
      mDirtyLineLoopTranslation(true)
{
    RendererVk *renderer = contextVk->getRenderer();

    VkBufferCreateInfo createInfo = {};
    createInfo.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createInfo.size               = 16;
    createInfo.usage              = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    (void)mTheNullBuffer.init(contextVk, createInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    mCurrentArrayBufferHandles.fill(mTheNullBuffer.getBuffer().getHandle());
    mCurrentArrayBufferOffsets.fill(0);
    mCurrentArrayBuffers.fill(&mTheNullBuffer);

    mDynamicVertexData.init(renderer, vk::kVertexBufferUsageFlags, vk::kVertexBufferAlignment,
                            kDynamicVertexDataSize, true);

    // We use an alignment of four for index data. This ensures that compute shaders can read index
    // elements from "uint" aligned addresses.
    mDynamicIndexData.init(renderer, vk::kIndexBufferUsageFlags, vk::kIndexBufferAlignment,
                           kDynamicIndexDataSize, true);
    mTranslatedByteIndexData.init(renderer, vk::kIndexBufferUsageFlags, vk::kIndexBufferAlignment,
                                  kDynamicIndexDataSize, true);
}

VertexArrayVk::~VertexArrayVk() {}

void VertexArrayVk::destroy(const gl::Context *context)
{
    ContextVk *contextVk = vk::GetImpl(context);

    mTheNullBuffer.release(contextVk);

    mDynamicVertexData.release(contextVk);
    mDynamicIndexData.release(contextVk);
    mTranslatedByteIndexData.release(contextVk);
    mLineLoopHelper.release(contextVk);
}

angle::Result VertexArrayVk::convertIndexBufferGPU(ContextVk *contextVk,
                                                   BufferVk *bufferVk,
                                                   const void *indices)
{
    intptr_t offsetIntoSrcData = reinterpret_cast<intptr_t>(indices);
    size_t srcDataSize         = static_cast<size_t>(bufferVk->getSize()) - offsetIntoSrcData;

    mTranslatedByteIndexData.releaseInFlightBuffers(contextVk);

    ANGLE_TRY(mTranslatedByteIndexData.allocate(contextVk, sizeof(GLushort) * srcDataSize, nullptr,
                                                nullptr, &mCurrentElementArrayBufferOffset,
                                                nullptr));
    mCurrentElementArrayBuffer = mTranslatedByteIndexData.getCurrentBuffer();

    vk::BufferHelper *dest = mTranslatedByteIndexData.getCurrentBuffer();
    vk::BufferHelper *src  = &bufferVk->getBuffer();

    // Copy relevant section of the source into destination at allocated offset.  Note that the
    // offset returned by allocate() above is in bytes. As is the indices offset pointer.
    UtilsVk::ConvertIndexParameters params = {};
    params.srcOffset                       = static_cast<uint32_t>(offsetIntoSrcData);
    params.dstOffset = static_cast<uint32_t>(mCurrentElementArrayBufferOffset);
    params.maxIndex  = static_cast<uint32_t>(bufferVk->getSize());

    return contextVk->getUtils().convertIndexBuffer(contextVk, dest, src, params);
}

angle::Result VertexArrayVk::convertIndexBufferCPU(ContextVk *contextVk,
                                                   gl::DrawElementsType indexType,
                                                   size_t indexCount,
                                                   const void *sourcePointer)
{
    ASSERT(!mState.getElementArrayBuffer() || indexType == gl::DrawElementsType::UnsignedByte);

    mDynamicIndexData.releaseInFlightBuffers(contextVk);

    size_t elementSize = gl::GetDrawElementsTypeSize(indexType);
    if (indexType == gl::DrawElementsType::UnsignedByte)
    {
        // 8-bit indices are not supported by Vulkan, so they are promoted to
        // 16-bit indices below
        elementSize = sizeof(GLushort);
    }

    const size_t amount = elementSize * indexCount;
    GLubyte *dst        = nullptr;

    ANGLE_TRY(mDynamicIndexData.allocate(contextVk, amount, &dst, nullptr,
                                         &mCurrentElementArrayBufferOffset, nullptr));
    mCurrentElementArrayBuffer = mDynamicIndexData.getCurrentBuffer();
    if (indexType == gl::DrawElementsType::UnsignedByte)
    {
        // Unsigned bytes don't have direct support in Vulkan so we have to expand the
        // memory to a GLushort.
        const GLubyte *in     = static_cast<const GLubyte *>(sourcePointer);
        GLushort *expandedDst = reinterpret_cast<GLushort *>(dst);
        bool primitiveRestart = contextVk->getState().isPrimitiveRestartEnabled();

        constexpr GLubyte kUnsignedByteRestartValue   = 0xFF;
        constexpr GLushort kUnsignedShortRestartValue = 0xFFFF;

        if (primitiveRestart)
        {
            for (size_t index = 0; index < indexCount; index++)
            {
                GLushort value = static_cast<GLushort>(in[index]);
                if (in[index] == kUnsignedByteRestartValue)
                {
                    // Convert from 8-bit restart value to 16-bit restart value
                    value = kUnsignedShortRestartValue;
                }
                expandedDst[index] = value;
            }
        }
        else
        {
            // Fast path for common case.
            for (size_t index = 0; index < indexCount; index++)
            {
                expandedDst[index] = static_cast<GLushort>(in[index]);
            }
        }
    }
    else
    {
        // The primitive restart value is the same for OpenGL and Vulkan,
        // so there's no need to perform any conversion.
        memcpy(dst, sourcePointer, amount);
    }
    return mDynamicIndexData.flush(contextVk);
}

// We assume the buffer is completely full of the same kind of data and convert
// and/or align it as we copy it to a DynamicBuffer. The assumption could be wrong
// but the alternative of copying it piecemeal on each draw would have a lot more
// overhead.
angle::Result VertexArrayVk::convertVertexBufferGPU(ContextVk *contextVk,
                                                    BufferVk *srcBuffer,
                                                    const gl::VertexBinding &binding,
                                                    size_t attribIndex,
                                                    const vk::Format &vertexFormat,
                                                    ConversionBuffer *conversion,
                                                    GLuint relativeOffset)
{
    const angle::Format &srcFormat  = vertexFormat.angleFormat();
    const angle::Format &destFormat = vertexFormat.bufferFormat();

    ASSERT(binding.getStride() % (srcFormat.pixelBytes / srcFormat.channelCount) == 0);

    unsigned srcFormatSize  = srcFormat.pixelBytes;
    unsigned destFormatSize = destFormat.pixelBytes;

    size_t numVertices = GetVertexCount(srcBuffer, binding, srcFormatSize);
    if (numVertices == 0)
    {
        return angle::Result::Continue;
    }

    ASSERT(GetVertexInputAlignment(vertexFormat) <= vk::kVertexBufferAlignment);

    // Allocate buffer for results
    conversion->data.releaseInFlightBuffers(contextVk);
    ANGLE_TRY(conversion->data.allocate(contextVk, numVertices * destFormatSize, nullptr, nullptr,
                                        &conversion->lastAllocationOffset, nullptr));

    ASSERT(conversion->dirty);
    conversion->dirty = false;

    UtilsVk::ConvertVertexParameters params;
    params.vertexCount = numVertices;
    params.srcFormat   = &srcFormat;
    params.destFormat  = &destFormat;
    params.srcStride   = binding.getStride();
    params.srcOffset   = binding.getOffset() + relativeOffset;
    params.destOffset  = static_cast<size_t>(conversion->lastAllocationOffset);

    ANGLE_TRY(contextVk->getUtils().convertVertexBuffer(
        contextVk, conversion->data.getCurrentBuffer(), &srcBuffer->getBuffer(), params));

    return angle::Result::Continue;
}

angle::Result VertexArrayVk::convertVertexBufferCPU(ContextVk *contextVk,
                                                    BufferVk *srcBuffer,
                                                    const gl::VertexBinding &binding,
                                                    size_t attribIndex,
                                                    const vk::Format &vertexFormat,
                                                    ConversionBuffer *conversion,
                                                    GLuint relativeOffset)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "VertexArrayVk::convertVertexBufferCpu");

    unsigned srcFormatSize = vertexFormat.angleFormat().pixelBytes;
    unsigned dstFormatSize = vertexFormat.bufferFormat().pixelBytes;

    conversion->data.releaseInFlightBuffers(contextVk);

    size_t numVertices = GetVertexCount(srcBuffer, binding, srcFormatSize);
    if (numVertices == 0)
    {
        return angle::Result::Continue;
    }

    void *src = nullptr;
    ANGLE_TRY(srcBuffer->mapImpl(contextVk, &src));
    const uint8_t *srcBytes = reinterpret_cast<const uint8_t *>(src);
    srcBytes += binding.getOffset() + relativeOffset;
    ASSERT(GetVertexInputAlignment(vertexFormat) <= vk::kVertexBufferAlignment);
    ANGLE_TRY(StreamVertexData(contextVk, &conversion->data, srcBytes, numVertices * dstFormatSize,
                               0, numVertices, binding.getStride(), vertexFormat.vertexLoadFunction,
                               &mCurrentArrayBuffers[attribIndex],
                               &conversion->lastAllocationOffset));
    srcBuffer->unmapImpl(contextVk);

    ASSERT(conversion->dirty);
    conversion->dirty = false;

    return angle::Result::Continue;
}

angle::Result VertexArrayVk::syncState(const gl::Context *context,
                                       const gl::VertexArray::DirtyBits &dirtyBits,
                                       gl::VertexArray::DirtyAttribBitsArray *attribBits,
                                       gl::VertexArray::DirtyBindingBitsArray *bindingBits)
{
    ASSERT(dirtyBits.any());

    ContextVk *contextVk = vk::GetImpl(context);

    const std::vector<gl::VertexAttribute> &attribs = mState.getVertexAttributes();
    const std::vector<gl::VertexBinding> &bindings  = mState.getVertexBindings();

    for (size_t dirtyBit : dirtyBits)
    {
        switch (dirtyBit)
        {
            case gl::VertexArray::DIRTY_BIT_ELEMENT_ARRAY_BUFFER:
            {
                gl::Buffer *bufferGL = mState.getElementArrayBuffer();
                if (bufferGL)
                {
                    BufferVk *bufferVk         = vk::GetImpl(bufferGL);
                    mCurrentElementArrayBuffer = &bufferVk->getBuffer();
                }
                else
                {
                    mCurrentElementArrayBuffer = nullptr;
                }

                mLineLoopBufferFirstIndex.reset();
                mLineLoopBufferLastIndex.reset();
                contextVk->setIndexBufferDirty();
                mDirtyLineLoopTranslation = true;
                break;
            }

            case gl::VertexArray::DIRTY_BIT_ELEMENT_ARRAY_BUFFER_DATA:
                mLineLoopBufferFirstIndex.reset();
                mLineLoopBufferLastIndex.reset();
                contextVk->setIndexBufferDirty();
                mDirtyLineLoopTranslation = true;
                break;

#define ANGLE_VERTEX_DIRTY_ATTRIB_FUNC(INDEX)                                                 \
    case gl::VertexArray::DIRTY_BIT_ATTRIB_0 + INDEX:                                         \
    {                                                                                         \
        const bool bufferOnly =                                                               \
            (*attribBits)[INDEX].to_ulong() ==                                                \
            angle::Bit<unsigned long>(gl::VertexArray::DIRTY_ATTRIB_POINTER_BUFFER);          \
        ANGLE_TRY(syncDirtyAttrib(contextVk, attribs[INDEX],                                  \
                                  bindings[attribs[INDEX].bindingIndex], INDEX, bufferOnly)); \
        (*attribBits)[INDEX].reset();                                                         \
        break;                                                                                \
    }

                ANGLE_VERTEX_INDEX_CASES(ANGLE_VERTEX_DIRTY_ATTRIB_FUNC)

#define ANGLE_VERTEX_DIRTY_BINDING_FUNC(INDEX)                                          \
    case gl::VertexArray::DIRTY_BIT_BINDING_0 + INDEX:                                  \
        for (size_t attribIndex : bindings[INDEX].getBoundAttributesMask())             \
        {                                                                               \
            ANGLE_TRY(syncDirtyAttrib(contextVk, attribs[attribIndex], bindings[INDEX], \
                                      attribIndex, false));                             \
        }                                                                               \
        (*bindingBits)[INDEX].reset();                                                  \
        break;

                ANGLE_VERTEX_INDEX_CASES(ANGLE_VERTEX_DIRTY_BINDING_FUNC)

#define ANGLE_VERTEX_DIRTY_BUFFER_DATA_FUNC(INDEX)                                       \
    case gl::VertexArray::DIRTY_BIT_BUFFER_DATA_0 + INDEX:                               \
        ANGLE_TRY(syncDirtyAttrib(contextVk, attribs[INDEX],                             \
                                  bindings[attribs[INDEX].bindingIndex], INDEX, false)); \
        break;

                ANGLE_VERTEX_INDEX_CASES(ANGLE_VERTEX_DIRTY_BUFFER_DATA_FUNC)

            default:
                UNREACHABLE();
                break;
        }
    }

    return angle::Result::Continue;
}

#undef ANGLE_VERTEX_DIRTY_ATTRIB_FUNC
#undef ANGLE_VERTEX_DIRTY_BINDING_FUNC
#undef ANGLE_VERTEX_DIRTY_BUFFER_DATA_FUNC

ANGLE_INLINE void VertexArrayVk::setDefaultPackedInput(ContextVk *contextVk, size_t attribIndex)
{
    const gl::State &glState = contextVk->getState();
    const gl::VertexAttribCurrentValueData &defaultValue =
        glState.getVertexAttribCurrentValues()[attribIndex];

    angle::FormatID format = GetCurrentValueFormatID(defaultValue.Type);

    contextVk->onVertexAttributeChange(attribIndex, 0, 0, format, 0);
}

angle::Result VertexArrayVk::syncDirtyAttrib(ContextVk *contextVk,
                                             const gl::VertexAttribute &attrib,
                                             const gl::VertexBinding &binding,
                                             size_t attribIndex,
                                             bool bufferOnly)
{
    if (attrib.enabled)
    {
        RendererVk *renderer           = contextVk->getRenderer();
        const vk::Format &vertexFormat = renderer->getFormat(attrib.format->id);

        GLuint stride;
        bool anyVertexBufferConvertedOnGpu = false;
        gl::Buffer *bufferGL               = binding.getBuffer().get();
        if (bufferGL)
        {
            BufferVk *bufferVk               = vk::GetImpl(bufferGL);
            const angle::Format &angleFormat = vertexFormat.angleFormat();
            bool bindingIsAligned = BindingIsAligned(binding, angleFormat, angleFormat.channelCount,
                                                     attrib.relativeOffset);

            if (vertexFormat.vertexLoadRequiresConversion || !bindingIsAligned)
            {
                ConversionBuffer *conversion = bufferVk->getVertexConversionBuffer(
                    renderer, angleFormat.id, binding.getStride(),
                    binding.getOffset() + attrib.relativeOffset);
                if (conversion->dirty)
                {
                    if (bindingIsAligned)
                    {
                        ANGLE_TRY(convertVertexBufferGPU(contextVk, bufferVk, binding, attribIndex,
                                                         vertexFormat, conversion,
                                                         attrib.relativeOffset));
                        anyVertexBufferConvertedOnGpu = true;
                    }
                    else
                    {
                        ANGLE_TRY(convertVertexBufferCPU(contextVk, bufferVk, binding, attribIndex,
                                                         vertexFormat, conversion,
                                                         attrib.relativeOffset));
                    }

                    // If conversion happens, the destination buffer stride may be changed,
                    // therefore an attribute change needs to be called. Note that it may trigger
                    // unnecessary vulkan PSO update when the destination buffer stride does not
                    // change, but for simplity just make it conservative
                    bufferOnly = false;
                }

                vk::BufferHelper *bufferHelper          = conversion->data.getCurrentBuffer();
                mCurrentArrayBuffers[attribIndex]       = bufferHelper;
                mCurrentArrayBufferHandles[attribIndex] = bufferHelper->getBuffer().getHandle();
                mCurrentArrayBufferOffsets[attribIndex] = conversion->lastAllocationOffset;

                // Converted buffer is tightly packed
                stride = vertexFormat.bufferFormat().pixelBytes;
            }
            else
            {
                if (bufferVk->getSize() == 0)
                {
                    mCurrentArrayBuffers[attribIndex] = &mTheNullBuffer;
                    mCurrentArrayBufferHandles[attribIndex] =
                        mTheNullBuffer.getBuffer().getHandle();
                    mCurrentArrayBufferOffsets[attribIndex] = 0;
                    stride                                  = 0;
                }
                else
                {
                    vk::BufferHelper &bufferHelper = bufferVk->getBuffer();
                    mCurrentArrayBuffers[attribIndex]       = &bufferHelper;
                    mCurrentArrayBufferHandles[attribIndex] = bufferHelper.getBuffer().getHandle();
                    mCurrentArrayBufferOffsets[attribIndex] = binding.getOffset();
                    stride                                  = binding.getStride();
                }
            }
        }
        else
        {
            mCurrentArrayBuffers[attribIndex]       = &mTheNullBuffer;
            mCurrentArrayBufferHandles[attribIndex] = mTheNullBuffer.getBuffer().getHandle();
            mCurrentArrayBufferOffsets[attribIndex] = 0;
            // Client side buffer will be transfered to a tightly packed buffer later
            stride = vertexFormat.bufferFormat().pixelBytes;
        }

        if (bufferOnly)
        {
            contextVk->invalidateVertexBuffers();
        }
        else
        {
            contextVk->onVertexAttributeChange(attribIndex, stride, binding.getDivisor(),
                                               attrib.format->id, attrib.relativeOffset);
        }

        if (anyVertexBufferConvertedOnGpu &&
            renderer->getFeatures().flushAfterVertexConversion.enabled)
        {
            ANGLE_TRY(contextVk->flushImpl(nullptr));
        }
    }
    else
    {
        contextVk->invalidateDefaultAttribute(attribIndex);

        // These will be filled out by the ContextVk.
        mCurrentArrayBuffers[attribIndex]       = &mTheNullBuffer;
        mCurrentArrayBufferHandles[attribIndex] = mTheNullBuffer.getBuffer().getHandle();
        mCurrentArrayBufferOffsets[attribIndex] = 0;

        setDefaultPackedInput(contextVk, attribIndex);
    }

    return angle::Result::Continue;
}

angle::Result VertexArrayVk::updateClientAttribs(const gl::Context *context,
                                                 GLint firstVertex,
                                                 GLsizei vertexOrIndexCount,
                                                 GLsizei instanceCount,
                                                 gl::DrawElementsType indexTypeOrInvalid,
                                                 const void *indices)
{
    ContextVk *contextVk                    = vk::GetImpl(context);
    const gl::AttributesMask &clientAttribs = context->getStateCache().getActiveClientAttribsMask();

    ASSERT(clientAttribs.any());

    GLint startVertex;
    size_t vertexCount;
    ANGLE_TRY(GetVertexRangeInfo(context, firstVertex, vertexOrIndexCount, indexTypeOrInvalid,
                                 indices, 0, &startVertex, &vertexCount));

    RendererVk *renderer = contextVk->getRenderer();
    mDynamicVertexData.releaseInFlightBuffers(contextVk);

    const auto &attribs  = mState.getVertexAttributes();
    const auto &bindings = mState.getVertexBindings();

    // TODO(fjhenigman): When we have a bunch of interleaved attributes, they end up
    // un-interleaved, wasting space and copying time.  Consider improving on that.
    for (size_t attribIndex : clientAttribs)
    {
        const gl::VertexAttribute &attrib = attribs[attribIndex];
        const gl::VertexBinding &binding  = bindings[attrib.bindingIndex];
        ASSERT(attrib.enabled && binding.getBuffer().get() == nullptr);

        const vk::Format &vertexFormat = renderer->getFormat(attrib.format->id);
        GLuint stride                  = vertexFormat.bufferFormat().pixelBytes;

        ASSERT(GetVertexInputAlignment(vertexFormat) <= vk::kVertexBufferAlignment);

        const uint8_t *src = static_cast<const uint8_t *>(attrib.pointer);
        if (binding.getDivisor() > 0)
        {
            // instanced attrib
            size_t count           = UnsignedCeilDivide(instanceCount, binding.getDivisor());
            size_t bytesToAllocate = count * stride;

            ANGLE_TRY(StreamVertexData(contextVk, &mDynamicVertexData, src, bytesToAllocate, 0,
                                       count, binding.getStride(), vertexFormat.vertexLoadFunction,
                                       &mCurrentArrayBuffers[attribIndex],
                                       &mCurrentArrayBufferOffsets[attribIndex]));
        }
        else
        {
            // Allocate space for startVertex + vertexCount so indexing will work.  If we don't
            // start at zero all the indices will be off.
            // Only vertexCount vertices will be used by the upcoming draw so that is all we copy.
            size_t bytesToAllocate = (startVertex + vertexCount) * stride;
            src += startVertex * binding.getStride();
            size_t destOffset = startVertex * stride;

            ANGLE_TRY(StreamVertexData(
                contextVk, &mDynamicVertexData, src, bytesToAllocate, destOffset, vertexCount,
                binding.getStride(), vertexFormat.vertexLoadFunction,
                &mCurrentArrayBuffers[attribIndex], &mCurrentArrayBufferOffsets[attribIndex]));
        }

        mCurrentArrayBufferHandles[attribIndex] =
            mCurrentArrayBuffers[attribIndex]->getBuffer().getHandle();
    }

    return angle::Result::Continue;
}

angle::Result VertexArrayVk::handleLineLoop(ContextVk *contextVk,
                                            GLint firstVertex,
                                            GLsizei vertexOrIndexCount,
                                            gl::DrawElementsType indexTypeOrInvalid,
                                            const void *indices,
                                            uint32_t *indexCountOut)
{
    if (indexTypeOrInvalid != gl::DrawElementsType::InvalidEnum)
    {
        // Handle GL_LINE_LOOP drawElements.
        if (mDirtyLineLoopTranslation)
        {
            gl::Buffer *elementArrayBuffer = mState.getElementArrayBuffer();

            if (!elementArrayBuffer)
            {
                ANGLE_TRY(mLineLoopHelper.streamIndices(
                    contextVk, indexTypeOrInvalid, vertexOrIndexCount,
                    reinterpret_cast<const uint8_t *>(indices), &mCurrentElementArrayBuffer,
                    &mCurrentElementArrayBufferOffset, indexCountOut));
            }
            else
            {
                // When using an element array buffer, 'indices' is an offset to the first element.
                intptr_t offset                = reinterpret_cast<intptr_t>(indices);
                BufferVk *elementArrayBufferVk = vk::GetImpl(elementArrayBuffer);
                ANGLE_TRY(mLineLoopHelper.getIndexBufferForElementArrayBuffer(
                    contextVk, elementArrayBufferVk, indexTypeOrInvalid, vertexOrIndexCount, offset,
                    &mCurrentElementArrayBuffer, &mCurrentElementArrayBufferOffset, indexCountOut));
            }
        }

        // If we've had a drawArrays call with a line loop before, we want to make sure this is
        // invalidated the next time drawArrays is called since we use the same index buffer for
        // both calls.
        mLineLoopBufferFirstIndex.reset();
        mLineLoopBufferLastIndex.reset();
        return angle::Result::Continue;
    }

    // Note: Vertex indexes can be arbitrarily large.
    uint32_t clampedVertexCount = gl::clampCast<uint32_t>(vertexOrIndexCount);

    // Handle GL_LINE_LOOP drawArrays.
    size_t lastVertex = static_cast<size_t>(firstVertex + clampedVertexCount);
    if (!mLineLoopBufferFirstIndex.valid() || !mLineLoopBufferLastIndex.valid() ||
        mLineLoopBufferFirstIndex != firstVertex || mLineLoopBufferLastIndex != lastVertex)
    {
        ANGLE_TRY(mLineLoopHelper.getIndexBufferForDrawArrays(
            contextVk, clampedVertexCount, firstVertex, &mCurrentElementArrayBuffer,
            &mCurrentElementArrayBufferOffset));

        mLineLoopBufferFirstIndex = firstVertex;
        mLineLoopBufferLastIndex  = lastVertex;
    }
    *indexCountOut = vertexOrIndexCount + 1;

    return angle::Result::Continue;
}

void VertexArrayVk::updateDefaultAttrib(ContextVk *contextVk,
                                        size_t attribIndex,
                                        VkBuffer bufferHandle,
                                        uint32_t offset)
{
    if (!mState.getEnabledAttributesMask().test(attribIndex))
    {
        mCurrentArrayBufferHandles[attribIndex] = bufferHandle;
        mCurrentArrayBufferOffsets[attribIndex] = offset;
        mCurrentArrayBuffers[attribIndex]       = nullptr;

        setDefaultPackedInput(contextVk, attribIndex);
    }
}
}  // namespace rx
