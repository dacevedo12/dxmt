/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unknwn.h>

#include "dxmt_thread_safety.h"
#include "util_error.hpp"


#include "./rc/util_rc.hpp"
#include "./rc/util_rc_ptr.hpp"

namespace dxmt {

/**
 * \brief Thread priority
 */
enum class ThreadPriority : int32_t {
  Normal,
  Lowest,
};

namespace this_thread {
/**
 * \brief Current OS thread id
 *
 * Declared ahead of the platform split so the debug ownership record below can
 * name it; each platform branch supplies the definition.  Neither backend ever
 * hands out 0, so 0 is a usable "unowned" sentinel.
 */
uint32_t get_id();
} // namespace this_thread

namespace detail {

/**
 * \brief Debug-only ownership record for \ref dxmt::mutex
 *
 * \c dxmt::mutex is a non-recursive exclusive lock on both backends (an
 * exclusive SRWLOCK on Windows, \c std::mutex elsewhere), so at most one thread
 * can own it and re-entry is a deadlock rather than a nesting.  The record
 * therefore holds either 0 or the single owning thread id, which is what makes
 * \c DXMT_ASSERT_CAPABILITY helpers checkable at run time instead of being a
 * bare promise to the analyzer.
 *
 * Under \c NDEBUG -- which meson sets for release builds via
 * \c b_ndebug=if-release -- every member below becomes an empty inline function
 * and the class becomes empty, so with \c [[no_unique_address]] a released
 * \c dxmt::mutex is still exactly one lock word and none of the calls survive
 * codegen.
 */
class MutexOwnerRecord {
public:
#ifndef NDEBUG
  /** \brief Rejects re-entry before the caller blocks on its own lock. */
  void BeforeAcquire() const noexcept {
    assert(owner_tid_.load(std::memory_order_relaxed) !=
               this_thread::get_id() &&
           "dxmt::mutex is not recursive: the calling thread already owns it");
  }

  void OnAcquire() noexcept {
    // The previous owner's release happens-before this acquisition through the
    // mutex itself, so the record needs no ordering of its own.
    assert(owner_tid_.load(std::memory_order_relaxed) == 0 &&
           "dxmt::mutex was acquired while another thread is recorded as its "
           "owner");
    owner_tid_.store(this_thread::get_id(), std::memory_order_relaxed);
  }

  void OnRelease() noexcept {
    assert(owner_tid_.load(std::memory_order_relaxed) ==
               this_thread::get_id() &&
           "dxmt::mutex is being released by a thread that does not own it");
    owner_tid_.store(0, std::memory_order_relaxed);
  }

  void AssertHeldByCaller() const noexcept {
    assert(owner_tid_.load(std::memory_order_relaxed) ==
               this_thread::get_id() &&
           "expected dxmt::mutex to be held by the calling thread");
  }

private:
  std::atomic<uint32_t> owner_tid_ = {0u};
#else
  void BeforeAcquire() const noexcept {}
  void OnAcquire() noexcept {}
  void OnRelease() noexcept {}
  void AssertHeldByCaller() const noexcept {}
#endif
};

/**
 * \brief Hands the ownership record over across a condition-variable wait
 *
 * A condition-variable wait releases and reacquires the mutex behind the
 * wrapper's back, so the record has to be cleared for the duration of the wait
 * and restored afterwards.  Both the Windows and the POSIX backend reacquire
 * the mutex even when the wait exits via an exception, so restoring from the
 * destructor is correct on every path.
 */
template <typename Mutex> class MutexWaitHandoff {
public:
  explicit MutexWaitHandoff(Mutex &owner) noexcept : owner_(owner) {
    owner_.note_wait_release();
  }

  MutexWaitHandoff(const MutexWaitHandoff &) = delete;
  MutexWaitHandoff &operator=(const MutexWaitHandoff &) = delete;

  ~MutexWaitHandoff() { owner_.note_wait_acquire(); }

private:
  Mutex &owner_;
};

} // namespace detail

#ifdef _WIN32
/**
 * \brief Thread helper class
 *
 * This is needed mostly  for winelib builds. Wine needs to setup each thread
 * that calls Windows APIs. It means that in winelib builds, we can't let
 * standard C++ library create threads and need to use Wine for that instead. We
 * use a thin wrapper around Windows thread functions so that the rest of code
 * just has to use dxmt::thread class instead of std::thread.
 */
class ThreadFn : public RcObject {
  using Proc = std::function<void()>;

public:
  ThreadFn(Proc &&proc) : m_proc(std::move(proc)) {
    // Reference for the thread function
    this->incRef();

    m_handle = ::CreateThread(nullptr, 0x100000, ThreadFn::threadProc, this,
                              STACK_SIZE_PARAM_IS_A_RESERVATION |
                                  CREATE_SUSPENDED,
                              nullptr);

    if (m_handle == nullptr)
      throw MTLD3DError("Failed to create thread");
  }

  ~ThreadFn() {
    if (this->joinable())
      std::terminate();
  }

  void start() {
    if (::ResumeThread(m_handle) == static_cast<DWORD>(-1)) {
      ::CloseHandle(m_handle);
      m_handle = nullptr;
      this->decRef();
      throw MTLD3DError("Failed to start thread");
    }
  }

  void detach() {
    ::CloseHandle(m_handle);
    m_handle = nullptr;
  }

  void join() {
    if (::WaitForSingleObjectEx(m_handle, INFINITE, FALSE) == WAIT_FAILED)
      throw MTLD3DError("Failed to join thread");
    this->detach();
  }

  bool joinable() const { return m_handle != nullptr; }

  void set_priority(ThreadPriority priority) {
    int32_t value;
    switch (priority) {
    default:
    case ThreadPriority::Normal:
      value = THREAD_PRIORITY_NORMAL;
      break;
    case ThreadPriority::Lowest:
      value = THREAD_PRIORITY_LOWEST;
      break;
    }
    ::SetThreadPriority(m_handle, int32_t(value));
  }

private:
  Proc m_proc;
  HANDLE m_handle;

  static DWORD WINAPI threadProc(void *arg) {
    auto thread = reinterpret_cast<ThreadFn *>(arg);
    thread->m_proc();
    thread->decRef();
    return 0;
  }
};

/**
 * \brief RAII thread wrapper
 *
 * Wrapper for \c ThreadFn that can be used
 * as a drop-in replacement for \c std::thread.
 */
class thread {

public:
  thread() {}

  explicit thread(std::function<void()> &&func)
      : m_thread(new ThreadFn(std::move(func))) {
    m_thread->start();
  }

  thread(thread &&other) : m_thread(std::move(other.m_thread)) {}

  thread &operator=(thread &&other) {
    m_thread = std::move(other.m_thread);
    return *this;
  }

  void detach() { m_thread->detach(); }

  void join() { m_thread->join(); }

  void join_noexcept() noexcept {
    try {
      join();
    } catch (...) {
      // Continuing teardown while the worker may still access its owner would
      // turn a join failure into a use-after-free.
      std::terminate();
    }
  }

  bool joinable() const { return m_thread != nullptr && m_thread->joinable(); }

  void set_priority(ThreadPriority priority) {
    m_thread->set_priority(priority);
  }

  static uint32_t hardware_concurrency() {
    SYSTEM_INFO info = {};
    ::GetSystemInfo(&info);
    return info.dwNumberOfProcessors;
  }

private:
  Rc<ThreadFn> m_thread;
};

namespace this_thread {
inline void yield() { SwitchToThread(); }

inline uint32_t get_id() { return uint32_t(GetCurrentThreadId()); }

bool isInModuleDetachment();
} // namespace this_thread

/**
 * \brief SRW-based mutex implementation
 *
 * Drop-in replacement for \c std::mutex that uses Win32
 * SRW locks, which are implemented with \c futex in wine.
 */
class DXMT_CAPABILITY("mutex") mutex {

public:
  using native_handle_type = PSRWLOCK;

  mutex() {}

  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;

  void lock() DXMT_ACQUIRE() {
    m_owner.BeforeAcquire();
    AcquireSRWLockExclusive(&m_lock);
    m_owner.OnAcquire();
  }

  void unlock() DXMT_RELEASE() {
    m_owner.OnRelease();
    ReleaseSRWLockExclusive(&m_lock);
  }

  bool try_lock() DXMT_TRY_ACQUIRE(true) {
    if (!TryAcquireSRWLockExclusive(&m_lock))
      return false;
    m_owner.OnAcquire();
    return true;
  }

  /**
   * \brief Debug-only proof that the caller owns this mutex
   *
   * Backs the \c DXMT_ASSERT_CAPABILITY helpers: telling the analyzer the lock
   * is held has to cost a run-time check in debug builds, otherwise the
   * annotation is an unconditional escape hatch.
   */
  void assert_held() const noexcept { m_owner.AssertHeldByCaller(); }

  /**
   * \brief Ownership handoff around a wait that drops the lock internally
   *
   * For \ref condition_variable only; \ref detail::MutexWaitHandoff is the
   * supported caller.
   */
  void note_wait_release() noexcept { m_owner.OnRelease(); }
  void note_wait_acquire() noexcept { m_owner.OnAcquire(); }

  native_handle_type native_handle() { return &m_lock; }

private:
  SRWLOCK m_lock = SRWLOCK_INIT;
  [[no_unique_address]] detail::MutexOwnerRecord m_owner;
};

/**
 * \brief SRW-based shared mutex implementation
 */
class DXMT_CAPABILITY("shared-mutex") shared_mutex {

public:
  using native_handle_type = PSRWLOCK;

  shared_mutex() {}

  shared_mutex(const shared_mutex &) = delete;
  shared_mutex &operator=(const shared_mutex &) = delete;

  void lock() DXMT_ACQUIRE() { AcquireSRWLockExclusive(&m_lock); }

  void lock_shared() DXMT_ACQUIRE() { AcquireSRWLockShared(&m_lock); }

  void unlock() DXMT_RELEASE() { ReleaseSRWLockExclusive(&m_lock); }

  void unlock_shared() DXMT_RELEASE() { ReleaseSRWLockShared(&m_lock); }

  bool try_lock() DXMT_TRY_ACQUIRE(true) {
    return TryAcquireSRWLockExclusive(&m_lock);
  }

  bool try_lock_shared() DXMT_TRY_ACQUIRE(true) {
    return TryAcquireSRWLockShared(&m_lock);
  }

  native_handle_type native_handle() { return &m_lock; }

private:
  SRWLOCK m_lock = SRWLOCK_INIT;
};

/**
 * \brief Recursive mutex implementation
 *
 * Drop-in replacement for \c std::recursive_mutex that
 * uses Win32 critical sections.
 */
class recursive_mutex {

public:
  using native_handle_type = PCRITICAL_SECTION;

  recursive_mutex() { InitializeCriticalSection(&m_lock); }

  ~recursive_mutex() { DeleteCriticalSection(&m_lock); }

  recursive_mutex(const recursive_mutex &) = delete;
  recursive_mutex &operator=(const recursive_mutex &) = delete;

  void lock() { EnterCriticalSection(&m_lock); }

  void unlock() { LeaveCriticalSection(&m_lock); }

  bool try_lock() { return TryEnterCriticalSection(&m_lock); }

  native_handle_type native_handle() { return &m_lock; }

private:
  CRITICAL_SECTION m_lock;
};

/**
 * \brief SRW-based condition variable implementation
 *
 * Drop-in replacement for \c std::condition_variable that
 * uses Win32 condition variables on SRW locks.
 */
class condition_variable {

public:
  using native_handle_type = PCONDITION_VARIABLE;

  condition_variable() { InitializeConditionVariable(&m_cond); }

  condition_variable(condition_variable &) = delete;

  condition_variable &operator=(condition_variable &) = delete;

  void notify_one() { WakeConditionVariable(&m_cond); }

  void notify_all() { WakeAllConditionVariable(&m_cond); }

  void wait(std::unique_lock<dxmt::mutex> &lock) {
    auto &owner = *lock.mutex();
    auto srw = owner.native_handle();
    detail::MutexWaitHandoff<dxmt::mutex> handoff(owner);
    SleepConditionVariableSRW(&m_cond, srw, INFINITE, 0);
  }

  template <typename Predicate>
  void wait(std::unique_lock<dxmt::mutex> &lock, Predicate pred) {
    while (!pred())
      wait(lock);
  }

  template <typename Clock, typename Duration>
  std::cv_status
  wait_until(std::unique_lock<dxmt::mutex> &lock,
             const std::chrono::time_point<Clock, Duration> &time) {
    auto now = Clock::now();

    return (now < time) ? wait_for(lock, time - now) : std::cv_status::timeout;
  }

  template <typename Clock, typename Duration, typename Predicate>
  bool wait_until(std::unique_lock<dxmt::mutex> &lock,
                  const std::chrono::time_point<Clock, Duration> &time,
                  Predicate pred) {
    while (!pred()) {
      const auto now = Clock::now();
      if (now >= time ||
          wait_for(lock, time - now) == std::cv_status::timeout)
        return pred();
    }
    return true;
  }

  template <typename Rep, typename Period>
  std::cv_status wait_for(std::unique_lock<dxmt::mutex> &lock,
                          const std::chrono::duration<Rep, Period> &timeout) {
    if (timeout <= std::chrono::duration<Rep, Period>::zero())
      return std::cv_status::timeout;

    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(timeout);
    const auto timeout_ms = static_cast<uint64_t>(ms.count()) >= INFINITE
                                ? INFINITE - 1
                                : static_cast<DWORD>(ms.count());
    auto &owner = *lock.mutex();
    auto srw = owner.native_handle();
    detail::MutexWaitHandoff<dxmt::mutex> handoff(owner);

    return SleepConditionVariableSRW(&m_cond, srw, timeout_ms, 0)
               ? std::cv_status::no_timeout
               : std::cv_status::timeout;
  }

  template <typename Rep, typename Period, typename Predicate>
  bool wait_for(std::unique_lock<dxmt::mutex> &lock,
                const std::chrono::duration<Rep, Period> &timeout,
                Predicate pred) {
    return wait_until(lock, std::chrono::steady_clock::now() + timeout,
                      std::move(pred));
  }

  native_handle_type native_handle() { return &m_cond; }

private:
  CONDITION_VARIABLE m_cond;
};

#else
class thread : public std::thread {
public:
  using std::thread::thread;

  void join_noexcept() noexcept {
    try {
      join();
    } catch (...) {
      // A joinable std::thread cannot be abandoned safely during owner
      // teardown.
      std::terminate();
    }
  }

  void set_priority(ThreadPriority priority) {
    ::sched_param param = {};
    int32_t policy;
    switch (priority) {
    default:
    case ThreadPriority::Normal:
      policy = SCHED_OTHER;
      break;
    case ThreadPriority::Lowest:
#ifdef __APPLE__
      policy = SCHED_FIFO; /* No SCHED_IDLE on macOS */
#else
      policy = SCHED_IDLE;
#endif
      break;
    }
    ::pthread_setschedparam(this->native_handle(), policy, &param);
  }
};

/**
 * \brief Ownership-tracking wrapper around \c std::mutex
 *
 * The native build has to carry the same debug ownership record as the PE
 * build, otherwise \c DXMT_ASSERT_CAPABILITY would degrade back into an
 * unchecked promise on one of the two configurations.  \c std::mutex cannot be
 * queried for its owner, so the record lives here and the standard mutex is
 * kept as the underlying primitive.
 */
class DXMT_CAPABILITY("mutex") mutex {

public:
  using native_handle_type = std::mutex::native_handle_type;

  // Constant-initialized, like the std::mutex this used to alias, so the
  // namespace-scope mutexes in the tree keep their static initialization
  // order guarantees.
  constexpr mutex() {}

  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;

  void lock() DXMT_ACQUIRE() {
    m_owner.BeforeAcquire();
    m_lock.lock();
    m_owner.OnAcquire();
  }

  void unlock() DXMT_RELEASE() {
    m_owner.OnRelease();
    m_lock.unlock();
  }

  bool try_lock() DXMT_TRY_ACQUIRE(true) {
    if (!m_lock.try_lock())
      return false;
    m_owner.OnAcquire();
    return true;
  }

  /**
   * \brief Debug-only proof that the caller owns this mutex
   *
   * Backs the \c DXMT_ASSERT_CAPABILITY helpers: telling the analyzer the lock
   * is held has to cost a run-time check in debug builds, otherwise the
   * annotation is an unconditional escape hatch.
   */
  void assert_held() const noexcept { m_owner.AssertHeldByCaller(); }

  /**
   * \brief Ownership handoff around a wait that drops the lock internally
   *
   * For \ref condition_variable only; \ref detail::MutexWaitHandoff is the
   * supported caller.
   */
  void note_wait_release() noexcept { m_owner.OnRelease(); }
  void note_wait_acquire() noexcept { m_owner.OnAcquire(); }

  /** \brief Underlying primitive, for the condition-variable adapter only. */
  std::mutex &standard_mutex() noexcept { return m_lock; }

  native_handle_type native_handle() { return m_lock.native_handle(); }

private:
  std::mutex m_lock;
  [[no_unique_address]] detail::MutexOwnerRecord m_owner;
};

using shared_mutex = std::shared_mutex;
using recursive_mutex = std::recursive_mutex;

/**
 * \brief \c std::condition_variable adapted to \ref dxmt::mutex
 *
 * Mirrors the Windows wrapper's interface so callers stay portable.  The wait
 * lends the already-held \c std::mutex to \c std::condition_variable for the
 * duration of the wait, which keeps the standard primitive (rather than the
 * heavier \c condition_variable_any) while letting the ownership record follow
 * the implicit release/reacquire.
 */
class condition_variable {

public:
  using native_handle_type = std::condition_variable::native_handle_type;

  // Constant-initialized, like the std::condition_variable this used to alias.
  constexpr condition_variable() {}

  condition_variable(condition_variable &) = delete;

  condition_variable &operator=(condition_variable &) = delete;

  void notify_one() { m_cond.notify_one(); }

  void notify_all() { m_cond.notify_all(); }

  void wait(std::unique_lock<dxmt::mutex> &lock) {
    LentLock inner(lock);
    m_cond.wait(inner.get());
  }

  template <typename Predicate>
  void wait(std::unique_lock<dxmt::mutex> &lock, Predicate pred) {
    while (!pred())
      wait(lock);
  }

  template <typename Clock, typename Duration>
  std::cv_status
  wait_until(std::unique_lock<dxmt::mutex> &lock,
             const std::chrono::time_point<Clock, Duration> &time) {
    auto now = Clock::now();

    return (now < time) ? wait_for(lock, time - now) : std::cv_status::timeout;
  }

  template <typename Clock, typename Duration, typename Predicate>
  bool wait_until(std::unique_lock<dxmt::mutex> &lock,
                  const std::chrono::time_point<Clock, Duration> &time,
                  Predicate pred) {
    while (!pred()) {
      const auto now = Clock::now();
      if (now >= time ||
          wait_for(lock, time - now) == std::cv_status::timeout)
        return pred();
    }
    return true;
  }

  template <typename Rep, typename Period>
  std::cv_status wait_for(std::unique_lock<dxmt::mutex> &lock,
                          const std::chrono::duration<Rep, Period> &timeout) {
    if (timeout <= std::chrono::duration<Rep, Period>::zero())
      return std::cv_status::timeout;

    LentLock inner(lock);
    return m_cond.wait_for(inner.get(), timeout);
  }

  template <typename Rep, typename Period, typename Predicate>
  bool wait_for(std::unique_lock<dxmt::mutex> &lock,
                const std::chrono::duration<Rep, Period> &timeout,
                Predicate pred) {
    return wait_until(lock, std::chrono::steady_clock::now() + timeout,
                      std::move(pred));
  }

  native_handle_type native_handle() { return m_cond.native_handle(); }

private:
  /**
   * \brief Lends the held \c std::mutex to \c std::condition_variable
   *
   * \c std::condition_variable only understands \c std::unique_lock<std::mutex>
   * The adapter adopts the already-locked primitive, and releases the borrowed
   * lock without unlocking it once the wait has reacquired it -- including when
   * the wait exits via an exception, which the standard requires to happen with
   * the mutex reacquired.
   */
  class LentLock {
  public:
    explicit LentLock(std::unique_lock<dxmt::mutex> &lock)
        : handoff_(*lock.mutex()),
          inner_(lock.mutex()->standard_mutex(), std::adopt_lock) {}

    LentLock(const LentLock &) = delete;
    LentLock &operator=(const LentLock &) = delete;

    ~LentLock() { inner_.release(); }

    std::unique_lock<std::mutex> &get() noexcept { return inner_; }

  private:
    detail::MutexWaitHandoff<dxmt::mutex> handoff_;
    std::unique_lock<std::mutex> inner_;
  };

  std::condition_variable m_cond;
};

namespace this_thread {
inline void yield() { std::this_thread::yield(); }

inline bool isInModuleDetachment() { return false; }
} // namespace this_thread
#endif

struct null_mutex {
  void lock() {}
  void unlock() noexcept {}
  bool try_lock() { return true; }
};

} // namespace dxmt
