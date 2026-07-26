#pragma once

// Shared plumbing for the CPU-visible readback dumps that the IA / CBV draw
// diagnostics take: allocating the shared staging buffer and encoding the
// blit that fills it. Both were repeated verbatim three times inside
// d3d12_command_queue_debug_dump.inc and never touched the queue instance.

#include "Metal.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

// A shared-storage Metal buffer plus its host mapping. `buffer` is null when
// the allocation failed; the helper has already released any host memory it
// reserved in that case.
struct DiagnosticReadbackStaging {
  WMT::Buffer buffer{};
  uint8_t *mapped = nullptr;
};

[[nodiscard]] DiagnosticReadbackStaging
AllocateDiagnosticReadbackStaging(WMT::Device device, uint64_t size);

// Queues the buffer-to-buffer blit that copies `size` bytes at `src_offset` of
// `allocation` into `staging`, keeping both alive until the GPU is done.
void EncodeDiagnosticReadbackBlit(CommandChunk *chunk,
                                  const Rc<BufferAllocation> &allocation,
                                  WMT::Buffer staging, uint64_t src_offset,
                                  uint64_t size);

} // namespace dxmt::d3d12
