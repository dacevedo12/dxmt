#include "dxmt_d3d12_submission_model.hpp"

#include <algorithm>
#include <limits>

namespace dxmt::d3d12::submission {
namespace {

struct ReadinessVisitor final {
  [[nodiscard]] PacketReadiness operator()(const ExecutePacket &) const noexcept {
    return PacketReadiness::Ready;
  }

  [[nodiscard]] PacketReadiness
  operator()(const QueueWorkPacket &) const noexcept {
    return PacketReadiness::Ready;
  }

  [[nodiscard]] PacketReadiness
  operator()(const SignalPacket &packet) const noexcept {
    return packet.fence ? PacketReadiness::Ready
                        : PacketReadiness::ExpiredDependency;
  }

  [[nodiscard]] PacketReadiness operator()(const WaitPacket &packet) const noexcept {
    const auto fence = packet.fence.lock();
    if (!fence)
      return PacketReadiness::ExpiredDependency;
    return fence->hasReached(packet.value) ? PacketReadiness::Ready
                                          : PacketReadiness::Blocked;
  }

  [[nodiscard]] PacketReadiness operator()(const StopPacket &) const noexcept {
    return PacketReadiness::Stop;
  }
};

struct RetirementVisitor final {
  bool complete;

  void operator()(AllocatorRetirement &action) const {
    if (action.state)
      action.state->retire(action.serial);
  }

  void operator()(FenceRetirement &action) const noexcept {
    if (!action.state)
      return;
    if (complete)
      action.state->publish(action.value);
    else
      action.state->fail();
  }

  void operator()(ResourceRetirement &action) const noexcept {
    if (action.state)
      action.state->retire(complete);
  }

  void operator()(ReadbackRetirement &action) const noexcept {
    if (action.state)
      action.state->retire(complete);
  }

  void operator()(PresentRetirement &action) const noexcept {
    if (action.state)
      action.state->retire(action.value, complete);
  }
};

} // namespace

uint64_t FenceLeafState::completedValue() const noexcept {
  return completed_value_.load(std::memory_order_acquire);
}

bool FenceLeafState::hasReached(uint64_t value) const noexcept {
  return failed_.load(std::memory_order_acquire) ||
         completedValue() >= value;
}

void FenceLeafState::publish(uint64_t value) noexcept {
  uint64_t current = completed_value_.load(std::memory_order_relaxed);
  while (current < value &&
         !completed_value_.compare_exchange_weak(
             current, value, std::memory_order_release,
             std::memory_order_relaxed)) {
  }
}

void FenceLeafState::fail() noexcept {
  failed_.store(true, std::memory_order_release);
}

bool FenceLeafState::failed() const noexcept {
  return failed_.load(std::memory_order_acquire);
}

void AllocatorLeafState::markSubmitted(SubmissionSerial serial) {
  StateLock lock(mutex_);
  const auto duplicate = std::find(pending_.begin(), pending_.end(), serial);
  if (duplicate == pending_.end())
    pending_.push_back(serial);
}

void AllocatorLeafState::retire(SubmissionSerial serial) {
  StateLock lock(mutex_);
  const auto found = std::find(pending_.begin(), pending_.end(), serial);
  if (found != pending_.end())
    pending_.erase(found);
}

size_t AllocatorLeafState::pendingCount() const {
  StateLock lock(mutex_);
  return pending_.size();
}

void ResourceLeafState::retire(bool complete) noexcept {
  status_.store(complete ? CompletionStatus::Complete
                         : CompletionStatus::Failed,
                std::memory_order_release);
}

CompletionStatus ResourceLeafState::status() const noexcept {
  return status_.load(std::memory_order_acquire);
}

void ReadbackLeafState::retire(bool complete) noexcept {
  status_.store(complete ? CompletionStatus::Complete
                         : CompletionStatus::Failed,
                std::memory_order_release);
}

CompletionStatus ReadbackLeafState::status() const noexcept {
  return status_.load(std::memory_order_acquire);
}

void PresentLeafState::retire(uint64_t value, bool complete) noexcept {
  if (complete)
    completed_value_.store(value, std::memory_order_relaxed);
  status_.store(complete ? CompletionStatus::Complete
                         : CompletionStatus::Failed,
                std::memory_order_release);
}

uint64_t PresentLeafState::completedValue() const noexcept {
  return completed_value_.load(std::memory_order_acquire);
}

CompletionStatus PresentLeafState::status() const noexcept {
  return status_.load(std::memory_order_acquire);
}

bool LogicalQueueState::enqueue(D3D12SubmissionPacket packet) {
  if (packet.queue() != id_)
    return false;
  StateLock lock(mutex_);
  if (close_state_ != QueueCloseState::Open)
    return false;
  pending_.push_back(std::move(packet));
  return true;
}

PacketReadiness LogicalQueueState::frontReadinessLocked() const {
  if (pending_.empty())
    return PacketReadiness::Blocked;
  return std::visit(ReadinessVisitor{}, pending_.front().payload());
}

PacketSelection LogicalQueueState::tryTakeReady() {
  StateLock lock(mutex_);
  const auto readiness = frontReadinessLocked();
  if (readiness == PacketReadiness::Blocked)
    return {readiness, std::nullopt};

  D3D12SubmissionPacket packet = std::move(pending_.front());
  pending_.pop_front();
  return {readiness, std::move(packet)};
}

std::vector<D3D12SubmissionPacket> LogicalQueueState::beginClose() {
  StateLock lock(mutex_);
  if (close_state_ == QueueCloseState::Joined)
    return {};
  if (close_state_ == QueueCloseState::Open)
    close_state_ = QueueCloseState::Closing;

  std::vector<D3D12SubmissionPacket> cancelled;
  cancelled.reserve(pending_.size());
  while (!pending_.empty()) {
    cancelled.push_back(std::move(pending_.front()));
    pending_.pop_front();
  }
  close_state_ = QueueCloseState::Draining;
  return cancelled;
}

void LogicalQueueState::markDraining() {
  StateLock lock(mutex_);
  if (close_state_ == QueueCloseState::Open ||
      close_state_ == QueueCloseState::Closing)
    close_state_ = QueueCloseState::Draining;
}

void LogicalQueueState::markJoined() {
  StateLock lock(mutex_);
  close_state_ = QueueCloseState::Joined;
}

QueueCloseState LogicalQueueState::closeState() const {
  StateLock lock(mutex_);
  return close_state_;
}

size_t LogicalQueueState::pendingCount() const {
  StateLock lock(mutex_);
  return pending_.size();
}

bool SubmissionSequencer::registerQueue(
    const std::shared_ptr<LogicalQueueState> &queue) {
  if (!queue)
    return false;

  StateLock lock(mutex_);
  for (auto iterator = queues_.begin(); iterator != queues_.end();) {
    const auto existing = iterator->lock();
    if (!existing) {
      iterator = queues_.erase(iterator);
      continue;
    }
    if (existing->id() == queue->id())
      return false;
    ++iterator;
  }
  queues_.push_back(queue);
  return true;
}

void SubmissionSequencer::unregisterQueue(LogicalQueueId id) {
  StateLock lock(mutex_);
  std::erase_if(queues_, [id](const auto &candidate) {
    const auto queue = candidate.lock();
    return !queue || queue->id() == id;
  });
  if (queues_.empty())
    next_queue_ = 0;
  else
    next_queue_ %= queues_.size();
}

PacketSelection SubmissionSequencer::takeReady() {
  std::vector<std::shared_ptr<LogicalQueueState>> queues;
  size_t start = 0;
  {
    StateLock lock(mutex_);
    std::erase_if(queues_, [](const auto &candidate) {
      return candidate.expired();
    });
    if (queues_.empty())
      return {};
    queues.reserve(queues_.size());
    for (const auto &candidate : queues_) {
      if (auto queue = candidate.lock())
        queues.push_back(std::move(queue));
    }
    if (queues.empty())
      return {};
    start = next_queue_ % queues.size();
    next_queue_ = (start + 1) % queues.size();
  }

  for (size_t offset = 0; offset < queues.size(); ++offset) {
    const size_t index = (start + offset) % queues.size();
    auto selection = queues[index]->tryTakeReady();
    if (selection.readiness != PacketReadiness::Blocked)
      return selection;
  }
  return {};
}

size_t SubmissionSequencer::queueCount() const {
  StateLock lock(mutex_);
  return static_cast<size_t>(std::count_if(
      queues_.begin(), queues_.end(),
      [](const auto &candidate) { return !candidate.expired(); }));
}

void GpuRetirementRecord::retire(bool complete) {
  for (auto &payload : payloads_)
    std::visit(RetirementVisitor{complete}, payload);
  payloads_.clear();
}

bool RetirementQueue::push(GpuRetirementRecord record) {
  StateLock lock(mutex_);
  if (record.token().value() <= last_retired_.value())
    return false;
  if (!pending_.empty() &&
      record.token().value() <= pending_.back().token().value())
    return false;
  pending_.push_back(std::move(record));
  return true;
}

size_t RetirementQueue::retireThrough(SubmissionToken token,
                                      bool complete) {
  size_t retired = 0;
  for (;;) {
    std::optional<GpuRetirementRecord> ready;
    {
      StateLock lock(mutex_);
      if (pending_.empty() ||
          pending_.front().token().value() > token.value())
        break;
      ready.emplace(std::move(pending_.front()));
      pending_.pop_front();
      last_retired_ = ready->token();
    }
    ready->retire(complete);
    ++retired;
  }
  return retired;
}

size_t RetirementQueue::failAll() {
  size_t retired = 0;
  for (;;) {
    std::optional<GpuRetirementRecord> failed;
    {
      StateLock lock(mutex_);
      if (pending_.empty())
        break;
      failed.emplace(std::move(pending_.front()));
      pending_.pop_front();
      last_retired_ = failed->token();
    }
    failed->retire(false);
    ++retired;
  }
  return retired;
}

size_t RetirementQueue::pendingCount() const {
  StateLock lock(mutex_);
  return pending_.size();
}

SubmissionToken RetirementQueue::lastRetiredToken() const {
  StateLock lock(mutex_);
  return last_retired_;
}

ExecuteResult SubmissionExecutor::executeOne(
    ReplayBackend &backend, BackendThreadCapability &backend_thread) {
  if (!backend_thread.isCurrentThread())
    return {ExecuteStatus::Failed, {}};

  auto selection = sequencer_.takeReady();
  if (!selection.packet)
    return {ExecuteStatus::Idle, {}};
  if (selection.readiness == PacketReadiness::Stop)
    return {ExecuteStatus::Stop, {}};
  if (selection.readiness == PacketReadiness::ExpiredDependency)
    return {ExecuteStatus::Failed, {}};

  const SubmissionToken token = next_token_;
  auto result =
      backend.submit(std::move(*selection.packet), token, backend_thread);
  if (auto *submitted = std::get_if<BackendSubmitted>(&result)) {
    if (!retirement_.push(std::move(submitted->retirement)))
      return {ExecuteStatus::Failed, token};
    next_token_ = SubmissionToken{token.value() + 1};
    return {ExecuteStatus::Submitted, token};
  }
  return {ExecuteStatus::Failed, token};
}

} // namespace dxmt::d3d12::submission
