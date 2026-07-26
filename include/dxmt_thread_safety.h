#pragma once

/*
 * Compile-time lock contracts for Clang's thread-safety analysis.  Keep these
 * macros inert on other compilers so the shared PE/native headers remain
 * portable.
 */
#if defined(__clang__)
#define DXMT_CAPABILITY(name) __attribute__((capability(name)))
#define DXMT_SCOPED_CAPABILITY __attribute__((scoped_lockable))
#define DXMT_GUARDED_BY(lock) __attribute__((guarded_by(lock)))
#define DXMT_PT_GUARDED_BY(lock) __attribute__((pt_guarded_by(lock)))
#define DXMT_ACQUIRE(...) __attribute__((acquire_capability(__VA_ARGS__)))
#define DXMT_RELEASE(...) __attribute__((release_capability(__VA_ARGS__)))
#define DXMT_TRY_ACQUIRE(success, ...) \
  __attribute__((try_acquire_capability(success, ##__VA_ARGS__)))
#define DXMT_REQUIRES(...) \
  __attribute__((requires_capability(__VA_ARGS__)))
#define DXMT_EXCLUDES(...) __attribute__((locks_excluded(__VA_ARGS__)))
#define DXMT_ASSERT_CAPABILITY(...) \
  __attribute__((assert_capability(__VA_ARGS__)))
#define DXMT_ACQUIRED_BEFORE(...) \
  __attribute__((acquired_before(__VA_ARGS__)))
#define DXMT_ACQUIRED_AFTER(...) \
  __attribute__((acquired_after(__VA_ARGS__)))
#define DXMT_NO_THREAD_SAFETY_ANALYSIS \
  __attribute__((no_thread_safety_analysis))
#define DXMT_LIFETIME_BOUND [[clang::lifetimebound]]
#define DXMT_NOESCAPE __attribute__((noescape))
#else
#define DXMT_CAPABILITY(name)
#define DXMT_SCOPED_CAPABILITY
#define DXMT_GUARDED_BY(lock)
#define DXMT_PT_GUARDED_BY(lock)
#define DXMT_ACQUIRE(...)
#define DXMT_RELEASE(...)
#define DXMT_TRY_ACQUIRE(success, ...)
#define DXMT_REQUIRES(...)
#define DXMT_EXCLUDES(...)
#define DXMT_ASSERT_CAPABILITY(...)
#define DXMT_ACQUIRED_BEFORE(...)
#define DXMT_ACQUIRED_AFTER(...)
#define DXMT_NO_THREAD_SAFETY_ANALYSIS
#define DXMT_LIFETIME_BOUND
#define DXMT_NOESCAPE
#endif
