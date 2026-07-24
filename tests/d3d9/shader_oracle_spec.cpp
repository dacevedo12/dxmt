// Drives the vendored vkd3d shader_runner over the in-tree d3d9 corpus.
//
// The runner is a separate Windows PE rather than linked-in code: it carries
// vkd3d's own test harness, which owns main() and its own global result state,
// and the vendored sources are kept byte-identical to upstream. Spawning it is
// also what the suite scheduler itself does with every worker.
//
// The gate is a baseline diff, not the exit status. The runner returns nonzero
// when any probe fails OR any todo unexpectedly passes, and a run carries
// environmental failures that are present in both environments (a missing
// optional shader compiler, for instance). Comparing against a checked-in
// baseline is what turns a partially red corpus into a usable gate: a failure
// that is already in the baseline is not news, a new one is.

#include <dxmt_test.hpp>

#include <support/wine_process.hpp>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

// The corpus cases, one outer test each. Kept in sync with the file names under
// tests/d3d9/shader-oracle/corpus.
constexpr const char *kCorpusCases[] = {
    "dp2add", "loop", "lrp", "mnxn",    "nrm",
    "pow",    "rep",  "texdepth", "texreg",
};

// The suite scheduler has no per-case timeout of its own, so a wedged runner
// would otherwise hold the whole unit run until the outer Meson timeout.
constexpr DWORD kDefaultTimeoutMs = 120000;

DWORD TimeoutMs() {
  if (const char *value = std::getenv("DXMT_D3D9_ORACLE_TIMEOUT_MS")) {
    const auto parsed = std::strtoul(value, nullptr, 10);
    if (parsed != 0)
      return static_cast<DWORD>(parsed);
  }
  return kDefaultTimeoutMs;
}

// The builder sets this for a staged run, where a missing runner or a missing
// dxmt runtime is a defect rather than a configuration this test may excuse. A
// plain build-tree run leaves it unset and keeps the skip.
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

// Resolved relative to the test image so the same layout works from the build
// tree and from a packaged oracle drop, where no source tree exists.
std::wstring FirstExistingPath(const std::vector<std::wstring> &candidates) {
  for (const auto &candidate : candidates)
    if (PathExists(candidate))
      return candidate;
  return {};
}

std::wstring RunnerPath() {
  if (const char *value = std::getenv("DXMT_D3D9_ORACLE_RUNNER"))
    return dxmt::test::WidenWineArgument(value);

  const auto directory = ExecutableDirectory();
  return FirstExistingPath({
      directory + L"\\d3d9-shader-oracle-runner.exe",
      directory + L"\\d3d9\\shader-oracle\\d3d9-shader-oracle-runner.exe",
  });
}

std::wstring CorpusDirectory() {
  if (const char *value = std::getenv("DXMT_D3D9_ORACLE_CORPUS"))
    return dxmt::test::WidenWineArgument(value);

  const auto directory = ExecutableDirectory();
  std::vector<std::wstring> candidates = {
      directory + L"\\d3d9-corpus",
      directory + L"\\d3d9\\d3d9-corpus",
  };
  // Last resort so a plain build-tree run works without any environment: the
  // packaged layouts above are what the native Windows oracle sees, where no
  // source tree exists.
#ifdef DXMT_D3D9_ORACLE_CORPUS_DIR
  candidates.push_back(dxmt::test::WidenWineArgument(DXMT_D3D9_ORACLE_CORPUS_DIR));
#endif
  return FirstExistingPath(candidates);
}

std::wstring BaselinePath() {
  const auto directory = ExecutableDirectory();
  return FirstExistingPath({
      directory + L"\\shader_oracle_baseline.txt",
      directory + L"\\d3d9\\shader_oracle_baseline.txt",
  });
}

// Baseline entries are matched as substrings rather than whole lines: the
// runner interpolates operating-system error text into some messages, and that
// text is localized, so an exact match would depend on the runner's locale.
std::vector<std::string> LoadBaseline() {
  std::vector<std::string> entries;
  const auto path = BaselinePath();
  if (path.empty())
    return entries;

  std::ifstream file(path.c_str());
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line.front() == '#')
      continue;
    entries.push_back(line);
  }
  return entries;
}

struct RunResult {
  bool started = false;
  bool timed_out = false;
  DWORD exit_code = 0;
  std::string output;
};

std::string TemporaryLogPath(const char *test_case) {
  std::vector<char> buffer(MAX_PATH + 1);
  const DWORD size = GetTempPathA(static_cast<DWORD>(buffer.size()), buffer.data());
  std::string path(buffer.data(), size);
  path += "dxmt-d3d9-oracle-" + std::to_string(GetCurrentProcessId()) + "-";
  path += test_case;
  path += ".log";
  return path;
}

RunResult RunCase(const std::wstring &runner, const std::wstring &corpus_file,
                  const char *test_case) {
  RunResult result;
  const auto log_path = TemporaryLogPath(test_case);

  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE log = CreateFileA(log_path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (log == INVALID_HANDLE_VALUE)
    return result;

  DWORD error = 0;
  std::vector<std::string> arguments;
  {
    const auto length =
        WideCharToMultiByte(CP_UTF8, 0, corpus_file.c_str(), -1, nullptr, 0,
                            nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length ? length - 1 : 0), '\0');
    if (length > 1)
      WideCharToMultiByte(CP_UTF8, 0, corpus_file.c_str(), -1, narrow.data(),
                          length, nullptr, nullptr);
    arguments.push_back(narrow);
  }

  auto process = dxmt::test::StartWineProcess(runner, arguments, &error, log);
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

  std::ifstream file(log_path);
  result.output.assign(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
  DeleteFileA(log_path.c_str());
  return result;
}

// A run that produced no summary line did not finish its corpus file, which is
// the only reliable crash signal: the exit status is a failure count, so a high
// value means many failed probes rather than a crash.
bool ReachedSummary(const std::string &output) {
  return output.find("tests executed") != std::string::npos;
}

std::vector<std::string> FailureLines(const std::string &output) {
  std::vector<std::string> failures;
  std::size_t begin = 0;
  while (begin < output.size()) {
    auto end = output.find('\n', begin);
    if (end == std::string::npos)
      end = output.size();
    std::string line = output.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.find("Test failed:") != std::string::npos)
      failures.push_back(line);
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

class ShaderOracleTest : public testing::TestWithParam<const char *> {};

TEST_P(ShaderOracleTest, MatchesBaseline) {
  const char *test_case = GetParam();

  const auto runner = RunnerPath();
  if (runner.empty()) {
    if (RuntimeRequired())
      FAIL() << "shader oracle runner not found next to the test image; a "
                "staged run must carry it";
    GTEST_SKIP() << "shader oracle runner not found next to the test image; "
                    "set DXMT_D3D9_ORACLE_RUNNER to override";
  }

  const auto corpus = CorpusDirectory();
  if (corpus.empty()) {
    if (RuntimeRequired())
      FAIL() << "shader oracle corpus not found next to the test image; a "
                "staged run must carry it";
    GTEST_SKIP() << "shader oracle corpus not found next to the test image; "
                    "set DXMT_D3D9_ORACLE_CORPUS to override";
  }

  const auto corpus_file =
      corpus + L"\\" + dxmt::test::WidenWineArgument(test_case) + L".shader_test";
  ASSERT_TRUE(PathExists(corpus_file)) << "missing corpus file for " << test_case;

  const auto result = RunCase(runner, corpus_file, test_case);
  ASSERT_TRUE(result.started) << "failed to start the shader oracle runner";
  ASSERT_FALSE(result.timed_out)
      << "shader oracle timed out after " << TimeoutMs() << " ms\n"
      << Tail(result.output, 20);
  ASSERT_TRUE(ReachedSummary(result.output))
      << "shader oracle produced no summary line, so it did not finish the "
         "corpus file (exit " << result.exit_code << ")\n"
      << Tail(result.output, 20);

  // Wine serves its own d3d9 when the dxmt runtime is not on the search path,
  // and the corpus would then pass against the wrong driver and report green.
  // The runner echoes the adapter's driver string, and dxmt is the only one
  // that names itself, so assert on it wherever a runtime is expected.
  if (RuntimeRequired())
    ASSERT_NE(result.output.find("DXMT (Metal)"), std::string::npos)
        << "the corpus ran against a driver that is not dxmt, so this result "
           "says nothing about dxmt\n"
        << Tail(result.output, 20);

  const auto baseline = LoadBaseline();
  std::vector<std::string> unexpected;
  for (const auto &failure : FailureLines(result.output))
    if (!IsBaselined(failure, baseline))
      unexpected.push_back(failure);

  std::string report;
  for (const auto &failure : unexpected)
    report += "  " + failure + "\n";
  EXPECT_TRUE(unexpected.empty())
      << unexpected.size() << " probe failure(s) not in the baseline:\n"
      << report;
}

INSTANTIATE_TEST_SUITE_P(Corpus, ShaderOracleTest,
                         testing::ValuesIn(kCorpusCases),
                         [](const testing::TestParamInfo<const char *> &info) {
                           return std::string(info.param);
                         });

} // namespace
