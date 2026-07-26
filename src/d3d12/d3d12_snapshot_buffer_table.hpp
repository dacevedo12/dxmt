#pragma once

// Slot-27 bindless buffer-table bindings rebuilt from a captured
// GraphicsBindingSnapshot.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc), together with its storage struct.
// The only piece of the command queue it ever used was the Metal device handle,
// which is now passed in, so it can be compiled and analysed on its own.

#include "Metal.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "dxmt_context.hpp"

#include <array>

namespace dxmt::d3d12 {

// Constant-buffer slots the legacy shader ABI reserves per pipeline stage.
inline constexpr unsigned kSnapshotConstantBufferSlotsPerStage = 14;

// Owning storage for one packBindlessStage buffer-table view.
struct BindlessBufferTableSnapshotStorage {
  std::array<ConstantBufferBindingSnapshot,
             kSnapshotConstantBufferSlotsPerStage * kStages>
      constant_buffers = {};
  std::array<ShaderResourceBindingSnapshot, kSRVBindings * kStages> resources =
      {};

  BindlessBufferTableSnapshot view() const {
    return BindlessBufferTableSnapshot{constant_buffers.data(),
                                       resources.data()};
  }
};

// Fills the CBV/SRV/UAV buffer-table slots `want_stage` reads from the
// snapshot's compiled descriptor tables and its own captured entries.
[[nodiscard]] BindlessBufferTableSnapshotStorage
BuildSnapshotBindlessBufferTableBindings(
    WMT::Device device, const GraphicsBindingSnapshot &snapshot,
    PipelineStage want_stage);

} // namespace dxmt::d3d12
