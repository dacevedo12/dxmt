#include "d3d12_submission_service.hpp"

#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_noexcept.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {
namespace {

class DeviceQueueWorkRequest final {
public:
  DeviceQueueWorkRequest(
      IMTLD3D12Device &device,
      std::unique_ptr<DxmtQueueSubmissionTarget> target) noexcept
      : device_(device), target_(std::move(target)) {}

  DeviceQueueWorkRequest(const DeviceQueueWorkRequest &) = delete;
  DeviceQueueWorkRequest &
  operator=(const DeviceQueueWorkRequest &) = delete;

  void Execute() noexcept {
    auto target = std::move(target_);
    if (!target) {
      Complete(false, 0);
      return;
    }
    uint64_t sequence = 0;
    Complete(target->Submit(device_, sequence), sequence);
  }

  void Cancel() noexcept {
    target_.reset();
    Complete(false, 0);
  }

  [[nodiscard]] bool Wait(uint64_t &sequence) noexcept {
    std::unique_lock lock(mutex_);
    AssertStateLocked();
    condition_.wait(lock, [this]() {
      AssertStateLocked();
      return completed_;
    });
    AssertStateLocked();
    sequence = sequence_;
    return succeeded_;
  }

private:
  // Bridges std::unique_lock and lambda bodies, which the analyzer cannot
  // follow, to the capability. Debug builds still pay for the claim: the
  // ownership record on dxmt::mutex is checked here, so an unlocked caller
  // aborts instead of silently satisfying the analyzer.
  void AssertStateLocked() const noexcept DXMT_ASSERT_CAPABILITY(mutex_) {
    mutex_.assert_held();
  }

  void Complete(bool succeeded, uint64_t sequence) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (completed_)
        return;
      succeeded_ = succeeded;
      sequence_ = sequence;
      completed_ = true;
    }
    condition_.notify_all();
  }

  IMTLD3D12Device &device_;
  std::unique_ptr<DxmtQueueSubmissionTarget> target_;
  dxmt::mutex mutex_;
  dxmt::condition_variable condition_;
  bool completed_ DXMT_GUARDED_BY(mutex_) = false;
  bool succeeded_ DXMT_GUARDED_BY(mutex_) = false;
  uint64_t sequence_ DXMT_GUARDED_BY(mutex_) = 0;
};

} // namespace

class DeviceSubmissionService::Impl final {
public:
  Impl() : worker_([this]() { WorkerMain(); }) {}

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  ~Impl() noexcept {
    std::deque<std::shared_ptr<DeviceQueueWorkRequest>> cancelled;
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
      wake_generation_++;
      cancelled.swap(device_work_);
    }
    for (const auto &request : cancelled)
      request->Cancel();
    condition_.notify_all();
    if (!worker_.joinable())
      return;
    if (dxmt::this_thread::get_id() ==
        worker_thread_id_.load(std::memory_order_acquire)) {
      ERR("D3D12CommandQueue: device submission service destroyed on its "
          "worker thread");
      std::terminate();
    }
    worker_.join_noexcept();
  }

  [[nodiscard]] bool
  RegisterQueue(std::shared_ptr<SubmissionQueueEndpoint> endpoint) {
    if (!endpoint)
      return false;
    {
      std::lock_guard lock(mutex_);
      if (closing_)
        return false;
      const auto duplicate =
          std::find_if(queues_.begin(), queues_.end(),
                       [&endpoint](const auto &candidate) {
                         return candidate.get() == endpoint.get();
                       });
      if (duplicate != queues_.end())
        return false;
      queues_.push_back(std::move(endpoint));
      wake_generation_++;
    }
    condition_.notify_one();
    return true;
  }

  void UnregisterQueue(SubmissionQueueEndpoint &endpoint) noexcept {
    std::shared_ptr<SubmissionQueueEndpoint> removed;
    {
      std::unique_lock lock(mutex_);
      AssertStateLocked();
      const auto found =
          std::find_if(queues_.begin(), queues_.end(),
                       [&endpoint](const auto &candidate) {
                         return candidate.get() == &endpoint;
                       });
      if (found != queues_.end()) {
        removed = std::move(*found);
        queues_.erase(found);
        if (queues_.empty())
          next_queue_ = 0;
        else
          next_queue_ %= queues_.size();
      }
      condition_.wait(lock, [this, &endpoint]() {
        AssertStateLocked();
        return active_queue_ != &endpoint;
      });
      AssertStateLocked();
    }
  }

  void Wake() noexcept {
    {
      std::lock_guard lock(mutex_);
      wake_generation_++;
    }
    condition_.notify_one();
  }

  [[nodiscard]] bool IsWorkerThread() const noexcept {
    return dxmt::this_thread::get_id() ==
           worker_thread_id_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool SubmitDeviceWork(
      IMTLD3D12Device &device,
      std::unique_ptr<DxmtQueueSubmissionTarget> target,
      uint64_t &sequence) {
    if (!target)
      return false;
    if (IsWorkerThread())
      return target->Submit(device, sequence);

    auto request = std::make_shared<DeviceQueueWorkRequest>(
        device, std::move(target));
    {
      std::lock_guard lock(mutex_);
      if (closing_)
        return false;
      device_work_.push_back(request);
      wake_generation_++;
    }
    condition_.notify_one();
    return request->Wait(sequence);
  }

private:
  // Bridges std::unique_lock and lambda bodies, which the analyzer cannot
  // follow, to the capability. Debug builds still pay for the claim: the
  // ownership record on dxmt::mutex is checked here, so an unlocked caller
  // aborts instead of silently satisfying the analyzer.
  void AssertStateLocked() const noexcept DXMT_ASSERT_CAPABILITY(mutex_) {
    mutex_.assert_held();
  }

  [[nodiscard]] std::vector<std::shared_ptr<SubmissionQueueEndpoint>>
  SnapshotQueues() {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<SubmissionQueueEndpoint>> snapshot;
    if (queues_.empty())
      return snapshot;
    snapshot.reserve(queues_.size());
    const size_t start = next_queue_ % queues_.size();
    for (size_t offset = 0; offset < queues_.size(); ++offset)
      snapshot.push_back(queues_[(start + offset) % queues_.size()]);
    next_queue_ = (start + 1) % queues_.size();
    return snapshot;
  }

  [[nodiscard]] bool
  ProcessQueue(const std::shared_ptr<SubmissionQueueEndpoint> &endpoint) {
    {
      std::lock_guard lock(mutex_);
      if (closing_)
        return false;
      const auto registered =
          std::find_if(queues_.begin(), queues_.end(),
                       [&endpoint](const auto &candidate) {
                         return candidate.get() == endpoint.get();
                       });
      if (registered == queues_.end())
        return false;
      active_queue_ = endpoint.get();
    }

    const bool processed = endpoint->ProcessReadySubmission();

    {
      std::lock_guard lock(mutex_);
      if (active_queue_ == endpoint.get())
        active_queue_ = nullptr;
    }
    condition_.notify_all();
    return processed;
  }

  [[nodiscard]] bool ProcessDeviceWork() noexcept {
    std::shared_ptr<DeviceQueueWorkRequest> request;
    {
      std::lock_guard lock(mutex_);
      if (closing_ || device_work_.empty())
        return false;
      request = std::move(device_work_.front());
      device_work_.pop_front();
    }
    request->Execute();
    return true;
  }

  void WorkerLoop() {
    env::setThreadName("dxmt-d3d12-sequencer");
    worker_thread_id_.store(dxmt::this_thread::get_id(),
                            std::memory_order_release);
    uint64_t observed_generation = 0;
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        AssertStateLocked();
        condition_.wait(lock, [this, observed_generation]() {
          AssertStateLocked();
          return closing_ || wake_generation_ != observed_generation;
        });
        AssertStateLocked();
        if (closing_)
          return;
        observed_generation = wake_generation_;
      }

      bool made_progress = false;
      do {
        made_progress = ProcessDeviceWork();
        for (const auto &endpoint : SnapshotQueues())
          made_progress = ProcessQueue(endpoint) || made_progress;
      } while (made_progress);
    }
  }

  void WorkerMain() noexcept {
    if (!dxmt::invokeNoexcept("D3D12 device submission service",
                              [this]() { WorkerLoop(); }))
      std::terminate();
  }

  dxmt::mutex mutex_;
  dxmt::condition_variable condition_;
  std::vector<std::shared_ptr<SubmissionQueueEndpoint>> queues_
      DXMT_GUARDED_BY(mutex_);
  std::deque<std::shared_ptr<DeviceQueueWorkRequest>> device_work_
      DXMT_GUARDED_BY(mutex_);
  SubmissionQueueEndpoint *active_queue_ DXMT_GUARDED_BY(mutex_) = nullptr;
  size_t next_queue_ DXMT_GUARDED_BY(mutex_) = 0;
  uint64_t wake_generation_ DXMT_GUARDED_BY(mutex_) = 1;
  bool closing_ DXMT_GUARDED_BY(mutex_) = false;
  std::atomic<uint32_t> worker_thread_id_{0};
  dxmt::thread worker_;
};

DeviceSubmissionService::DeviceSubmissionService()
    : impl_(std::make_unique<Impl>()) {}

DeviceSubmissionService::~DeviceSubmissionService() noexcept = default;

bool DeviceSubmissionService::RegisterQueue(
    std::shared_ptr<SubmissionQueueEndpoint> endpoint) {
  return impl_->RegisterQueue(std::move(endpoint));
}

void DeviceSubmissionService::UnregisterQueue(
    SubmissionQueueEndpoint &endpoint) noexcept {
  impl_->UnregisterQueue(endpoint);
}

void DeviceSubmissionService::Wake() noexcept {
  impl_->Wake();
}

bool DeviceSubmissionService::IsWorkerThread() const noexcept {
  return impl_->IsWorkerThread();
}

bool DeviceSubmissionService::SubmitDeviceWork(
    IMTLD3D12Device &device,
    std::unique_ptr<DxmtQueueSubmissionTarget> target,
    uint64_t &submitted_sequence) {
  return impl_->SubmitDeviceWork(device, std::move(target),
                                 submitted_sequence);
}

} // namespace dxmt::d3d12
