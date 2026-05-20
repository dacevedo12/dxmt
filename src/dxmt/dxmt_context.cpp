#include "dxmt_context.hpp"
#include "Metal.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_deptrack.hpp"
#include "dxmt_format.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_presenter.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "wsi_platform.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace dxmt {

static bool
ResolveRenderPassColorAttachment(
    const char *message, unsigned slot, const TextureViewRef &attachment, WMT::Texture &resolved_texture
) {
  if (!attachment)
    return true;

  auto allocation = attachment->allocation;
  auto texture = allocation ? allocation->descriptor : nullptr;
  auto actual_texture = attachment.texture();
  if (!actual_texture) {
    WARN(message, " slot=", slot, " view=", uint64_t(attachment->key), " reason=missing Metal texture");
    return false;
  }

  auto actual_usage = actual_texture.usage();
  if (actual_usage & WMTTextureUsageRenderTarget) {
    resolved_texture = actual_texture;
    return true;
  }

  if (!texture) {
    WARN(
        message,
        " slot=", slot,
        " actual_usage=", uint32_t(actual_usage),
        " view=", uint64_t(attachment->key),
        " reason=missing texture descriptor"
    );
    return false;
  }

  auto usage = texture->usage();
  WARN(
      message,
      " slot=", slot,
      " descriptor_usage=", uint32_t(usage),
      " actual_usage=", uint32_t(actual_usage),
      " texture=", reinterpret_cast<const void *>(texture),
      " view=", uint64_t(attachment->key),
      " format=", uint32_t(texture->pixelFormat()),
      " type=", uint32_t(texture->textureType()),
      " size=", texture->width(), "x", texture->height(), "x", texture->depth(),
      " array_size=", texture->arrayLength(),
      " mip_levels=", texture->miplevelCount(),
      " sample_count=", texture->sampleCount()
  );
  return false;
}

static bool
ResolveRenderPassBufferColorAttachment(
    const char *message, unsigned slot, WMT::Texture attachment, WMT::Texture &resolved_texture
) {
  if (!attachment) {
    WARN(message, " slot=", slot, " reason=missing Metal buffer texture");
    return false;
  }

  auto actual_usage = attachment.usage();
  if (actual_usage & WMTTextureUsageRenderTarget) {
    resolved_texture = attachment;
    return true;
  }

  WARN(
      message,
      " slot=", slot,
      " actual_usage=", uint32_t(actual_usage),
      " format=", uint32_t(attachment.pixelFormat()),
      " size=", attachment.width(), "x", attachment.height(), "x", attachment.depth(),
      " array_size=", attachment.arrayLength(),
      " mip_levels=", attachment.mipmapLevelCount()
  );
  return false;
}

ArgumentEncodingContext::ArgumentEncodingContext(CommandQueue &queue, WMT::Device device, InternalCommandLibrary &lib) :
    emulated_cmd(device, lib, *this),
    clear_rt_cmd(device, lib, *this),
    resolve_texture_cmd(device, lib, *this),
    blit_depth_stencil_cmd(device, lib, *this),
    clear_res_cmd(device, lib, *this),
    mv_scale_cmd(device, lib, *this),
    tile_barrier_cmd(device, lib, *this),
    timestamp_state_(device),
    device_(device),
    queue_(queue) {
  dummy_sampler_info_.support_argument_buffers = true;
  dummy_sampler_info_.border_color = WMTSamplerBorderColorTransparentBlack;
  dummy_sampler_info_.compare_function = WMTCompareFunctionNever;
  dummy_sampler_info_.normalized_coords = true;
  dummy_sampler_info_.r_address_mode = WMTSamplerAddressModeClampToEdge;
  dummy_sampler_info_.s_address_mode = WMTSamplerAddressModeClampToEdge;
  dummy_sampler_info_.t_address_mode = WMTSamplerAddressModeClampToEdge;
  dummy_sampler_info_.min_filter = WMTSamplerMinMagFilterNearest;
  dummy_sampler_info_.mag_filter = WMTSamplerMinMagFilterNearest;
  dummy_sampler_info_.mip_filter = WMTSamplerMipFilterNotMipmapped;
  dummy_sampler_info_.lod_min_clamp = 0.0f;
  dummy_sampler_info_.lod_max_clamp = FLT_MAX;
  dummy_sampler_info_.max_anisotroy = 1;
  dummy_sampler_info_.lod_average = false;
  dummy_sampler_ = device.newSamplerState(dummy_sampler_info_);
  dummy_cbuffer_host_ = wsi::aligned_malloc(65536, DXMT_PAGE_SIZE);
  dummy_cbuffer_info_.length = 65536;
  dummy_cbuffer_info_.memory.set(dummy_cbuffer_host_);
  dummy_cbuffer_info_.options = WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceStorageModeShared |
                                WMTResourceHazardTrackingModeUntracked;
  dummy_cbuffer_ = device.newBuffer(dummy_cbuffer_info_);
  std::memset(dummy_cbuffer_info_.memory.get(), 0, 65536);
  cpu_buffer_chunks_.emplace_back();
  barrier_event_ = device_.newEvent();
  for (unsigned i = 0; i < kParityLane; i++) {
    fence_pool_[i] = device.newFence();
  }
};

ArgumentEncodingContext::~ArgumentEncodingContext() {
  wsi::aligned_free(dummy_cbuffer_host_);
};

template void ArgumentEncodingContext::encodeVertexBuffers<PipelineKind::Ordinary>(uint32_t slot_mask, uint64_t argument_buffer_offset);
template void ArgumentEncodingContext::encodeVertexBuffers<PipelineKind::Tessellation>(uint32_t slot_mask, uint64_t argument_buffer_offset);
template void ArgumentEncodingContext::encodeVertexBuffers<PipelineKind::Geometry>(uint32_t slot_mask, uint64_t argument_buffer_offset);

template <PipelineKind kind>
void
ArgumentEncodingContext::encodeVertexBuffers(uint32_t slot_mask, uint64_t offset) {
  struct VERTEX_BUFFER_ENTRY {
    uint64_t buffer_handle;
    uint32_t stride;
    uint32_t length;
  };
  uint32_t max_slot = 32 - __builtin_clz(slot_mask);

  VERTEX_BUFFER_ENTRY *entries = getMappedArgumentBuffer<VERTEX_BUFFER_ENTRY>(offset);

  for (unsigned slot = 0, index = 0; slot < max_slot; slot++) {
    if (!(slot_mask & (1 << slot)))
      continue;
    auto &state = vbuf_[slot];
    auto &buffer = state.buffer;
    if (!buffer.ptr()) {
      entries[index].buffer_handle = 0;
      entries[index].stride = 0;
      entries[index++].length = 0;
      continue;
    }
    auto valid_length = buffer->length() > state.offset ? buffer->length() - state.offset : 0;
    auto [buffer_alloc, buffer_offset] =
        access<PipelineStage::Vertex>(buffer, state.offset, valid_length, ResourceAccess::Read);
    entries[index].buffer_handle = buffer_alloc->gpuAddress() + buffer_offset + state.offset;
    entries[index].stride = state.stride;
    entries[index++].length = valid_length;
    // FIXME: did we intended to use the whole buffer?
    makeResident<PipelineStage::Vertex, kind>(buffer.ptr());
  };
  {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    cmd.offset = getFinalArgumentBufferOffset(offset);
    cmd.index = 16;
    if constexpr (kind == PipelineKind::Geometry || kind == PipelineKind::Tessellation)
      cmd.type = WMTRenderCommandSetObjectBufferOffset;
    else
      cmd.type = WMTRenderCommandSetVertexBufferOffset;
  }
}

template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Hull, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Domain, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Compute, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Geometry, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);

template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Hull, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Domain, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Compute, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Geometry, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::encodeConstantBuffers(const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT * constant_buffers, uint64_t offset) {
  uint64_t *encoded_buffer = getMappedArgumentBuffer<uint64_t, stage == PipelineStage::Compute>(offset);

  for (unsigned i = 0; i < reflection->NumConstantBuffers; i++) {
    auto &arg = constant_buffers[i];
    auto slot = 14 * unsigned(stage) + arg.SM50BindingSlot;
    switch (arg.Type) {
    case SM50BindingType::ConstantBuffer: {
      auto &cbuf = cbuf_[slot];
      if (!cbuf.buffer.ptr()) {
        encoded_buffer[arg.StructurePtrOffset] = dummy_cbuffer_info_.gpu_address;
        makeResident<stage, kind>(dummy_cbuffer_, GetResidencyMask<kind>(stage, true, false));
        continue;
      }
      auto argbuf = cbuf.buffer;
      auto valid_length = argbuf->length() > cbuf.offset ? argbuf->length() - cbuf.offset : 0;
      auto [argbuf_alloc, argbuf_offset] = access<stage>(argbuf, cbuf.offset, valid_length, ResourceAccess::Read);
      encoded_buffer[arg.StructurePtrOffset] = argbuf_alloc->gpuAddress() + argbuf_offset + cbuf.offset;
      makeResident<stage, kind>(argbuf.ptr());
      break;
    }
    default:
      DXMT_UNREACHABLE
    }
  }

  /* kConstantBufferTableBinding = 29 */
  if constexpr (stage == PipelineStage::Compute) {
    auto &cmd = encodeComputeCommand<wmtcmd_compute_setbufferoffset>();
    cmd.type = WMTComputeCommandSetBufferOffset;
    cmd.offset = getFinalArgumentBufferOffset<true>(offset);
    cmd.index = 29;
  } else {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    cmd.offset = getFinalArgumentBufferOffset(offset);
    cmd.index = 29;
    if constexpr (stage == PipelineStage::Vertex) {
      if constexpr (kind == PipelineKind::Geometry)
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
      else if constexpr (kind == PipelineKind::Tessellation) {
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
        cmd.index = 27;
      } else
        cmd.type = WMTRenderCommandSetVertexBufferOffset;
    } else if constexpr (stage == PipelineStage::Pixel) {
      cmd.type = WMTRenderCommandSetFragmentBufferOffset;
    } else if constexpr (stage == PipelineStage::Hull) {
      cmd.type = WMTRenderCommandSetObjectBufferOffset;
    } else if constexpr (stage == PipelineStage::Domain) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else if constexpr (stage == PipelineStage::Geometry) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else {
      assert(0 && "Not implemented or unreachable");
    }
  }
};

template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Vertex, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Pixel, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Vertex, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Pixel, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Hull, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Domain, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Compute, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Vertex, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Geometry, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Pixel, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);

inline uint64_t
TextureMetadata(uint32_t array_length, float min_lod) {
  return ((uint64_t)array_length << 32) | (uint64_t)std::bit_cast<uint32_t>(min_lod);
}

static constexpr uint32_t kDummyTextureKindCount = 7;
static constexpr uint32_t kDummyTextureFormatCount = 4;

enum class DummyTextureKind : uint32_t {
  Texture2D = 0,
  Texture2DArray = 1,
  Texture2DMultisampled = 2,
  Texture2DMultisampledArray = 3,
  Texture3D = 4,
  TextureCube = 5,
  TextureCubeArray = 6,
};

enum class DummyTextureFormat : uint32_t {
  Float = 0,
  Uint = 1,
  Sint = 2,
  Depth = 3,
};

static DummyTextureKind
DummyTextureKindForArgument(const MTL_SM50_SHADER_ARGUMENT &arg) {
  bool is_array = arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_CUBE)
    return is_array ? DummyTextureKind::TextureCubeArray : DummyTextureKind::TextureCube;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_3D)
    return DummyTextureKind::Texture3D;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED)
    return is_array ? DummyTextureKind::Texture2DMultisampledArray : DummyTextureKind::Texture2DMultisampled;
  return is_array ? DummyTextureKind::Texture2DArray : DummyTextureKind::Texture2D;
}

static DummyTextureFormat
DummyTextureFormatForArgument(const MTL_SM50_SHADER_ARGUMENT &arg) {
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_DEPTH)
    return DummyTextureFormat::Depth;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_SINT)
    return DummyTextureFormat::Sint;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_UINT)
    return DummyTextureFormat::Uint;
  return DummyTextureFormat::Float;
}

static uint32_t
DummyTextureIndex(DummyTextureKind kind, DummyTextureFormat format) {
  return uint32_t(format) * kDummyTextureKindCount + uint32_t(kind);
}

static WMTPixelFormat
DummyTexturePixelFormat(DummyTextureFormat format) {
  switch (format) {
  case DummyTextureFormat::Uint:
    return WMTPixelFormatRGBA32Uint;
  case DummyTextureFormat::Sint:
    return WMTPixelFormatRGBA32Sint;
  case DummyTextureFormat::Depth:
    return WMTPixelFormatDepth32Float;
  case DummyTextureFormat::Float:
  default:
    return WMTPixelFormatRGBA32Float;
  }
}

static WMTTextureType
DummyTextureType(DummyTextureKind kind) {
  switch (kind) {
  case DummyTextureKind::Texture2DArray:
    return WMTTextureType2DArray;
  case DummyTextureKind::Texture2DMultisampled:
    return WMTTextureType2DMultisample;
  case DummyTextureKind::Texture2DMultisampledArray:
    return WMTTextureType2DMultisampleArray;
  case DummyTextureKind::Texture3D:
    return WMTTextureType3D;
  case DummyTextureKind::TextureCube:
    return WMTTextureTypeCube;
  case DummyTextureKind::TextureCubeArray:
    return WMTTextureTypeCubeArray;
  case DummyTextureKind::Texture2D:
  default:
    return WMTTextureType2D;
  }
}

static uint32_t
DummyTextureArrayLength(DummyTextureKind kind) {
  switch (kind) {
  case DummyTextureKind::Texture2DArray:
  case DummyTextureKind::Texture2DMultisampledArray:
  case DummyTextureKind::TextureCubeArray:
    return 1;
  default:
    return 1;
  }
}

static bool
DebugEnabledEnv(const char *name) {
  auto value = env::getEnvVar(name);
  return value == "1" || value == "true" || value == "yes" || value == "trace";
}

static void
NormalizeRenderPassInfo(WMTRenderPassInfo &info) {
  if (!info.default_raster_sample_count)
    info.default_raster_sample_count = 1;
  if (!info.render_target_array_length)
    info.render_target_array_length = 1;
  if (info.depth.texture && info.depth.depth_plane >= WMT::Texture{info.depth.texture}.depth()) {
    WARN("RenderPass guard: clamping invalid depth attachment depthPlane=",
         info.depth.depth_plane);
    info.depth.depth_plane = 0;
  }
  if (info.stencil.texture && info.stencil.depth_plane >= WMT::Texture{info.stencil.texture}.depth()) {
    WARN("RenderPass guard: clamping invalid stencil attachment depthPlane=",
         info.stencil.depth_plane);
    info.stencil.depth_plane = 0;
  }
}

static double
DebugMillis(clock::duration duration) {
  return duration.count() / 1000000.0;
}

static bool
DebugShaderHashSelected(const std::string &shader_hash) {
  auto filters = env::getEnvVar("DXMT_DIAG_SHADER_HASHES");
  if (filters.empty() || filters == "all")
    return true;

  for (auto filter : str::split(filters, ",; ")) {
    if (filter == "all")
      return true;
    if (shader_hash == filter)
      return true;
    if (shader_hash.starts_with(filter))
      return true;
  }

  return false;
}

static bool
DebugShouldLogBinding(const std::string &shader_hash) {
  static const bool enabled = DebugEnabledEnv("DXMT_DIAG_BINDINGS");
  return enabled && (shader_hash.empty() || DebugShaderHashSelected(shader_hash));
}

static bool
DebugShouldLogRenderCommands() {
  static const bool enabled =
      DebugEnabledEnv("DXMT_DIAG_RENDER_COMMANDS") ||
      DebugEnabledEnv("DXMT_DIAG_COMMAND_QUEUE");
  return enabled;
}

static bool
DebugShouldLogRenderPasses() {
  static const bool enabled = DebugEnabledEnv("DXMT_DIAG_RENDER_PASS");
  return enabled;
}

static bool
DebugPresentReadbackEnabled() {
  static const bool enabled = DebugEnabledEnv("DXMT_DIAG_PRESENT_READBACK");
  return enabled;
}

static bool
DebugRenderReadbackEnabled() {
  static const bool enabled = DebugEnabledEnv("DXMT_DIAG_RENDER_READBACK");
  return enabled;
}

static bool
DebugPresentReadbackGridEnabled() {
  static const bool enabled =
      DebugEnabledEnv("DXMT_DIAG_PRESENT_READBACK_GRID");
  return enabled;
}

static uint32_t
DebugPresentReadbackGridSize() {
  static const uint32_t size = []() {
    auto value = env::getEnvVar("DXMT_DIAG_PRESENT_READBACK_GRID_SIZE");
    if (value.empty())
      return 3u;
    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 3u;
    auto clamped = std::clamp<unsigned long>(parsed, 1, 33);
    return static_cast<uint32_t>(clamped | 1u);
  }();
  return size;
}

static uint32_t
DebugPresentReadbackLimit() {
  static const uint32_t limit = []() {
    auto value = env::getEnvVar("DXMT_DIAG_PRESENT_READBACK_LIMIT");
    if (value.empty())
      return 16u;
    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 16u;
    return static_cast<uint32_t>(std::max<unsigned long>(1, parsed));
  }();
  return limit;
}

static uint32_t
DebugPresentReadbackSkip() {
  static const uint32_t skip = []() {
    auto value = env::getEnvVar("DXMT_DIAG_PRESENT_READBACK_SKIP");
    if (value.empty())
      return 0u;
    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 0u;
    return static_cast<uint32_t>(parsed);
  }();
  return skip;
}

static uint32_t
DebugPresentReadbackInterval() {
  static const uint32_t interval = []() {
    auto value = env::getEnvVar("DXMT_DIAG_PRESENT_READBACK_INTERVAL");
    if (value.empty())
      return 1u;
    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 1u;
    return static_cast<uint32_t>(std::max<unsigned long>(1, parsed));
  }();
  return interval;
}

static uint32_t
DebugRenderReadbackLimit() {
  static const uint32_t limit = []() {
    auto value = env::getEnvVar("DXMT_DIAG_RENDER_READBACK_LIMIT");
    if (value.empty())
      return 64u;
    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 64u;
    return static_cast<uint32_t>(std::max<unsigned long>(1, parsed));
  }();
  return limit;
}

static uint64_t
DebugRenderReadbackFrameSkip() {
  static const uint64_t skip = []() {
    auto value = env::getEnvVar("DXMT_DIAG_RENDER_READBACK_FRAME_SKIP");
    if (value.empty())
      return 0ull;
    char *end = nullptr;
    auto parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 0ull;
    return static_cast<uint64_t>(parsed);
  }();
  return skip;
}

static uint64_t
DebugRenderReadbackFrameInterval() {
  static const uint64_t interval = []() {
    auto value = env::getEnvVar("DXMT_DIAG_RENDER_READBACK_FRAME_INTERVAL");
    if (value.empty())
      return 1ull;
    char *end = nullptr;
    auto parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 1ull;
    return static_cast<uint64_t>(std::max<unsigned long long>(1, parsed));
  }();
  return interval;
}

static bool
DebugShouldSamplePresentReadback(uint32_t &present_index) {
  static std::atomic<uint32_t> present_count = 0;
  static std::atomic<uint32_t> sample_count = 0;
  if (!DebugPresentReadbackEnabled())
    return false;
  present_index = present_count.fetch_add(1, std::memory_order_relaxed);
  auto skip = DebugPresentReadbackSkip();
  if (present_index < skip)
    return false;
  if (((present_index - skip) % DebugPresentReadbackInterval()) != 0)
    return false;
  return sample_count.fetch_add(1, std::memory_order_relaxed) < DebugPresentReadbackLimit();
}

static bool
DebugShouldSampleRenderReadback(uint64_t frame_id) {
  static std::atomic<uint32_t> sample_count = 0;
  if (!DebugRenderReadbackEnabled())
    return false;
  auto skip = DebugRenderReadbackFrameSkip();
  if (frame_id < skip)
    return false;
  if (((frame_id - skip) % DebugRenderReadbackFrameInterval()) != 0)
    return false;
  return sample_count.fetch_add(1, std::memory_order_relaxed) < DebugRenderReadbackLimit();
}

static bool
DebugSupportsTextureReadback(WMT::Texture texture) {
  if (!texture)
    return false;
  auto format = texture.pixelFormat();
  if (!MTLGetTexelSize(format))
    return false;
  if (IsBlockCompressionFormat(format))
    return false;
  return true;
}

static void
DebugEncodeTexturePointReadback(QueryReadbacks &readbacks, WMT::CommandBuffer cmdbuf,
                                WMT::Device device, WMT::Texture texture,
                                const char *label, uint64_t frame_id,
                                uint64_t seq_id, uint64_t encoder_id,
                                uint32_t index, uint32_t point_x,
                                uint32_t point_y, uint16_t level, uint16_t slice,
                                uint32_t width, uint32_t height) {
  if (!DebugSupportsTextureReadback(texture)) {
    INFO("DXMT diagnostic: texture readback skipped",
         " label=", label,
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", encoder_id,
         " index=", index,
         " texture=", uint64_t(texture),
         " format=", texture ? uint32_t(texture.pixelFormat()) : 0,
         " size=", texture ? texture.width() : 0, "x", texture ? texture.height() : 0,
         " reason=unsupported_texture");
    return;
  }

  if (!width || !height) {
    INFO("DXMT diagnostic: texture readback skipped",
         " label=", label,
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", encoder_id,
         " index=", index,
         " texture=", uint64_t(texture),
         " format=", uint32_t(texture.pixelFormat()),
         " size=", width, "x", height,
         " reason=empty_texture");
    return;
  }

  const auto texel_size = MTLGetTexelSize(texture.pixelFormat());
  const auto row_pitch = std::max<uint32_t>(256, texel_size);
  constexpr uint32_t readback_size = 256;
  WMTBufferInfo buffer_info = {};
  buffer_info.length = readback_size;
  buffer_info.options = WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked;
  buffer_info.memory.set(nullptr);
#ifdef __i386__
  buffer_info.memory.set(wsi::aligned_malloc(readback_size, DXMT_PAGE_SIZE));
#endif
  auto buffer = device.newBuffer(buffer_info);
  auto *mapped = static_cast<uint8_t *>(buffer_info.memory.get_accessible_or_null());
  if (!buffer || !mapped) {
    INFO("DXMT diagnostic: texture readback skipped",
         " label=", label,
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", encoder_id,
         " index=", index,
         " texture=", uint64_t(texture),
         " format=", uint32_t(texture.pixelFormat()),
         " size=", width, "x", height,
         " reason=buffer_allocation_failed");
#ifdef __i386__
    wsi::aligned_free(buffer_info.memory.get_accessible_or_null());
#endif
    return;
  }

  const auto x = std::min(point_x, width - 1);
  const auto y = std::min(point_y, height - 1);

  auto encoder = cmdbuf.blitCommandEncoder();
  wmtcmd_blit_copy_from_texture_to_buffer copy = {};
  copy.type = WMTBlitCommandCopyFromTextureToBuffer;
  copy.next.set(nullptr);
  copy.src = texture;
  copy.slice = slice;
  copy.level = level;
  copy.origin = {x, y, 0};
  copy.size = {1, 1, 1};
  copy.dst = buffer;
  copy.offset = 0;
  copy.bytes_per_row = row_pitch;
  copy.bytes_per_image = readback_size;
  encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
  encoder.endEncoding();

  const auto format = texture.pixelFormat();
  readbacks.diagnostics.push_back(
       [buffer = WMT::Reference<WMT::Buffer>(buffer), mapped, label = std::string(label),
       frame_id, seq_id, texture_id = uint64_t(texture), format, width, height,
       x, y, texel_size, encoder_id, index, level, slice]() {
        uint8_t bytes[16] = {};
        const auto copy_size = std::min<uint32_t>(texel_size, sizeof(bytes));
        std::memcpy(bytes, mapped, copy_size);
        const uint32_t u32 = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
                             (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
        INFO("DXMT diagnostic: texture readback",
             " label=", label,
             " frame=", frame_id,
             " seq=", seq_id,
             " encoder=", encoder_id,
             " index=", index,
             " texture=", texture_id,
             " format=", uint32_t(format),
             " size=", width, "x", height,
             " level=", uint32_t(level),
             " slice=", uint32_t(slice),
             " xy=", x, ",", y,
             " texelSize=", texel_size,
             " bytes=", uint32_t(bytes[0]), ",", uint32_t(bytes[1]), ",",
             uint32_t(bytes[2]), ",", uint32_t(bytes[3]),
             " u32=", u32);
#ifdef __i386__
        wsi::aligned_free(mapped);
#endif
      });
}

static void
DebugEncodeTextureCenterReadback(QueryReadbacks &readbacks, WMT::CommandBuffer cmdbuf,
                                 WMT::Device device, WMT::Texture texture,
                                 const char *label, uint64_t frame_id,
                                 uint64_t seq_id, uint32_t present_index) {
  const auto width = texture ? texture.width() : 0;
  const auto height = texture ? texture.height() : 0;
  DebugEncodeTexturePointReadback(readbacks, cmdbuf, device, texture, label, frame_id,
                                  seq_id, present_index, present_index,
                                  width / 2, height / 2, 0, 0, width, height);
}

static void
DebugEncodePresentReadbacks(QueryReadbacks &readbacks, WMT::CommandBuffer cmdbuf,
                            WMT::Device device, WMT::Texture texture,
                            const char *label, uint64_t frame_id,
                            uint64_t seq_id, uint32_t present_index) {
  if (!DebugPresentReadbackGridEnabled()) {
    DebugEncodeTextureCenterReadback(readbacks, cmdbuf, device, texture, label,
                                     frame_id, seq_id, present_index);
    return;
  }

  const auto width = texture ? texture.width() : 0;
  const auto height = texture ? texture.height() : 0;
  const auto grid_size = DebugPresentReadbackGridSize();
  for (uint32_t y = 0; y < grid_size; y++) {
    for (uint32_t x = 0; x < grid_size; x++) {
      const auto index = y * grid_size + x;
      const auto point_x = width ? ((uint64_t(x) * width) + (width / 2)) / grid_size : 0;
      const auto point_y = height ? ((uint64_t(y) * height) + (height / 2)) / grid_size : 0;
    DebugEncodeTexturePointReadback(
        readbacks, cmdbuf, device, texture, label, frame_id, seq_id,
          present_index, index, point_x, point_y, 0, 0, width, height);
    }
  }
}

static void
DebugEncodeRenderAttachmentReadbacks(QueryReadbacks &readbacks, WMT::CommandBuffer cmdbuf,
                                     WMT::Device device, uint64_t frame_id,
                                     uint64_t seq_id, const RenderEncoderData *data,
                                     bool sample, const char *label) {
  if (!sample)
    return;

  if (data->default_raster_sample_count > 1) {
    INFO("DXMT diagnostic: render readback skipped",
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", data->id,
         " samples=", uint32_t(data->default_raster_sample_count),
         " reason=multisample");
    return;
  }

  for (unsigned i = 0; i < std::size(data->colors); i++) {
    auto &color = data->colors[i];
    if (!color.attachment && !color.buffer_texture)
      continue;

    WMT::Texture texture;
    uint16_t level = color.level;
    uint16_t slice = color.slice;
    uint32_t width = data->render_target_width;
    uint32_t height = data->render_target_height;
    if (color.attachment) {
      texture = color.attachment.texture();
    } else {
      texture = color.buffer_texture;
      level = 0;
      slice = 0;
      width = texture ? texture.width() : 0;
      height = texture ? texture.height() : 0;
    }

    DebugEncodeTexturePointReadback(readbacks, cmdbuf, device, texture,
                                    label, frame_id, seq_id,
                                    data->id, i, width / 2, height / 2, level,
                                    slice, width, height);
  }
}

static uint32_t
DebugBindingLogLimit() {
  static const uint32_t limit = []() {
    auto value = env::getEnvVar("DXMT_DIAG_BINDING_LIMIT");
    if (value.empty())
      return uint32_t(4096);

    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed == 0)
      return uint32_t(4096);

    return uint32_t(std::min<unsigned long>(parsed, 100000));
  }();
  return limit;
}

static const char *
DebugPipelineStageName(PipelineStage stage) {
  switch (stage) {
  case PipelineStage::Vertex:
    return "VS";
  case PipelineStage::Pixel:
    return "PS";
  case PipelineStage::Geometry:
    return "GS";
  case PipelineStage::Hull:
    return "HS";
  case PipelineStage::Domain:
    return "DS";
  case PipelineStage::Compute:
    return "CS";
  }
  return "?";
}

static void
DebugAccumulateRenderCommands(FrameStatistics &statistics,
                              const wmtcmd_render_nop *cmd_head) {
  if (!DebugShouldLogRenderCommands())
    return;

  auto command = reinterpret_cast<const wmtcmd_base *>(cmd_head->next.ptr);
  while (command) {
    statistics.render_command_count++;
    switch (static_cast<WMTRenderCommandType>(command->type)) {
    case WMTRenderCommandSetPSO:
      statistics.render_pso_bind_count++;
      break;
    case WMTRenderCommandDraw:
      statistics.render_draw_count++;
      break;
    case WMTRenderCommandDrawIndexed:
      statistics.render_indexed_draw_count++;
      break;
    case WMTRenderCommandDrawIndirect:
    case WMTRenderCommandDrawIndexedIndirect:
      statistics.render_indirect_draw_count++;
      break;
    case WMTRenderCommandDrawMeshThreadgroups:
    case WMTRenderCommandDrawMeshThreadgroupsIndirect:
    case WMTRenderCommandDXMTGeometryDraw:
    case WMTRenderCommandDXMTGeometryDrawIndexed:
    case WMTRenderCommandDXMTGeometryDrawIndirect:
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect:
    case WMTRenderCommandDXMTTessellationMeshDraw:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
    case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect:
      statistics.render_mesh_draw_count++;
      break;
    case WMTRenderCommandDispatchThreadsPerTile:
      statistics.render_tile_dispatch_count++;
      break;
    default:
      break;
    }
    command = static_cast<const wmtcmd_base *>(command->next.ptr);
  }
}

struct DebugRenderCommandSummary {
  uint32_t command_count = 0;
  uint32_t pso_binds = 0;
  uint32_t draws = 0;
  uint32_t indexed_draws = 0;
  uint32_t indirect_draws = 0;
  uint32_t mesh_draws = 0;
  uint32_t tile_dispatches = 0;
};

static DebugRenderCommandSummary
DebugSummarizeRenderCommands(const wmtcmd_render_nop *cmd_head) {
  DebugRenderCommandSummary summary;
  auto command = reinterpret_cast<const wmtcmd_base *>(cmd_head->next.ptr);
  while (command) {
    summary.command_count++;
    switch (static_cast<WMTRenderCommandType>(command->type)) {
    case WMTRenderCommandSetPSO:
      summary.pso_binds++;
      break;
    case WMTRenderCommandDraw:
      summary.draws++;
      break;
    case WMTRenderCommandDrawIndexed:
      summary.indexed_draws++;
      break;
    case WMTRenderCommandDrawIndirect:
    case WMTRenderCommandDrawIndexedIndirect:
      summary.indirect_draws++;
      break;
    case WMTRenderCommandDrawMeshThreadgroups:
    case WMTRenderCommandDrawMeshThreadgroupsIndirect:
    case WMTRenderCommandDXMTGeometryDraw:
    case WMTRenderCommandDXMTGeometryDrawIndexed:
    case WMTRenderCommandDXMTGeometryDrawIndirect:
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect:
    case WMTRenderCommandDXMTTessellationMeshDraw:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
    case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect:
      summary.mesh_draws++;
      break;
    case WMTRenderCommandDispatchThreadsPerTile:
      summary.tile_dispatches++;
      break;
    default:
      break;
    }
    command = static_cast<const wmtcmd_base *>(command->next.ptr);
  }
  return summary;
}

static void
DebugLogRenderPassInfo(uint64_t frame_id, uint64_t seq_id, uint64_t encoder_id,
                       const RenderEncoderData *data,
                       const DebugRenderCommandSummary &summary) {
  if (!DebugShouldLogRenderPasses())
    return;

  static std::atomic<uint32_t> log_count = 0;
  auto index = log_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= DebugBindingLogLimit())
    return;

  INFO(
      "DXMT diagnostic: render pass",
      " frame=", frame_id,
      " seq=", seq_id,
      " encoder=", encoder_id,
      " size=", data->render_target_width, "x", data->render_target_height,
      " rt_count=", uint32_t(data->render_target_count),
      " samples=", uint32_t(data->default_raster_sample_count),
      " array_length=", uint32_t(data->render_target_array_length),
      " commands=", summary.command_count,
      " psoBinds=", summary.pso_binds,
      " draws=", summary.draws,
      " indexedDraws=", summary.indexed_draws,
      " indirectDraws=", summary.indirect_draws,
      " meshDraws=", summary.mesh_draws,
      " tileDispatches=", summary.tile_dispatches
  );

  for (unsigned i = 0; i < std::size(data->colors); i++) {
    auto &color = data->colors[i];
    if (!color.attachment && !color.buffer_texture)
      continue;
    WMT::Texture texture;
    if (color.attachment)
      texture = color.attachment.texture();
    else
      texture = color.buffer_texture;
    auto *allocation = color.attachment ? color.attachment->allocation : nullptr;
    auto *descriptor = allocation ? allocation->descriptor : nullptr;
    INFO(
        "DXMT diagnostic: render color attachment",
        " frame=", frame_id,
        " encoder=", encoder_id,
        " slot=", i,
        " texture=", uint64_t(texture),
        " texture_descriptor=", uint64_t(descriptor),
        " allocation=", uint64_t(allocation),
        " allocation_texture=", allocation ? uint64_t(allocation->texture()) : 0,
        " view=", color.attachment ? uint64_t(color.attachment->key) : color.buffer_view_id,
        " load=", uint32_t(color.load_action),
        " store=", uint32_t(color.store_action),
        " clear=", color.clear_color.r, ",", color.clear_color.g, ",", color.clear_color.b, ",", color.clear_color.a,
        " level=", uint32_t(color.level),
        " slice=", uint32_t(color.slice),
        " resolve=", uint64_t(color.resolve_attachment ? color.resolve_attachment.texture() : WMT::Texture{})
    );
  }
}

static void
DebugLogClearPassInfo(uint64_t frame_id, uint64_t seq_id, uint64_t encoder_id,
                      const ClearEncoderData *data) {
  if (!DebugShouldLogRenderPasses())
    return;

  static std::atomic<uint32_t> log_count = 0;
  auto index = log_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= DebugBindingLogLimit())
    return;

  WMT::Texture texture;
  if (data->attachment)
    texture = data->attachment.texture();
  else
    texture = data->buffer_texture;

  INFO(
      "DXMT diagnostic: clear pass",
      " frame=", frame_id,
      " seq=", seq_id,
      " encoder=", encoder_id,
      " size=", data->width, "x", data->height,
      " array_length=", uint32_t(data->array_length),
      " clear_dsv=", uint32_t(data->clear_dsv),
      " level=", uint32_t(data->level),
      " slice=", uint32_t(data->slice),
      " depth_plane=", uint32_t(data->depth_plane),
      " stencil_depth_plane=", uint32_t(data->stencil_depth_plane),
      " texture=", uint64_t(texture),
      " view=", data->attachment ? uint64_t(data->attachment->key) : data->buffer_view_id,
      " color=", data->color.r, ",", data->color.g, ",", data->color.b, ",", data->color.a,
      " depth=", data->depth_stencil.first,
      " stencil=", uint32_t(data->depth_stencil.second)
  );
}

static const char *
DebugPipelineKindName(PipelineKind kind) {
  switch (kind) {
  case PipelineKind::Ordinary:
    return "ordinary";
  case PipelineKind::Tessellation:
    return "tessellation";
  case PipelineKind::Geometry:
    return "geometry";
  }
  return "?";
}

template <PipelineStage stage, PipelineKind kind>
static void
DebugLogConstantBufferBinding(
    const std::string &shader_hash, const MTL_SM50_SHADER_ARGUMENT &arg,
    const ConstantBufferBinding &binding, uint64_t encoded_address,
    uint64_t valid_length, uint64_t encoder_id, bool dummy
) {
  if (!DebugShouldLogBinding(shader_hash))
    return;

  static std::atomic<uint32_t> log_count = 0;
  auto index = log_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= DebugBindingLogLimit())
    return;

  INFO(
      "DXMT diagnostic: shader constant buffer binding",
      " stage=", DebugPipelineStageName(stage),
      " kind=", DebugPipelineKindName(kind),
      " shader=", shader_hash,
      " encoder=", encoder_id,
      " slot=", arg.SM50BindingSlot,
      " arg_index=", GetArgumentIndex(arg.Type, arg.SM50BindingSlot),
      " struct_qword=", arg.StructurePtrOffset,
      " register_lower=", arg.RegisterLowerBound,
      " register_count=", arg.RegisterCount,
      " register_space=", arg.RegisterSpace,
      " cbuffer_vec4=", arg.CBufferSizeInVec4,
      " buffer=", uint64_t(binding.buffer.ptr()),
      " offset=", uint32_t(binding.offset),
      " length=", valid_length,
      " encoded=0x", std::hex, encoded_address, std::dec,
      " dummy=", dummy
  );
}

static const char *
DebugTextureTypeName(WMTTextureType type) {
  switch (type) {
  case WMTTextureType1D:
    return "1D";
  case WMTTextureType1DArray:
    return "1DArray";
  case WMTTextureType2D:
    return "2D";
  case WMTTextureType2DArray:
    return "2DArray";
  case WMTTextureType2DMultisample:
    return "2DMS";
  case WMTTextureType2DMultisampleArray:
    return "2DMSArray";
  case WMTTextureTypeCube:
    return "Cube";
  case WMTTextureTypeCubeArray:
    return "CubeArray";
  case WMTTextureType3D:
    return "3D";
  case WMTTextureTypeTextureBuffer:
    return "TextureBuffer";
  default:
    return "Unknown";
  }
}

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::encodeConstantBuffers(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers, uint64_t offset,
    const ConstantBufferBinding *bindings
) {
  uint64_t *encoded_buffer = getMappedArgumentBuffer<uint64_t, stage == PipelineStage::Compute>(offset);
  auto encoder_id = currentEncoderId();

  for (unsigned i = 0; i < reflection->NumConstantBuffers; i++) {
    auto &arg = constant_buffers[i];
    switch (arg.Type) {
    case SM50BindingType::ConstantBuffer: {
      auto &cbuf = bindings[i];
      if (!cbuf.buffer.ptr()) {
        encoded_buffer[arg.StructurePtrOffset] = dummy_cbuffer_info_.gpu_address;
        DebugLogConstantBufferBinding<stage, kind>(
            "", arg, cbuf, dummy_cbuffer_info_.gpu_address, 0, encoder_id,
            true);
        makeResident<stage, kind>(dummy_cbuffer_, GetResidencyMask<kind>(stage, true, false));
        continue;
      }
      auto argbuf = cbuf.buffer;
      auto valid_length = argbuf->length() > cbuf.offset ? argbuf->length() - cbuf.offset : 0;
      auto [argbuf_alloc, argbuf_offset] = access<stage>(argbuf, cbuf.offset, valid_length, ResourceAccess::Read);
      encoded_buffer[arg.StructurePtrOffset] = argbuf_alloc->gpuAddress() + argbuf_offset + cbuf.offset;
      DebugLogConstantBufferBinding<stage, kind>(
          "", arg, cbuf, encoded_buffer[arg.StructurePtrOffset],
          valid_length, encoder_id, false);
      makeResident<stage, kind>(argbuf.ptr());
      break;
    }
    default:
      DXMT_UNREACHABLE
    }
  }

  /* kConstantBufferTableBinding = 29 */
  if constexpr (stage == PipelineStage::Compute) {
    auto &cmd = encodeComputeCommand<wmtcmd_compute_setbufferoffset>();
    cmd.type = WMTComputeCommandSetBufferOffset;
    cmd.offset = getFinalArgumentBufferOffset<true>(offset);
    cmd.index = 29;
  } else {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    cmd.offset = getFinalArgumentBufferOffset(offset);
    cmd.index = 29;
    if constexpr (stage == PipelineStage::Vertex) {
      if constexpr (kind == PipelineKind::Geometry)
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
      else if constexpr (kind == PipelineKind::Tessellation) {
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
        cmd.index = 27;
      } else
        cmd.type = WMTRenderCommandSetVertexBufferOffset;
    } else if constexpr (stage == PipelineStage::Pixel) {
      cmd.type = WMTRenderCommandSetFragmentBufferOffset;
    } else if constexpr (stage == PipelineStage::Hull) {
      cmd.type = WMTRenderCommandSetObjectBufferOffset;
    } else if constexpr (stage == PipelineStage::Domain) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else if constexpr (stage == PipelineStage::Geometry) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else {
      assert(0 && "Not implemented or unreachable");
    }
  }
}

static bool
DebugTextureTypeIsMultisampled(WMTTextureType type) {
  return type == WMTTextureType2DMultisample || type == WMTTextureType2DMultisampleArray;
}

template <PipelineStage stage, PipelineKind kind>
static void
DebugLogNullShaderBinding(
    const char *binding_type, const char *expected, const std::string &shader_hash,
    const MTL_SM50_SHADER_ARGUMENT &arg, bool has_buffer_binding, bool has_texture_binding,
    bool has_counter_binding, uint64_t encoder_id, const char *action = "zero"
) {
  if (!DebugShouldLogBinding(shader_hash))
    return;

  static std::atomic<uint32_t> log_count = 0;
  auto index = log_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= DebugBindingLogLimit())
    return;

  WARN(
      "DXMT diagnostic: null shader binding",
      " stage=", DebugPipelineStageName(stage),
      " kind=", DebugPipelineKindName(kind),
      " shader=", shader_hash,
      " encoder=", encoder_id,
      " binding=", binding_type,
      " expected=", expected,
      " slot=", arg.SM50BindingSlot,
      " arg_index=", GetArgumentIndex(arg.Type, arg.SM50BindingSlot),
      " struct_qword=", arg.StructurePtrOffset,
      " flags=0x", std::hex, arg.Flags, std::dec,
      " has_buffer=", has_buffer_binding,
      " has_texture=", has_texture_binding,
      " has_counter=", has_counter_binding,
      " action=", action
  );
}

template <PipelineStage stage, PipelineKind kind>
static void
DebugLogTextureBindingMismatch(
    const std::string &shader_hash, const MTL_SM50_SHADER_ARGUMENT &arg, Texture *texture, TextureViewKey view_id,
    uint64_t encoder_id
) {
  if (!DebugShouldLogBinding(shader_hash))
    return;

  auto actual_type = texture->textureType(view_id);
  bool shader_expects_ms = arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED;
  bool actual_is_ms = DebugTextureTypeIsMultisampled(actual_type);
  if (shader_expects_ms == actual_is_ms)
    return;

  static std::atomic<uint32_t> log_count = 0;
  auto index = log_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= DebugBindingLogLimit())
    return;

  WARN(
      "DXMT diagnostic: texture binding type mismatch",
      " stage=", DebugPipelineStageName(stage),
      " kind=", DebugPipelineKindName(kind),
      " shader=", shader_hash,
      " encoder=", encoder_id,
      " binding=SRV",
      " slot=", arg.SM50BindingSlot,
      " arg_index=", GetArgumentIndex(arg.Type, arg.SM50BindingSlot),
      " struct_qword=", arg.StructurePtrOffset,
      " flags=0x", std::hex, arg.Flags, std::dec,
      " shader_expects_ms=", shader_expects_ms,
      " view_is_ms=", actual_is_ms,
      " view=", uint64_t(view_id),
      " view_type=", DebugTextureTypeName(actual_type), "(", uint32_t(actual_type), ")",
      " resource_type=", DebugTextureTypeName(texture->textureType()), "(", uint32_t(texture->textureType()), ")",
      " sample_count=", texture->sampleCount(),
      " format=", uint32_t(texture->pixelFormat(view_id)),
      " size=", texture->width(view_id), "x", texture->height(view_id),
      " array_size=", texture->arrayLength(view_id),
      " action=keep_original"
  );
}

template <PipelineStage stage, PipelineKind kind>
static void
DebugLogShaderTextureBinding(
    const std::string &shader_hash, const MTL_SM50_SHADER_ARGUMENT &arg, Texture *texture, TextureViewKey view_id,
    uint64_t encoder_id
) {
  if (!DebugShouldLogBinding(shader_hash))
    return;

  static std::atomic<uint32_t> log_count = 0;
  auto index = log_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= DebugBindingLogLimit())
    return;

  INFO(
      "DXMT diagnostic: shader texture binding",
      " stage=", DebugPipelineStageName(stage),
      " kind=", DebugPipelineKindName(kind),
      " shader=", shader_hash,
      " encoder=", encoder_id,
      " binding=SRV",
      " slot=", arg.SM50BindingSlot,
      " arg_index=", GetArgumentIndex(arg.Type, arg.SM50BindingSlot),
      " struct_qword=", arg.StructurePtrOffset,
      " flags=0x", std::hex, arg.Flags, std::dec,
      " view=", uint64_t(view_id),
      " view_format=", uint32_t(texture->pixelFormat(view_id)),
      " view_type=", DebugTextureTypeName(texture->textureType(view_id)), "(", uint32_t(texture->textureType(view_id)), ")",
      " view_size=", texture->width(view_id), "x", texture->height(view_id),
      " view_array=", texture->arrayLength(view_id),
      " resource_format=", uint32_t(texture->pixelFormat()),
      " resource_type=", DebugTextureTypeName(texture->textureType()), "(", uint32_t(texture->textureType()), ")",
      " resource_size=", texture->width(), "x", texture->height(), "x", texture->depth(),
      " sample_count=", texture->sampleCount()
  );
}

DummyTextureBinding &
ArgumentEncodingContext::dummySRVTexture(const MTL_SM50_SHADER_ARGUMENT &arg) {
  auto kind = DummyTextureKindForArgument(arg);
  auto format = DummyTextureFormatForArgument(arg);
  auto &binding = dummy_srv_textures_[DummyTextureIndex(kind, format)];
  if (binding.texture)
    return binding;

  WMTTextureInfo info = {};
  info.type = DummyTextureType(kind);
  info.pixel_format = DummyTexturePixelFormat(format);
  info.usage = WMTTextureUsageShaderRead;
  info.options = WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked;
  info.width = 1;
  info.height = 1;
  info.depth = 1;
  info.mipmap_level_count = 1;
  info.sample_count = (kind == DummyTextureKind::Texture2DMultisampled ||
                       kind == DummyTextureKind::Texture2DMultisampledArray)
                          ? 2
                          : 1;
  info.array_length = DummyTextureArrayLength(kind);

  binding.texture = device_.newTexture(info);
  binding.gpu_resource_id = info.gpu_resource_id;
  binding.array_length = info.array_length;
  if (!binding.texture) {
    WARN(
        "DXMT diagnostic: failed to create dummy SRV texture",
        " type=", uint32_t(info.type),
        " format=", uint32_t(info.pixel_format),
        " sample_count=", uint32_t(info.sample_count)
    );
    return binding;
  }

  if (format != DummyTextureFormat::Depth && info.sample_count == 1) {
    std::array<uint32_t, 4> zero = {};
    uint32_t slices = info.type == WMTTextureTypeCube ? 6 : info.type == WMTTextureTypeCubeArray ? 6 * info.array_length
                                                                                                 : info.array_length;
    for (uint32_t slice = 0; slice < slices; slice++) {
      binding.texture.replaceRegion({0, 0, 0}, {1, 1, 1}, 0, slice, zero.data(), sizeof(zero), sizeof(zero));
    }
  }

  return binding;
}

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::encodeShaderResources(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t offset,
    const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings,
    uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
) {
  auto BindingCount = reflection->NumArguments;
  uint64_t *encoded_buffer = getMappedArgumentBuffer<uint64_t, stage == PipelineStage::Compute>(offset);

  auto &UAVBindingSet = stage == PipelineStage::Compute ? cs_uav_ : om_uav_;
  auto encoder_id = currentEncoderId();

  for (unsigned i = 0; i < BindingCount; i++) {
    auto arg = arguments[i];
    if constexpr (stage == PipelineStage::Pixel) {
      if (arg.Type == SM50BindingType::SRV && arg.SM50BindingSlot < 128) {
        bool demote_msaa =
            arg.SM50BindingSlot < 64
                ? (demote_msaa_srv_mask_lo & (uint64_t(1) << arg.SM50BindingSlot))
                : (demote_msaa_srv_mask_hi & (uint64_t(1) << (arg.SM50BindingSlot - 64)));
        if (demote_msaa) {
          arg.Flags = MTL_SM50_SHADER_ARGUMENT_FLAG(
              arg.Flags & ~MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED
          );
        }
      }
    }
    switch (arg.Type) {
    case SM50BindingType::ConstantBuffer: {
      DXMT_UNREACHABLE
    }
    case SM50BindingType::Sampler: {
      auto slot = 16 * unsigned(stage) + arg.SM50BindingSlot;
      auto sampler = bindings ? bindings[i].sampler : sampler_[slot].sampler.ptr();
      if (!sampler) {
        encoded_buffer[arg.StructurePtrOffset] = dummy_sampler_info_.gpu_resource_id;
        encoded_buffer[arg.StructurePtrOffset + 1] = dummy_sampler_info_.gpu_resource_id;
        encoded_buffer[arg.StructurePtrOffset + 2] = (uint64_t)std::bit_cast<uint32_t>(0.0f);
        break;
      }
      encoded_buffer[arg.StructurePtrOffset] = sampler->sampler_state_handle;
      encoded_buffer[arg.StructurePtrOffset + 1] = sampler->sampler_state_cube_handle;
      encoded_buffer[arg.StructurePtrOffset + 2] = (uint64_t)std::bit_cast<uint32_t>(sampler->lod_bias);
      break;
    }
    case SM50BindingType::SRV: {
      auto slot = 128 * unsigned(stage) + arg.SM50BindingSlot;
      auto &srv = bindings ? bindings[i].srv : resview_[slot];

      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
        if (srv.buffer.ptr()) {
          auto [srv_alloc, offset] = access<stage>(srv.buffer, srv.slice.byteOffset, srv.slice.byteLength, ResourceAccess::Read);
          encoded_buffer[arg.StructurePtrOffset] = srv_alloc->gpuAddress() + offset + srv.slice.byteOffset;
          encoded_buffer[arg.StructurePtrOffset + 1] = srv.slice.byteLength;
          makeResident<stage, kind>(srv.buffer.ptr());
        } else {
          DebugLogNullShaderBinding<stage, kind>(
              "SRV", "buffer", shader_hash, arg, bool(srv.buffer.ptr()), bool(srv.texture.ptr()), false, encoder_id
          );
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      } else if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
        if (srv.buffer.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET);
          auto [view, offset] = access<stage>(srv.buffer, srv.viewId, ResourceAccess::Read);
          encoded_buffer[arg.StructurePtrOffset] = view.gpu_resource_id;
          encoded_buffer[arg.StructurePtrOffset + 1] =
              ((uint64_t)srv.slice.elementCount << 32) | (uint64_t)(srv.slice.firstElement + offset);
          makeResident<stage, kind>(srv.buffer.ptr(), srv.viewId);
        } else if (srv.texture.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP);
          auto viewIdChecked = srv.texture->checkViewUseArray(srv.viewId, arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);
          DebugLogTextureBindingMismatch<stage, kind>(shader_hash, arg, srv.texture.ptr(), viewIdChecked, encoder_id);
          DebugLogShaderTextureBinding<stage, kind>(shader_hash, arg, srv.texture.ptr(), viewIdChecked, encoder_id);
          encoded_buffer[arg.StructurePtrOffset] =
              access<stage>(srv.texture, viewIdChecked, ResourceAccess::Read).gpuResourceID;
          encoded_buffer[arg.StructurePtrOffset + 1] = TextureMetadata(srv.texture->arrayLength(viewIdChecked), 0);
          makeResident<stage, kind>(srv.texture.ptr(), viewIdChecked);
        } else {
          auto &dummy_texture = dummySRVTexture(arg);
          DebugLogNullShaderBinding<stage, kind>(
              "SRV", "texture", shader_hash, arg, bool(srv.buffer.ptr()), bool(srv.texture.ptr()), false, encoder_id,
              dummy_texture.texture ? "dummy_texture" : "zero"
          );
          if (dummy_texture.texture) {
            encoded_buffer[arg.StructurePtrOffset] = dummy_texture.gpu_resource_id;
            encoded_buffer[arg.StructurePtrOffset + 1] = TextureMetadata(dummy_texture.array_length, 0);
            DXMT_RESOURCE_RESIDENCY requested = GetResidencyMask<kind>(stage, true, false);
            if (CheckResourceResidency(dummy_texture.residency, currentEncoderId(), requested))
              makeResident<stage, kind>(dummy_texture.texture, requested);
          } else {
            encoded_buffer[arg.StructurePtrOffset] = 0;
            encoded_buffer[arg.StructurePtrOffset + 1] = 0;
          }
        }
      }
      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
        assert(0 && "srv can not have counter associated");
      }
      break;
    }
    case SM50BindingType::UAV: {
      auto &uav = UAVBindingSet[arg.SM50BindingSlot];
      bool read = (arg.Flags >> 10) & 1, write = (arg.Flags >> 10) & 2;
      if (!read && !write) {
        read = true;
        write = true;
      }
      int access_flags = (read ? ResourceAccess::Read : 0) |
                         (write ? ResourceAccess::Write : 0) |
                         ResourceAccess::UAV;

      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
        if (uav.buffer.ptr()) {
          auto [uav_alloc, offset] = access<stage>(uav.buffer, uav.slice.byteOffset, uav.slice.byteLength, access_flags);
          encoded_buffer[arg.StructurePtrOffset] = uav_alloc->gpuAddress() + offset + uav.slice.byteOffset;
          encoded_buffer[arg.StructurePtrOffset + 1] = uav.slice.byteLength;
          makeResident<stage, kind>(uav.buffer.ptr(), read, write);
        } else {
          DebugLogNullShaderBinding<stage, kind>(
              "UAV", "buffer", shader_hash, arg, bool(uav.buffer.ptr()), bool(uav.texture.ptr()), bool(uav.counter.ptr()),
              encoder_id
          );
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      } else if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
        if (uav.buffer.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET);
          auto [view, offset] = access<stage>(uav.buffer, uav.viewId, access_flags);
          encoded_buffer[arg.StructurePtrOffset] = view.gpu_resource_id;
          encoded_buffer[arg.StructurePtrOffset + 1] =
              ((uint64_t)uav.slice.elementCount << 32) | (uint64_t)(uav.slice.firstElement + offset);
          makeResident<stage, kind>(uav.buffer.ptr(), uav.viewId, read, write);
        } else if (uav.texture.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP);
          auto viewIdChecked = uav.texture->checkViewUseArray(uav.viewId, arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);
          encoded_buffer[arg.StructurePtrOffset] = access<stage>(uav.texture, viewIdChecked, access_flags).gpuResourceID;
          encoded_buffer[arg.StructurePtrOffset + 1] = TextureMetadata(uav.texture->arrayLength(viewIdChecked), 0);
          makeResident<stage, kind>(uav.texture.ptr(), viewIdChecked, read, write);
        } else {
          DebugLogNullShaderBinding<stage, kind>(
              "UAV", "texture", shader_hash, arg, bool(uav.buffer.ptr()), bool(uav.texture.ptr()), bool(uav.counter.ptr()),
              encoder_id
          );
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      }
      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
        if (uav.counter) {
          auto [counter_alloc, offset] = access<stage>(uav.counter, 0, 4, ResourceAccess::All);
          encoded_buffer[arg.StructurePtrOffset + 2] = counter_alloc->gpuAddress() + offset;
          makeResident<stage, kind>(uav.counter.ptr(), true, true);
        } else {
          DebugLogNullShaderBinding<stage, kind>(
              "UAV_COUNTER", "counter", shader_hash, arg, bool(uav.buffer.ptr()), bool(uav.texture.ptr()),
              bool(uav.counter.ptr()), encoder_id
          );
          /*
           * potentially cause gpu pagefault, even providing a dummy buffer doesn't improve since the returned
           * counter value is likely to be used as an index to another read/write operation.
           */
          encoded_buffer[arg.StructurePtrOffset + 2] = 0;
        }
      }
      break;
    }
    }
  }

  if constexpr (stage == PipelineStage::Compute) {
    auto &cmd = encodeComputeCommand<wmtcmd_compute_setbufferoffset>();
    cmd.type = WMTComputeCommandSetBufferOffset;
    cmd.offset = getFinalArgumentBufferOffset<true>(offset);
    cmd.index = 30;
  } else {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    cmd.offset = getFinalArgumentBufferOffset(offset);
    cmd.index = 30;
    if constexpr (stage == PipelineStage::Vertex) {
      if constexpr (kind == PipelineKind::Geometry)
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
      else if constexpr (kind == PipelineKind::Tessellation) {
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
        cmd.index = 28;
      } else
        cmd.type = WMTRenderCommandSetVertexBufferOffset;
    } else if constexpr (stage == PipelineStage::Pixel) {
      cmd.type = WMTRenderCommandSetFragmentBufferOffset;
    } else if constexpr (stage == PipelineStage::Hull) {
      cmd.type = WMTRenderCommandSetObjectBufferOffset;
    } else if constexpr (stage == PipelineStage::Domain) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else if constexpr (stage == PipelineStage::Geometry) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else {
      assert(0 && "Not implemented or unreachable");
    }
  }
}

void
ArgumentEncodingContext::retainAllocation(Allocation* allocation) {
  if (allocation->checkRetained(seq_id_))
    return;
  queue_.Retain(seq_id_, allocation);
}

void
ArgumentEncodingContext::clearColor(Rc<Texture> &&texture, uint64_t viewId, unsigned arrayLength, WMTClearColor color) {
  assert(!encoder_current);
  auto encoder_info = allocate<ClearEncoderData>();
  encoder_info->type = EncoderType::Clear;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->clear_dsv = 0;
  encoder_info->color = color;
  encoder_info->array_length = arrayLength;
  encoder_info->width = texture->width();
  encoder_info->height = texture->height();
  encoder_info->sample_count = texture->sampleCount();
  encoder_current = encoder_info;

  encoder_info->attachment = access(texture, viewId, ResourceAccess::Write);
  encoder_info->level = 0;
  encoder_info->slice = 0;
  encoder_info->depth_plane = 0;
  encoder_info->stencil_depth_plane = 0;

  currentFrameStatistics().clear_pass_count++;

  endPass();
}

void
ArgumentEncodingContext::clearColor(Rc<Buffer> &&buffer, uint64_t viewId, unsigned width, WMTClearColor color) {
  assert(!encoder_current);
  auto encoder_info = allocate<ClearEncoderData>();
  encoder_info->type = EncoderType::Clear;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->clear_dsv = 0;
  encoder_info->color = color;
  encoder_info->array_length = 0;
  encoder_info->width = width;
  encoder_info->height = 1;
  encoder_info->sample_count = 1;
  encoder_current = encoder_info;

  auto [view, suballocation_offset] = access<PipelineStage::Pixel>(buffer, viewId, ResourceAccess::Write);
  if (suballocation_offset)
    WARN("ClearRenderTargetView: buffer RTV suballocation offset is not supported offset=", suballocation_offset);
  encoder_info->buffer_attachment = std::move(buffer);
  encoder_info->buffer_view_id = viewId;
  encoder_info->buffer_texture = view.texture;

  currentFrameStatistics().clear_pass_count++;

  endPass();
}

void
ArgumentEncodingContext::clearDepthStencil(
    Rc<Texture> &&texture, uint64_t viewId, unsigned arrayLength, unsigned flag, float depth, uint8_t stencil
) {
  assert(!encoder_current);
  auto encoder_info = allocate<ClearEncoderData>();
  encoder_info->type = EncoderType::Clear;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->clear_dsv = flag & DepthStencilPlanarFlags(texture->pixelFormat());
  encoder_info->depth_stencil = {depth, stencil};
  encoder_info->array_length = arrayLength;
  encoder_info->width = texture->width();
  encoder_info->height = texture->height();
  encoder_info->sample_count = texture->sampleCount();
  encoder_current = encoder_info;

  encoder_info->attachment = access(texture, viewId, ResourceAccess::Write);
  encoder_info->level = 0;
  encoder_info->slice = 0;
  encoder_info->depth_plane = 0;
  encoder_info->stencil_depth_plane = 0;

  currentFrameStatistics().clear_pass_count++;
  
  endPass();
}

void
ArgumentEncodingContext::resolveTexture(
    Rc<Texture> &&src, TextureViewKey src_view, Rc<Texture> &&dst, TextureViewKey dst_view,
    WMT::RenderPipelineState pso, std::optional<WMTScissorRect> src_rect,
    WMTOrigin dst_origin, WMTSize resolve_size
) {
  assert(!encoder_current);
  auto encoder_info = allocate<ResolveEncoderData>();
  encoder_info->type = EncoderType::Resolve;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_current = encoder_info;

  encoder_info->src = access(src, src_view, ResourceAccess::Read);
  encoder_info->dst = access(dst, dst_view, ResourceAccess::Write);
  encoder_info->pso = pso;
  encoder_info->src_rect = src_rect;
  encoder_info->dst_origin = dst_origin;
  encoder_info->resolve_size = resolve_size;

  endPass();
};

void
ArgumentEncodingContext::present(Rc<Texture> &texture, Rc<Presenter> &presenter, double after, DXMTPresentMetadata metadata) {
  present(texture, texture->fullView, presenter, after, metadata);
}

void
ArgumentEncodingContext::present(Rc<Texture> &texture, TextureViewKey view, Rc<Presenter> &presenter, double after, DXMTPresentMetadata metadata) {
  assert(!encoder_current);
  auto encoder_info = allocate<PresentData>();
  encoder_info->type = EncoderType::Present;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->presenter = presenter;
  encoder_info->after = after;
  encoder_info->metadata = metadata;

  encoder_current = encoder_info;
  TextureViewKey present_view = uint64_t(view) ? view : texture->fullView;
  encoder_info->backbuffer = access(texture, present_view, ResourceAccess::Read).texture;
  endPass();
}

void
ArgumentEncodingContext::upscale(Rc<Texture> &texture, Rc<Texture> &upscaled, Rc<SpatialScaler> &scaler) {
  assert(!encoder_current);
  auto encoder_info = allocate<SpatialUpscaleData>();
  encoder_info->type = EncoderType::SpatialUpscale;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->scaler = scaler;

  encoder_current = encoder_info;
  encoder_info->backbuffer = access(texture, texture->fullView, ResourceAccess::Read).texture;
  encoder_info->upscaled = access(upscaled, upscaled->fullView, ResourceAccess::Write).texture;
  endPass();
}

void
ArgumentEncodingContext::upscaleTemporal(
    Rc<Texture> &input, Rc<Texture> &output, Rc<Texture> &depth, Rc<Texture> &motion_vector, TextureViewKey mvViewId,
    Rc<Texture> &exposure, Rc<TemporalScaler> &scaler, const WMTFXTemporalScalerProps &props
) {
  assert(!encoder_current);
  auto encoder_info = allocate<TemporalUpscaleData>();
  encoder_info->type = EncoderType::TemporalUpscale;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->scaler = scaler;
  encoder_info->props = props;

  encoder_current = encoder_info;
  encoder_info->input = access(input, input->fullView, ResourceAccess::Read).texture;
  encoder_info->depth = access(depth, depth->fullView, ResourceAccess::Read).texture;
  encoder_info->motion_vector = access(motion_vector, mvViewId, ResourceAccess::Read).texture;
  encoder_info->output = access(output, output->fullView, ResourceAccess::Write).texture;
  if (exposure) {
    encoder_info->exposure = access(exposure, exposure->fullView, ResourceAccess::Read).texture;
  }
  endPass();
}

void
ArgumentEncodingContext::signalEvent(uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<SignalEventData>();
  encoder_info->type = EncoderType::SignalEvent;
  encoder_info->id = ~0ull;
  encoder_info->event = queue_.event;
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::signalEvent(WMT::Reference<WMT::Event> &&event, uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<SignalEventData>();
  encoder_info->type = EncoderType::SignalEvent;
  encoder_info->id = ~0ull;
  encoder_info->event = std::move(event);
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::waitEvent(WMT::Reference<WMT::Event> &&event, uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<WaitForEventData>();
  encoder_info->type = EncoderType::WaitForEvent;
  encoder_info->id = ~0ull;
  encoder_info->event = std::move(event);
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

RenderEncoderData *
ArgumentEncodingContext::startRenderPass(
    uint8_t dsv_planar_flags, uint8_t dsv_readonly_flags, uint8_t render_target_count, uint64_t encoder_argbuf_size
) {
  assert(!encoder_current);
  auto encoder_info = allocate<RenderEncoderData>();
  encoder_info->type = EncoderType::Render;
  encoder_info->encoder_id_vertex = nextEncoderId();
  encoder_info->fence_wait_vertex = {};
  encoder_info->fence_update_vertex = {encoder_info->encoder_id_vertex};
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->cmd_head.type = WMTRenderCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;
  encoder_info->dsv_planar_flags = dsv_planar_flags;
  encoder_info->dsv_readonly_flags = dsv_readonly_flags;
  encoder_info->render_target_count = render_target_count;
  auto argbuf = queue_.AllocateArgumentBuffer(seq_id_, encoder_argbuf_size);
  encoder_info->allocated_argbuf = argbuf.gpu_buffer;
  encoder_info->allocated_argbuf_offset = argbuf.offset;
  encoder_info->allocated_argbuf_size = argbuf.length;
  encoder_info->allocated_argbuf_mapping = argbuf.mapped;
  encoder_info->allocated_argbuf_needs_flush = argbuf.needs_flush;
  encoder_current = encoder_info;

  currentFrameStatistics().render_pass_count++;

  vro_state_.beginEncoder();

  return encoder_info;
}

EncoderData *
ArgumentEncodingContext::startComputePass(uint64_t encoder_argbuf_size) {
  assert(!encoder_current);
  auto encoder_info = allocate<ComputeEncoderData>();
  encoder_info->type = EncoderType::Compute;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->cmd_head.type = WMTComputeCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;
  auto argbuf = queue_.AllocateArgumentBuffer(seq_id_, encoder_argbuf_size);
  encoder_info->allocated_argbuf = argbuf.gpu_buffer;
  encoder_info->allocated_argbuf_offset = argbuf.offset;
  encoder_info->allocated_argbuf_size = argbuf.length;
  encoder_info->allocated_argbuf_mapping = argbuf.mapped;
  encoder_info->allocated_argbuf_needs_flush = argbuf.needs_flush;
  encoder_current = encoder_info;

  currentFrameStatistics().compute_pass_count++;

  return encoder_info;
}

EncoderData *
ArgumentEncodingContext::startBlitPass() {
  assert(!encoder_current);
  auto encoder_info = allocate<BlitEncoderData>();
  encoder_info->type = EncoderType::Blit;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->cmd_head.type = WMTBlitCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;
  encoder_current = encoder_info;

  currentFrameStatistics().blit_pass_count++;

  return encoder_info;
}

void
ArgumentEncodingContext::endPass() {
  assert(encoder_current);
  encoder_last->next = encoder_current;
  encoder_last = encoder_current;

  if (encoder_current->id != ~0ull) {
    if (encoder_current->type == EncoderType::Render) {
      vro_state_.endEncoder();
      auto render_encoder = static_cast<RenderEncoderData *>(encoder_current);

      if (render_encoder->depth.attachment && !(render_encoder->dsv_readonly_flags & 1))
        access<PipelineStage::Pixel>(
            render_encoder->depth.attachment->allocation->descriptor, render_encoder->depth.attachment->key,
            ResourceAccess::Write
        );
      if (render_encoder->stencil.attachment && !(render_encoder->dsv_readonly_flags & 2))
        access<PipelineStage::Pixel>(
            render_encoder->stencil.attachment->allocation->descriptor, render_encoder->stencil.attachment->key,
            ResourceAccess::Write
        );

      render_encoder->fence_wait_vertex =
          fence_locality_.collectAndSimplifyWaits(render_encoder->fence_wait_vertex, render_encoder->encoder_id_vertex);
      encoder_current->fence_wait =
          fence_locality_.collectAndSimplifyWaits(encoder_current->fence_wait, encoder_last->id, true);
    } else {
      encoder_current->fence_wait =
          fence_locality_.collectAndSimplifyWaits(encoder_current->fence_wait, encoder_last->id);
    }
  }

  encoder_current = nullptr;
  encoder_count_++;
}

std::pair<WMT::Buffer, size_t>
ArgumentEncodingContext::allocateTempBuffer(size_t size, size_t alignment) {
  return queue_.AllocateTempBuffer(seq_id_, size, alignment);
};

AllocatedTempBufferSlice
ArgumentEncodingContext::allocateTempBuffer1(size_t size, size_t alignment) {
  return queue_.AllocateTempBuffer1(seq_id_, size, alignment);
};

void
ArgumentEncodingContext::beginVisibilityResultQuery(Rc<VisibilityResultQuery> &&query) {
  query->begin(seq_id_, vro_state_.getNextReadOffset());
  active_visibility_query_count_++;
  pending_queries_.push_back(std::move(query));
}
void
ArgumentEncodingContext::endVisibilityResultQuery(Rc<VisibilityResultQuery> &&query) {
  query->end(seq_id_, vro_state_.getNextReadOffset());
  assert(active_visibility_query_count_);
  active_visibility_query_count_--;
}
void
ArgumentEncodingContext::bumpVisibilityResultOffset() {
  auto render_encoder = currentRenderEncoder();
  render_encoder->use_visibility_result = render_encoder->use_visibility_result || bool(active_visibility_query_count_);

  uint64_t offset;
  if (vro_state_.tryGetNextWriteOffset(active_visibility_query_count_, offset)) {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setvisibilitymode>();
    cmd.type = WMTRenderCommandSetVisibilityMode;
    if (~offset == 0) {
      cmd.mode = WMTVisibilityResultModeDisabled;
      cmd.offset = 0;
    } else {
      cmd.mode = WMTVisibilityResultModeCounting;
      cmd.offset = offset << 3;
    }
  }
}

FrameStatistics&
ArgumentEncodingContext::currentFrameStatistics() {
  return queue_.statistics.at(frame_id_);
}

void
ArgumentEncodingContext::sampleTimestamp(Rc<TimestampQuery> &&query) {
  assert(!encoder_current);
  if (encoder_last && encoder_last->type == EncoderType::SampleTimestamp) {
    timestamp_state_.coalesceQuery(query.ptr());
    static_cast<SampleTimestampData *>(encoder_last)->queries.push_back(std::move(query));
    return;
  }
  auto encoder_info = allocate<SampleTimestampData>();
  encoder_info->type = EncoderType::SampleTimestamp;
  encoder_info->id = ~0ull;
  encoder_info->readback_index = timestamp_state_.addQuery(query.ptr());
  encoder_info->queries = {};
  encoder_info->queries.push_back(std::move(query));

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::resolveComputePassBarrier() {
  assert(encoder_current);
  assert(encoder_current->type == EncoderType::Compute);
  auto &barrier_state = encoder_current->barrier_state;
  if (barrier_state.barrierSet & ~intrapass_barrier_control_bits_) {
    auto &cmd = encodeComputeCommand<wmtcmd_compute_memory_barrier>();
    cmd.type = WMTComputeCommandMemoryBarrier;
    cmd.scope = WMTBarrierScopeBuffers | WMTBarrierScopeTextures;
    barrier_state.barrierSet = 0;
  }
}

void
ArgumentEncodingContext::resolveRenderPassBarrier() {
  assert(encoder_current);
  assert(encoder_current->type == EncoderType::Render);
  auto &barrier_state = encoder_current->barrier_state;
  if (barrier_state.barrierPreRasterAfterFragmentSet & ~intrapass_barrier_control_bits_) {
    // TODO(barrier): encoder split
    WARN("A fragment-vertex barrier is omitted");
    barrier_state.barrierSet = 0;
    barrier_state.barrierPreRasterSet = 0;
    barrier_state.barrierFragmentAfterPreRasterSet = 0;
    barrier_state.barrierPreRasterAfterFragmentSet = 0;
    return;
  }
  // Individual barriers
  if (barrier_state.barrierSet & ~intrapass_barrier_control_bits_) {
    tile_barrier_cmd.dispatch();
    barrier_state.barrierSet = 0;
  }
  if (barrier_state.barrierPreRasterSet & ~intrapass_barrier_control_bits_) {
    auto &cmd = encodeRenderCommand<wmtcmd_render_memory_barrier>();
    cmd.type = WMTRenderCommandMemoryBarrier;
    cmd.scope = WMTBarrierScopeBuffers | WMTBarrierScopeTextures;
    cmd.stages_before = WMTRenderStagePreRaster;
    cmd.stages_after = WMTRenderStagePreRaster;
    barrier_state.barrierPreRasterSet = 0;
  }
  if (barrier_state.barrierFragmentAfterPreRasterSet & ~intrapass_barrier_control_bits_) {
    auto &cmd = encodeRenderCommand<wmtcmd_render_memory_barrier>();
    cmd.type = WMTRenderCommandMemoryBarrier;
    cmd.scope = WMTBarrierScopeBuffers | WMTBarrierScopeTextures;
    cmd.stages_before = WMTRenderStageFragment;
    cmd.stages_after = WMTRenderStagePreRaster;
    barrier_state.barrierFragmentAfterPreRasterSet = 0;
  }
}

void
ArgumentEncodingContext::$$setEncodingContext(uint64_t seq_id, uint64_t frame_id) {
  current_buffer_chunk_ = 0;
  cpu_buffer_ = cpu_buffer_chunks_[current_buffer_chunk_].ptr;
  cpu_buffer_offset_ = 0;
  seq_id_ = seq_id;
  frame_id_ = frame_id;
}

constexpr unsigned kEncoderOptimizerThreshold = 64;

static void
FlushRenderEncoderArgumentBuffer(RenderEncoderData *data) {
  if (!data->allocated_argbuf_needs_flush || !data->allocated_argbuf_size)
    return;

  data->allocated_argbuf.updateContents(
      data->allocated_argbuf_offset, data->allocated_argbuf_mapping, data->allocated_argbuf_size
  );
  data->allocated_argbuf_needs_flush = false;
}

void
ArgumentEncodingContext::appendRenderArgumentBufferBindings(
    RenderEncoderData *data, WMT::Buffer buffer, bool use_geometry,
    bool use_tessellation) {
  auto append_setbuffer = [&](WMTRenderCommandType type, uint8_t index) {
    auto cmd = reinterpret_cast<wmtcmd_render_setbuffer *>(
        allocate_cpu_heap(sizeof(wmtcmd_render_setbuffer), 16));
    cmd->type = type;
    cmd->next.set(0);
    cmd->buffer = buffer;
    cmd->offset = 0;
    cmd->index = index;
    data->cmd_tail->next.set(cmd);
    data->cmd_tail = reinterpret_cast<wmtcmd_base *>(cmd);
  };

  append_setbuffer(WMTRenderCommandSetVertexBuffer, 16);
  append_setbuffer(WMTRenderCommandSetVertexBuffer, 29);
  append_setbuffer(WMTRenderCommandSetVertexBuffer, 30);
  append_setbuffer(WMTRenderCommandSetFragmentBuffer, 29);
  append_setbuffer(WMTRenderCommandSetFragmentBuffer, 30);

  if (use_geometry || use_tessellation) {
    append_setbuffer(WMTRenderCommandSetObjectBuffer, 16);
    append_setbuffer(WMTRenderCommandSetObjectBuffer, 21);
    if (use_tessellation) {
      append_setbuffer(WMTRenderCommandSetObjectBuffer, 27);
      append_setbuffer(WMTRenderCommandSetObjectBuffer, 28);
    }
    append_setbuffer(WMTRenderCommandSetObjectBuffer, 29);
    append_setbuffer(WMTRenderCommandSetObjectBuffer, 30);
    append_setbuffer(WMTRenderCommandSetMeshBuffer, 29);
    append_setbuffer(WMTRenderCommandSetMeshBuffer, 30);
  }
}

QueryReadbacks
ArgumentEncodingContext::flushCommands(WMT::CommandBuffer cmdbuf, uint64_t seqId, uint64_t event_seq_id) {
  assert(!encoder_current);

  unsigned encoder_count = encoder_count_;
  unsigned encoder_index = 0;
  EncoderData **encoders =
      reinterpret_cast<EncoderData **>(allocate_cpu_heap(sizeof(EncoderData *) * encoder_count, alignof(EncoderData *))
      );

  {
    EncoderData *current = encoder_head.next;
    while (current) {
      encoders[encoder_index++] = current;
      current = current->next;
    }
    assert(encoder_index == encoder_count);
  }

  if (encoder_count > 1) {
    unsigned j, i;
    for (j = encoder_count - 2; j != ~0u; j--) {
      if (encoders[j]->type != EncoderType::Clear &&
          encoders[j]->type != EncoderType::Render &&
          encoders[j]->type != EncoderType::Blit)
        continue;
      for (i = j + 1; i < encoder_count; i++) {
        if (encoders[i]->type == EncoderType::Null)
          continue;
        if (checkEncoderRelation(encoders[j], encoders[i]) == DXMT_ENCODER_LIST_OP_SYNCHRONIZE)
          break;
      }
    }
  }

  QueryReadbacks readbacks{};

  if (auto count = vro_state_.reset()) {
    readbacks.visibility = std::make_unique<VisibilityResultReadback>(
        device_, seqId, count, pending_queries_
    );
  }
  std::erase_if(pending_queries_, [=](auto &query) -> bool { return query->queryEndAt() == seqId; });

  readbacks.timestamp = timestamp_state_.flush(cmdbuf);

  while (encoder_index) {
    auto current = encoders[encoder_count - encoder_index];
    switch (current->type) {
    case EncoderType::Render: {
      auto data = static_cast<RenderEncoderData *>(current);
      WMTRenderPassInfo render_pass_info;
      WMT::InitializeRenderPassInfo(render_pass_info);
      bool valid_render_pass = true;
      {
        for (unsigned i = 0; i < std::size(render_pass_info.colors); i++) {
          auto &color_data = data->colors[i];
          if (!color_data.attachment && !color_data.buffer_texture)
            continue;
          WMT::Texture color_texture;
          bool valid_color_attachment = color_data.attachment
              ? ResolveRenderPassColorAttachment(
                    "RenderPass guard: color attachment missing WMTTextureUsageRenderTarget", i, color_data.attachment,
                    color_texture
                )
              : ResolveRenderPassBufferColorAttachment(
                    "RenderPass guard: buffer color attachment missing WMTTextureUsageRenderTarget", i,
                    color_data.buffer_texture, color_texture
                );
          if (!valid_color_attachment) {
            valid_render_pass = false;
            continue;
          }
          auto &color_info = render_pass_info.colors[i];
          color_info.texture = color_texture;
          color_info.load_action = color_data.load_action;
          color_info.store_action = color_data.store_action;
          color_info.level = color_data.level;
          color_info.slice = color_data.slice;
          color_info.depth_plane = color_data.depth_plane;
          color_info.clear_color = color_data.clear_color;
          color_info.resolve_texture = color_data.resolve_attachment.texture();
          color_info.resolve_level = color_data.resolve_level;
          color_info.resolve_slice = color_data.resolve_slice;
          color_info.resolve_depth_plane = color_data.resolve_depth_plane;
        }
        if (data->depth.attachment) {
          auto &depth_info = render_pass_info.depth;
          auto &depth_data = data->depth;
          depth_info.texture = depth_data.attachment.texture();
          depth_info.load_action = depth_data.load_action;
          depth_info.store_action = depth_data.store_action;
          depth_info.level = depth_data.level;
          depth_info.slice = depth_data.slice;
          depth_info.depth_plane = depth_data.depth_plane;
          depth_info.clear_depth = depth_data.clear_depth;
        }
        if (data->stencil.attachment) {
          auto &stencil_info = render_pass_info.stencil;
          auto &stencil_data = data->stencil;
          stencil_info.texture = stencil_data.attachment.texture();
          stencil_info.load_action = stencil_data.load_action;
          stencil_info.store_action = stencil_data.store_action;
          stencil_info.level = stencil_data.level;
          stencil_info.slice = stencil_data.slice;
          stencil_info.depth_plane = stencil_data.depth_plane;
          stencil_info.clear_stencil = stencil_data.clear_stencil;
        }
        render_pass_info.default_raster_sample_count = data->default_raster_sample_count;
        render_pass_info.render_target_array_length = data->render_target_array_length;
        render_pass_info.render_target_width = data->render_target_width;
        render_pass_info.render_target_height = data->render_target_height;
      }
      if (data->use_visibility_result) {
        assert(readbacks.visibility);
        render_pass_info.visibility_buffer = readbacks.visibility->visibility_result_heap;
      }
      if (!valid_render_pass) {
        WARN("RenderPass guard: skipped unsafe render pass encoder=", data->id);
        data->~RenderEncoderData();
        break;
      }
      NormalizeRenderPassInfo(render_pass_info);
      FlushRenderEncoderArgumentBuffer(data);
      auto gpu_buffer_ = data->allocated_argbuf;
      const bool sample_render_readback = DebugShouldSampleRenderReadback(frame_id_);
      DebugEncodeRenderAttachmentReadbacks(readbacks, cmdbuf, device_, frame_id_,
                                           seqId, data, sample_render_readback,
                                           "render-color-before-pass");
      auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
      data->fence_wait.forEach(
          data->fence_wait_vertex, // if a fence is waited pre-raster, no need to wait again at fragment
          [&](auto id) { encoder.waitForFence(fence_pool_[id], WMTRenderStagePreRaster); },
          [&](auto id) { encoder.waitForFence(fence_pool_[id], WMTRenderStageFragment); }
      );
      if (data->allocated_argbuf_size) {
        encoder.setVertexBuffer(gpu_buffer_, 0, 16);
        encoder.setVertexBuffer(gpu_buffer_, 0, 29);
        encoder.setVertexBuffer(gpu_buffer_, 0, 30);
        encoder.setFragmentBuffer(gpu_buffer_, 0, 29);
        encoder.setFragmentBuffer(gpu_buffer_, 0, 30);
      }
      if ((data->use_geometry || data->use_tessellation) && data->allocated_argbuf_size) {
        encoder.setObjectBuffer(gpu_buffer_, 0, 16);
        encoder.setObjectBuffer(gpu_buffer_, 0, 21); // draw arguments
        if (data->use_tessellation) {
          encoder.setObjectBuffer(gpu_buffer_, 0, 27);
          encoder.setObjectBuffer(gpu_buffer_, 0, 28);
        }
        encoder.setObjectBuffer(gpu_buffer_, 0, 29);
        encoder.setObjectBuffer(gpu_buffer_, 0, 30);
        encoder.setMeshBuffer(gpu_buffer_, 0, 29);
        encoder.setMeshBuffer(gpu_buffer_, 0, 30);
      }
      if (data->gs_arg_marshal_tasks.size()) {
        auto task_count = data->gs_arg_marshal_tasks.size();
        struct GS_MARSHAL_TASK {
          uint64_t draw_args;
          uint64_t dispatch_args_out;
          uint64_t max_object_threadgroups;
          uint32_t vertex_count_per_warp;
          uint32_t end_of_command;
        };
        auto task_argbuf = queue_.AllocateArgumentBuffer(seq_id_, sizeof(GS_MARSHAL_TASK) * task_count);
        auto tasks_data = (GS_MARSHAL_TASK *)task_argbuf.mapped;
        for (unsigned i = 0; i<task_count; i++) {
          auto & task = data->gs_arg_marshal_tasks[i];
          tasks_data[i].draw_args = task.draw_arguments_va;
          tasks_data[i].dispatch_args_out = task.dispatch_arguments_va;
          tasks_data[i].max_object_threadgroups = task.max_object_threadgroups;
          tasks_data[i].vertex_count_per_warp = task.vertex_count_per_warp;
          tasks_data[i].end_of_command = 0;
          encoder.useResource(task.draw_arguments, WMTResourceUsageRead, WMTRenderStageVertex);
          encoder.useResource(task.dispatch_arguments_buffer, WMTResourceUsageWrite, WMTRenderStageVertex);
        }
        tasks_data[task_count - 1].end_of_command = 1;
        if (task_argbuf.needs_flush) {
          task_argbuf.gpu_buffer.updateContents(task_argbuf.offset, task_argbuf.mapped, task_argbuf.length);
        }
        emulated_cmd.MarshalGSDispatchArguments(encoder, task_argbuf.gpu_buffer, task_argbuf.offset);
      }
      if (data->ts_arg_marshal_tasks.size()) {
        auto task_count = data->ts_arg_marshal_tasks.size();
        struct TS_MARSHAL_TASK {
          uint64_t draw_args;
          uint64_t dispatch_args_out;
          uint64_t max_object_threadgroups;
          uint16_t control_point_count;
          uint16_t patch_per_group;
          uint32_t end_of_command;
        };
        auto task_argbuf = queue_.AllocateArgumentBuffer(seq_id_, sizeof(TS_MARSHAL_TASK) * task_count);
        auto tasks_data = (TS_MARSHAL_TASK *)task_argbuf.mapped;
        for (unsigned i = 0; i<task_count; i++) {
          auto & task = data->ts_arg_marshal_tasks[i];
          tasks_data[i].draw_args = task.draw_arguments_va;
          tasks_data[i].dispatch_args_out = task.dispatch_arguments_va;
          tasks_data[i].max_object_threadgroups = task.max_object_threadgroups;
          tasks_data[i].control_point_count = task.control_point_count;
          tasks_data[i].patch_per_group = task.patch_per_group;
          tasks_data[i].end_of_command = 0;
          encoder.useResource(task.draw_arguments, WMTResourceUsageRead, WMTRenderStageVertex);
          encoder.useResource(task.dispatch_arguments_buffer, WMTResourceUsageWrite, WMTRenderStageVertex);
        }
        tasks_data[task_count - 1].end_of_command = 1;
        if (task_argbuf.needs_flush) {
          task_argbuf.gpu_buffer.updateContents(task_argbuf.offset, task_argbuf.mapped, task_argbuf.length);
        }
        emulated_cmd.MarshalTSDispatchArguments(encoder, task_argbuf.gpu_buffer, task_argbuf.offset);
      }
      if (data->gs_arg_marshal_tasks.size() > 0 || data->ts_arg_marshal_tasks.size() > 0) {
        encoder.memoryBarrier(
            WMTBarrierScopeBuffers, WMTRenderStageVertex,
            WMTRenderStageVertex | WMTRenderStageMesh | WMTRenderStageObject
        );
      }
      auto command_summary = DebugSummarizeRenderCommands(&data->cmd_head);
      DebugAccumulateRenderCommands(currentFrameStatistics(), &data->cmd_head);
      DebugLogRenderPassInfo(frame_id_, seqId, data->id, data, command_summary);
      encoder.encodeCommands(&data->cmd_head);
      data->fence_update_vertex.forEach(
          data->fence_update, // if a fence is updated at fragment, no need to update again pre-raster
          [&](auto id) { encoder.updateFence(fence_pool_[id], WMTRenderStageFragment); },
          [&](auto id) { encoder.updateFence(fence_pool_[id], WMTRenderStagePreRaster); }
      );
      encoder.endEncoding();
      DebugEncodeRenderAttachmentReadbacks(readbacks, cmdbuf, device_, frame_id_,
                                           seqId, data, sample_render_readback,
                                           "render-color-after-pass");
      data->~RenderEncoderData();
      break;
    }
    case EncoderType::Compute: {
      auto data = static_cast<ComputeEncoderData *>(current);
      if (data->allocated_argbuf_needs_flush) {
        data->allocated_argbuf.updateContents(data->allocated_argbuf_offset, data->allocated_argbuf_mapping,
                                              data->allocated_argbuf_size);
      }
      auto encoder = cmdbuf.computeCommandEncoder(true);
      data->fence_wait.forEach([&](auto id) { encoder.waitForFence(fence_pool_[id]); });
      if (data->allocated_argbuf_size) {
        struct wmtcmd_compute_setbuffer setcmd;
        setcmd.type = WMTComputeCommandSetBuffer;
        setcmd.next.set(nullptr);
        setcmd.buffer = data->allocated_argbuf;
        setcmd.offset = 0;
        setcmd.index = 29;
        encoder.encodeCommands((const wmtcmd_compute_nop *)&setcmd);
        setcmd.index = 30;
        encoder.encodeCommands((const wmtcmd_compute_nop *)&setcmd);
      }
      encoder.encodeCommands(&data->cmd_head);
      data->fence_update.forEach([&](auto id) { encoder.updateFence(fence_pool_[id]); });
      encoder.endEncoding();
      data->~ComputeEncoderData();
      break;
    }
    case EncoderType::Blit: {
      auto data = static_cast<BlitEncoderData *>(current);
      auto encoder = cmdbuf.blitCommandEncoder();
      data->fence_wait.forEach([&](auto id) { encoder.waitForFence(fence_pool_[id]); });
      encoder.encodeCommands(&data->cmd_head);
      data->fence_update.forEach([&](auto id) { encoder.updateFence(fence_pool_[id]); });
      encoder.endEncoding();
      data->~BlitEncoderData();
      break;
    }
    case EncoderType::Present: {
      auto data = static_cast<PresentData *>(current);
      auto t0 = clock::now();
      currentFrameStatistics().present_pass_count++;
      uint32_t present_index = 0;
      const bool sample_present_readback = DebugShouldSamplePresentReadback(present_index);
      if (sample_present_readback) {
        DebugEncodePresentReadbacks(
            readbacks, cmdbuf, device_, data->backbuffer, "backbuffer-before-present",
            currentFrameId(), seqId, present_index);
      }
      auto drawable = data->presenter->encodeCommands(
          cmdbuf, data->backbuffer, data->metadata,
          [&](WMT::RenderCommandEncoder encoder) {
            data->fence_wait.forEach([&](auto id) { encoder.waitForFence(fence_pool_[id], WMTRenderStageFragment); });
          },
          [&](WMT::RenderCommandEncoder encoder) {
            data->fence_update.forEach([&](auto id) { encoder.updateFence(fence_pool_[id], WMTRenderStageFragment); });
          }
      );
      auto t1 = clock::now();
      auto present_encode_interval = t1 - t0;
      currentFrameStatistics().drawable_blocking_interval += present_encode_interval;
      if (sample_present_readback) {
        DebugEncodePresentReadbacks(
            readbacks, cmdbuf, device_, drawable.texture(), "drawable-after-present",
            currentFrameId(), seqId, present_index);
      }
      if (DebugEnabledEnv("DXMT_DIAG_SWAPCHAIN") || DebugMillis(present_encode_interval) > 250.0) {
        static std::atomic<uint64_t> present_diag_count = 0;
        auto index = present_diag_count.fetch_add(1, std::memory_order_relaxed);
        if (index < 64 || (index % 120) == 0 || DebugMillis(present_encode_interval) > 250.0) {
          INFO("DXMT: Present encode frame=", currentFrameId(), " backbuffer=", data->backbuffer.width(), "x",
               data->backbuffer.height(), " elapsedMs=", DebugMillis(present_encode_interval),
               " presentAfter=", data->after);
        }
      }
      if (data->after > 0)
        cmdbuf.presentDrawableAfterMinimumDuration(drawable, data->after);
      else
        cmdbuf.presentDrawable(drawable);
      data->~PresentData();
      break;
    }
    case EncoderType::Clear: {
      auto data = static_cast<ClearEncoderData *>(current);
      DebugLogClearPassInfo(frame_id_, seqId, data->id, data);
      {
        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        if (data->clear_dsv) {
          if (data->clear_dsv & 1) {
            info.depth.clear_depth = data->depth_stencil.first;
            info.depth.texture = data->attachment.texture();
            info.depth.level = data->level;
            info.depth.slice = data->slice;
            info.depth.depth_plane = data->depth_plane;
            info.depth.load_action = WMTLoadActionClear;
            info.depth.store_action = WMTStoreActionStore;
          }
          if (data->clear_dsv & 2) {
            info.stencil.clear_stencil = data->depth_stencil.second;
            info.stencil.texture = data->attachment.texture();
            info.stencil.level = data->level;
            info.stencil.slice = data->slice;
            info.stencil.depth_plane = data->stencil_depth_plane;
            info.stencil.load_action = WMTLoadActionClear;
            info.stencil.store_action = WMTStoreActionStore;
          }
          info.render_target_width = data->width;
          info.render_target_height = data->height;
        } else {
          WMT::Texture color_texture;
          bool valid_color_attachment = data->attachment
              ? ResolveRenderPassColorAttachment(
                    "ClearPass guard: color attachment missing WMTTextureUsageRenderTarget", 0, data->attachment,
                    color_texture
                )
              : ResolveRenderPassBufferColorAttachment(
                    "ClearPass guard: buffer color attachment missing WMTTextureUsageRenderTarget", 0,
                    data->buffer_texture, color_texture
                );
          if (!valid_color_attachment) {
            WARN("ClearPass guard: skipped unsafe clear pass encoder=", data->id);
            data->~ClearEncoderData();
            break;
          }
          info.colors[0].clear_color = data->color;
          info.colors[0].texture = color_texture;
          info.colors[0].level = data->level;
          info.colors[0].slice = data->slice;
          info.colors[0].depth_plane = data->depth_plane;
          info.colors[0].load_action = WMTLoadActionClear;
          info.colors[0].store_action = WMTStoreActionStore;
        }
        info.render_target_array_length = data->array_length;
        info.default_raster_sample_count = data->sample_count;
        NormalizeRenderPassInfo(info);
        auto encoder = cmdbuf.renderCommandEncoder(info);
        encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
        data->fence_wait.forEach([&](auto id) { encoder.waitForFence(fence_pool_[id], WMTRenderStageFragment); });
        data->fence_update.forEach([&](auto id) { encoder.updateFence(fence_pool_[id], WMTRenderStageFragment); });
        encoder.endEncoding();
      }
      data->~ClearEncoderData();
      break;
    }
    case EncoderType::Resolve: {
      auto data = static_cast<ResolveEncoderData *>(current);
      {
        WMT::Texture src_texture;
        if (!ResolveRenderPassColorAttachment(
                "ResolvePass guard: source attachment missing WMTTextureUsageRenderTarget", 0, data->src,
                src_texture
            )) {
          WARN("ResolvePass guard: skipped unsafe resolve pass encoder=", data->id);
          data->~ResolveEncoderData();
          break;
        }
        auto *src_allocation = data->src ? data->src->allocation : nullptr;
        auto *src_descriptor = src_allocation ? src_allocation->descriptor : nullptr;
        auto *dst_allocation = data->dst ? data->dst->allocation : nullptr;
        auto *dst_descriptor = dst_allocation ? dst_allocation->descriptor : nullptr;

        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        info.colors[0].texture = data->pso ? data->dst.texture() : src_texture;
        info.colors[0].load_action = WMTLoadActionLoad;
        info.colors[0].store_action =
            data->pso ? WMTStoreActionStore : WMTStoreActionStoreAndMultisampleResolve;
        info.colors[0].resolve_texture = data->pso ? WMT::Texture{} : data->dst.texture();
        if (dst_descriptor && data->pso) {
          info.render_target_width = dst_descriptor->width(data->dst->key);
          info.render_target_height = dst_descriptor->height(data->dst->key);
        }
        if (src_descriptor) {
          if (!info.render_target_width)
            info.render_target_width = src_descriptor->width(data->src->key);
          if (!info.render_target_height)
            info.render_target_height = src_descriptor->height(data->src->key);
          info.render_target_array_length = 1;
          info.default_raster_sample_count = data->pso ? 1 : src_descriptor->sampleCount();
        }

        NormalizeRenderPassInfo(info);
        auto encoder = cmdbuf.renderCommandEncoder(info);
        encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
        data->fence_wait.forEach([&](auto id) { encoder.waitForFence(fence_pool_[id], WMTRenderStageFragment); });
        if (data->pso) {
          struct ResolveMetadata {
            uint32_t src_origin[2];
            uint32_t dst_origin[2];
            uint32_t size[2];
          } metadata = {};
          metadata.src_origin[0] = data->src_rect ? data->src_rect->x : 0;
          metadata.src_origin[1] = data->src_rect ? data->src_rect->y : 0;
          metadata.dst_origin[0] = data->dst_origin.x;
          metadata.dst_origin[1] = data->dst_origin.y;
          metadata.size[0] = data->resolve_size.width;
          metadata.size[1] = data->resolve_size.height;
          if (!metadata.size[0])
            metadata.size[0] = info.render_target_width;
          if (!metadata.size[1])
            metadata.size[1] = info.render_target_height;
          encoder.setRenderPipelineState(data->pso);
          encoder.setFragmentTexture(src_texture, 0);
          encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
          encoder.setViewport({
              double(metadata.dst_origin[0]), double(metadata.dst_origin[1]),
              double(metadata.size[0]), double(metadata.size[1]), 0.0, 1.0});
          encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
        }
        data->fence_update.forEach([&](auto id) { encoder.updateFence(fence_pool_[id], WMTRenderStageFragment); });
        encoder.endEncoding();
      }
      data->~ResolveEncoderData();
      break;
    }
    case EncoderType::SpatialUpscale: {
      auto data = static_cast<SpatialUpscaleData *>(current);

      auto begin_scaler = cmdbuf.blitCommandEncoder();
      begin_scaler.setLabel(WMT::String::string("BeginScaler", WMTUTF8StringEncoding));
      data->fence_wait.forEach([&](auto id) { begin_scaler.waitForFence(fence_pool_[id]); });
      begin_scaler.updateFence(data->scaler->fence());
      begin_scaler.endEncoding();

      cmdbuf.encodeSpatialScale(data->scaler->scaler(), data->backbuffer, data->upscaled, data->scaler->fence());

      auto end_scaler = cmdbuf.blitCommandEncoder();
      end_scaler.waitForFence(data->scaler->fence());
      end_scaler.setLabel(WMT::String::string("EndScaler", WMTUTF8StringEncoding));
      data->fence_update.forEach([&](auto id) { end_scaler.updateFence(fence_pool_[id]); });
      end_scaler.endEncoding();

      data->~SpatialUpscaleData();
      break;
    }
    case EncoderType::SignalEvent: {
      auto data = static_cast<SignalEventData *>(current);
      cmdbuf.encodeSignalEvent(data->event, data->value);
      data->~SignalEventData();
      break;
    }
    case EncoderType::WaitForEvent: {
      auto data = static_cast<WaitForEventData *>(current);
      cmdbuf.encodeWaitForEvent(data->event, data->value);
      data->~WaitForEventData();
      break;
    }
    case EncoderType::TemporalUpscale: {
      auto data = static_cast<TemporalUpscaleData *>(current);

      auto begin_scaler = cmdbuf.blitCommandEncoder();
      begin_scaler.setLabel(WMT::String::string("BeginScaler", WMTUTF8StringEncoding));
      data->fence_wait.forEach([&](auto id) { begin_scaler.waitForFence(fence_pool_[id]); });
      begin_scaler.updateFence(data->scaler->fence());
      begin_scaler.endEncoding();

      cmdbuf.encodeTemporalScale(
          data->scaler->scaler(), data->input, data->output, data->depth, data->motion_vector, data->exposure,
          data->scaler->fence(), data->props
      );

      auto end_scaler = cmdbuf.blitCommandEncoder();
      end_scaler.waitForFence(data->scaler->fence());
      end_scaler.setLabel(WMT::String::string("EndScaler", WMTUTF8StringEncoding));
      data->fence_update.forEach([&](auto id) { end_scaler.updateFence(fence_pool_[id]); });
      end_scaler.endEncoding();
      data->~TemporalUpscaleData();
      break;
    }
    case EncoderType::SampleTimestamp: {
      auto data = static_cast<SampleTimestampData *>(current);
      if (auto readback = readbacks.timestamp.get(); readback->sampleBuffer()) {

        /**
        Since Metal driver may change the execution order of encoders, implement a "barrier" to prevent that
        FIXME: Not an elegant implementation, should get rid of it when fence-based synchronization is done
        */
        barrierOnQueue(cmdbuf);

        WMTSampleBufferAttachmentInfo sample_buffer_info{};
        sample_buffer_info.sample_buffer = readback->sampleBuffer();
        sample_buffer_info.start_of_encoder_sample_index = data->readback_index;
        sample_buffer_info.end_of_encoder_sample_index = ~0ull; /* MTLCounterDontSample */
        auto encoder = cmdbuf.blitCommandEncoderWithSampleBuffers(&sample_buffer_info, 1);
        encoder.setLabel(WMT::String::string("SampleTimestamp", WMTUTF8StringEncoding));
        {
          /**
          `sampleBufferAttachments` does not work when the blit encoder is empty, just do something
          FIXME: potential perf overhead? 
          */
          struct wmtcmd_blit_fillbuffer fill;
          fill.next.set(nullptr);
          fill.type = WMTBlitCommandFillBuffer;
          fill.buffer = dummy_cbuffer_;
          fill.offset = 0;
          fill.length = 4;
          fill.value = 0;
          MTLBlitCommandEncoder_encodeCommands(encoder, (const struct wmtcmd_base *)&fill);
        }
        encoder.endEncoding();

      } else {
        // Use timestamp from command buffer's `gpuEndTime`
      }
      data->~SampleTimestampData();
      break;
    }
    default:
      break;
    }
    encoder_index--;
  }
  encoder_head.next = nullptr;
  encoder_last = &encoder_head;
  encoder_count_ = 0;

  cmdbuf.encodeSignalEvent(queue_.event, event_seq_id);

  for (size_t i = cpu_buffer_chunks_.size() - 1; i > current_buffer_chunk_; i--) {
    if (++cpu_buffer_chunks_[i].underused_times > kEncodingContextCPUHeapLifetime) {
      cpu_buffer_chunks_.pop_back();
    }
  }

  return readbacks;
}

DXMT_ENCODER_LIST_OP
ArgumentEncodingContext::checkEncoderRelation(EncoderData *former, EncoderData *latter) {

  if (former->type == EncoderType::Null)
    return DXMT_ENCODER_LIST_OP_SWAP;
  if (latter->type == EncoderType::Null)
    return DXMT_ENCODER_LIST_OP_SWAP;
  if (former->type == EncoderType::SignalEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (latter->type == EncoderType::SignalEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (former->type == EncoderType::WaitForEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (latter->type == EncoderType::WaitForEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (former->type == EncoderType::SampleTimestamp)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (latter->type == EncoderType::SampleTimestamp)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;

  if (former->type == EncoderType::Blit && latter->type == EncoderType::Blit) {
    if (tryMergeBlitEncoders(
            reinterpret_cast<BlitEncoderData *>(former),
            reinterpret_cast<BlitEncoderData *>(latter)
        ))
      return DXMT_ENCODER_LIST_OP_SWAP;

    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  }

  while (former->type != latter->type) {
    if (former->type == EncoderType::Clear && latter->type == EncoderType::Render) {
      auto render = reinterpret_cast<RenderEncoderData *>(latter);
      auto clear = reinterpret_cast<ClearEncoderData *>(former);

      if (render->render_target_array_length != clear->array_length)
        break;

      if (clear->clear_dsv) {
        if (auto depth_attachment = isClearDepthSignatureMatched(clear, render)) {
          if (depth_attachment->load_action == WMTLoadActionLoad) {
            depth_attachment->clear_depth = clear->depth_stencil.first;
            depth_attachment->load_action = WMTLoadActionClear;
          }
          clear->clear_dsv &= ~1;
        }
        if (auto stencil_attachment = isClearStencilSignatureMatched(clear, render)) {
          if (stencil_attachment->load_action == WMTLoadActionLoad) {
            stencil_attachment->clear_stencil = clear->depth_stencil.second;
            stencil_attachment->load_action = WMTLoadActionClear;
          }
          clear->clear_dsv &= ~2;
        }
        if (clear->clear_dsv == 0) {
          render->fence_update.merge(clear->fence_update);
          render->fence_wait.merge(clear->fence_wait);
          render->fence_wait.subtract(clear->fence_update);
          currentFrameStatistics().clear_pass_optimized++;
          clear->~ClearEncoderData();
          clear->next = nullptr;
          clear->type = EncoderType::Null;
          return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
        }
      } else {
        if (auto attachment = isClearColorSignatureMatched(clear, render)) {
          if (attachment->load_action == WMTLoadActionLoad) {
            attachment->load_action = WMTLoadActionClear;
            attachment->clear_color = clear->color;
          }
          render->fence_update.merge(clear->fence_update);
          render->fence_wait.merge(clear->fence_wait);
          render->fence_wait.subtract(clear->fence_update);
          currentFrameStatistics().clear_pass_optimized++;
          clear->~ClearEncoderData();
          clear->next = nullptr;
          clear->type = EncoderType::Null;
          return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
        }
      }
      break;
    }
    if (latter->type == EncoderType::Clear && former->type == EncoderType::Render) {
      auto render = reinterpret_cast<RenderEncoderData *>(former);
      auto clear = reinterpret_cast<ClearEncoderData *>(latter);

      // DontCare can be used because it's going to be cleared anyway
      // just keep in mind DontCare != DontStore
      if (clear->clear_dsv & 1 && render->depth.attachment == clear->attachment)
        render->depth.store_action = WMTStoreActionDontCare;
      if (clear->clear_dsv & 2 && render->stencil.attachment == clear->attachment)
        render->stencil.store_action = WMTStoreActionDontCare;
    }
    if (former->type == EncoderType::Render && latter->type == EncoderType::Resolve) {
      auto render = reinterpret_cast<RenderEncoderData *>(former);
      auto resolve = reinterpret_cast<ResolveEncoderData *>(latter);
      auto result = isResolveSignatureMatched(render, resolve);
      if (result.src) {
        result.src->store_action = WMTStoreActionStoreAndMultisampleResolve;
        result.src->resolve_attachment = result.dst;
        render->fence_update.merge(resolve->fence_update);
        render->fence_wait.merge(resolve->fence_wait);
        render->fence_wait.subtract(resolve->fence_update);

        currentFrameStatistics().resolve_pass_optimized++;
        resolve->~ResolveEncoderData();
        resolve->next = nullptr;
        resolve->type = EncoderType::Null;
        return DXMT_ENCODER_LIST_OP_SWAP; // carry on (RENDER -> RESOLVE -> RESOLVE -> ...)
      }
    }
    return hasDataDependency(latter, former) ? DXMT_ENCODER_LIST_OP_SYNCHRONIZE : DXMT_ENCODER_LIST_OP_SWAP;
  }

  if (former->type == EncoderType::Render) {
    auto r1 = reinterpret_cast<RenderEncoderData *>(latter);
    auto r0 = reinterpret_cast<RenderEncoderData *>(former);

    if (isEncoderSignatureMatched(r0, r1) &&
        !r1->fence_wait_vertex.intersectedWith(r0->fence_update)) {
      for (unsigned i = 0; i < r0->render_target_count; i++) {
        auto &a0 = r0->colors[i];
        auto &a1 = r1->colors[i];
        a1.load_action = a0.load_action;
        a1.clear_color = a0.clear_color;
      }

      r1->depth.load_action = r0->depth.load_action;
      r1->depth.clear_depth = r0->depth.clear_depth;
      r1->depth.store_action = r0->depth.store_action;
      r1->depth.level = r0->depth.level;
      r1->depth.slice = r0->depth.slice;
      r1->depth.depth_plane = r0->depth.depth_plane;
      r1->stencil.load_action = r0->stencil.load_action;
      r1->stencil.clear_stencil = r0->stencil.clear_stencil;
      r1->stencil.store_action = r0->stencil.store_action;
      r1->stencil.level = r0->stencil.level;
      r1->stencil.slice = r0->stencil.slice;
      r1->stencil.depth_plane = r0->stencil.depth_plane;

      if ((void *)r0->cmd_tail != &r0->cmd_head) {
        if (r0->allocated_argbuf != r1->allocated_argbuf) {
          auto original_head = r0->cmd_head.next.get();
          auto original_tail = r0->cmd_tail;
          r0->cmd_head.next.set(nullptr);
          r0->cmd_tail = reinterpret_cast<wmtcmd_base *>(&r0->cmd_head);
          appendRenderArgumentBufferBindings(
              r0, r0->allocated_argbuf, r0->use_geometry,
              r0->use_tessellation);
          r0->cmd_tail->next.set(original_head);
          r0->cmd_tail = original_tail;
          appendRenderArgumentBufferBindings(
              r0, r1->allocated_argbuf, r1->use_geometry,
              r1->use_tessellation);
        }
        r0->cmd_tail->next.set(r1->cmd_head.next.get());
        r1->cmd_head.next.set(r0->cmd_head.next.get());
        r0->cmd_head.next.set(nullptr);
        r0->cmd_tail = (wmtcmd_base *)&r0->cmd_head;
      }
      r1->use_tessellation = r0->use_tessellation || r1->use_tessellation;
      r1->use_geometry = r0->use_geometry || r1->use_geometry;
      std::move(
        r1->gs_arg_marshal_tasks.begin(),
        r1->gs_arg_marshal_tasks.end(),
        std::back_inserter(r0->gs_arg_marshal_tasks)
      );
      std::move(
        r1->ts_arg_marshal_tasks.begin(),
        r1->ts_arg_marshal_tasks.end(),
        std::back_inserter(r0->ts_arg_marshal_tasks)
      );
      r1->gs_arg_marshal_tasks = std::move(r0->gs_arg_marshal_tasks);
      r1->ts_arg_marshal_tasks = std::move(r0->ts_arg_marshal_tasks);
      r1->use_visibility_result = r0->use_visibility_result || r1->use_visibility_result;

      r1->fence_update.merge(r0->fence_update);
      r1->fence_wait.merge(r0->fence_wait);
      r1->fence_wait.subtract(r0->fence_update);
      r1->fence_update_vertex.merge(r0->fence_update_vertex);
      r1->fence_wait_vertex.merge(r0->fence_wait_vertex);
      r1->fence_wait_vertex.subtract(r0->fence_update_vertex);

      // just in case
      r1->fence_wait.subtract(r0->fence_update_vertex);
      /* 
      r1->fence_wait_vertex.subtract(r0->fence_update);
      does not make sense
      */

      // r0's commands are prepended into r1, but r0 itself will not be encoded after this point.
      // On 32-bit builds the argument buffer writes live in a shadow allocation until explicitly flushed.
      FlushRenderEncoderArgumentBuffer(r0);

      currentFrameStatistics().render_pass_optimized++;
      r0->~RenderEncoderData();
      r0->next = nullptr;
      r0->type = EncoderType::Null;

      return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
    }
  }

  if (hasDataDependency(latter, former)) {
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  }
  return DXMT_ENCODER_LIST_OP_SWAP;
}

bool
ArgumentEncodingContext::tryMergeBlitEncoders(BlitEncoderData *former, BlitEncoderData *latter) {
  if (hasDataDependency(latter, former))
    return false;

  if ((void *)former->cmd_tail != &former->cmd_head) {
    former->cmd_tail->next.set(latter->cmd_head.next.get());
    latter->cmd_head.next.set(former->cmd_head.next.get());
    if ((void *)latter->cmd_tail == &latter->cmd_head)
      latter->cmd_tail = former->cmd_tail;
    former->cmd_head.next.set(nullptr);
    former->cmd_tail = reinterpret_cast<wmtcmd_base *>(&former->cmd_head);
  }

  latter->fence_update.merge(former->fence_update);
  latter->fence_wait.merge(former->fence_wait);
  latter->fence_wait.subtract(former->fence_update);

  former->~BlitEncoderData();
  former->next = nullptr;
  former->type = EncoderType::Null;
  return true;
}

bool
ArgumentEncodingContext::hasDataDependency(EncoderData *latter, EncoderData *former) {
  if (former->type == EncoderType::Render) {
    auto r0 = reinterpret_cast<RenderEncoderData *>(former);
    FenceSet fence_wait_r0 = r0->fence_wait.unionOf(r0->fence_wait_vertex);
    FenceSet fence_update_r0 = r0->fence_update_vertex.unionOf(r0->fence_update);
    if (latter->type == EncoderType::Render) {
      auto r1 = reinterpret_cast<RenderEncoderData *>(latter);
      FenceSet fence_wait_r1 = r1->fence_wait.unionOf(r1->fence_wait_vertex);
      FenceSet fence_update_r1 = r1->fence_update_vertex.unionOf(r1->fence_update);
      return fence_update_r0.intersectedWith(fence_wait_r1) || fence_update_r1.intersectedWith(fence_wait_r0);
    }
    return fence_update_r0.intersectedWith(latter->fence_wait) || latter->fence_update.intersectedWith(fence_wait_r0);
  }
  if (latter->type == EncoderType::Render) {
    auto r1 = reinterpret_cast<RenderEncoderData *>(latter);
    FenceSet fence_wait = r1->fence_wait.unionOf(r1->fence_wait_vertex);
    FenceSet fence_update = r1->fence_update_vertex.unionOf(r1->fence_update);
    return former->fence_update.intersectedWith(fence_wait) || fence_update.intersectedWith(former->fence_wait);
  }
  return former->fence_update.intersectedWith(latter->fence_wait) ||
         latter->fence_update.intersectedWith(former->fence_wait);
}

bool
ArgumentEncodingContext::isEncoderSignatureMatched(RenderEncoderData *r0, RenderEncoderData *r1) {
  // FIXME: it can be different?
  if (r0->render_target_count != r1->render_target_count)
    return false;
  if (r0->dsv_planar_flags != r1->dsv_planar_flags)
    return false;
  if (r0->dsv_readonly_flags != r1->dsv_readonly_flags)
    return false;
  if (r0->render_target_array_length != r1->render_target_array_length)
    return false;
  if (r0->dsv_planar_flags & 1) {
    if (r0->depth.attachment != r1->depth.attachment)
      return false;
    if (r0->depth.level != r1->depth.level || r0->depth.slice != r1->depth.slice ||
        r0->depth.depth_plane != r1->depth.depth_plane)
      return false;
    if (r0->dsv_readonly_flags & 1) {
      if (r1->depth.load_action == WMTLoadActionClear)
        return false;
    } else {
      if (r0->depth.store_action != WMTStoreActionStore)
        return false;
      if (r1->depth.load_action != WMTLoadActionLoad)
        return false;
    }
  }
  if (r0->dsv_planar_flags & 2) {
    if (r0->stencil.attachment != r1->stencil.attachment)
      return false;
    if (r0->stencil.level != r1->stencil.level || r0->stencil.slice != r1->stencil.slice ||
        r0->stencil.depth_plane != r1->stencil.depth_plane)
      return false;
    if (r0->dsv_readonly_flags & 2) {
      if (r1->stencil.load_action == WMTLoadActionClear)
        return false;
    } else {
      if (r0->stencil.store_action != WMTStoreActionStore)
        return false;
      if (r1->stencil.load_action != WMTLoadActionLoad)
        return false;
    }
  }
  for (unsigned i = 0; i < r0->render_target_count; i++) {
    auto &a0 = r0->colors[i];
    auto &a1 = r1->colors[i];
    if (a0.attachment != a1.attachment)
      return false;
    if (a0.buffer_attachment.ptr() != a1.buffer_attachment.ptr())
      return false;
    if (a0.buffer_view_id != a1.buffer_view_id)
      return false;
    if (a0.depth_plane != a1.depth_plane)
      return false;
    if (!a0.attachment && !a0.buffer_texture)
      continue;
    if (a0.store_action != WMTStoreActionStore)
      return false;
    if (a1.load_action != WMTLoadActionLoad)
      return false;
  }
  return true;
}

RenderEncoderColorAttachmentData *
ArgumentEncodingContext::isClearColorSignatureMatched(ClearEncoderData *clear, RenderEncoderData *render) {
  for (unsigned i = 0; i < render->render_target_count; i++) {
    auto &attachment = render->colors[i];
    if (attachment.attachment == clear->attachment) {
      return &attachment;
    }
    if (attachment.buffer_attachment.ptr() &&
        attachment.buffer_attachment.ptr() == clear->buffer_attachment.ptr() &&
        attachment.buffer_view_id == clear->buffer_view_id) {
      return &attachment;
    }
  }
  return nullptr;
}

RenderEncoderDepthAttachmentData *
ArgumentEncodingContext::isClearDepthSignatureMatched(ClearEncoderData *clear, RenderEncoderData *render) {
  if ((clear->clear_dsv & 1) == 0)
    return nullptr;
  if (render->depth.attachment != clear->attachment)
    return nullptr;
  if (render->depth.level != clear->level || render->depth.slice != clear->slice ||
      render->depth.depth_plane != clear->depth_plane)
    return nullptr;
  return &render->depth;
}

RenderEncoderStencilAttachmentData *
ArgumentEncodingContext::isClearStencilSignatureMatched(ClearEncoderData *clear, RenderEncoderData *render) {
  if ((clear->clear_dsv & 2) == 0)
    return nullptr;
  if (render->stencil.attachment != clear->attachment)
    return nullptr;
  if (render->stencil.level != clear->level || render->stencil.slice != clear->slice ||
      render->stencil.depth_plane != clear->stencil_depth_plane)
    return nullptr;
  return &render->stencil;
}

ArgumentEncodingContext::ResolveSignatureMatchResult
ArgumentEncodingContext::isResolveSignatureMatched(RenderEncoderData *render, ResolveEncoderData *resolve) {
  ResolveSignatureMatchResult ret{};
  for (unsigned i = 0; i < render->render_target_count; i++) {
    auto &color = render->colors[i];
    if (!color.attachment)
      continue;
    if (color.store_action != WMTStoreActionStore)
      continue;
    if (color.resolve_attachment)
      continue;
    if (color.attachment->allocation != resolve->src->allocation)
      continue;
    if (color.attachment->key == resolve->src->key) {
      ret.src = &color;
      ret.dst = resolve->dst;
      break;
    };
    auto &descriptor_src = resolve->src->allocation->descriptor;
    auto &descriptor_dst = resolve->dst->allocation->descriptor;
    auto color_format = descriptor_src->pixelFormat(color.attachment->key);
    auto view_src_in_color_format = descriptor_src->checkViewUseFormat(resolve->src->key, color_format);
    if (color.attachment->key == view_src_in_color_format) {
      auto view_dst_in_color_format = descriptor_dst->checkViewUseFormat(resolve->dst->key, color_format);
      ret.src = &color;
      ret.dst = descriptor_dst->view(view_dst_in_color_format, resolve->dst->allocation);
      break;
    }
  }
  return ret;
}

} // namespace dxmt
