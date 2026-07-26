#include "dxmt_d3d12_submission_model.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace model = dxmt::d3d12::submission;

namespace {

class FakeBackend final : public model::ReplayBackend {
public:
  [[nodiscard]] model::BackendSubmitResult
  submit(model::D3D12SubmissionPacket packet, model::SubmissionToken token,
         model::BackendThreadCapability &backend_thread) override
      DXMT_REQUIRES(backend_thread) {
    if (!backend_thread.isCurrentThread() || fail_next_) {
      fail_next_ = false;
      return model::BackendFailed{std::move(packet)};
    }

    std::vector<model::RetirementPayload> payloads;
    if (auto *signal =
            std::get_if<model::SignalPacket>(&packet.payload())) {
      payloads.emplace_back(
          model::FenceRetirement{signal->fence, signal->value});
    }
    submitted_.push_back(token);
    return model::BackendSubmitted{
        model::GpuRetirementRecord{token, std::move(payloads)}};
  }

  void failNext() noexcept { fail_next_ = true; }
  [[nodiscard]] std::span<const model::SubmissionToken>
  submitted() const noexcept {
    return submitted_;
  }

private:
  bool fail_next_ = false;
  std::vector<model::SubmissionToken> submitted_;
};

[[noreturn]] void Fail(const char *message) {
  std::cerr << "d3d12 submission model test failed: " << message << '\n';
  std::abort();
}

void Check(bool value, const char *message) {
  if (!value)
    Fail(message);
}

void TestCheckedSubspan() {
  std::array<uint32_t, 4> values = {1, 2, 3, 4};
  auto middle = model::CheckedSubspan<uint32_t>(values, 1, 2);
  Check(middle && (*middle)[0] == 2 && (*middle)[1] == 3,
        "valid checked subspan was rejected");
  Check(!model::CheckedSubspan<uint32_t>(values, 3, 2),
        "out-of-range checked subspan was accepted");
  Check(!model::CheckedSubspan<uint32_t>(
            values, static_cast<size_t>(-1), 1),
        "overflowing checked subspan was accepted");
}

void TestBlockedQueueDoesNotBlockOtherQueue() {
  auto fence =
      std::make_shared<model::FenceLeafState>(model::FenceId{1}, 0);
  auto blocked =
      std::make_shared<model::LogicalQueueState>(model::LogicalQueueId{1});
  auto producer =
      std::make_shared<model::LogicalQueueState>(model::LogicalQueueId{2});

  Check(blocked->enqueue(model::D3D12SubmissionPacket{
            blocked->id(),
            model::WaitPacket{std::weak_ptr<model::FenceLeafState>(fence),
                              fence->id(), 7}}),
        "failed to enqueue wait");
  Check(producer->enqueue(model::D3D12SubmissionPacket{
            producer->id(), model::SignalPacket{fence, 7}}),
        "failed to enqueue signal");

  model::SubmissionSequencer sequencer;
  Check(sequencer.registerQueue(blocked), "failed to register blocked queue");
  Check(sequencer.registerQueue(producer), "failed to register producer queue");

  auto signal = sequencer.takeReady();
  Check(signal.packet.has_value(), "ready signal was globally blocked");
  const auto *signal_payload =
      std::get_if<model::SignalPacket>(&signal.packet->payload());
  Check(signal_payload && signal_payload->value == 7,
        "sequencer selected the wrong packet");
  signal_payload->fence->publish(signal_payload->value);

  auto wait = sequencer.takeReady();
  Check(wait.packet.has_value() &&
            std::holds_alternative<model::WaitPacket>(wait.packet->payload()),
        "satisfied wait did not become ready");
}

void TestExpiredDependencyIsExplicit() {
  auto queue =
      std::make_shared<model::LogicalQueueState>(model::LogicalQueueId{3});
  {
    auto fence =
        std::make_shared<model::FenceLeafState>(model::FenceId{3}, 0);
    Check(queue->enqueue(model::D3D12SubmissionPacket{
              queue->id(),
              model::WaitPacket{std::weak_ptr<model::FenceLeafState>(fence),
                                fence->id(), 1}}),
          "failed to enqueue expiring wait");
  }
  auto selection = queue->tryTakeReady();
  Check(selection.readiness == model::PacketReadiness::ExpiredDependency &&
            selection.packet.has_value(),
        "expired wait dependency was hidden as a blocked queue");
}

void TestRetirementSuccessAndFailure() {
  auto allocator = std::make_shared<model::AllocatorLeafState>();
  auto fence =
      std::make_shared<model::FenceLeafState>(model::FenceId{4}, 0);
  auto resource = std::make_shared<model::ResourceLeafState>();
  auto readback = std::make_shared<model::ReadbackLeafState>();
  auto present = std::make_shared<model::PresentLeafState>();
  allocator->markSubmitted(model::SubmissionSerial{9});

  std::vector<model::RetirementPayload> first;
  first.emplace_back(
      model::AllocatorRetirement{allocator, model::SubmissionSerial{9}});
  first.emplace_back(model::FenceRetirement{fence, 12});
  first.emplace_back(model::ResourceRetirement{resource});
  first.emplace_back(model::ReadbackRetirement{readback});
  first.emplace_back(model::PresentRetirement{present, 2});

  model::RetirementQueue queue;
  Check(queue.push(
            model::GpuRetirementRecord{model::SubmissionToken{1},
                                       std::move(first)}),
        "failed to enqueue retirement record");
  Check(queue.retireThrough(model::SubmissionToken{1}, true) == 1,
        "retirement record was not consumed");
  Check(allocator->pendingCount() == 0, "allocator serial leaked");
  Check(fence->completedValue() == 12, "fence value was not published");
  Check(resource->status() == model::CompletionStatus::Complete,
        "resource did not complete");
  Check(readback->status() == model::CompletionStatus::Complete,
        "readback did not complete");
  Check(present->completedValue() == 2 &&
            present->status() == model::CompletionStatus::Complete,
        "present did not complete");

  auto failed_resource = std::make_shared<model::ResourceLeafState>();
  std::vector<model::RetirementPayload> second;
  second.emplace_back(model::ResourceRetirement{failed_resource});
  Check(queue.push(
            model::GpuRetirementRecord{model::SubmissionToken{2},
                                       std::move(second)}),
        "failed to enqueue failure retirement record");
  Check(queue.failAll() == 1, "failure retirement record was not consumed");
  Check(failed_resource->status() == model::CompletionStatus::Failed,
        "failure retirement did not propagate");
}

void TestConcurrentAdmissionAndClose() {
  auto queue =
      std::make_shared<model::LogicalQueueState>(model::LogicalQueueId{5});
  std::atomic<bool> start{false};
  std::atomic<uint64_t> accepted{0};

  std::vector<std::thread> producers;
  for (uint64_t producer = 0; producer < 4; ++producer) {
    producers.emplace_back([queue, producer, &start, &accepted]() {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (uint64_t index = 0; index < 1000; ++index) {
        if (queue->enqueue(model::D3D12SubmissionPacket{
                queue->id(),
                model::ExecutePacket{
                    model::WorkId{producer * 1000 + index + 1}}}))
          accepted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  start.store(true, std::memory_order_release);
  auto cancelled = queue->beginClose();
  for (auto &producer : producers)
    producer.join();

  Check(queue->closeState() == model::QueueCloseState::Draining,
        "queue did not enter draining state");
  Check(queue->pendingCount() == 0, "packets were admitted after close");
  Check(cancelled.size() <= accepted.load(std::memory_order_relaxed),
        "close returned packets that were not admitted");
  Check(!queue->enqueue(model::D3D12SubmissionPacket{
            queue->id(), model::StopPacket{}}),
        "closed queue accepted a packet");
  queue->markJoined();
  Check(queue->closeState() == model::QueueCloseState::Joined,
        "queue did not enter joined state");
}

void TestExecutorOwnsBackendAndRetirementOrder() {
  auto fence =
      std::make_shared<model::FenceLeafState>(model::FenceId{8}, 0);
  auto queue =
      std::make_shared<model::LogicalQueueState>(model::LogicalQueueId{8});
  Check(queue->enqueue(model::D3D12SubmissionPacket{
            queue->id(), model::SignalPacket{fence, 41}}),
        "failed to enqueue executor signal");

  model::SubmissionSequencer sequencer;
  model::RetirementQueue retirement;
  Check(sequencer.registerQueue(queue),
        "failed to register executor queue");
  model::SubmissionExecutor executor(sequencer, retirement);
  FakeBackend backend;
  model::BackendThreadCapability backend_thread;
  model::BackendThreadScope backend_scope(backend_thread);

  const auto submitted = executor.executeOne(backend, backend_thread);
  Check(submitted.status == model::ExecuteStatus::Submitted &&
            submitted.token == model::SubmissionToken{1},
        "executor did not assign the first monotonic token");
  Check(fence->completedValue() == 0,
        "backend submission bypassed timeline retirement");
  Check(retirement.retireThrough(submitted.token, true) == 1,
        "submitted token did not retire");
  Check(fence->completedValue() == 41,
        "retirement did not publish the fence leaf");
}

void TestExecutorFailureDoesNotPublishCompletion() {
  auto fence =
      std::make_shared<model::FenceLeafState>(model::FenceId{9}, 0);
  auto queue =
      std::make_shared<model::LogicalQueueState>(model::LogicalQueueId{9});
  Check(queue->enqueue(model::D3D12SubmissionPacket{
            queue->id(), model::SignalPacket{fence, 99}}),
        "failed to enqueue failing signal");

  model::SubmissionSequencer sequencer;
  model::RetirementQueue retirement;
  Check(sequencer.registerQueue(queue),
        "failed to register failing queue");
  model::SubmissionExecutor executor(sequencer, retirement);
  FakeBackend backend;
  backend.failNext();
  model::BackendThreadCapability backend_thread;
  model::BackendThreadScope backend_scope(backend_thread);

  const auto failed = executor.executeOne(backend, backend_thread);
  Check(failed.status == model::ExecuteStatus::Failed,
        "backend failure was reported as submission");
  Check(retirement.pendingCount() == 0,
        "backend failure created a successful retirement record");
  Check(fence->completedValue() == 0,
        "backend failure published a fence value");
}

void TestBoundedDeterministicWaitSignalSchedules() {
  const char *seed_text = std::getenv("DXMT_TEST_SEED");
  const uint64_t seed =
      seed_text ? std::strtoull(seed_text, nullptr, 10) : 0;
  std::array<unsigned, 3> steps = {0, 1, 2};
  std::vector<std::array<unsigned, 3>> schedules;
  do {
    schedules.push_back(steps);
  } while (std::next_permutation(steps.begin(), steps.end()));
  std::rotate(schedules.begin(),
              schedules.begin() + seed % schedules.size(), schedules.end());

  for (const auto &schedule : schedules) {
    auto fence =
        std::make_shared<model::FenceLeafState>(model::FenceId{10}, 0);
    auto waiting =
        std::make_shared<model::LogicalQueueState>(model::LogicalQueueId{10});
    auto signaling =
        std::make_shared<model::LogicalQueueState>(model::LogicalQueueId{11});
    model::SubmissionSequencer sequencer;
    Check(sequencer.registerQueue(waiting),
          "failed to register scheduled wait queue");
    Check(sequencer.registerQueue(signaling),
          "failed to register scheduled signal queue");

    auto drain_one = [&sequencer]() {
      return sequencer.takeReady();
    };
    auto complete_signal = [](model::PacketSelection &selection) {
      if (!selection.packet)
        return;
      if (auto *signal =
              std::get_if<model::SignalPacket>(&selection.packet->payload()))
        signal->fence->publish(signal->value);
    };

    for (const unsigned step : schedule) {
      if (step == 0) {
        Check(waiting->enqueue(model::D3D12SubmissionPacket{
                  waiting->id(),
                  model::WaitPacket{
                      std::weak_ptr<model::FenceLeafState>(fence),
                      fence->id(), 5}}),
              "scheduled wait admission failed");
      } else if (step == 1) {
        Check(signaling->enqueue(model::D3D12SubmissionPacket{
                  signaling->id(), model::SignalPacket{fence, 5}}),
              "scheduled signal admission failed");
      } else {
        auto selection = drain_one();
        complete_signal(selection);
      }
    }

    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      auto selection = drain_one();
      complete_signal(selection);
    }
    Check(fence->completedValue() == 5,
          "bounded schedule failed to complete its signal");
    Check(waiting->pendingCount() == 0 && signaling->pendingCount() == 0,
          "bounded schedule left a packet pending");
  }
}

} // namespace

int main() {
  TestCheckedSubspan();
  TestBlockedQueueDoesNotBlockOtherQueue();
  TestExpiredDependencyIsExplicit();
  TestRetirementSuccessAndFailure();
  TestConcurrentAdmissionAndClose();
  TestExecutorOwnsBackendAndRetirementOrder();
  TestExecutorFailureDoesNotPublishCompletion();
  TestBoundedDeterministicWaitSignalSchedules();
  return 0;
}
