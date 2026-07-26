#pragma once

// Root-parameter binding capture: turning a root CBV/SRV/UAV address or a set
// of root 32-bit constants into GraphicsBindingSnapshot entries.
//
// These helpers used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc). They only read their arguments,
// so hoisting them to dxmt::d3d12 lets them be compiled and analysed on their
// own.

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_root_signature.hpp"

#include <span>

#include <d3d12.h>

namespace dxmt {
class Resource;
}

namespace dxmt::d3d12 {

// Descriptor range kind implied by a root descriptor's record type.
[[nodiscard]] D3D12_DESCRIPTOR_RANGE_TYPE
RootDescriptorRangeType(DescriptorRecordType type);

// Diagnostic label ("root-cbv" / "root-srv" / "root-uav") for a root
// descriptor's record type.
[[nodiscard]] const char *RootDescriptorDebugKind(DescriptorRecordType type);

// Result of resolving a root descriptor's GPU virtual address into a buffer
// view. Only meaningful when BuildRootBufferDescriptor returned true.
struct RootBufferDescriptorBuild {
  Resource *resource = nullptr;
  UINT64 offset = 0;
  DescriptorRecord descriptor = {};
};

// Resolves `address` to its owning buffer resource and fills `out` with a
// whole-remaining-range buffer descriptor of `type`. Returns false (and warns,
// for misaligned root CBVs) when the address cannot be bound.
[[nodiscard]] bool
BuildRootBufferDescriptor(D3D12_GPU_VIRTUAL_ADDRESS address,
                          DescriptorRecordType type,
                          RootBufferDescriptorBuild &out);

// Appends one snapshot entry per visible stage that reflects the root
// descriptor at `root_index`, and folds each entry into the snapshot's content
// fingerprint.
void CaptureGraphicsRootDescriptorAddress(
    GraphicsBindingSnapshot &snapshot, const PipelineState &pipeline,
    UINT root_index, const RootSignatureParameter &parameter,
    DescriptorRecordType type, D3D12_GPU_VIRTUAL_ADDRESS address,
    bool compute = false);

// Appends one root-constants snapshot entry per visible stage. `values` is the
// currently set window written at `dst_offset`; the recorded constant count
// still covers the root signature's declared Num32BitValues.
void CaptureGraphicsRootConstantsValues(
    GraphicsBindingSnapshot &snapshot, const PipelineState &pipeline,
    UINT root_index, const RootSignatureParameter &parameter,
    std::span<const UINT> values, UINT dst_offset = 0, bool compute = false);

} // namespace dxmt::d3d12
