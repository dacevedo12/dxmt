#include "wine_process.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Suite {
  std::string_view name;
  std::wstring_view executable;
};

struct SuiteRun {
  const Suite *suite = nullptr;
  std::string log_path;
  dxmt::test::WineProcess process;
};

struct JobSelection {
  std::size_t count = 1;
  bool explicit_count = false;
};

constexpr std::array<Suite, 5> kSuites = {{
    {"framework", L"dxmt-wine-framework-tests.exe"},
    {"d3d9", L"dxmt-wine-d3d9-tests.exe"},
    {"d3d10", L"dxmt-wine-d3d10-tests.exe"},
    {"d3d11", L"dxmt-wine-d3d11-tests.exe"},
    {"d3d12", L"dxmt-wine-d3d12-tests.exe"},
}};

std::optional<std::size_t> ParsePositiveInteger(std::string_view value) {
  std::size_t result = 0;
  const auto parse =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parse.ec != std::errc() || parse.ptr != value.data() + value.size() ||
      result == 0) {
    return std::nullopt;
  }
  return result;
}

std::optional<JobSelection>
SelectJobCount(const std::vector<std::string> &arguments) {
  JobSelection selection = {
      std::max(1u, std::thread::hardware_concurrency()), false};
  constexpr std::string_view prefix = "--dxmt-test-jobs=";

  if (const char *value = std::getenv("DXMT_TEST_JOBS")) {
    const auto parsed = ParsePositiveInteger(value);
    if (!parsed) {
      std::fprintf(stderr,
                   "DXMT_TEST_JOBS must be a positive integer, got '%s'\n",
                   value);
      return std::nullopt;
    }
    selection.count = *parsed;
    selection.explicit_count = true;
  }

  for (const auto &argument : arguments) {
    const std::string_view value(argument);
    if (!value.starts_with(prefix))
      continue;
    const auto parsed = ParsePositiveInteger(value.substr(prefix.size()));
    if (!parsed) {
      std::fprintf(stderr,
                   "--dxmt-test-jobs must be a positive integer, got '%s'\n",
                   argument.c_str() + prefix.size());
      return std::nullopt;
    }
    selection.count = *parsed;
    selection.explicit_count = true;
  }
  return selection;
}

std::vector<std::string>
BuildSuiteArguments(const std::vector<std::string> &original_arguments,
                    std::size_t jobs, bool explicit_jobs) {
  std::vector<std::string> arguments;
  arguments.reserve(original_arguments.size());
  for (std::size_t index = 1; index < original_arguments.size(); ++index) {
    const std::string_view argument(original_arguments[index]);
    if (!argument.starts_with("--dxmt-test-jobs=") &&
        !argument.starts_with("--dxmt-test-suite=") &&
        !argument.starts_with("--dxmt-test-report=") &&
        argument != "--dxmt-test-worker") {
      arguments.push_back(original_arguments[index]);
    }
  }
  arguments.push_back(std::string(explicit_jobs ? "--dxmt-test-jobs="
                                                : "--dxmt-test-auto-jobs=") +
                      std::to_string(jobs));
  return arguments;
}

std::optional<std::vector<const Suite *>>
SelectSuites(const std::vector<std::string> &arguments) {
  std::string_view selected = "all";
  constexpr std::string_view prefix = "--dxmt-test-suite=";
  for (const auto &argument : arguments) {
    const std::string_view value(argument);
    if (value.starts_with(prefix))
      selected = value.substr(prefix.size());
  }

  std::vector<const Suite *> suites;
  for (const auto &suite : kSuites) {
    if (selected == "all" || selected == suite.name)
      suites.push_back(&suite);
  }
  if (suites.empty()) {
    std::fprintf(stderr, "unknown DXMT Wine test suite: %.*s\n",
                 static_cast<int>(selected.size()), selected.data());
    return std::nullopt;
  }
  return suites;
}

std::wstring ExecutableDirectory() {
  auto path = dxmt::test::WineExecutablePath();
  const auto separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos)
    return L".";
  return path.substr(0, separator);
}

std::string BuildLogPrefix() {
  std::vector<char> buffer(32768);
  const DWORD size =
      GetTempPathA(static_cast<DWORD>(buffer.size()), buffer.data());
  std::string path;
  if (size == 0 || size >= buffer.size()) {
    path = ".\\";
  } else {
    path.assign(buffer.data(), size);
    if (!path.empty() && path.back() != '\\' && path.back() != '/')
      path.push_back('\\');
  }
  path += "dxmt-wine-suites-" + std::to_string(GetCurrentProcessId()) + "-";
  path += std::to_string(Clock::now().time_since_epoch().count());
  return path;
}

HANDLE CreateSuiteLog(const std::string &path) {
  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  return CreateFileA(path.c_str(), GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
}

std::string ReadAndRemoveLog(const std::string &path) {
  std::string output;
  auto *file = std::fopen(path.c_str(), "r");
  if (file != nullptr) {
    char buffer[4096];
    while (const auto size = std::fread(buffer, 1, sizeof(buffer), file))
      output.append(buffer, size);
    std::fclose(file);
  }
  DeleteFileA(path.c_str());
  return output;
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string> original_arguments;
  original_arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index)
    original_arguments.emplace_back(argv[index]);

  const auto jobs = SelectJobCount(original_arguments);
  if (!jobs)
    return 2;
  const auto selected_suites = SelectSuites(original_arguments);
  if (!selected_suites)
    return 2;

  const auto directory = ExecutableDirectory();
  if (directory.empty()) {
    std::fprintf(stderr, "failed to resolve Wine suite scheduler path\n");
    return 2;
  }

  const auto concurrency = std::min(jobs->count, selected_suites->size());
  const auto log_prefix = BuildLogPrefix();
  std::size_t failed_suites = 0;
  std::string scheduler_errors;
  std::array<std::string, kSuites.size()> suite_logs;
  const auto run_start = Clock::now();

  for (std::size_t wave = 0; wave < selected_suites->size(); wave += concurrency) {
    const auto wave_size =
        std::min(concurrency, selected_suites->size() - wave);
    const auto jobs_per_suite = jobs->count / wave_size;
    const auto extra_jobs = jobs->count % wave_size;
    std::vector<SuiteRun> runs;
    runs.reserve(wave_size);

    for (std::size_t offset = 0; offset < wave_size; ++offset) {
      const auto suite_index = wave + offset;
      const auto &suite = *selected_suites->at(suite_index);
      const auto suite_jobs = jobs_per_suite + (offset < extra_jobs ? 1 : 0);
      const auto arguments =
          BuildSuiteArguments(original_arguments, suite_jobs,
                              jobs->explicit_count);
      const auto executable = directory + L"\\" + suite.executable.data();
      const auto log_path =
          log_prefix + "-" + std::string(suite.name) + ".log";
      HANDLE log = CreateSuiteLog(log_path);
      if (log == INVALID_HANDLE_VALUE) {
        scheduler_errors += "failed to create log for Wine suite ";
        scheduler_errors += suite.name;
        scheduler_errors += ": ";
        scheduler_errors += dxmt::test::WineErrorMessage(GetLastError());
        scheduler_errors += '\n';
        ++failed_suites;
        continue;
      }

      DWORD error = ERROR_SUCCESS;
      auto process = dxmt::test::StartWineProcess(executable, arguments, &error,
                                                  log);
      CloseHandle(log);
      if (!process) {
        scheduler_errors += "failed to start Wine suite ";
        scheduler_errors += suite.name;
        scheduler_errors += ": ";
        scheduler_errors += dxmt::test::WineErrorMessage(error);
        scheduler_errors += '\n';
        DeleteFileA(log_path.c_str());
        ++failed_suites;
        continue;
      }
      runs.push_back({&suite, log_path, *process});
    }

    for (const auto &run : runs) {
      DWORD exit_code = 1;
      const DWORD wait_result = WaitForSingleObject(run.process.handle, INFINITE);
      if (wait_result != WAIT_OBJECT_0 ||
          !GetExitCodeProcess(run.process.handle, &exit_code)) {
        scheduler_errors += "failed to wait for Wine suite ";
        scheduler_errors += run.suite->name;
        scheduler_errors += ": ";
        scheduler_errors += dxmt::test::WineErrorMessage(GetLastError());
        scheduler_errors += '\n';
        ++failed_suites;
      } else if (exit_code != 0) {
        ++failed_suites;
      }
      CloseHandle(run.process.handle);

      const auto suite_index = static_cast<std::size_t>(run.suite - kSuites.data());
      suite_logs[suite_index] = ReadAndRemoveLog(run.log_path);
    }
  }

  for (std::size_t index = 0; index < kSuites.size(); ++index) {
    if (suite_logs[index].empty())
      continue;
    std::printf("[ DXMT     ] %s suite output\n%s", kSuites[index].name.data(),
                suite_logs[index].c_str());
    if (suite_logs[index].back() != '\n')
      std::putchar('\n');
  }
  if (!scheduler_errors.empty()) {
    std::fprintf(stderr, "[ DXMT     ] suite scheduler errors\n%s",
                 scheduler_errors.c_str());
  }

  const auto run_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
          Clock::now() - run_start)
          .count();
  std::printf("[ DXMT     ] Wine suites %s (wall %.3f ms)\n",
              failed_suites == 0 ? "passed" : "failed", run_ms);
  return failed_suites == 0 ? 0 : 1;
}
