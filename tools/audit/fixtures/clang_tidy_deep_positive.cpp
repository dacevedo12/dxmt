/*
 * Deep-analysis checker fixture.
 *
 * A check that reports nothing is indistinguishable from a check that never
 * ran.  Several deep checks currently have zero findings across the tree,
 * which is only reassuring if we can prove they would have spoken up.  Each
 * function below is a minimal, deliberate violation of one such check; the
 * audit requires every one of them to be reported.
 *
 * If a check is ever dropped from .clang-tidy-deep, or silently stops working
 * after a toolchain bump, the corresponding expectation here fails instead of
 * the tree looking clean.
 *
 * This file is compiled against the macOS SDK (-isysroot) so that libc++ and
 * CoreFoundation are available: several of the checks below only have a
 * meaningful trigger in terms of real library types (std::optional,
 * std::string_view, CFStringRef) rather than hand-rolled stand-ins.
 */

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>

// bugprone-integer-division: integer division fed into a floating-point
// context silently truncates.
double TriggerIntegerDivision(int numerator, int denominator) {
  return numerator / denominator;
}

// bugprone-too-small-loop-variable: the induction variable cannot represent
// every value the bound can take, so the loop can never terminate.
long TriggerTooSmallLoopVariable(long bound) {
  long total = 0;
  for (short index = 0; index < bound; ++index)
    total += index;
  return total;
}

// bugprone-inc-dec-in-conditions: the variable is both mutated and read in the
// same condition, so the observed value depends on evaluation order.
bool TriggerIncDecInConditions(int &value) {
  return ++value > 0 && value < 10;
}

// clang-analyzer-optin.cplusplus.UninitializedObject: constructor leaves a
// member holding indeterminate storage.
struct TriggerUninitializedObject final {
  TriggerUninitializedObject() : initialized_(0) {}

  int initialized_;
  int never_initialized_;
};

int ReadUninitializedObject() {
  const TriggerUninitializedObject object;
  return object.never_initialized_;
}

// clang-analyzer-optin.cplusplus.VirtualCall: the override does not exist yet
// while the base constructor runs.
struct TriggerVirtualCallBase {
  TriggerVirtualCallBase() { Describe(); }
  virtual ~TriggerVirtualCallBase() = default;
  virtual void Describe();
};

struct TriggerVirtualCallDerived final : TriggerVirtualCallBase {
  void Describe() override;
};

// bugprone-assert-side-effect: the assertion carries the increment, so the
// counter stops advancing in an NDEBUG build.
void TriggerAssertSideEffect(int &counter) { assert(++counter > 0); }

// bugprone-dangling-handle: the view outlives the temporary string it points
// into -- the shape of every "return a std::string_view of a formatted name"
// helper that reads freed memory.
std::string_view TriggerDanglingHandle(const char *text) {
  return std::string(text);
}

// bugprone-exception-escape: a noexcept function that can throw terminates the
// process instead of propagating the error.
void TriggerExceptionEscape() noexcept { throw 1; }

// bugprone-infinite-loop: nothing in the body advances the induction variable,
// so the loop condition can never become false.
int TriggerInfiniteLoop(int limit) {
  int total = 0;
  int index = 0;
  while (index < limit)
    total += index;
  return total;
}

// bugprone-misplaced-widening-cast: the multiply is performed in 32 bits and
// only widened afterwards, so the cast hides an overflow instead of avoiding
// one.
long TriggerMisplacedWideningCast(int width, int height) {
  return static_cast<long>(width * height);
}

// bugprone-implicit-widening-of-multiplication-result: a byte count computed
// as a 32-bit product before being widened -- how an undersized staging
// allocation turns into an out-of-bounds copy.
std::size_t TriggerImplicitWideningMultiplication(int width, int height) {
  std::size_t bytes = width * height;
  return bytes;
}

// bugprone-multiple-statement-macro: only the first statement is guarded by
// the if, so the second runs unconditionally.
#define FIXTURE_MULTIPLE_STATEMENT(first, second) ++(first); ++(second)
void TriggerMultipleStatementMacro(int &first, int &second, bool flag) {
  if (flag)
    FIXTURE_MULTIPLE_STATEMENT(first, second);
}

// bugprone-sizeof-container: measures the container header, not the elements,
// which is how a memcpy of a vector's payload ends up copying a few pointers.
std::size_t TriggerSizeofContainer(const std::vector<int> &values) {
  return sizeof(values);
}

// bugprone-sizeof-expression: sizeof(sizeof(x)) is the size of a size_t, never
// the size the author meant.
std::size_t TriggerSizeofExpression(int value) { return sizeof(sizeof(value)); }

// bugprone-suspicious-memset-usage: the fill value is the character '0'
// (0x30), not the integer 0, so the buffer is filled with ASCII zeroes.
void TriggerSuspiciousMemset(void *buffer, std::size_t size) {
  std::memset(buffer, '0', size);
}

// bugprone-unused-raii: the scope guard is a temporary destroyed at the
// semicolon, so the region it was meant to guard is unprotected.
struct FixtureScopedFlag final {
  explicit FixtureScopedFlag(int depth);
  ~FixtureScopedFlag();
};
int TriggerUnusedRaii(int depth) {
  FixtureScopedFlag(depth + 1);
  return depth;
}

// bugprone-signed-char-misuse: a signed char widened to int sign-extends, so
// byte values above 0x7f arrive negative.
int TriggerSignedCharMisuse(signed char value) {
  int widened = value;
  return widened;
}

// bugprone-unchecked-optional-access: dereferencing an optional that was never
// proven to hold a value.
int TriggerUncheckedOptionalAccess(const std::optional<int> &value) {
  return *value;
}

// misc-no-recursion: unbounded recursion driven by a caller-supplied depth is
// a stack overflow waiting for the right input.
int TriggerNoRecursion(int depth) {
  return depth <= 0 ? 0 : TriggerNoRecursion(depth - 1);
}

// concurrency-mt-unsafe: getenv shares process-global state with every other
// thread, exactly like the std::localtime race this check was re-enabled for.
const char *TriggerMtUnsafe(const char *name) { return std::getenv(name); }

// clang-analyzer-deadcode.DeadStores: the first computation is overwritten
// before anyone reads it, which usually means the wrong variable was assigned.
int TriggerDeadStore(int input) {
  int value = input * 2;
  value = input + 1;
  return value;
}

// clang-analyzer-nullability.NullPassedToNonnull: a null literal handed to a
// parameter annotated as never-null.
void FixtureRequiresNonnull(int *_Nonnull pointer);
void TriggerNullPassedToNonnull() { FixtureRequiresNonnull(nullptr); }

// clang-analyzer-unix.Malloc: the allocation escapes nowhere and is never
// freed -- a plain leak on every call.
void TriggerMallocLeak(std::size_t size) {
  void *block = std::malloc(size);
  (void)block;
}

// clang-analyzer-unix.MismatchedDeallocator: new paired with free skips the
// destructor and hands the pointer to the wrong allocator.
void TriggerMismatchedDeallocator() {
  int *value = new int(1);
  std::free(value);
}

// clang-analyzer-unix.cstring.BadSizeArg: strncat's bound is the space
// remaining, not the buffer size, so this writes past the end.
void TriggerBadSizeArg(const char *text) {
  char buffer[16];
  buffer[0] = '\0';
  std::strncat(buffer, text, sizeof(buffer));
}

// clang-analyzer-unix.cstring.NullArg: a null pointer reaching a string
// routine that dereferences it unconditionally.
std::size_t TriggerCStringNullArg() {
  const char *text = nullptr;
  return std::strlen(text);
}

// performance-noexcept-move-constructor: a move constructor that is not
// noexcept makes every container growth fall back to copying.
class FixtureThrowingMove final {
public:
  FixtureThrowingMove() = default;
  FixtureThrowingMove(FixtureThrowingMove &&other) : value_(other.value_) {}
  FixtureThrowingMove &operator=(FixtureThrowingMove &&other) {
    value_ = other.value_;
    return *this;
  }

private:
  int value_ = 0;
};

// clang-analyzer-osx.cocoa.RetainCount: a CoreFoundation object created with a
// Create rule function and never released.  This is the only deep check that
// needs Objective-C/CoreFoundation semantics, so it is fixtured through the
// CF side of the checker rather than an ObjC class.
void TriggerRetainCountLeak() {
  CFStringRef text = CFStringCreateWithCString(kCFAllocatorDefault, "leak",
                                               kCFStringEncodingUTF8);
  (void)text;
}
