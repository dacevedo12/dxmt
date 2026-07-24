// Drives Wine's own d3d9 conformance modules, the mature reference suite this
// frontend is measured against.
//
// Same envelope as the shader oracle next to it: spawn the module as a separate
// process, bound the wait, and gate on a baseline diff. The reasons differ in
// one way worth stating. Wine's harness exits with a FAILURE COUNT capped at
// 255, so a nonzero status carries no more information than "some assertions
// failed" and cannot be a gate on a corpus that is not yet fully green. What
// distinguishes a crash from failures is the absence of the trailing summary
// line, exactly as for the shader oracle.
//
// The modules run with WINETEST_PLATFORM=windows on every platform. Wine's
// todo_wine marks record where Wine itself is known to deviate, and a todo
// block that SUCCEEDS is counted as a failure, so running as "wine" would score
// this frontend being more correct than Wine as a regression. Holding the
// platform to windows keeps the assertions at their strict meaning and lets the
// baseline carry what is not yet met.

#include <dxmt_test.hpp>

#include <support/wine_process.hpp>

#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr const char *kWineModules[] = {
    "stateblock",
    "visual",
    "d3d9ex",
    "device",
};

// A module that crashes takes the functions after it with it, so it is re-run
// with the offender skipped. The cap keeps a module that crashes repeatedly
// from spending the whole run on it; what it does not finish is reported.
constexpr int kMaxAttempts = 12;

constexpr DWORD kDefaultTimeoutMs = 600000;

DWORD TimeoutMs() {
  if (const char *value = std::getenv("DXMT_D3D9_WINE_TIMEOUT_MS")) {
    const auto parsed = std::strtoul(value, nullptr, 10);
    if (parsed != 0)
      return static_cast<DWORD>(parsed);
  }
  return kDefaultTimeoutMs;
}

bool RuntimeRequired() {
  const char *value = std::getenv("DXMT_TEST_REQUIRE_RUNTIME");
  return value != nullptr && value[0] == '1';
}

std::wstring ExecutableDirectory() {
  auto path = dxmt::test::WineExecutablePath();
  const auto separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos)
    return L".";
  return path.substr(0, separator);
}

bool PathExists(const std::wstring &path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ModulePath(const char *module) {
  const auto directory = ExecutableDirectory();
  const auto name =
      L"d3d9-wine-" + dxmt::test::WidenWineArgument(module) + L"-test.exe";
  for (const auto &candidate :
       {directory + L"\\" + name, directory + L"\\d3d9\\" + name})
    if (PathExists(candidate))
      return candidate;
  return {};
}

std::vector<std::string> LoadBaseline(const char *module) {
  std::vector<std::string> entries;
  const auto directory = ExecutableDirectory();
  std::wstring path;
  for (const auto &candidate :
       {directory + L"\\wine_conformance_baseline.txt",
        directory + L"\\d3d9\\wine_conformance_baseline.txt"})
    if (PathExists(candidate)) {
      path = candidate;
      break;
    }
  if (path.empty())
    return entries;

  // Entries are "<module> <substring>", so one file covers every module and a
  // baselined failure cannot leak across modules.
  const std::string prefix = std::string(module) + " ";
  std::ifstream file(path.c_str());
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line.front() == '#')
      continue;
    if (line.compare(0, prefix.size(), prefix) == 0)
      entries.push_back(line.substr(prefix.size()));
  }
  return entries;
}

// Functions declared unrunnable up front, one per line as "<module> <function>".
// Unlike the crash-discovered skips the driver finds at run time, these are for
// functions that hang or are non-deterministic for a reason outside DXMT, so
// gating on them would make the suite flaky. They are seeded into every run's
// skip list, counted toward coverage, and do not count as a failure. Same file
// layout as the baseline, so the reader is the same.
std::vector<std::string> LoadKnownSkips(const char *module) {
  std::vector<std::string> entries;
  const auto directory = ExecutableDirectory();
  std::wstring path;
  for (const auto &candidate :
       {directory + L"\\wine_known_skips.txt",
        directory + L"\\d3d9\\wine_known_skips.txt"})
    if (PathExists(candidate)) {
      path = candidate;
      break;
    }
  if (path.empty())
    return entries;

  const std::string prefix = std::string(module) + " ";
  std::ifstream file(path.c_str());
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line.front() == '#')
      continue;
    if (line.compare(0, prefix.size(), prefix) == 0)
      entries.push_back(line.substr(prefix.size()));
  }
  return entries;
}

struct RunResult {
  bool started = false;
  bool timed_out = false;
  DWORD exit_code = 0;
  std::string output;
};

RunResult RunModule(const std::wstring &executable, const char *module,
                    const std::string &skip) {
  RunResult result;
  std::vector<char> temp(MAX_PATH + 1);
  const DWORD size = GetTempPathA(static_cast<DWORD>(temp.size()), temp.data());
  std::string log_path(temp.data(), size);
  log_path += "dxmt-d3d9-wine-" + std::to_string(GetCurrentProcessId()) + "-" +
              module + ".log";

  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE log = CreateFileA(log_path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (log == INVALID_HANDLE_VALUE)
    return result;

  // Wine's own harness picks its platform by detecting Wine unless told, and
  // under "wine" a todo block that succeeds counts as a failure, which would
  // score this frontend being more correct than Wine as a regression. The
  // builder sets this for a staged run; setting it here too keeps a plain
  // build-tree run from silently inverting those semantics.
  SetEnvironmentVariableA("WINETEST_PLATFORM", "windows");
  SetEnvironmentVariableA("DXMT_TEST_SKIP", skip.c_str());

  DWORD error = 0;
  auto process = dxmt::test::StartWineProcess(executable, {module}, &error, log);
  if (!process) {
    CloseHandle(log);
    DeleteFileA(log_path.c_str());
    return result;
  }
  result.started = true;

  if (WaitForSingleObject(process->handle, TimeoutMs()) == WAIT_TIMEOUT) {
    result.timed_out = true;
    TerminateProcess(process->handle, 1);
    WaitForSingleObject(process->handle, INFINITE);
  }
  GetExitCodeProcess(process->handle, &result.exit_code);
  CloseHandle(process->handle);
  CloseHandle(log);

  std::ifstream file(log_path.c_str());
  result.output.assign(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
  DeleteFileA(log_path.c_str());
  return result;
}

// Every module ships the list of test functions the build wrapped, which is
// what tells the suite how much of a module actually ran.
std::vector<std::string> LoadManifest(const char *module) {
  std::vector<std::string> names;
  const auto directory = ExecutableDirectory();
  const auto leaf = dxmt::test::WidenWineArgument(module) + L".functions";
  for (const auto &candidate :
       {directory + L"\\" + leaf, directory + L"\\d3d9\\" + leaf}) {
    if (!PathExists(candidate))
      continue;
    std::ifstream file(candidate.c_str());
    std::string line;
    while (std::getline(file, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (!line.empty())
        names.push_back(line);
    }
    break;
  }
  return names;
}

// Each wrapped function announces itself before running. A crash is attributed
// by Wine's exception filter, but a hang prints nothing, so the last
// announcement is the only thing that names the function that stopped a module.
std::vector<std::string> AnnouncedFunctions(const std::string &output) {
  static constexpr const char kMarker[] = "dxmt-conformance: running ";
  std::vector<std::string> names;
  std::size_t position = 0;
  while ((position = output.find(kMarker, position)) != std::string::npos) {
    position += sizeof(kMarker) - 1;
    const auto end = output.find_first_of("\r\n", position);
    names.push_back(output.substr(position, end - position));
    position = end == std::string::npos ? output.size() : end;
  }
  return names;
}

bool ReachedSummary(const std::string &output) {
  return output.find("tests executed") != std::string::npos;
}

// Every failing form routes through the same context printer, so each carries
// its file and line. A todo block that succeeds is included deliberately: it
// means the assertion now passes where Wine records a known deviation, which is
// a result worth surfacing rather than discarding.
std::vector<std::string> FailureLines(const std::string &output) {
  static const char *kMarkers[] = {
      "Test failed:",
      "Test succeeded inside todo block:",
      "Test marked flaky:",
      "Test succeeded inside flaky todo block:",
  };
  std::vector<std::string> failures;
  std::size_t begin = 0;
  while (begin < output.size()) {
    auto end = output.find('\n', begin);
    if (end == std::string::npos)
      end = output.size();
    std::string line = output.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    for (const auto *marker : kMarkers)
      if (line.find(marker) != std::string::npos) {
        failures.push_back(line);
        break;
      }
    begin = end + 1;
  }
  return failures;
}

bool IsBaselined(const std::string &failure,
                 const std::vector<std::string> &baseline) {
  for (const auto &entry : baseline)
    if (failure.find(entry) != std::string::npos)
      return true;
  return false;
}

std::string Tail(const std::string &output, std::size_t lines) {
  std::size_t position = output.size();
  for (std::size_t i = 0; i < lines && position != 0; ++i) {
    const auto previous = output.find_last_of('\n', position - 1);
    if (previous == std::string::npos)
      return output;
    position = previous;
  }
  return output.substr(position);
}

class WineConformanceTest : public testing::TestWithParam<const char *> {};

TEST_P(WineConformanceTest, MatchesBaseline) {
  const char *module = GetParam();

  // Not having the corpus at all is a build configuration, not a defect, so it
  // stays a skip even in a staged run. Having been built and then not found is
  // a staging failure and is reported as one.
#ifndef DXMT_D3D9_WINE_TESTS_BUILT
  GTEST_SKIP() << "Wine d3d9 conformance modules were not built; point "
                  "wine_source_path at a Wine source tree (the external/wine "
                  "submodule) to enable them";
#else
  const auto executable = ModulePath(module);
  if (executable.empty()) {
    if (RuntimeRequired())
      FAIL() << "Wine d3d9 conformance module " << module
             << " was built but is not next to the test image";
    GTEST_SKIP() << "Wine d3d9 conformance module " << module << " not found";
  }

  // Run the module, and if it does not reach its summary line, blame the last
  // function it announced and run it again without that one. Repeat until it
  // finishes or the attempt cap is reached. Which functions need isolating is
  // therefore discovered here rather than kept in a list that would drift out
  // of date, and the cost is one extra process per function that actually
  // fails, not one per function in the corpus.
  // Declared skips are seeded into every run so the suite stays deterministic.
  // They are kept separate from the crash-discovered skips below: a declared
  // skip is expected and does not fail the suite, while a function that dies on
  // its own run is a real regression that must.
  const auto known_skips = LoadKnownSkips(module);
  std::vector<std::string> skipped;
  std::string accumulated;
  RunResult result;

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    std::string skip;
    for (const auto &name : known_skips)
      skip += (skip.empty() ? "" : ",") + name;
    for (const auto &name : skipped)
      skip += (skip.empty() ? "" : ",") + name;

    result = RunModule(executable, module, skip);
    ASSERT_TRUE(result.started) << "failed to start " << module;
    accumulated += result.output;

    if (ReachedSummary(result.output))
      break;

    const auto announced = AnnouncedFunctions(result.output);
    if (announced.empty()) {
      // Nothing ran at all, so there is no function to blame and re-running
      // would repeat the same result. The coverage check below reports it.
      break;
    }
    skipped.push_back(announced.back());
  }

  ASSERT_TRUE(ReachedSummary(result.output))
      << module << " never reached its summary line in " << kMaxAttempts
      << " attempts, so it did not finish; the exit status is a failure count "
         "and cannot distinguish this (exit "
      << result.exit_code << ")\n"
      << Tail(result.output, 20);

  // A module that stops early reports no failures, and a gate that only looks
  // for failures would read that as success. The clearest case is the display
  // mode check these modules open with, which abandons the whole module when
  // the current mode does not match the registry mode. Account for every
  // function instead: what ran, plus what was skipped after a crash.
  const auto manifest = LoadManifest(module);
  if (!manifest.empty()) {
    const auto announced = AnnouncedFunctions(accumulated);
    std::vector<std::string> unique(announced.begin(), announced.end());
    // A declared skip never runs and so never announces itself; count it as
    // covered rather than missing, but only if it is a real function in the
    // manifest, so a typo in the skip list still shows up as a coverage gap.
    for (const auto &name : known_skips)
      if (std::find(manifest.begin(), manifest.end(), name) != manifest.end())
        unique.push_back(name);
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    EXPECT_GE(unique.size(), manifest.size())
        << module << " accounted for only " << unique.size() << " of "
        << manifest.size()
        << " test functions, so most of it never ran and a clean result would "
           "mean nothing\n"
        << Tail(accumulated, 20);
  }

  if (!skipped.empty()) {
    std::string names;
    for (const auto &name : skipped)
      names += "  " + name + "\n";
    ADD_FAILURE() << skipped.size() << " function(s) in " << module
                  << " did not survive their own run and were skipped so the "
                     "rest of the module could report:\n"
                  << names;
  }

  const auto baseline = LoadBaseline(module);
  std::vector<std::string> unexpected;
  for (const auto &failure : FailureLines(accumulated))
    if (!IsBaselined(failure, baseline))
      unexpected.push_back(failure);

  // Carry the WHOLE unbaselined set, not a slice: recording a baseline needs
  // every line. The assertion message is the reliable place for it. The suite
  // scheduler captures each child's stdout and stderr into the suite log it
  // reports, and the message is a string with no length limit, so the full set
  // rides that log back. The raw dxmt-conformance-failure lines are kept as a
  // second, machine-greppable channel for a harness that reads stdout directly.
  if (!unexpected.empty()) {
    std::printf("dxmt-conformance: %zu unbaselined failure(s) in %s\n",
                unexpected.size(), module);
    for (const auto &failure : unexpected)
      std::printf("dxmt-conformance-failure: %s\n", failure.c_str());
    std::fflush(stdout);
  }

  std::string report;
  for (const auto &failure : unexpected)
    report += "  " + failure + "\n";
  EXPECT_TRUE(unexpected.empty())
      << unexpected.size() << " assertion failure(s) not in the baseline for "
      << module << ":\n"
      << report;
#endif
}

// These modules create windows, take focus and query the display mode, and one
// of them abandons itself entirely when the current mode stops matching the
// registry mode. A test running beside them that changes the mode would gut a
// module into a clean-looking result, so they share one serial group and run
// one at a time. They are also minutes long, which the scheduler would
// otherwise cost at a single unit and pack into a parallel shard.
DXMT_GROUP_SERIAL_TESTS("Wine/WineConformanceTest.*", "d3d9-wine-conformance");
DXMT_SLOW_TEST_PATTERN("Wine/WineConformanceTest.*/*");

INSTANTIATE_TEST_SUITE_P(Wine, WineConformanceTest,
                         testing::ValuesIn(kWineModules),
                         [](const testing::TestParamInfo<const char *> &info) {
                           return std::string(info.param);
                         });

} // namespace
