#pragma once

// Consistency diagnosis for a single selected descriptor: how the captured
// DescriptorRecord lines up with the shader argument that consumes it and with
// the backend payload the descriptor mirror currently holds for its slot.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_descriptor_binding.inc). It reads only the record, the
// argument and the mirror the record itself points at, so it never touches the
// command queue instance and can be compiled and analysed on its own.

#include "airconv_dx12_metal4.h"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"

#include <cstdint>
#include <string>

#include <d3d12.h>

namespace dxmt::d3d12 {

struct SelectedDescriptorConsistency {
  uint32_t flags = 0;
  bool legal_null = false;
  bool needs_fill = false;
  DescriptorBackendSlotKind expected_kind = DescriptorBackendSlotKind::Empty;
  DescriptorBackendSlotKind actual_kind = DescriptorBackendSlotKind::Empty;
  dxmt::DescriptorSlotVersion stale_version = {};
  dxmt::DescriptorSlotVersion filled_version = {};
  DescriptorTableEntry table = {};
  BufferDescriptorRecord native = {};
  DescriptorTextureSlotPayload texture = {};
  uint32_t native_diag_flags = 0;
  std::string reasons;
};

// Sets `flag` on the diagnosis and appends `reason` to its comma separated
// reason list.
void AddSelectedDescriptorReason(SelectedDescriptorConsistency &diag,
                                 uint32_t flag, const char *reason);

// Diagnoses `descriptor` against the shader `argument` it is bound to
// (`argument` may be null). A zero `flags` field means no inconsistency was
// found.
[[nodiscard]] SelectedDescriptorConsistency
DiagnoseSelectedDescriptor(const DescriptorRecord &descriptor,
                           const DXMT12_MTL4_SHADER_ARGUMENT *argument);

} // namespace dxmt::d3d12
