#pragma once

#include "Metal.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_context.hpp"

#include <optional>
#include <utility>

#include <d3d12.h>

namespace dxmt::d3d12 {

// A Metal texture-buffer view plus the element bias introduced by rounding the
// view's byte offset down to Metal's linear texture alignment.
struct BufferViewBinding {
  BufferViewKey key = 0;
  UINT firstElementBias = 0;
};

// A Metal texture plus one of its views. Falsy when either half is missing.
struct TextureViewBinding {
  Rc<Texture> texture;
  TextureViewKey view;

  explicit operator bool() const {
    return texture && uint64_t(view);
  }
};

// --- descriptor range kinds ------------------------------------------------

[[nodiscard]] DescriptorRecordType
ExpectedDescriptorTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE range_type);

[[nodiscard]] D3D12_DESCRIPTOR_HEAP_TYPE
DescriptorHeapTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE range_type);

// --- buffer slices ---------------------------------------------------------

// Whole-buffer (or [offset, offset+requested_size)) slice clamped to the
// resource width, with byte-granular element addressing.
[[nodiscard]] BufferSlice DefaultBufferSlice(Resource &resource,
                                             UINT64 offset = 0,
                                             UINT64 requested_size = 0);

// Warns and returns false when the range cannot be expressed in the legacy
// 32-bit shader ABI.
[[nodiscard]] bool ValidateLegacyBufferSliceRange(const char *binding_type,
                                                  UINT64 byte_offset,
                                                  UINT64 byte_length);

// Slice with element addressing in units of `stride`.
[[nodiscard]] BufferSlice StructuredBufferSlice(Resource &resource,
                                                UINT64 offset, UINT64 byte_size,
                                                UINT stride);

// Structured slice rebased to zero, for bindings backed by a texture-buffer
// view whose own byte offset already carries the base.
[[nodiscard]] BufferSlice TextureBufferSlice(Resource &resource, UINT64 offset,
                                             UINT64 byte_size, UINT stride);

// Resolves a GPU virtual address to its owning buffer resource; returns the
// byte offset within that resource. Callers that only need `resource` may
// discard the offset, so this is deliberately not [[nodiscard]].
UINT64
ResolveBufferGpuAddress(D3D12_GPU_VIRTUAL_ADDRESS address, Resource *&resource);

// --- buffer views ----------------------------------------------------------

// Creates a Metal texture-buffer view over [offset, offset+byte_size) of a
// buffer resource, or nullopt when the format/range cannot be represented.
[[nodiscard]] std::optional<BufferViewBinding>
CreateBufferView(WMT::Device device, Resource &resource, DXGI_FORMAT format,
                 UINT64 offset, UINT64 byte_size, WMTTextureUsage usage);

// Typeless-uint view format matching a structured buffer stride, or UNKNOWN.
[[nodiscard]] DXGI_FORMAT UintBufferViewFormatForStride(UINT stride);

[[nodiscard]] std::optional<std::pair<BufferViewBinding, BufferSlice>>
CreateShaderResourceTextureBufferBinding(WMT::Device device, Resource &resource,
                                         const DescriptorRecord &descriptor,
                                         WMTTextureUsage usage);

[[nodiscard]] std::optional<std::pair<BufferViewBinding, BufferSlice>>
CreateUnorderedAccessTextureBufferBinding(WMT::Device device,
                                          Resource &resource,
                                          const DescriptorRecord &descriptor,
                                          WMTTextureUsage usage);

// --- bindless mirror publication -------------------------------------------

// Publishes a residency target for `slot`, retiring the previous owner through
// the encoder's queue. The caller must hold mirror.AcquireLock().
[[nodiscard]] bool ReplaceDescriptorMirrorResidencyTargetForEncode(
    ArgumentEncodingContext &enc, DescriptorHeapMirror &mirror, UINT slot,
    dxmt::DescriptorSlotVersion expected_version,
    DescriptorResidencyTarget target);

// Fills one bindless mirror slot with a texture-buffer view binding.
[[nodiscard]] bool FillBindlessTextureBufferMirrorSlot(
    ArgumentEncodingContext &enc, DescriptorHeapMirror &mirror, UINT slot,
    PipelineStage stage, const Rc<Buffer> &buffer,
    const BufferViewBinding &binding, const BufferSlice &slice,
    int access_flags, dxmt::DescriptorSlotVersion expected_version);

// --- texture views ---------------------------------------------------------

[[nodiscard]] bool IsDepthStencilResourceFormat(DXGI_FORMAT format);

// Shader-read view of one depth/stencil plane of a single subresource.
[[nodiscard]] TextureViewKey
CreateDepthStencilPlaneReadView(dxmt::Texture *texture, UINT plane, UINT level,
                                UINT slice);

// Clamps/validates a texture view's mip and array ranges against the resource,
// warning with `context` when they do not fit.
[[nodiscard]] bool ValidateTextureViewRange(const char *context,
                                            TextureViewDescriptor &view,
                                            const Resource &resource);

[[nodiscard]] TextureViewBinding
CreateShaderResourceTextureView(WMT::Device device, Resource &resource,
                                const DescriptorRecord &descriptor);

[[nodiscard]] TextureViewBinding
CreateUnorderedAccessTextureView(WMT::Device device, Resource &resource,
                                 const DescriptorRecord &descriptor,
                                 bool allow_3d_slice_subrange = false);

} // namespace dxmt::d3d12
