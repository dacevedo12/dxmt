#pragma once

#include "d3d12_device.hpp"

#include <cstdint>
#include <memory>

namespace dxmt::d3d12 {

class DxmtQueueSubmissionTarget {
public:
  DxmtQueueSubmissionTarget() = default;
  DxmtQueueSubmissionTarget(const DxmtQueueSubmissionTarget &) = delete;
  DxmtQueueSubmissionTarget &
  operator=(const DxmtQueueSubmissionTarget &) = delete;
  virtual ~DxmtQueueSubmissionTarget() noexcept = default;

  [[nodiscard]] virtual bool
  Submit(IMTLD3D12Device &device, uint64_t &submitted_sequence) noexcept = 0;
};

class SubmissionQueueEndpoint {
public:
  SubmissionQueueEndpoint() = default;
  SubmissionQueueEndpoint(const SubmissionQueueEndpoint &) = delete;
  SubmissionQueueEndpoint &
  operator=(const SubmissionQueueEndpoint &) = delete;
  virtual ~SubmissionQueueEndpoint() noexcept = default;

  [[nodiscard]] virtual bool ProcessReadySubmission() noexcept = 0;
};

class DeviceSubmissionService final {
public:
  DeviceSubmissionService();
  DeviceSubmissionService(const DeviceSubmissionService &) = delete;
  DeviceSubmissionService &operator=(const DeviceSubmissionService &) = delete;
  ~DeviceSubmissionService() noexcept;

  [[nodiscard]] bool
  RegisterQueue(std::shared_ptr<SubmissionQueueEndpoint> endpoint);
  void UnregisterQueue(SubmissionQueueEndpoint &endpoint) noexcept;
  void Wake() noexcept;

  [[nodiscard]] bool IsWorkerThread() const noexcept;
  [[nodiscard]] bool SubmitDeviceWork(
      IMTLD3D12Device &device,
      std::unique_ptr<DxmtQueueSubmissionTarget> target,
      uint64_t &submitted_sequence);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace dxmt::d3d12
