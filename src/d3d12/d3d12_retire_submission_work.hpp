#pragma once

// GPU-completion handlers for the allocator / fence / present retirement
// payloads declared in d3d12_queue_work_types.hpp.
//
// These were `static` members of CommandQueueImpl only because their payload
// types were nested in it; nothing here reads the queue instance. Now that the
// payloads live at namespace scope the handlers hoist unchanged, and the queue
// keeps thin `Retire()` wrappers so the std::visit-based RetirementVisitor
// still sees one complete overload set.

#include "d3d12_queue_work_types.hpp"
#include "dxmt_command_queue.hpp"

namespace dxmt::d3d12 {

// Completes every command-allocator use recorded for the submission, which is
// what makes those allocators reusable. Runs regardless of completion status:
// a failed submission still has to release its allocators.
void RetireAllocatorWork(AllocatorRetirementWork &work,
                         GpuCompletionStatus status) noexcept;

// Publishes the signalled value on the fence. A moved-from payload holds no
// fence and is a no-op.
void RetireFenceWork(FenceRetirementWork &work,
                     GpuCompletionStatus status) noexcept;

// Releases the present state and wakes the swap-chain waitable semaphores.
void RetirePresentWork(PresentRetirementWork &work,
                       GpuCompletionStatus status) noexcept;

} // namespace dxmt::d3d12
