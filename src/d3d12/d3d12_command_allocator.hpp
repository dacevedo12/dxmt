#pragma once

#include "d3d12_device.hpp"
#include <cstddef>
#include <d3d12.h>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace dxmt::d3d12 {

class CommandAllocatorSubmissionState final {
public:
  CommandAllocatorSubmissionState() = default;
  CommandAllocatorSubmissionState(const CommandAllocatorSubmissionState &) =
      delete;
  CommandAllocatorSubmissionState &
  operator=(const CommandAllocatorSubmissionState &) = delete;
  ~CommandAllocatorSubmissionState() noexcept = default;

  [[nodiscard]] UINT64 MarkSubmitted();
  void Complete(UINT64 serial);
  [[nodiscard]] size_t PendingCount() const;

private:
  mutable std::mutex mutex_;
  UINT64 last_submission_serial_ = 0;
  UINT64 last_completed_submission_serial_ = 0;
  std::unordered_set<UINT64> pending_submission_serials_;
};

struct SubmittedCommandAllocatorUse final {
  std::shared_ptr<CommandAllocatorSubmissionState> state;
  UINT64 serial = 0;
};

class CommandAllocator {
public:
  virtual ~CommandAllocator() = default;

  virtual IMTLD3D12Device *GetParentDevice() const = 0;
  virtual D3D12_COMMAND_LIST_TYPE GetCommandListType() const = 0;
  virtual bool BeginCommandListRecording(void *command_list) = 0;
  virtual void EndCommandListRecording(void *command_list) = 0;
  [[nodiscard]] virtual SubmittedCommandAllocatorUse
  MarkCommandListSubmitted() = 0;
};

class CommandAllocatorObject : public ID3D12CommandAllocator,
                               public CommandAllocator {
public:
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

Com<ID3D12CommandAllocator>
CreateCommandAllocator(IMTLD3D12Device *device, D3D12_COMMAND_LIST_TYPE type);

} // namespace dxmt::d3d12
