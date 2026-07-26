#pragma once

// Installation of a Close-time compiled packet as the compatibility replay
// input.
//
// These used to be private members of class CommandQueueImpl
// (d3d12_command_queue_execute.inc). None of them reads a CommandQueueImpl
// instance member or names `this`: they only mutate the promoted ReplayState
// value type and call free helpers (GetRootSignature(),
// ResolveReplayVertexBuffer(), ResolveReplayIndexBuffer()). Hoisting them into
// dxmt::d3d12 lets them be compiled and analyzed independently of the
// ~20k-line queue translation unit.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_replay_queue_state_types.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

struct CompiledGraphicsPacket;
struct CompiledComputePacket;

// Installs a compiled packet's root arguments into the compatibility replay
// state. A compiled packet is a complete semantic state package, so this
// replaces replaying every preceding setter.
template <typename PacketT>
void ApplyCompiledRootBindings(ReplayState &state, const PacketT &packet,
                               bool compute) {
  auto &tables = compute ? state.compute_tables
                         : state.graphics_tables;
  tables.fill({});
  for (const auto &table : packet.root_tables) {
    if (table.root_parameter_index < tables.size())
      tables[table.root_parameter_index] = table.base_descriptor;
  }

  auto &constants = compute ? state.compute_root_constants
                            : state.graphics_root_constants;
  auto &cbv = compute ? state.compute_cbv_roots
                      : state.graphics_cbv_roots;
  auto &srv = compute ? state.compute_srv_roots
                      : state.graphics_srv_roots;
  auto &uav = compute ? state.compute_uav_roots
                      : state.graphics_uav_roots;
  for (auto &slot : constants)
    slot = {};
  cbv.fill({});
  srv.fill({});
  uav.fill({});
  for (const auto &entry : packet.root_constants) {
    if (entry.root_parameter_index >= constants.size())
      continue;
    auto &slot = constants[entry.root_parameter_index];
    slot.valid = true;
    slot.values = entry.values.copy();
  }
  for (const auto &entry : packet.root_descriptors) {
    if (entry.root_parameter_index >= cbv.size())
      continue;
    auto *slot = entry.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV
                     ? &cbv[entry.root_parameter_index]
                 : entry.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV
                     ? &srv[entry.root_parameter_index]
                     : &uav[entry.root_parameter_index];
    slot->valid = true;
    slot->address = entry.address;
  }
}

// Installs a compiled graphics packet as the compatibility encoder input.
void ApplyCompiledGraphicsCompatibilityState(
    ReplayState &state, const CompiledGraphicsPacket &packet);

// Installs a compiled compute packet as the compatibility encoder input.
void ApplyCompiledComputeCompatibilityState(
    ReplayState &state, const CompiledComputePacket &packet);

} // namespace dxmt::d3d12
