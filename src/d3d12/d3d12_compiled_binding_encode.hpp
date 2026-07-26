#pragma once

#include "airconv_dx12_metal4.h"
#include "d3d12_command_list.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_compiled_payload_types.hpp"
#include "dxmt_context.hpp"
#include "dxmt_descriptor_revision.hpp"

#include <cstdint>
#include <type_traits>

namespace dxmt::d3d12 {

class PipelineState;
class RootSignature;

// Replays a compiled native-descriptor-table binding recipe. Each op declares
// the dirty fields (and, for root-backed argument buffers, the root slot masks)
// that make it relevant; ops whose declaration does not intersect the encoder's
// current delta are skipped. `test_telemetry` may be null.
void EncodeCompiledNativeBindingRecipe(
    ArgumentEncodingContext &enc, const CompiledNativeBindingRecipe &recipe,
    uint32_t dirty_fields, uint64_t root_table_dirty_mask,
    uint64_t root_constant_dirty_mask, uint64_t root_descriptor_dirty_mask,
    CompiledCommandTestTelemetry *test_telemetry = nullptr);

// Binds the native descriptor-table argument buffers backing one descriptor
// heap mirror. Sampler heaps only carry the sampler table; resource heaps also
// carry the buffer resource table and the buffer descriptor record table.
// A null mirror, or one whose backend is not materialized, is ignored.
inline void
EncodeNativeDescriptorMirrorTables(ArgumentEncodingContext &enc,
                                   DescriptorHeapMirror *mirror, bool compute,
                                   WMTRenderStages render_stages) {
  if (!mirror || !mirror->descriptorTableBackendReady())
    return;
  enc.bindNativeArgumentBuffer(
      mirror->descriptorTableBuffer(), 0,
      mirror->isSamplerHeap() ? DXMT12_MTL4_NATIVE_SAMPLER_HEAP_BIND_INDEX
                              : DXMT12_MTL4_NATIVE_DESCRIPTOR_HEAP_BIND_INDEX,
      compute, render_stages);
  if (mirror->isSamplerHeap())
    return;
  enc.bindNativeArgumentBuffer(
      mirror->bufferResourceTableBuffer(), 0,
      DXMT12_MTL4_NATIVE_BUFFER_RESOURCE_TABLE_BIND_INDEX, compute,
      render_stages);
  enc.bindNativeArgumentBuffer(
      mirror->bufferDescriptorRecordBuffer(), 0,
      DXMT12_MTL4_NATIVE_BUFFER_DESCRIPTOR_RECORD_BIND_INDEX, compute,
      render_stages);
}

// Binds the native descriptor-table argument buffers of both heaps referenced
// by a replay/binding state. `State` is any state exposing `cbv_srv_uav_heap`
// and `sampler_heap`.
template <typename State>
void
EncodeNativeArgumentTables(ArgumentEncodingContext &enc, const State &state,
                           bool compute, WMTRenderStages render_stages) {
  auto *resource_heap =
      dynamic_cast<DescriptorHeap *>(state.cbv_srv_uav_heap.ptr());
  auto *sampler_heap = dynamic_cast<DescriptorHeap *>(state.sampler_heap.ptr());
  if (resource_heap)
    EncodeNativeDescriptorMirrorTables(enc, resource_heap->GetMirror(), compute,
                                       render_stages);
  if (sampler_heap)
    EncodeNativeDescriptorMirrorTables(enc, sampler_heap->GetMirror(), compute,
                                       render_stages);
}

// Materializes the packet's root constants and root descriptors into the
// encoder-facing slot arrays, then overlays the frozen root-constant windows
// and root descriptors carried by the submission snapshot. `snapshot` may be
// null for packets created without a submission snapshot.
void FillCompiledBindingState(
    CompiledPacketBindingState &state,
    const std::vector<CompiledCommandRootConstants> &constants,
    const std::vector<CompiledCommandRootDescriptor> &descriptors,
    const GraphicsBindingSnapshot *snapshot = nullptr);

// Identity fingerprint of a compiled direct graphics packet's binding payload.
// A compiled binding program subsumes every layout input, so it short-circuits
// the per-component hashing.
[[nodiscard]] uint64_t BuildCompiledDirectGraphicsBindingFingerprint(
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const PipelineState *pipeline, const RootSignature *root,
    const CompiledVertexBindingRecipe *vertex_binding_recipe,
    const GraphicsBindingSnapshot *bindless_snapshot);

// Collects the pointer identities the encoder binding-state caches compare
// against. Vertex bindings only participate for graphics packets.
template <typename Packet>
[[nodiscard]] CompiledEncoderBindingIdentity
BuildCompiledEncoderBindingIdentity(const Packet &packet) {
  CompiledEncoderBindingIdentity identity = {};
  identity.program = packet.binding_program.get();
  identity.resource_heap = packet.descriptor_heaps.cbv_srv_uav.ptr();
  identity.sampler_heap = packet.descriptor_heaps.sampler.ptr();
  identity.root_tables = packet.root_tables.identity();
  identity.root_constants = packet.root_constants.identity();
  identity.root_descriptors = packet.root_descriptors.identity();
  if constexpr (std::is_same_v<Packet, CompiledGraphicsPacket>)
    identity.vertex_bindings = packet.vertex_binding_recipe.get();
  return identity;
}

// Narrows the compiled command's own binding delta against what the render
// encoder currently holds. A changed binding program forces a full rebind;
// otherwise each field is dirtied independently and its slot mask is only
// reusable while the encoder still holds the compiled command's base identity.
[[nodiscard]] CompiledBindingDelta ResolveRenderEncoderBindingDelta(
    const CompiledEncoderBindingIdentity &identity,
    const CompiledBindingDelta &compiled,
    dxmt::DescriptorContentRevision descriptor_content_revision,
    const dxmt::RenderBindingStateCache &cache);

// Compute counterpart of ResolveRenderEncoderBindingDelta. Compute packets
// carry no vertex bindings, and their identity and compiled delta both travel
// inside the payload.
[[nodiscard]] CompiledBindingDelta ResolveComputeEncoderBindingDelta(
    const CompiledDirectComputeBindingPayload &payload,
    const dxmt::ComputeBindingStateCache &cache);

} // namespace dxmt::d3d12
