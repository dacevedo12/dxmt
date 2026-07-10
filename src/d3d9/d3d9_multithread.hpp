/*
 * This file is part of DXMT.
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <windows.h>

namespace dxmt {

/* A spinlock the same thread can acquire multiple times, keyed by the
 * Windows thread id. Public D3D9 entry points nest (Reset applies
 * state through the same setters an app calls), so the device lock
 * must be recursive. DXVK sync_recursive.h. */
class D9RecursiveSpinlock {
public:
  void
  lock() {
    // Bounded pause-spin, then yield the core: the holder can be parked
    // on a GPU fence for milliseconds (a synchronizing Lock), and a pure
    // spin would burn a core for that whole window. DXVK's sync::spin
    // takes the same two-phase shape.
    while (!try_lock()) {
      for (uint32_t i = 0; i < 2000; i++) {
        YieldProcessor();
        if (try_lock())
          return;
      }
      ::SwitchToThread();
    }
  }

  void
  unlock() {
    if (m_counter == 0)
      m_owner.store(0, std::memory_order_release);
    else
      m_counter -= 1;
  }

  bool
  try_lock() {
    uint32_t thread_id = ::GetCurrentThreadId();
    uint32_t expected = 0;

    bool status = m_owner.compare_exchange_weak(expected, thread_id, std::memory_order_acquire);
    if (status)
      return true;

    if (expected != thread_id)
      return false;

    m_counter += 1;
    return true;
  }

private:
  std::atomic<uint32_t> m_owner = {0u};
  uint32_t m_counter = {0u};
};

/* RAII device lock, cheaper than std::unique_lock: one pointer, no
 * state flags. Default-constructed = no-op, so the unprotected path
 * costs nothing. DXVK d3d9_multithread.h. */
class D9DeviceLock {
public:
  D9DeviceLock() : m_mutex(nullptr) {}

  D9DeviceLock(D9RecursiveSpinlock &mutex) : m_mutex(&mutex) {
    mutex.lock();
  }

  D9DeviceLock(D9DeviceLock &&other) : m_mutex(other.m_mutex) {
    other.m_mutex = nullptr;
  }

  D9DeviceLock &
  operator=(D9DeviceLock &&other) {
    if (m_mutex)
      m_mutex->unlock();
    m_mutex = other.m_mutex;
    other.m_mutex = nullptr;
    return *this;
  }

  D9DeviceLock(const D9DeviceLock &) = delete;
  D9DeviceLock &operator=(const D9DeviceLock &) = delete;

  ~D9DeviceLock() {
    if (m_mutex != nullptr)
      m_mutex->unlock();
  }

private:
  D9RecursiveSpinlock *m_mutex;
};

/* Serializes the D3D9 API when the app created the device with
 * D3DCREATE_MULTITHREADED: every public entry point takes the lock, so
 * app worker threads (resource streaming) cannot race the render
 * thread through the calling-thread state. Without the flag the app
 * promises single-threaded use and AcquireLock degenerates to a no-op,
 * matching the native runtime and DXVK; wined3d locks unconditionally.
 * Coverage is broader than DXVK's: resource methods and constant
 * getters take the lock too, for uniform greppable coverage; the cost
 * is a recursive re-acquire on the forwarding paths.
 * The queue's encode / finish threads never take this lock: their
 * shared state is either owned by them alone or independently
 * synchronized, and GPU progress must not depend on it. */
class D9Multithread {
public:
  D9Multithread(bool is_protected) : m_protected(is_protected) {}

  D9DeviceLock
  AcquireLock() {
    return m_protected ? D9DeviceLock(m_mutex) : D9DeviceLock();
  }

private:
  bool m_protected;
  D9RecursiveSpinlock m_mutex;
};

} // namespace dxmt
