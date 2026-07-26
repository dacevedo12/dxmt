#include "d3d12_replay_diagnostics.hpp"

#include "wsi_platform.hpp"
#include <utility>

namespace dxmt::d3d12 {

DiagnosticReadbackBuffer::DiagnosticReadbackBuffer(
    WMT::Buffer buffer, uint8_t *mapped) noexcept
    : buffer_(buffer), mapped_(mapped) {}

DiagnosticReadbackBuffer::DiagnosticReadbackBuffer(
    DiagnosticReadbackBuffer &&other) noexcept
    : buffer_(std::move(other.buffer_)), mapped_(other.mapped_) {
  other.mapped_ = nullptr;
}

DiagnosticReadbackBuffer &DiagnosticReadbackBuffer::operator=(
    DiagnosticReadbackBuffer &&other) noexcept {
  if (this == &other)
    return *this;
  Reset();
  buffer_ = std::move(other.buffer_);
  mapped_ = other.mapped_;
  other.mapped_ = nullptr;
  return *this;
}

DiagnosticReadbackBuffer::~DiagnosticReadbackBuffer() noexcept {
  Reset();
}

void DiagnosticReadbackBuffer::Reset() noexcept {
#ifdef __i386__
  wsi::aligned_free(mapped_);
#endif
  mapped_ = nullptr;
  buffer_ = nullptr;
}

} // namespace dxmt::d3d12
