// Replay-state helpers that require CommandQueueImpl's own members.
// Included inside class CommandQueueImpl; not a standalone translation unit.
//
// All of the former type definitions in this fragment now live at dxmt::d3d12
// namespace scope:
//   - d3d12_replay_pass_types.hpp             render-pass attachments, command
//                                             kinds, encoder arena, blit pass
//   - d3d12_replay_binding_types.hpp          descriptor / binding snapshot and
//                                             frozen native descriptor store
//   - d3d12_replay_compiled_payload_types.hpp compiled packet binding payloads
//   - d3d12_replay_state_types.hpp            pending query work, resolved
//                                             vertex/index buffer bindings
//   - d3d12_replay_queue_state_types.hpp      ReplayPassEncodeCommand and its
//                                             derived encode commands, the
//                                             graphics/compute pass commands
//                                             and batches, ReplayState, and
//                                             ReplayDispatchPacket
//
// The last header is the chain that names CommandQueueImpl:
// ReplayPassEncodeCommand::Encode takes a CommandQueueImpl reference,
// ReplayGraphicsPassCommand / ReplayComputePassCommand hold a shared_ptr to
// that encoder base, ReplayGraphicsPassBatch / ReplayComputePassBatch hold
// vectors of those commands, ReplayState holds both batches, and
// ReplayDispatchPacket holds an optional ReplayState. It became hoistable once
// CommandQueueImpl moved from the anonymous namespace of
// d3d12_command_queue.cpp into the named dxmt::d3d12 namespace, so a header
// forward declaration denotes the same class and `override` still matches.
//
// CloneReplayStateWithoutBatch() now lives in d3d12_replay_state_clone.hpp;
// unqualified calls resolve to dxmt::d3d12. Its last queue-side dependency was
// the perDrawSubTimers() ledger, which became a namespace-level free function
// (d3d12_replay_perf_timers.hpp).
//
// ResetReplayCommandListSemanticState() now lives in
// d3d12_replay_api_state_reset.hpp; unqualified calls resolve to dxmt::d3d12.
// Its body only wrote fields of the ReplayState it was handed; the one thing
// that kept it a member was the call to the member ResetReplayApiState, which
// moved into the same header.
//
// Nothing in this fragment still needs CommandQueueImpl's own members.
