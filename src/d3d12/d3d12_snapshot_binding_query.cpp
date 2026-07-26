#include "d3d12_snapshot_binding_query.hpp"

#include "d3d12_descriptor_record_query.hpp"

#include "airconv_dx12_metal4.h"

namespace dxmt::d3d12 {

namespace {

// Binding slots covered by the demote mask pair (mask_lo plus mask_hi).
constexpr UINT kPixelShaderMsaaSrvDemoteSlots =
    2 * kPixelShaderMsaaSrvDemoteMaskBits;

} // namespace

const FrozenBindlessStageTables &
FrozenBindlessTablesForStage(const GraphicsBindingSnapshot &snapshot,
                             PipelineStage stage) {
  switch (stage) {
  case PipelineStage::Pixel:
    return snapshot.frozen_bindless_pixel;
  case PipelineStage::Compute:
    return snapshot.frozen_bindless_compute;
  case PipelineStage::Vertex:
  default:
    return snapshot.frozen_bindless_vertex;
  }
}

FrozenBindlessStageTables &
MutableFrozenBindlessTablesForStage(GraphicsBindingSnapshot &snapshot,
                                    PipelineStage stage) {
  switch (stage) {
  case PipelineStage::Pixel:
    return snapshot.frozen_bindless_pixel;
  case PipelineStage::Compute:
    return snapshot.frozen_bindless_compute;
  case PipelineStage::Vertex:
  default:
    return snapshot.frozen_bindless_vertex;
  }
}

std::pair<uint64_t, uint64_t>
PixelShaderSingleSampleMsaaSRVDemoteMask(
    const GraphicsBindingSnapshot &snapshot, const PipelineState &pipeline) {
  (void)pipeline;
  uint64_t mask_lo = 0;
  uint64_t mask_hi = 0;
  for (const auto &access : snapshot.native_descriptor_accesses) {
    if (access.stage != PipelineStage::Pixel ||
        access.range_type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV ||
        access.slot >= kPixelShaderMsaaSrvDemoteSlots ||
        !(access.argument.Flags &
          MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED))
      continue;
    const auto &descriptor =
        snapshot.descriptor_records->records[access.descriptor_index];
    if (!ShaderResourceViewIsMultisampledTexture(descriptor))
      SetPixelShaderMsaaSrvDemoteBit(mask_lo, mask_hi, access.slot);
  }
  if (snapshot.native_descriptor_recipe) {
    const auto &recipe_entries = snapshot.native_descriptor_recipe->entries;
    for (size_t i = 0; i < recipe_entries.size(); ++i) {
      const auto &entry = recipe_entries[i];
      if (entry.stage != static_cast<uint16_t>(PipelineStage::Pixel) ||
          entry.range_type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV ||
          snapshot.native_descriptor_indices[i] == UINT32_MAX ||
          entry.slot >= kPixelShaderMsaaSrvDemoteSlots ||
          !(entry.argument.Flags &
            MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED))
        continue;
      if (!ShaderResourceViewIsMultisampledTexture(
              SnapshotNativeDescriptor(snapshot, i)))
        SetPixelShaderMsaaSrvDemoteBit(mask_lo, mask_hi, entry.slot);
    }
  }
  for (const auto &entry : snapshot.entries) {
    if (entry.kind != GraphicsBindingSnapshotEntry::Kind::Descriptor ||
        entry.stage != PipelineStage::Pixel ||
        entry.range_type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV ||
        !entry.has_descriptor ||
        entry.slot >= kPixelShaderMsaaSrvDemoteSlots ||
        !entry.argument || !(entry.argument->Flags &
          MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED))
      continue;
    if (!ShaderResourceViewIsMultisampledTexture(
            SnapshotDescriptor(snapshot, entry)))
      SetPixelShaderMsaaSrvDemoteBit(mask_lo, mask_hi, entry.slot);
  }
  return {mask_lo, mask_hi};
}

} // namespace dxmt::d3d12
