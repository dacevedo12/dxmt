#pragma once

#include "dxmt_thread_safety.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace dxmt::d3d12::submission {

template <typename Tag>
class StrongId final {
public:
  constexpr StrongId() noexcept = default;
  explicit constexpr StrongId(uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
  friend constexpr auto operator<=>(StrongId, StrongId) noexcept = default;

private:
  uint64_t value_ = 0;
};

using LogicalQueueId = StrongId<struct LogicalQueueIdTag>;
using WorkId = StrongId<struct WorkIdTag>;
using FenceId = StrongId<struct FenceIdTag>;
using SubmissionToken = StrongId<struct SubmissionTokenTag>;
using SubmissionSerial = StrongId<struct SubmissionSerialTag>;

template <typename T>
[[nodiscard]] std::optional<std::span<T>>
CheckedSubspan(std::span<T> source, size_t offset, size_t count) noexcept {
  if (offset > source.size() || count > source.size() - offset)
    return std::nullopt;
  return source.subspan(offset, count);
}

class DXMT_CAPABILITY("d3d12-submission-state") StateMutex final {
public:
  void lock() DXMT_ACQUIRE() { mutex_.lock(); }
  void unlock() DXMT_RELEASE() { mutex_.unlock(); }
  [[nodiscard]] bool try_lock() DXMT_TRY_ACQUIRE(true) {
    return mutex_.try_lock();
  }

private:
  std::mutex mutex_;
};

class DXMT_SCOPED_CAPABILITY StateLock final {
public:
  explicit StateLock(StateMutex &mutex) DXMT_ACQUIRE(mutex)
      : mutex_(mutex) {
    mutex_.lock();
  }

  ~StateLock() noexcept DXMT_RELEASE() { mutex_.unlock(); }

  StateLock(const StateLock &) = delete;
  StateLock &operator=(const StateLock &) = delete;

private:
  StateMutex &mutex_;
};

enum class QueueCloseState : uint8_t {
  Open,
  Closing,
  Draining,
  Joined,
};

enum class CompletionStatus : uint8_t {
  Pending,
  Complete,
  Failed,
};

class GpuLifetimeLeaf {
public:
  GpuLifetimeLeaf() = default;
  GpuLifetimeLeaf(const GpuLifetimeLeaf &) = delete;
  GpuLifetimeLeaf &operator=(const GpuLifetimeLeaf &) = delete;
  virtual ~GpuLifetimeLeaf() noexcept = default;
};

class FenceLeafState final : public GpuLifetimeLeaf {
public:
  explicit FenceLeafState(FenceId id, uint64_t initial_value = 0) noexcept
      : id_(id), completed_value_(initial_value) {}

  [[nodiscard]] FenceId id() const noexcept { return id_; }
  [[nodiscard]] uint64_t completedValue() const noexcept;
  [[nodiscard]] bool hasReached(uint64_t value) const noexcept;
  void publish(uint64_t value) noexcept;
  void fail() noexcept;
  [[nodiscard]] bool failed() const noexcept;

private:
  const FenceId id_;
  std::atomic<uint64_t> completed_value_;
  std::atomic<bool> failed_{false};
};

class AllocatorLeafState final : public GpuLifetimeLeaf {
public:
  void markSubmitted(SubmissionSerial serial);
  void retire(SubmissionSerial serial);
  [[nodiscard]] size_t pendingCount() const;

private:
  mutable StateMutex mutex_;
  std::vector<SubmissionSerial> pending_ DXMT_GUARDED_BY(mutex_);
};

class ResourceLeafState final : public GpuLifetimeLeaf {
public:
  void retire(bool complete) noexcept;
  [[nodiscard]] CompletionStatus status() const noexcept;

private:
  std::atomic<CompletionStatus> status_{CompletionStatus::Pending};
};

class ReadbackLeafState final : public GpuLifetimeLeaf {
public:
  void retire(bool complete) noexcept;
  [[nodiscard]] CompletionStatus status() const noexcept;

private:
  std::atomic<CompletionStatus> status_{CompletionStatus::Pending};
};

class PresentLeafState final : public GpuLifetimeLeaf {
public:
  void retire(uint64_t value, bool complete) noexcept;
  [[nodiscard]] uint64_t completedValue() const noexcept;
  [[nodiscard]] CompletionStatus status() const noexcept;

private:
  std::atomic<uint64_t> completed_value_{0};
  std::atomic<CompletionStatus> status_{CompletionStatus::Pending};
};

struct ExecutePacket final {
  WorkId work;
};

struct QueueWorkPacket final {
  WorkId work;
};

struct SignalPacket final {
  std::shared_ptr<FenceLeafState> fence;
  uint64_t value = 0;
};

struct WaitPacket final {
  std::weak_ptr<FenceLeafState> fence;
  FenceId fence_id;
  uint64_t value = 0;
};

struct StopPacket final {};

using SubmissionPayload =
    std::variant<ExecutePacket, QueueWorkPacket, SignalPacket, WaitPacket,
                 StopPacket>;

class D3D12SubmissionPacket final {
public:
  D3D12SubmissionPacket(LogicalQueueId queue, SubmissionPayload payload,
                        uint64_t frame = 0) noexcept
      : queue_(queue), frame_(frame), payload_(std::move(payload)) {}

  D3D12SubmissionPacket(const D3D12SubmissionPacket &) = delete;
  D3D12SubmissionPacket &operator=(const D3D12SubmissionPacket &) = delete;
  D3D12SubmissionPacket(D3D12SubmissionPacket &&) noexcept = default;
  D3D12SubmissionPacket &
  operator=(D3D12SubmissionPacket &&) noexcept = default;
  ~D3D12SubmissionPacket() noexcept = default;

  [[nodiscard]] LogicalQueueId queue() const noexcept { return queue_; }
  [[nodiscard]] uint64_t frame() const noexcept { return frame_; }
  [[nodiscard]] const SubmissionPayload &payload() const noexcept {
    return payload_;
  }
  [[nodiscard]] SubmissionPayload &payload() noexcept { return payload_; }

private:
  LogicalQueueId queue_;
  uint64_t frame_ = 0;
  SubmissionPayload payload_;
};

static_assert(!std::is_copy_constructible_v<D3D12SubmissionPacket>);
static_assert(std::is_nothrow_move_constructible_v<D3D12SubmissionPacket>);

enum class PacketReadiness : uint8_t {
  Ready,
  Blocked,
  ExpiredDependency,
  Stop,
};

struct PacketSelection final {
  PacketReadiness readiness = PacketReadiness::Blocked;
  std::optional<D3D12SubmissionPacket> packet;
};

class LogicalQueueState final {
public:
  explicit LogicalQueueState(LogicalQueueId id) noexcept : id_(id) {}

  LogicalQueueState(const LogicalQueueState &) = delete;
  LogicalQueueState &operator=(const LogicalQueueState &) = delete;
  ~LogicalQueueState() noexcept = default;

  [[nodiscard]] LogicalQueueId id() const noexcept { return id_; }
  [[nodiscard]] bool enqueue(D3D12SubmissionPacket packet);
  [[nodiscard]] PacketSelection tryTakeReady();
  [[nodiscard]] std::vector<D3D12SubmissionPacket> beginClose();
  void markDraining();
  void markJoined();
  [[nodiscard]] QueueCloseState closeState() const;
  [[nodiscard]] size_t pendingCount() const;

private:
  [[nodiscard]] PacketReadiness
  frontReadinessLocked() const DXMT_REQUIRES(mutex_);

  const LogicalQueueId id_;
  mutable StateMutex mutex_;
  std::deque<D3D12SubmissionPacket> pending_ DXMT_GUARDED_BY(mutex_);
  QueueCloseState close_state_ DXMT_GUARDED_BY(mutex_) =
      QueueCloseState::Open;
};

class SubmissionSequencer final {
public:
  [[nodiscard]] bool
  registerQueue(const std::shared_ptr<LogicalQueueState> &queue);
  void unregisterQueue(LogicalQueueId id);
  [[nodiscard]] PacketSelection takeReady();
  [[nodiscard]] size_t queueCount() const;

private:
  mutable StateMutex mutex_;
  std::vector<std::weak_ptr<LogicalQueueState>> queues_
      DXMT_GUARDED_BY(mutex_);
  size_t next_queue_ DXMT_GUARDED_BY(mutex_) = 0;
};

struct AllocatorRetirement final {
  std::shared_ptr<AllocatorLeafState> state;
  SubmissionSerial serial;
};

struct FenceRetirement final {
  std::shared_ptr<FenceLeafState> state;
  uint64_t value = 0;
};

struct ResourceRetirement final {
  std::shared_ptr<ResourceLeafState> state;
};

struct ReadbackRetirement final {
  std::shared_ptr<ReadbackLeafState> state;
};

struct PresentRetirement final {
  std::shared_ptr<PresentLeafState> state;
  uint64_t value = 0;
};

using RetirementPayload =
    std::variant<AllocatorRetirement, FenceRetirement, ResourceRetirement,
                 ReadbackRetirement, PresentRetirement>;

template <typename T>
concept GpuRetirementPayload =
    std::same_as<T, AllocatorRetirement> ||
    std::same_as<T, FenceRetirement> ||
    std::same_as<T, ResourceRetirement> ||
    std::same_as<T, ReadbackRetirement> ||
    std::same_as<T, PresentRetirement>;

class GpuRetirementRecord final {
public:
  GpuRetirementRecord(SubmissionToken token,
                      std::vector<RetirementPayload> payloads) noexcept
      : token_(token), payloads_(std::move(payloads)) {}

  GpuRetirementRecord(const GpuRetirementRecord &) = delete;
  GpuRetirementRecord &operator=(const GpuRetirementRecord &) = delete;
  GpuRetirementRecord(GpuRetirementRecord &&) noexcept = default;
  GpuRetirementRecord &operator=(GpuRetirementRecord &&) noexcept = default;
  ~GpuRetirementRecord() noexcept = default;

  [[nodiscard]] SubmissionToken token() const noexcept { return token_; }
  [[nodiscard]] std::span<const RetirementPayload>
  payloads() const noexcept DXMT_LIFETIME_BOUND {
    return payloads_;
  }
  void retire(bool complete);

private:
  SubmissionToken token_;
  std::vector<RetirementPayload> payloads_;
};

static_assert(!std::is_copy_constructible_v<GpuRetirementRecord>);
static_assert(std::is_nothrow_move_constructible_v<GpuRetirementRecord>);

class RetirementQueue final {
public:
  [[nodiscard]] bool push(GpuRetirementRecord record);
  [[nodiscard]] size_t retireThrough(SubmissionToken token,
                                     bool complete);
  [[nodiscard]] size_t failAll();
  [[nodiscard]] size_t pendingCount() const;
  [[nodiscard]] SubmissionToken lastRetiredToken() const;

private:
  mutable StateMutex mutex_;
  std::deque<GpuRetirementRecord> pending_ DXMT_GUARDED_BY(mutex_);
  SubmissionToken last_retired_ DXMT_GUARDED_BY(mutex_);
};

class DXMT_CAPABILITY("d3d12-backend-thread")
    BackendThreadCapability final {
public:
  BackendThreadCapability() noexcept : owner_(std::this_thread::get_id()) {}

  void acquire() noexcept DXMT_ACQUIRE() {}
  void release() noexcept DXMT_RELEASE() {}
  [[nodiscard]] bool isCurrentThread() const noexcept {
    return owner_ == std::this_thread::get_id();
  }

private:
  const std::thread::id owner_;
};

class DXMT_SCOPED_CAPABILITY BackendThreadScope final {
public:
  explicit BackendThreadScope(BackendThreadCapability &capability) noexcept
      DXMT_ACQUIRE(capability)
      : capability_(capability) {
    capability_.acquire();
  }

  ~BackendThreadScope() noexcept DXMT_RELEASE() { capability_.release(); }

  BackendThreadScope(const BackendThreadScope &) = delete;
  BackendThreadScope &operator=(const BackendThreadScope &) = delete;

private:
  BackendThreadCapability &capability_;
};

struct BackendSubmitted final {
  GpuRetirementRecord retirement;
};

struct BackendFailed final {
  D3D12SubmissionPacket packet;
};

using BackendSubmitResult = std::variant<BackendSubmitted, BackendFailed>;

class ReplayBackend {
public:
  ReplayBackend() = default;
  ReplayBackend(const ReplayBackend &) = delete;
  ReplayBackend &operator=(const ReplayBackend &) = delete;
  virtual ~ReplayBackend() noexcept = default;

  [[nodiscard]] virtual BackendSubmitResult
  submit(D3D12SubmissionPacket packet, SubmissionToken token,
         BackendThreadCapability &backend_thread)
      DXMT_REQUIRES(backend_thread) = 0;
};

enum class ExecuteStatus : uint8_t {
  Idle,
  Submitted,
  Failed,
  Stop,
};

struct ExecuteResult final {
  ExecuteStatus status = ExecuteStatus::Idle;
  SubmissionToken token;
};

class SubmissionExecutor final {
public:
  SubmissionExecutor(SubmissionSequencer &sequencer,
                     RetirementQueue &retirement) noexcept
      : sequencer_(sequencer), retirement_(retirement) {}

  [[nodiscard]] ExecuteResult
  executeOne(ReplayBackend &backend,
             BackendThreadCapability &backend_thread)
      DXMT_REQUIRES(backend_thread);

private:
  SubmissionSequencer &sequencer_;
  RetirementQueue &retirement_;
  SubmissionToken next_token_{1};
};

} // namespace dxmt::d3d12::submission
