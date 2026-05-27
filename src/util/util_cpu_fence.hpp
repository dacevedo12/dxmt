#pragma once
#include <cstdint>
#ifdef _WIN32
#include "thread.hpp"
#else
#include <atomic>
#endif

namespace dxmt {

class CpuFence {
public:
  void wait(uint64_t value) {
#ifdef _WIN32
    std::unique_lock<dxmt::mutex> lock(mutex_);
    cond_.wait(lock, [&]() {
      return value_ >= value;
    });
#else
    auto current = value_.load(std::memory_order_acquire);
    while (current < value) {
      value_.wait(current);
      current = value_.load(std::memory_order_acquire);
    }
#endif
  }

  void signal(uint64_t value) {
#ifdef _WIN32
    {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      if (value <= value_)
        return;
      value_ = value;
    }
    cond_.notify_all();
#else
    auto current = value_.load(std::memory_order_relaxed);
    do {
      if (value <= current)
        return;
    } while (!value_.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed));
    value_.notify_all();
#endif
  }

  uint64_t signaledValue() {
#ifdef _WIN32
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return value_;
#else
    return value_.load(std::memory_order_acquire);
#endif
  }

  CpuFence(): value_(0) {}
  CpuFence(uint64_t initial_value): value_(initial_value) {}

private:
#ifdef _WIN32
  dxmt::mutex mutex_;
  dxmt::condition_variable cond_;
  uint64_t value_;
#else
  std::atomic<uint64_t> value_;
#endif
};
} // namespace dxmt
