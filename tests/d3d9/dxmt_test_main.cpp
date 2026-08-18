#include "dxmt_test.hpp"

namespace dxmt::test {

CaseState &
currentCase() {
  static CaseState state;
  return state;
}

std::vector<RegisteredCase> &
registry() {
  static std::vector<RegisteredCase> cases;
  return cases;
}

int
runAllTests(std::string_view filter) {
  size_t passed = 0, failed = 0, skipped = 0, filtered = 0;
  for (const auto &entry : registry()) {
    if (!filter.empty() && entry.name.find(filter) == std::string::npos) {
      filtered++;
      continue;
    }
    currentCase() = {};
    std::printf("[ RUN      ] %s\n", entry.name.c_str());
    std::fflush(stdout);

    entry.make()->TestBody();

    const auto &state = currentCase();
    switch (state.outcome) {
    case Outcome::Passed:
      passed++;
      std::printf("[       OK ] %s\n", entry.name.c_str());
      break;
    case Outcome::Skipped:
      skipped++;
      std::printf("[  SKIPPED ] %s\n%s\n", entry.name.c_str(), state.detail.c_str());
      break;
    case Outcome::Failed:
      failed++;
      std::printf("[  FAILED  ] %s\n%s\n", entry.name.c_str(), state.detail.c_str());
      break;
    }
    std::fflush(stdout);
  }

  std::printf("[==========] %zu passed, %zu failed, %zu skipped", passed, failed, skipped);
  if (filtered)
    std::printf(", %zu filtered out", filtered);
  std::printf("\n");
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

} // namespace dxmt::test

// One optional argument, a substring of the case name. A module takes minutes,
// so re-running the one that failed has to be possible without the other three.
int
main(int argc, char **argv) {
  return dxmt::test::runAllTests(argc > 1 ? argv[1] : "");
}
