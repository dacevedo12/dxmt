#include "d3d12_snapshot_root_constants.hpp"

#include <algorithm>
#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

void BindRootConstantsSnapshot(::dxmt::CommandQueue &queue,
                               ArgumentEncodingContext &enc,
                               const GraphicsBindingSnapshotEntry &entry) {
  const auto constant_count =
      std::max<UINT>(entry.constant_count, UINT(entry.constants.size()));
  if (!constant_count)
    return;

  const auto byte_length = uint64_t(constant_count) * sizeof(UINT);
  auto constants = queue.AllocateArgumentBuffer(
      enc.currentSeqId(), byte_length,
      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  if (!constants.valid() || !constants.fill_zero())
    return;
  if (!entry.constants.empty()) {
    const auto nbytes = std::min<uint64_t>(
        uint64_t(entry.constants.size()) * sizeof(UINT), constants.length);
    if (!constants.write(0, entry.constants.data(), size_t(nbytes)))
      return;
  }
  constants.flush_if_needed();
  const auto gpu_address = constants.gpu_address + constants.offset;

  switch (entry.stage) {
  case PipelineStage::Compute:
    enc.bindConstantBufferDirect<PipelineStage::Compute>(
        entry.slot, constants.gpu_buffer, gpu_address, byte_length);
    break;
  case PipelineStage::Pixel:
    enc.bindConstantBufferDirect<PipelineStage::Pixel>(
        entry.slot, constants.gpu_buffer, gpu_address, byte_length);
    break;
  case PipelineStage::Geometry:
    enc.bindConstantBufferDirect<PipelineStage::Geometry>(
        entry.slot, constants.gpu_buffer, gpu_address, byte_length);
    break;
  case PipelineStage::Hull:
    enc.bindConstantBufferDirect<PipelineStage::Hull>(
        entry.slot, constants.gpu_buffer, gpu_address, byte_length);
    break;
  case PipelineStage::Domain:
    enc.bindConstantBufferDirect<PipelineStage::Domain>(
        entry.slot, constants.gpu_buffer, gpu_address, byte_length);
    break;
  case PipelineStage::Vertex:
  default:
    enc.bindConstantBufferDirect<PipelineStage::Vertex>(
        entry.slot, constants.gpu_buffer, gpu_address, byte_length);
    break;
  }
}

} // namespace dxmt::d3d12
