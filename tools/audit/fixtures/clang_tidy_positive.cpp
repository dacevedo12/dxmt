#include "dxmt_thread_safety.h"

namespace std {
template <typename T>
constexpr T &&move(T &value) noexcept {
  return static_cast<T &&>(value);
}
} // namespace std

class FixtureMoveOnly final {
public:
  FixtureMoveOnly() = default;
  FixtureMoveOnly(FixtureMoveOnly &&) noexcept = default;
  FixtureMoveOnly &operator=(FixtureMoveOnly &&) noexcept = default;
  int value() const noexcept { return 1; }
};

class DXMT_CAPABILITY("fixture-mutex") FixtureMutex final {
public:
  void lock() DXMT_ACQUIRE() {}
  void unlock() DXMT_RELEASE() {}
};

class FixtureGuardedState final {
public:
  int readWithoutLock() const { return value_; }

private:
  mutable FixtureMutex mutex_;
  int value_ DXMT_GUARDED_BY(mutex_) = 0;
};

int TriggerUseAfterMove() {
  FixtureMoveOnly source;
  FixtureMoveOnly destination = std::move(source);
  return source.value() + destination.value();
}

void TriggerMismatchedDelete() {
  int *values = new int[4];
  delete values;
}

int TriggerUseAfterFree() {
  int *value = new int(9);
  delete value;
  return *value;
}

int TriggerPointerArithmetic(const int *values, unsigned long index) {
  return *(values + index);
}

/*
 * Lock-ordering fixture.
 *
 * Deadlock prevention in this project relies on declaring the acquisition
 * order between real member mutexes (see DeviceResidencySubmissionOwner vs
 * DeviceResidencyMutex).  Clang only verifies that ordering under
 * -Wthread-safety-beta, so a silent loss of that flag would turn the whole
 * mechanism into decoration without any build failing.  This fixture takes the
 * two locks in the wrong order on purpose; the audit requires the resulting
 * "must be acquired before" diagnostic to actually appear.
 */
class FixtureOrderedLocks final {
public:
  void CorrectOrder() {
    first_.lock();
    second_.lock();
    second_.unlock();
    first_.unlock();
  }

  void TriggerLockOrderInversion() {
    second_.lock();
    first_.lock();
    first_.unlock();
    second_.unlock();
  }

private:
  FixtureMutex first_;
  FixtureMutex second_ DXMT_ACQUIRED_AFTER(first_);
};
