#pragma once

// The suite's test runner.
//
// It covers exactly what the two specs use: parameterised cases over a fixed
// value list, fatal and non-fatal assertions, and an explicit skip, each able
// to carry a streamed message. Both specs spend their time in child processes,
// so the runner needs no sharding, timing or death-test machinery, and keeping
// it here means the conformance gate has no dependency to resolve before it can
// tell whether the layer still conforms.

#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt::test {

enum class Outcome { Passed, Failed, Skipped };

class Message {
public:
  template <typename T>
  Message &
  operator<<(const T &value) {
    stream_ << value;
    return *this;
  }

  std::string
  str() const {
    return stream_.str();
  }

private:
  std::ostringstream stream_;
};

// The outcome of the case being run. A case reports at most one outcome: the
// first assertion to fail decides it, and later EXPECTs append their message
// rather than overwriting the reason it failed.
struct CaseState {
  Outcome outcome = Outcome::Passed;
  std::string detail;
};

CaseState &currentCase();

class AssertHelper {
public:
  AssertHelper(Outcome outcome, const char *file, int line) : outcome_(outcome), file_(file), line_(line) {}

  void
  operator=(const Message &message) const {
    auto &state = currentCase();
    if (state.outcome == Outcome::Passed)
      state.outcome = outcome_;
    if (!state.detail.empty())
      state.detail += "\n";
    state.detail += std::string(file_) + ":" + std::to_string(line_) + ": " + message.str();
  }

private:
  Outcome outcome_;
  const char *file_;
  int line_;
};

class TestBase {
public:
  virtual ~TestBase() = default;
  virtual void TestBody() = 0;
};

struct RegisteredCase {
  std::string name;
  std::function<std::unique_ptr<TestBase>()> make;
};

std::vector<RegisteredCase> &registry();

// Declared bodies of one parameterised fixture, filled by TEST_P and consumed
// by the INSTANTIATE below it. Both live at file scope in the same translation
// unit, so the bodies are registered first.
template <typename Fixture> struct ParamBody {
  std::string name;
  std::function<std::unique_ptr<Fixture>()> make;
};

template <typename Fixture>
std::vector<ParamBody<Fixture>> &
paramBodies() {
  static std::vector<ParamBody<Fixture>> bodies;
  return bodies;
}

template <typename Fixture> struct ParamBodyRegistrar {
  ParamBodyRegistrar(const char *name, std::function<std::unique_ptr<Fixture>()> make) {
    paramBodies<Fixture>().push_back({name, std::move(make)});
  }
};

int runAllTests(std::string_view filter = {});

} // namespace dxmt::test

namespace testing {

template <typename T> struct TestParamInfo {
  T param;
  size_t index;
};

template <typename T> class TestWithParam : public ::dxmt::test::TestBase {
public:
  const T &
  GetParam() const {
    return param_;
  }

  void
  SetParam(const T &param) {
    param_ = param;
  }

private:
  T param_{};
};

template <typename Container>
auto
ValuesIn(const Container &values) {
  using Value = std::decay_t<decltype(*std::begin(values))>;
  return std::vector<Value>(std::begin(values), std::end(values));
}

// A function rather than a class so the value list and the naming lambda are
// each written once at the call site: a lambda expression has a unique closure
// type, so naming its type in a second macro expansion would name a different
// type than the argument passed.
template <typename Fixture, typename Value, typename Namer>
int
instantiate(const char *prefix, const std::vector<Value> &values, Namer namer) {
  size_t index = 0;
  for (const auto &value : values) {
    const std::string label = namer(TestParamInfo<Value>{value, index});
    for (const auto &body : ::dxmt::test::paramBodies<Fixture>()) {
      auto make = body.make;
      ::dxmt::test::registry().push_back(
          {std::string(prefix) + "/" + body.name + "/" + label,
           [make, value]() -> std::unique_ptr<::dxmt::test::TestBase> {
             auto instance = make();
             instance->SetParam(value);
             return instance;
           }}
      );
    }
    index++;
  }
  return 0;
}

} // namespace testing

#define DXMT_TEST_CONCAT_INNER_(a, b) a##b
#define DXMT_TEST_CONCAT_(a, b) DXMT_TEST_CONCAT_INNER_(a, b)

#define DXMT_TEST_REPORT_(outcome) ::dxmt::test::AssertHelper(outcome, __FILE__, __LINE__) = ::dxmt::test::Message()

// Fatal forms end the case, so they carry the return; the non-fatal ones record
// and let the body continue, which is what lets a spec collect every mismatch
// in one run instead of one per invocation.
#define DXMT_TEST_FATAL_(outcome) return DXMT_TEST_REPORT_(outcome)

#define FAIL() DXMT_TEST_FATAL_(::dxmt::test::Outcome::Failed)
#define GTEST_SKIP() DXMT_TEST_FATAL_(::dxmt::test::Outcome::Skipped)

// The switch makes each check one unambiguous statement, so an assertion used
// as the body of a brace-less if cannot capture that if's else.
#define DXMT_TEST_ELSE_BLOCKER_                                                                                        \
  switch (0)                                                                                                           \
  case 0:                                                                                                              \
  default:

#define DXMT_TEST_ASSERT_(condition, text)                                                                             \
  DXMT_TEST_ELSE_BLOCKER_                                                                                              \
  if (condition) {                                                                                                     \
  } else                                                                                                               \
    DXMT_TEST_FATAL_(::dxmt::test::Outcome::Failed) << text << ": "

#define DXMT_TEST_EXPECT_(condition, text)                                                                             \
  DXMT_TEST_ELSE_BLOCKER_                                                                                              \
  if (condition) {                                                                                                     \
  } else                                                                                                               \
    DXMT_TEST_REPORT_(::dxmt::test::Outcome::Failed) << text << ": "

#define ASSERT_TRUE(condition) DXMT_TEST_ASSERT_(condition, "expected " #condition)
#define ASSERT_FALSE(condition) DXMT_TEST_ASSERT_(!(condition), "expected !" #condition)
#define ASSERT_NE(lhs, rhs) DXMT_TEST_ASSERT_((lhs) != (rhs), "expected " #lhs " != " #rhs)
#define EXPECT_TRUE(condition) DXMT_TEST_EXPECT_(condition, "expected " #condition)
#define EXPECT_GE(lhs, rhs) DXMT_TEST_EXPECT_((lhs) >= (rhs), "expected " #lhs " >= " #rhs)
#define ADD_FAILURE() DXMT_TEST_REPORT_(::dxmt::test::Outcome::Failed)

#define TEST_P(fixture, name)                                                                                          \
  class fixture##_##name : public fixture {                                                                            \
  public:                                                                                                              \
    void TestBody() override;                                                                                          \
  };                                                                                                                   \
  static const ::dxmt::test::ParamBodyRegistrar<fixture> DXMT_TEST_CONCAT_(dxmt_param_body_, __LINE__)(                 \
      #name, []() { return std::unique_ptr<fixture>(new fixture##_##name()); }                                          \
  );                                                                                                                   \
  void fixture##_##name::TestBody()

#define INSTANTIATE_TEST_SUITE_P(prefix, fixture, generator, namer)                                                    \
  static const int DXMT_TEST_CONCAT_(dxmt_instantiate_, __LINE__) =                                                    \
      ::testing::instantiate<fixture>(#prefix, generator, namer)

// A case that takes a window, the focus or the display mode cannot share a
// machine with another that does. This runner is serial, so these record the
// requirement without needing to enforce it.
namespace dxmt::test {

class SerialTestRegistration {
public:
  explicit SerialTestRegistration(std::string_view) {}
};

} // namespace dxmt::test

#define DXMT_GROUP_SERIAL_TESTS(pattern, group)                                                                        \
  static const ::dxmt::test::SerialTestRegistration DXMT_TEST_CONCAT_(dxmt_serial_group_, __LINE__)(pattern)

#define DXMT_SLOW_TEST_PATTERN(pattern)                                                                                \
  static const ::dxmt::test::SerialTestRegistration DXMT_TEST_CONCAT_(dxmt_slow_test_, __LINE__)(pattern)
