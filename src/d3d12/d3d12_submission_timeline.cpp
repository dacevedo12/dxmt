#include "d3d12_submission_timeline.hpp"

#include "dxmt_command_queue.hpp"

namespace dxmt::d3d12 {

uint64_t
PreviousFrameSeq(uint64_t current_frame_seq) noexcept {
  return current_frame_seq - 1;
}

uint64_t
CommandChunkSlot(uint64_t chunk_id) noexcept {
  return chunk_id % kCommandChunkCount;
}

uint64_t
NextChunkEventSeqId(uint64_t current_event_seq_id) noexcept {
  return current_event_seq_id + 1;
}

} // namespace dxmt::d3d12
