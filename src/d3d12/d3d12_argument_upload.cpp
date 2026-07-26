#include "d3d12_argument_upload.hpp"

#include "log/log.hpp"

namespace dxmt::d3d12 {

AllocatedArgumentBufferSlice
UploadArgumentBufferBytes(::dxmt::CommandQueue &queue, ArgumentEncodingContext &enc,
                          const void *bytes, size_t size, size_t alignment) {
  if (!bytes || !size)
    return {};
  // Cap pathological uploads: a corrupted freeze must not OOM/memcpy
  // multi-GB into a bad mapping (observed as AV write near 0x2bxxxxxx).
  if (size > kMaxArgumentUploadBytes) {
    ERR("DXMT bindless materialize: argument upload too large size=", size,
        " max=", kMaxArgumentUploadBytes, " seq=", enc.currentSeqId());
    return {};
  }
  auto slice =
      queue.AllocateArgumentBuffer(enc.currentSeqId(), size, alignment);
  if (!slice.valid() || slice.length < size || !slice.write(0, bytes, size)) {
    ERR("DXMT bindless materialize: AllocateArgumentBuffer failed size=", size,
        " mapped=", slice.mapped != nullptr, " gpu=", bool(slice.gpu_buffer),
        " length=", slice.length);
    return {};
  }
  slice.flush_if_needed();
  return slice;
}

AllocatedArgumentBufferSlice
UploadNativeRootTableBases(::dxmt::CommandQueue &queue, ArgumentEncodingContext &enc,
                           const std::vector<uint32_t> &root_bases) {
  if (root_bases.empty())
    return {};
  auto slice = queue.AllocateArgumentBuffer(
      enc.currentSeqId(), uint64_t(root_bases.size()) * sizeof(uint32_t));
  if (!slice.valid() ||
      !slice.write(0, root_bases.data(), size_t(slice.length)))
    return {};
  slice.flush_if_needed();
  return slice;
}

AllocatedArgumentBufferSlice UploadFrozenBindlessStageTables(
    ::dxmt::CommandQueue &queue, ArgumentEncodingContext &enc,
    const FrozenBindlessStageTables &frozen, BindlessMirrorWindow *window) {
  if (!frozen.valid)
    return {};
  AllocatedArgumentBufferSlice root_offsets;
  if (!frozen.root_offsets.empty()) {
    root_offsets = UploadArgumentBufferBytes(
        queue, enc, frozen.root_offsets.data(),
        frozen.root_offsets.size() * sizeof(uint32_t));
  }
  if (window) {
    *window = {};
    window->texture_field_pairs = frozen.texture_field_pairs;
    if (!frozen.texture_window.empty()) {
      window->texture = UploadArgumentBufferBytes(
          queue, enc, frozen.texture_window.data(),
          frozen.texture_window.size() * sizeof(uint64_t));
    }
    if (!frozen.sampler_window.empty()) {
      window->sampler = UploadArgumentBufferBytes(
          queue, enc, frozen.sampler_window.data(),
          frozen.sampler_window.size() * sizeof(uint64_t));
    }
  }
  return root_offsets;
}

} // namespace dxmt::d3d12
