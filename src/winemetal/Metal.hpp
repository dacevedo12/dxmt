#pragma once

#include "./winemetal.h"
#include <cstddef>
#include <string>
#include <cstring>
#include <vector>

namespace WMT {

class String;

struct OperatingSystemVersion {
  uint64_t major;
  uint64_t minor;
  uint64_t patch;
};

class Object {
public:
  obj_handle_t handle;

  Object() {
    handle = NULL_OBJECT_HANDLE;
  }

  Object(obj_handle_t h) {
    handle = h;
  }

  void
  retain() {
    NSObject_retain(handle);
  }

  void
  release() {
    NSObject_release(handle);
  }

  String description();

  bool
  operator==(std::nullptr_t) const {
    return handle == 0;
  }
  bool
  operator!=(std::nullptr_t) const {
    return handle != 0;
  }
  bool
  operator==(Object obj) const {
    return handle == obj.handle;
  }
  bool
  operator!=(Object obj) const {
    return handle != obj.handle;
  }

  operator bool() const {
    return handle != 0;
  }

  operator obj_handle_t() const {
    return handle;
  }
};

template <typename Class> class Reference : public Class {
public:
  Reference() {
    this->handle = NULL_OBJECT_HANDLE;
  }
  Reference(const Reference &copy) {
    this->handle = copy.handle;
    this->retain();
  }
  Reference(Reference &&move) {
    this->handle = move.handle;
    move.handle = NULL_OBJECT_HANDLE;
  }
  Reference(Class non_retained) {
    this->handle = non_retained.handle;
    this->retain();
  }
  Reference(obj_handle_t retained) {
    this->handle = retained;
  }

  Reference &
  operator=(const Reference &copy) {
    if (this->handle != NULL_OBJECT_HANDLE)
      this->release();
    this->handle = copy.handle;
    this->retain();
    return *this;
  }
  Reference &
  operator=(Class non_retained) {
    if (this->handle != NULL_OBJECT_HANDLE)
      this->release();
    this->handle = non_retained.handle;
    this->retain();
    return *this;
  }
  Reference &
  operator=(Reference &&move) {
    if (this->handle != NULL_OBJECT_HANDLE)
      this->release();
    this->handle = move.handle;
    move.handle = NULL_OBJECT_HANDLE;
    return *this;
  }
  Reference &
  operator=(std::nullptr_t) {
    if (this->handle != NULL_OBJECT_HANDLE)
      this->release();
    this->handle = NULL_OBJECT_HANDLE;
    return *this;
  }

  ~Reference() {
    if (this->handle == NULL_OBJECT_HANDLE)
      return;
    this->release();
    this->handle = NULL_OBJECT_HANDLE;
  }
};

template <typename Class> class Array : public Object {
public:
  uint64_t
  count() const {
    return NSArray_count(handle);
  }

  Class
  object(uint64_t index) const {
    return Class{NSArray_object(handle, index)};
  }
};

class String : public Object {
public:
  static String
  string(const char *data, WMTStringEncoding encoding) {
    return String{NSString_string(data, encoding)};
  }

  bool
  getCString(char *buffer, uint64_t maxLength, WMTStringEncoding encoding) {
    return NSString_getCString(handle, buffer, maxLength, encoding);
  }

  uint64_t
  lengthOfBytesUsingEncoding(WMTStringEncoding encoding) {
    return NSString_lengthOfBytesUsingEncoding(handle, encoding);
  }

  std::string
  getUTF8String() {
    auto len = lengthOfBytesUsingEncoding(WMTUTF8StringEncoding);
    std::vector<char> str(len + 1);
    getCString(str.data(), len + 1, WMTUTF8StringEncoding);
    return std::string(str.data());
  }
};

inline String
Object::description() {
  return String{NSObject_description(handle)};
}

class Error : public Object {
public:
};

class DispatchData : public Object {
public:
};

class Event : public Object {
public:
};

class SharedEvent : public Event {
public:
  uint64_t
  signaledValue() {
    return MTLSharedEvent_signaledValue(handle);
  }

  void
  signalValue(uint64_t value) {
    MTLSharedEvent_signalValue(handle, value);
  }

  mach_port_t
  createMachPort() {
    return MTLSharedEvent_createMachPort(handle);
  }

  bool
  waitUntilSignaledValue(uint64_t value, uint64_t timeout) {
    return MTLSharedEvent_waitUntilSignaledValue(handle, value, timeout);
  }
};

class Fence : public Object {
public:
};

class CounterSampleBuffer : public Object {
public:
  void
  resolveCounterRange(uint32_t start, uint32_t len, void *data_out, uint64_t data_length) {
    MTLCounterSampleBuffer_resolveCounterRange(handle, start, len, data_out, data_length);
  }
};

class Resource : public Object {
public:
};

class Heap : public Object {
public:
};

class Texture : public Resource {
public:
  Reference<Texture>
  newTextureView(
      WMTPixelFormat format, WMTTextureType texture_type, uint16_t level_start, uint16_t level_count,
      uint16_t slice_start, uint16_t slice_count, struct WMTTextureSwizzleChannels swizzle,
      uint64_t &out_gpu_resource_id
  ) {
    return Reference<Texture>(MTLTexture_newTextureView(
        handle, format, texture_type, level_start, level_count, slice_start, slice_count, swizzle, &out_gpu_resource_id
    ));
  }

  WMTPixelFormat
  pixelFormat() {
    return MTLTexture_pixelFormat(handle);
  }

  WMTTextureUsage
  usage() {
    return MTLTexture_usage(handle);
  }

  uint64_t
  width() {
    return MTLTexture_width(handle);
  }

  uint64_t
  height() {
    return MTLTexture_height(handle);
  }

  uint64_t
  depth() {
    return MTLTexture_depth(handle);
  }

  uint64_t
  arrayLength() {
    return MTLTexture_arrayLength(handle);
  }

  uint64_t
  mipmapLevelCount() {
    return MTLTexture_mipmapLevelCount(handle);
  }

  void
  replaceRegion(
      WMTOrigin origin, WMTSize size, uint64_t level, uint64_t slice, const void *pixelBytes, uint64_t bytesPerRow,
      uint64_t bytesPerImage
  ) {
    WMTMemoryPointer data;
    data.set((void *)pixelBytes);
    return MTLTexture_replaceRegion(handle, origin, size, level, slice, data, bytesPerRow, bytesPerImage);
  }
};

class Buffer : public Resource {
public:
  Reference<Texture>
  newTexture(WMTTextureInfo &info, uint64_t offset, uint64_t bytes_per_row) {
    return Reference<Texture>(MTLBuffer_newTexture(handle, &info, offset, bytes_per_row));
  }

  void
  didModifyRange(uint64_t start, uint64_t length) {
    MTLBuffer_didModifyRange(handle, start, length);
  }

  void
  updateContents(uint64_t offset, const void *data, uint64_t length) {
    WMTConstMemoryPointer ptr;
    ptr.set(data);
    MTLBuffer_updateContents(handle, offset, ptr, length);
  }
};

class SamplerState : public Object {
public:
};

class DepthStencilState : public Object {
public:
};

class CommandEncoder : public Object {
public:
  void
  endEncoding() {
    MTLCommandEncoder_endEncoding(handle);
  }

  void
  setLabel(String label) {
    MTLCommandEncoder_setLabel(handle, label);
  }
};

class ComputePipelineState : public Object {
public:
};

class RenderPipelineState : public Object {
public:
};

class RenderCommandEncoder : public CommandEncoder {
public:
  void
  encodeCommands(const wmtcmd_render_nop *cmd_head) {
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)cmd_head);
  }

  void
  setVertexBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    struct wmtcmd_render_setbuffer cmd;
    cmd.type = WMTRenderCommandSetVertexBuffer;
    cmd.next.set(nullptr);
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setFragmentBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    struct wmtcmd_render_setbuffer cmd;
    cmd.type = WMTRenderCommandSetFragmentBuffer;
    cmd.next.set(nullptr);
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setMeshBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    struct wmtcmd_render_setbuffer cmd;
    cmd.type = WMTRenderCommandSetMeshBuffer;
    cmd.next.set(nullptr);
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setObjectBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    struct wmtcmd_render_setbuffer cmd;
    cmd.type = WMTRenderCommandSetObjectBuffer;
    cmd.next.set(nullptr);
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  memoryBarrier(WMTBarrierScope scope, WMTRenderStages stages_after, WMTRenderStages stages_before) {
    struct wmtcmd_render_memory_barrier cmd;
    cmd.type = WMTRenderCommandMemoryBarrier;
    cmd.next.set(nullptr);
    cmd.scope = scope;
    cmd.stages_before = stages_before;
    cmd.stages_after = stages_after;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  useResource(Resource resource, WMTResourceUsage usage, WMTRenderStages stages) {
    struct wmtcmd_render_useresource cmd;
    cmd.type = WMTRenderCommandUseResource;
    cmd.next.set(nullptr);
    cmd.resource = resource;
    cmd.usage = usage;
    cmd.stages = stages;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  };

  void
  setRenderPipelineState(RenderPipelineState pso) {
    struct wmtcmd_render_setpso cmd;
    cmd.type = WMTRenderCommandSetPSO;
    cmd.next.set(nullptr);
    cmd.pso = pso;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setDepthStencilState(DepthStencilState dsso, uint8_t stencil_ref = 0) {
    struct wmtcmd_render_setdsso cmd;
    cmd.type = WMTRenderCommandSetDSSO;
    cmd.next.set(nullptr);
    cmd.dsso = dsso;
    cmd.stencil_ref = stencil_ref;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setBlendColor(float red, float green, float blue, float alpha) {
    struct wmtcmd_render_setblendcolor_only cmd;
    cmd.type = WMTRenderCommandSetBlendColor;
    cmd.next.set(nullptr);
    cmd.red = red;
    cmd.green = green;
    cmd.blue = blue;
    cmd.alpha = alpha;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  drawPrimitives(
      WMTPrimitiveType primitive_type, uint32_t vertex_start, uint32_t vertex_count, uint32_t instance_count = 1
  ) {
    struct wmtcmd_render_draw cmd;
    cmd.type = WMTRenderCommandDraw;
    cmd.next.set(nullptr);
    cmd.primitive_type = primitive_type;
    cmd.vertex_start = vertex_start;
    cmd.vertex_count = vertex_count;
    cmd.base_instance = 0;
    cmd.instance_count = instance_count;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  drawIndexedPrimitives(
      WMTPrimitiveType primitive_type, uint64_t index_count, WMTIndexType index_type, Buffer index_buffer,
      uint64_t index_buffer_offset, int32_t base_vertex, uint32_t instance_count = 1
  ) {
    struct wmtcmd_render_draw_indexed cmd;
    cmd.type = WMTRenderCommandDrawIndexed;
    cmd.next.set(nullptr);
    cmd.primitive_type = primitive_type;
    cmd.index_type = index_type;
    cmd.index_count = index_count;
    cmd.index_buffer = index_buffer.handle;
    cmd.index_buffer_offset = index_buffer_offset;
    cmd.instance_count = instance_count;
    cmd.base_vertex = base_vertex;
    cmd.base_instance = 0;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setFragmentTexture(Texture texture, uint8_t index) {
    struct wmtcmd_render_settexture cmd;
    cmd.type = WMTRenderCommandSetFragmentTexture;
    cmd.next.set(nullptr);
    cmd.texture = texture;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  };

  void
  setFragmentSamplerState(SamplerState sampler, uint8_t index) {
    struct wmtcmd_render_setsamplerstate cmd;
    cmd.type = WMTRenderCommandSetFragmentSamplerState;
    cmd.next.set(nullptr);
    cmd.sampler = sampler;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setVertexTexture(Texture texture, uint8_t index) {
    struct wmtcmd_render_settexture cmd;
    cmd.type = WMTRenderCommandSetVertexTexture;
    cmd.next.set(nullptr);
    cmd.texture = texture;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setVertexSamplerState(SamplerState sampler, uint8_t index) {
    struct wmtcmd_render_setsamplerstate cmd;
    cmd.type = WMTRenderCommandSetVertexSamplerState;
    cmd.next.set(nullptr);
    cmd.sampler = sampler;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setFragmentBytes(const void *buf, uint64_t length, uint8_t index) {
    struct wmtcmd_render_setbytes cmd;
    cmd.type = WMTRenderCommandSetFragmentBytes;
    cmd.next.set(nullptr);
    cmd.bytes.set((void *)buf);
    cmd.length = length;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setRasterizerState(
      WMTTriangleFillMode fill_mode, WMTCullMode cull_mode, WMTWinding winding,
      WMTDepthClipMode depth_clip_mode = WMTDepthClipModeClip, float depth_bias = 0.0f, float slope_scale = 0.0f,
      float depth_bias_clamp = 0.0f
  ) {
    struct wmtcmd_render_setrasterizerstate cmd;
    cmd.type = WMTRenderCommandSetRasterizerState;
    cmd.next.set(nullptr);
    cmd.fill_mode = fill_mode;
    cmd.cull_mode = cull_mode;
    cmd.depth_clip_mode = depth_clip_mode;
    cmd.winding = winding;
    cmd.depth_bias = depth_bias;
    cmd.scole_scale = slope_scale; // sic: typo in winemetal.h struct
    cmd.depth_bias_clamp = depth_bias_clamp;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setViewport(WMTViewport viewport) {
    struct wmtcmd_render_setviewports cmd;
    cmd.type = WMTRenderCommandSetViewports;
    cmd.next.set(nullptr);
    cmd.viewports.set(&viewport);
    cmd.viewport_count = 1;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  };

  void
  setScissorRect(WMTScissorRect rect) {
    struct wmtcmd_render_setscissorrect cmd;
    cmd.type = WMTRenderCommandSetScissorRect;
    cmd.next.set(nullptr);
    cmd.scissor_rect = rect;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  waitForFence(Fence fence, WMTRenderStages before) {
    struct wmtcmd_render_fence_op cmd;
    cmd.type = WMTRenderCommandWaitForFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    cmd.stages = before;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  updateFence(Fence fence, WMTRenderStages after) {
    struct wmtcmd_render_fence_op cmd;
    cmd.type = WMTRenderCommandUpdateFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    cmd.stages = after;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }
};

class BlitCommandEncoder : public CommandEncoder {
public:
  void
  encodeCommands(const wmtcmd_blit_nop *cmd_head) {
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)cmd_head);
  }

  void
  waitForFence(Fence fence) {
    struct wmtcmd_blit_fence_op cmd;
    cmd.type = WMTBlitCommandWaitForFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  updateFence(Fence fence) {
    struct wmtcmd_blit_fence_op cmd;
    cmd.type = WMTBlitCommandUpdateFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  resolveCounters(CounterSampleBuffer sample_buffer, uint32_t start, uint32_t len, Buffer dst, uint64_t dst_offset) {
    struct wmtcmd_blit_resolvecounters cmd;
    cmd.type = WMTBlitCommandResolveCounters;
    cmd.next.set(nullptr);
    cmd.sample_buffer = sample_buffer.handle;
    cmd.start = start;
    cmd.len = len;
    cmd.dst_buffer = dst.handle;
    cmd.dst_offset = dst_offset;
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  generateMipmaps(Texture texture) {
    struct wmtcmd_blit_generate_mipmaps cmd;
    cmd.type = WMTBlitCommandGenerateMipmaps;
    cmd.next.set(nullptr);
    cmd.texture = texture.handle;
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  copyFromBufferToTexture(
      Buffer src, uint64_t src_offset, uint32_t bytes_per_row, uint32_t bytes_per_image, WMTSize size, Texture dst,
      uint32_t dst_slice, uint32_t dst_level, WMTOrigin dst_origin
  ) {
    struct wmtcmd_blit_copy_from_buffer_to_texture cmd;
    cmd.type = WMTBlitCommandCopyFromBufferToTexture;
    cmd.next.set(nullptr);
    cmd.src = src.handle;
    cmd.src_offset = src_offset;
    cmd.bytes_per_row = bytes_per_row;
    cmd.bytes_per_image = bytes_per_image;
    cmd.size = size;
    cmd.dst = dst.handle;
    cmd.slice = dst_slice;
    cmd.level = dst_level;
    cmd.origin = dst_origin;
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }
};

class ComputeCommandEncoder : public CommandEncoder {
public:
  void
  encodeCommands(const wmtcmd_compute_nop *cmd_head) {
    MTLComputeCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)cmd_head);
  }

  void
  waitForFence(Fence fence) {
    struct wmtcmd_compute_fence_op cmd;
    cmd.type = WMTComputeCommandWaitForFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    MTLComputeCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  updateFence(Fence fence) {
    struct wmtcmd_compute_fence_op cmd;
    cmd.type = WMTComputeCommandUpdateFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    MTLComputeCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }
};

class MetalDrawable : public Object {
public:
  Texture
  texture() {
    return Texture{MetalDrawable_texture(handle)};
  };
};

class MetalLayer : public Object {
public:
  MetalDrawable
  nextDrawable() {
    return MetalDrawable{MetalLayer_nextDrawable(handle)};
  };

  void
  setProps(const WMTLayerProps &props) {
    MetalLayer_setProps(handle, &props);
  };

  void
  getProps(WMTLayerProps &props) {
    MetalLayer_getProps(handle, &props);
  };

  bool
  setColorSpace(WMTColorSpace colorspace) {
    return MetalLayer_setColorSpace(handle, colorspace);
  };
};

class FXTemporalScaler : public Object {
public:
};

class FXSpatialScaler : public Object {
public:
};

class LogContainer : public Object {
public:
  std::vector<Object>
  elements() {
    std::vector<Object> ret;
    uint64_t read = 0;
    uint64_t start;
    do {
      start = ret.size();
      ret.resize(start + 4);
      read = MTLLogContainer_enumerate(handle, start, 4, (obj_handle_t *)ret.data() + start);
    } while (read == 4);
    ret.resize(start + read);
    return ret;
  };
};

class CommandBuffer : public Object {
public:
  void
  commit() {
    return MTLCommandBuffer_commit(handle);
  }

  void
  waitUntilCompleted() {
    return MTLCommandBuffer_waitUntilCompleted(handle);
  }

  WMTCommandBufferStatus
  status() {
    return MTLCommandBuffer_status(handle);
  }

  Error
  error() {
    return Error{MTLCommandBuffer_error(handle)};
  }

  LogContainer
  logs() {
    return LogContainer{MTLCommandBuffer_logs(handle)};
  }

  uint64_t
  gpuEndTime() {
    return MTLCommandBuffer_property(handle, WMTCommandBufferPropertyGPUEndTime);
  }

  void
  encodeSignalEvent(Event event, uint64_t value) {
    return MTLCommandBuffer_encodeSignalEvent(handle, event.handle, value);
  }

  void
  encodeWaitForEvent(Event event, uint64_t value) {
    return MTLCommandBuffer_encodeWaitForEvent(handle, event.handle, value);
  }

  RenderCommandEncoder
  renderCommandEncoder(WMTRenderPassInfo &info) {
    return RenderCommandEncoder{MTLCommandBuffer_renderCommandEncoder(handle, &info)};
  }

  BlitCommandEncoder
  blitCommandEncoder() {
    return BlitCommandEncoder{MTLCommandBuffer_blitCommandEncoder(handle)};
  }

  BlitCommandEncoder
  blitCommandEncoderWithSampleBuffers(WMTSampleBufferAttachmentInfo *attachments, uint64_t num_attachments) {
    return BlitCommandEncoder{
        MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(handle, attachments, num_attachments)
    };
  }

  ComputeCommandEncoder
  computeCommandEncoder(bool concurrent) {
    return ComputeCommandEncoder{MTLCommandBuffer_computeCommandEncoder(handle, concurrent)};
  }

  void
  presentDrawable(MetalDrawable drawable) {
    MTLCommandBuffer_presentDrawable(handle, drawable);
  }

  void
  presentDrawableAfterMinimumDuration(MetalDrawable drawable, double after) {
    MTLCommandBuffer_presentDrawableAfterMinimumDuration(handle, drawable, after);
  }

  void
  encodeSpatialScale(FXSpatialScaler scaler, Texture color, Texture output, Fence fence) {
    MTLCommandBuffer_encodeSpatialScale(handle, scaler, color, output, fence);
  }

  void
  encodeTemporalScale(
      FXTemporalScaler scaler, Texture color, Texture output, Texture depth, Texture motion, Texture exposure,
      Fence fence, const WMTFXTemporalScalerProps &props
  ) {
    MTLCommandBuffer_encodeTemporalScale(handle, scaler, color, output, depth, motion, exposure, fence, &props);
  }
};

class CommandQueue : public Object {
public:
  CommandBuffer
  commandBuffer() {
    return CommandBuffer{MTLCommandQueue_commandBuffer(handle)};
  }
};

class Function : public Object {};

class Library : public Object {
public:
  Reference<Function>
  newFunction(const char *name) {
    return Reference<Function>(MTLLibrary_newFunction(handle, name));
  }

  Reference<Function>
  newFunctionWithConstants(
      const char *name, const WMTFunctionConstant *constants, uint32_t num_constants, Error &error
  ) {
    return Reference<Function>(
        MTLLibrary_newFunctionWithConstants(handle, name, constants, num_constants, &error.handle)
    );
  }
};

class BinaryArchive : public Object {
public:
  void
  serialize(const char *url, Error &error) {
    MTLBinaryArchive_serialize(handle, url, &error.handle);
  }
};

class Device : public Object {
public:
  uint64_t
  recommendedMaxWorkingSetSize() const {
    return MTLDevice_recommendedMaxWorkingSetSize(handle);
  }

  uint64_t
  currentAllocatedSize() const {
    return MTLDevice_currentAllocatedSize(handle);
  }

  String
  name() const {
    return String{MTLDevice_name(handle)};
  }

  uint64_t
  registryID() const {
    return MTLDevice_registryID(handle);
  }

  Reference<CommandQueue>
  newCommandQueue(uint64_t maxCommandBufferCount) {
    return Reference<CommandQueue>(MTLDevice_newCommandQueue(handle, maxCommandBufferCount));
  }

  Reference<SharedEvent>
  newSharedEvent() {
    return Reference<SharedEvent>(MTLDevice_newSharedEvent(handle));
  }

  Reference<SharedEvent>
  newSharedEventWithMachPort(mach_port_t mach_port) {
    return Reference<SharedEvent>(MTLDevice_newSharedEventWithMachPort(handle, mach_port));
  }

  Reference<Buffer>
  newBuffer(WMTBufferInfo &info) {
    return Reference<Buffer>(MTLDevice_newBuffer(handle, &info));
  }

  Reference<SamplerState>
  newSamplerState(WMTSamplerInfo &info) {
    return Reference<SamplerState>(MTLDevice_newSamplerState(handle, &info));
  }

  Reference<DepthStencilState>
  newDepthStencilState(const WMTDepthStencilInfo &info) {
    return Reference<DepthStencilState>(MTLDevice_newDepthStencilState(handle, &info));
  }

  Reference<Texture>
  newTexture(WMTTextureInfo &info) {
    return Reference<Texture>(MTLDevice_newTexture(handle, &info));
  }

  bool
  supportsPlacementSparse() const {
    return MTLDevice_supportsPlacementSparse(handle);
  }

  bool
  sparseTileSize(const WMTTextureInfo &info, WMTSparseTileSize &tile_size) const {
    return MTLDevice_sparseTileSize(handle, &info, &tile_size);
  }

  Reference<Heap>
  newPlacementHeap(const WMTPlacementHeapInfo &info) {
    return Reference<Heap>(MTLDevice_newPlacementHeap(handle, &info));
  }

  bool
  updateSparseTextureMappings(Texture texture, Heap heap,
                              const WMTSparseTextureMappingOperation *operations,
                              uint64_t operation_count) const {
    return MTLDevice_updateSparseTextureMappings(handle, texture.handle,
                                                 heap.handle, operations,
                                                 operation_count);
  }

  Reference<Library>
  newLibrary(const void *bytecode, uint64_t bytecode_length, Error &error) {
    auto data = DispatchData_alloc_init((uint64_t)bytecode, bytecode_length);
    auto ret = Reference<Library>(MTLDevice_newLibrary(handle, data, &error.handle));
    NSObject_release(data);
    return ret;
  }

  Reference<Library>
  newLibraryFromNativeBuffer(uint64_t bytecode, uint64_t bytecode_length, Error &error) {
    auto data = DispatchData_alloc_init(bytecode, bytecode_length);
    auto ret = Reference<Library>(MTLDevice_newLibrary(handle, data, &error.handle));
    NSObject_release(data);
    return ret;
  }

  Reference<Library>
  newLibrary(DispatchData data, Error &error) {
    return Reference<Library>(MTLDevice_newLibrary(handle, data, &error.handle));
  }

  Reference<ComputePipelineState>
  newComputePipelineState(const Function &compute_function, Error &error) {
    WMTComputePipelineInfo info;
    info.compute_function = compute_function;
    info.tgsize_is_multiple_of_sgwidth = false;
    info.immutable_buffers = 0;
    info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
    info.binary_archives_for_lookup.set(nullptr);
    info.num_binary_archives_for_lookup = 0;
    info.fail_on_binary_archive_miss = false;
    return Reference<ComputePipelineState>(MTLDevice_newComputePipelineState(handle, &info, &error.handle));
  }

  Reference<ComputePipelineState>
  newComputePipelineState(const Function &compute_function, bool tgsize_is_multiple_of_sgwidth, Error &error) {
    WMTComputePipelineInfo info;
    info.compute_function = compute_function;
    info.tgsize_is_multiple_of_sgwidth = tgsize_is_multiple_of_sgwidth;
    info.immutable_buffers = 0;
    info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
    info.binary_archives_for_lookup.set(nullptr);
    info.num_binary_archives_for_lookup = 0;
    info.fail_on_binary_archive_miss = false;
    return Reference<ComputePipelineState>(MTLDevice_newComputePipelineState(handle, &info, &error.handle));
  }

  Reference<RenderPipelineState>
  newRenderPipelineState(const WMTRenderPipelineInfo &info, Error &error) {
    return Reference<RenderPipelineState>(MTLDevice_newRenderPipelineState(handle, &info, &error.handle));
  }

  Reference<ComputePipelineState>
  newComputePipelineState(const WMTComputePipelineInfo &info, Error &error) {
    return Reference<ComputePipelineState>(MTLDevice_newComputePipelineState(handle, &info, &error.handle));
  }

  Reference<RenderPipelineState>
  newRenderPipelineState(const WMTMeshRenderPipelineInfo &info, Error &error) {
    return Reference<RenderPipelineState>(MTLDevice_newMeshRenderPipelineState(handle, &info, &error.handle));
  }

  Reference<RenderPipelineState>
  newRenderPipelineState(const WMTTileRenderPipelineInfo &info, Error &error) {
    return Reference<RenderPipelineState>(MTLDevice_newTileRenderPipelineState(handle, &info, &error.handle));
  }

  Reference<Fence>
  newFence() {
    return Reference<Fence>(MTLDevice_newFence(handle));
  }

  Reference<Event>
  newEvent() {
    return Reference<Event>(MTLDevice_newEvent(handle));
  }

  uint64_t
  minimumLinearTextureAlignmentForPixelFormat(WMTPixelFormat format) {
    return MTLDevice_minimumLinearTextureAlignmentForPixelFormat(handle, format);
  }

  bool
  supportsFamily(WMTGPUFamily gpu_family) {
    return MTLDevice_supportsFamily(handle, gpu_family);
  }

  bool
  supportsBCTextureCompression() {
    return MTLDevice_supportsBCTextureCompression(handle);
  }

  bool
  supportsTextureSampleCount(uint8_t sample_count) {
    return MTLDevice_supportsTextureSampleCount(handle, sample_count);
  }

  bool
  hasUnifiedMemory() {
    return MTLDevice_hasUnifiedMemory(handle);
  }

  Reference<FXTemporalScaler>
  newTemporalScaler(const WMTFXTemporalScalerInfo &info) {
    return Reference<FXTemporalScaler>(MTLDevice_newTemporalScaler(handle, &info));
  }

  Reference<FXSpatialScaler>
  newSpatialScaler(const WMTFXSpatialScalerInfo &info) {
    return Reference<FXSpatialScaler>(MTLDevice_newSpatialScaler(handle, &info));
  }

  Reference<BinaryArchive>
  newBinaryArchive(const char *url, Error &error) {
    return Reference<BinaryArchive>(MTLDevice_newBinaryArchive(handle, url, &error.handle));
  }

  bool
  supportsFXSpatialScaler() {
    return MTLDevice_supportsFXSpatialScaler(handle);
  }

  bool
  supportsFXTemporalScaler() {
    return MTLDevice_supportsFXTemporalScaler(handle);
  }

  void
  setShouldMaximizeConcurrentCompilation(bool value) {
    MTLDevice_setShouldMaximizeConcurrentCompilation(handle, value);
  }

  Reference<Texture>
  newSharedTexture(WMTTextureInfo &info) {
    return Reference<Texture>(MTLDevice_newSharedTexture(handle, &info));
  }

  Reference<CounterSampleBuffer>
  newCounterSampleBuffer(uint32_t sample_count, bool shared = true) {
    return Reference<CounterSampleBuffer>(MTLCounterSampleBuffer_newTimestampBuffer(handle, sample_count, shared));
  }
};

inline Reference<Array<Device>>
CopyAllDevices() {
  return Reference<Array<Device>>(WMTCopyAllDevices());
}

inline OperatingSystemVersion
GetOperatingSystemVersion() {
  OperatingSystemVersion version = {};
  WMTGetOSVersion(&version.major, &version.minor, &version.patch);
  return version;
}

class CaptureManager : public Object {
public:
  static CaptureManager
  sharedCaptureManager() {
    return CaptureManager{MTLCaptureManager_sharedCaptureManager()};
  };

  bool
  startCapture(WMTCaptureInfo &info) {
    return MTLCaptureManager_startCapture(handle, &info);
  };

  void
  stopCapture() {
    MTLCaptureManager_stopCapture(handle);
  };
};

class DeveloperHUDProperties : public Object {
public:
  static DeveloperHUDProperties
  instance() {
    return DeveloperHUDProperties{DeveloperHUDProperties_instance()};
  };

  bool
  addLabel(String label, String after) {
    return DeveloperHUDProperties_addLabel(handle, label, after);
  }

  void
  updateLabel(String label, String value) {
    DeveloperHUDProperties_updateLabel(handle, label, value);
  }

  void
  remove(String label) {
    DeveloperHUDProperties_remove(handle, label);
  }
};

class CacheReader : public Object {
public:
  static Reference<CacheReader>
  alloc_init(const char *path, uint64_t version) {
    return Reference<CacheReader>(CacheReader_alloc_init(path, version));
  };

  Reference<DispatchData>
  get(const void *key, uint64_t length) {
    return Reference<DispatchData>(CacheReader_get(handle, key, length));
  };

  template <typename K>
  Reference<DispatchData>
  get(const K &key) {
    return Reference<DispatchData>(CacheReader_get(handle, reinterpret_cast<const void *>(&key), sizeof(key)));
  };
};

class CacheWriter : public Object {
public:
  static Reference<CacheWriter>
  alloc_init(const char *path, uint64_t version) {
    return Reference<CacheWriter>(CacheWriter_alloc_init(path, version));
  };

  void
  set(const void *key, uint64_t key_length, DispatchData value) {
    CacheWriter_set(handle, key, key_length, value);
  };

  template <typename K>
  void
  set(const K &key, DispatchData value) {
    set(reinterpret_cast<const void *>(&key), sizeof(key), value);
  };
};

inline Reference<Object>
MakeAutoreleasePool() {
  return Reference<Object>(NSAutoreleasePool_alloc_init());
}

inline Reference<String>
MakeString(const char *data, WMTStringEncoding encoding) {
  return Reference<String>(NSString_alloc_init(data, encoding));
}

inline Reference<DispatchData>
MakeDispatchData(uint64_t native_ptr, uint64_t length) {
  return Reference<DispatchData>(DispatchData_alloc_init(native_ptr, length));
}

inline Reference<DispatchData>
MakeDispatchData(void *native_ptr, uint64_t length) {
  return Reference<DispatchData>(DispatchData_alloc_init((uint64_t)native_ptr, length));
}

inline Object
CreateMetalViewFromHWND(intptr_t hwnd, Device device, MetalLayer &layer) {
  return {::CreateMetalViewFromHWND(hwnd, device.handle, &layer.handle)};
}

inline void
ReleaseMetalView(Object view) {
  return ::ReleaseMetalView(view.handle);
}

inline void
InitializeRenderPassInfo(WMTRenderPassInfo &info) {
  std::memset(&info, 0, sizeof(WMTRenderPassInfo));
  for (unsigned i = 0; i < 8; i++) {
    info.colors[i].store_action = WMTStoreActionStore;
    info.colors[i].clear_color.a = 1.0f;
  }
  info.stencil.load_action = WMTLoadActionClear;
  info.depth.load_action = WMTLoadActionClear;
  info.depth.clear_depth = 1.0f;
};

inline void
InitializeRenderPipelineInfo(WMTRenderPipelineInfo &info) {
  for (unsigned i = 0; i < 8; i++) {
    info.colors[i].pixel_format = WMTPixelFormatInvalid;
    info.colors[i].write_mask = WMTColorWriteMaskAll;
    info.colors[i].blending_enabled = false;
    info.colors[i].alpha_blend_operation = WMTBlendOperationAdd;
    info.colors[i].rgb_blend_operation = WMTBlendOperationAdd;
    info.colors[i].dst_alpha_blend_factor = WMTBlendFactorZero;
    info.colors[i].dst_rgb_blend_factor = WMTBlendFactorZero;
    info.colors[i].src_alpha_blend_factor = WMTBlendFactorOne;
    info.colors[i].src_rgb_blend_factor = WMTBlendFactorOne;
  }
  info.alpha_to_coverage_enabled = false;
  info.rasterization_enabled = true;
  info.raster_sample_count = 1;
  info.depth_pixel_format = WMTPixelFormatInvalid;
  info.stencil_pixel_format = WMTPixelFormatInvalid;
  info.logic_operation_enabled = false;
  info.logic_operation = WMTLogicOperationClear;

  info.input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;
  info.max_tessellation_factor = 16.0f;
  info.tessellation_factor_step = WMTTessellationFactorStepFunctionConstant;
  info.tessellation_partition_mode = WMTTessellationPartitionModePow2;
  info.tessellation_output_winding_order = WMTWindingClockwise;

  info.vertex_function = NULL_OBJECT_HANDLE;
  info.fragment_function = NULL_OBJECT_HANDLE;
  info.immutable_vertex_buffers = 0;
  info.immutable_fragment_buffers = 0;
  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  info.binary_archives_for_lookup.set(nullptr);
  info.num_binary_archives_for_lookup = 0;
  info.fail_on_binary_archive_miss = false;
}

inline void
InitializeComputePipelineInfo(WMTComputePipelineInfo &info) {
  info.compute_function = NULL_OBJECT_HANDLE;
  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  info.binary_archives_for_lookup.set(nullptr);
  info.num_binary_archives_for_lookup = 0;
  info.fail_on_binary_archive_miss = false;
  info.tgsize_is_multiple_of_sgwidth = false;
  info.immutable_buffers = 0;
}

inline void
InitializeMeshRenderPipelineInfo(WMTMeshRenderPipelineInfo &info) {
  for (unsigned i = 0; i < 8; i++) {
    info.colors[i].pixel_format = WMTPixelFormatInvalid;
    info.colors[i].write_mask = WMTColorWriteMaskAll;
    info.colors[i].blending_enabled = false;
    info.colors[i].alpha_blend_operation = WMTBlendOperationAdd;
    info.colors[i].rgb_blend_operation = WMTBlendOperationAdd;
    info.colors[i].dst_alpha_blend_factor = WMTBlendFactorZero;
    info.colors[i].dst_rgb_blend_factor = WMTBlendFactorZero;
    info.colors[i].src_alpha_blend_factor = WMTBlendFactorOne;
    info.colors[i].src_rgb_blend_factor = WMTBlendFactorOne;
  }
  info.alpha_to_coverage_enabled = false;
  info.rasterization_enabled = true;
  info.raster_sample_count = 1;
  info.depth_pixel_format = WMTPixelFormatInvalid;
  info.stencil_pixel_format = WMTPixelFormatInvalid;
  info.logic_operation_enabled = false;
  info.logic_operation = WMTLogicOperationClear;

  info.object_function = NULL_OBJECT_HANDLE;
  info.mesh_function = NULL_OBJECT_HANDLE;
  info.fragment_function = NULL_OBJECT_HANDLE;
  info.immutable_object_buffers = 0;
  info.immutable_mesh_buffers = 0;
  info.immutable_fragment_buffers = 0;
  info.payload_memory_length = 0;
  info.mesh_tgsize_is_multiple_of_sgwidth = 0;
  info.object_tgsize_is_multiple_of_sgwidth = 0;
  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  info.binary_archives_for_lookup.set(nullptr);
  info.num_binary_archives_for_lookup = 0;
  info.fail_on_binary_archive_miss = false;
}

inline void
InitializeTileRenderPipelineInfo(WMTTileRenderPipelineInfo &info) {
  for (unsigned i = 0; i < 8; i++) {
    info.color_formats[i] = WMTPixelFormatInvalid;
  }
  info.tile_function = NULL_OBJECT_HANDLE;
  info.immutable_tile_buffers = 0;
  info.raster_sample_count = 1;
  info.tgsize_matches_tile_size = 0;

  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  info.binary_archives_for_lookup.set(nullptr);
  info.num_binary_archives_for_lookup = 0;
  info.fail_on_binary_archive_miss = false;
}

} // namespace WMT
