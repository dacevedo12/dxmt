#pragma once

// Copy-record geometry planning for CopyResource() and CopyTextureRegion().
//
// These decisions used to sit inline in CommandQueueImpl
// (d3d12_command_queue_copy_clear.inc). Everything up to the point where the
// blit is handed to the queue is pure record/resource geometry - subresource
// decomposition, box clipping, footprint pitches, format compatibility - so it
// is planned here and the caller keeps only the QueueBlitCommand() submission.
// The one piece of queue identity the planning reached for through `this`,
// device_->GetMTLDevice() for the DXGI format queries, is now an explicit
// parameter.

#include "Metal.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_copy_footprint.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_texture.hpp"

#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

/** How a whole-resource CopyResource() lowers. */
enum class CopyResourceKind {
  // The two resources have no compatible allocation pair; nothing to encode.
  None,
  // Both sides are buffers: copy the prefix both of them cover.
  Buffer,
  // Both sides are textures: copy `subresource_count` matching subresources.
  Texture,
};

struct CopyResourcePlan {
  CopyResourceKind kind = CopyResourceKind::None;
  // Engaged for CopyResourceKind::Buffer.
  UINT64 byte_count = 0;
  // Engaged for CopyResourceKind::Texture.
  UINT subresource_count = 0;
};

/** Materializes reserved texture allocations on the way, because CopyResource()
 *  can only tell the buffer path from the texture path once they exist. */
[[nodiscard]] CopyResourcePlan PlanCopyResource(Resource &dst, Resource &src);

/** How a texture-to-texture CopyTextureRegion() lowers. */
enum class TextureRegionCopyKind {
  // Unsupported or malformed; the reason has already been logged.
  Skip,
  // Matching Metal pixel formats: a plain texture-to-texture blit.
  Direct,
  // R32G32B32A32_UINT texels reinterpreted as 16-byte BC blocks, which Metal
  // only allows through a staging buffer.
  BlockReinterpret,
};

struct TextureRegionCopyPlan {
  TextureRegionCopyKind kind = TextureRegionCopyKind::Skip;
  Rc<Texture> dst_texture;
  Rc<Texture> src_texture;
  UINT dst_slice = 0;
  UINT dst_level = 0;
  UINT src_slice = 0;
  UINT src_level = 0;
  WMTOrigin src_origin = {};
  WMTOrigin dst_origin = {};
  WMTSize size = {};
  // Engaged for TextureRegionCopyKind::BlockReinterpret.
  BlockReinterpretCopyLayout block_layout = {};
};

/** Plans the texture-to-texture half of CopyTextureRegion(). The caller has
 *  already established that both sides own a Metal texture. Clears the
 *  destination's present-source view as a side effect, since the copy
 *  invalidates it. */
[[nodiscard]] TextureRegionCopyPlan
PlanTextureRegionCopy(WMT::Device device, Resource &dst, Resource &src,
                      const CopyTextureRegionRecord &record,
                      UINT dst_subresource, UINT src_subresource);

/** How a buffer<->texture CopyTextureRegion() lowers. */
enum class BufferTextureCopyKind {
  // Unsupported or malformed; the reason has already been logged.
  Skip,
  // Multi-plane depth/stencil, which needs the dedicated depth-stencil blit
  // encoder rather than the batched blit path.
  DepthStencilPlane,
  Blit,
};

struct BufferTextureCopyPlan {
  BufferTextureCopyKind kind = BufferTextureCopyKind::Skip;
  bool dst_is_buffer = false;
  Rc<Buffer> buffer;
  Rc<Texture> texture;
  UINT64 buffer_offset = 0;
  UINT row_pitch = 0;
  UINT image_pitch = 0;
  WMTSize size = {};
  WMTOrigin origin = {};
  UINT slice = 0;
  UINT level = 0;
  UINT plane = 0;
  // D24 is emulated, so plane 0 of a D24S8 readback takes a dedicated path.
  bool emulated_d24 = false;
  // Engaged for DepthStencilPlane readbacks only.
  TextureViewKey read_view = {};
  // Carried purely so the encode-time hang diagnostics can print them.
  DXGI_FORMAT footprint_format = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
  uint32_t texture_format = 0;
  UINT footprint_row_count = 1;
  UINT footprint_block_height = 1;
};

/** Plans the buffer<->texture half of CopyTextureRegion(). Returns Skip unless
 *  exactly one side is a buffer and the other side carries a placed footprint.
 *  Clears the texture's present-source view when the texture is written. */
[[nodiscard]] BufferTextureCopyPlan
PlanBufferTextureCopy(WMT::Device device, const CopyTextureRegionRecord &record,
                      Resource &dst, Resource &src);

} // namespace dxmt::d3d12
