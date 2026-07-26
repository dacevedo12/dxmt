#pragma once

#include "d3d12_replay_binding_types.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

// Full packet binding identity used to key the per-submission descriptor
// snapshot cache: binding program (or its layout fingerprint plus shader ABI),
// root signature, both descriptor heaps, and the three immutable root argument
// store identities.
template <typename Packet>
[[nodiscard]] uint64_t
HashCompiledDescriptorBindingIdentity(const Packet &packet, bool compute) {
  auto mix = [](uint64_t hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
  };
  uint64_t hash = 1469598103934665603ull;
  hash = mix(hash, compute);
  if (packet.binding_program) {
    hash = mix(hash,
               reinterpret_cast<uintptr_t>(packet.binding_program.get()));
  } else {
    hash = mix(hash, packet.pipeline.metadata.binding_layout_fingerprint);
    hash = mix(
        hash,
        static_cast<uint32_t>(packet.pipeline.metadata.shader_abi_version));
  }
  hash = mix(hash,
             reinterpret_cast<uintptr_t>(packet.pipeline.root_signature.ptr()));
  hash = mix(hash, reinterpret_cast<uintptr_t>(
                       packet.descriptor_heaps.cbv_srv_uav.ptr()));
  hash = mix(hash,
             reinterpret_cast<uintptr_t>(packet.descriptor_heaps.sampler.ptr()));
  hash = mix(hash, reinterpret_cast<uintptr_t>(packet.root_tables.identity()));
  hash = mix(hash,
             reinterpret_cast<uintptr_t>(packet.root_descriptors.identity()));
  hash = mix(hash,
             reinterpret_cast<uintptr_t>(packet.root_constants.identity()));
  return hash;
}

// Exact (non-hash) comparison of a cached snapshot against a compiled packet.
// The hash above only selects the bucket; every identity field is re-checked
// here before a snapshot may be reused.
template <typename Packet>
[[nodiscard]] bool
CompiledDescriptorBindingIdentityMatches(
    const GraphicsBindingSnapshot &snapshot, const Packet &packet,
    bool compute) {
  if (snapshot.compiled_compute != compute ||
      snapshot.compiled_binding_identity_hash !=
          HashCompiledDescriptorBindingIdentity(packet, compute) ||
      snapshot.compiled_binding_program_identity !=
          packet.binding_program.get() ||
      snapshot.root_signature.ptr() != packet.pipeline.root_signature.ptr() ||
      snapshot.cbv_srv_uav_heap.ptr() !=
          packet.descriptor_heaps.cbv_srv_uav.ptr() ||
      snapshot.sampler_heap.ptr() != packet.descriptor_heaps.sampler.ptr() ||
      snapshot.compiled_root_tables_identity !=
          packet.root_tables.identity() ||
      snapshot.compiled_root_descriptors_identity !=
          packet.root_descriptors.identity() ||
      snapshot.compiled_root_constants_identity !=
          packet.root_constants.identity())
    return false;
  return true;
}

// Replays the bounded heap change journal of every mirror the snapshot froze
// and reports whether none of the descriptor slots it captured were rewritten
// since capture. A journal that overflowed (incomplete change set) or that
// observed a used slot is permanently invalidated.
[[nodiscard]] bool
CompiledDescriptorSnapshotStillCurrent(GraphicsBindingSnapshot &snapshot);

// Normalizes the recorded slot lists (sort + unique so lookups can binary
// search) and captures a journal cursor for mirrors that had none.
void FinalizeCompiledDescriptorSnapshot(GraphicsBindingSnapshot &snapshot);

// Registers `mirror` in the snapshot's journal token array, capturing its
// current change cursor. Already-registered mirrors and null mirrors are
// ignored; the array is fixed size, so mirrors beyond its capacity are dropped.
void CaptureCompiledDescriptorJournalCursor(GraphicsBindingSnapshot &snapshot,
                                            DescriptorHeapMirror *mirror);

// Records the heap slot a captured descriptor was read from, so a later write
// to that slot invalidates the snapshot.
void RecordCompiledDescriptorJournalSlot(GraphicsBindingSnapshot &snapshot,
                                         const DescriptorRecord &descriptor);

// Bulk variant of the above for a frozen native descriptor span.
void RecordCompiledDescriptorJournalSpan(
    GraphicsBindingSnapshot &snapshot,
    const SubmittedNativeDescriptorSpan &span);

} // namespace dxmt::d3d12
