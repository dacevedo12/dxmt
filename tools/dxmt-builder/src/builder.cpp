#include "builder.hpp"

#include "sha256.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

namespace dxmt::builder {
namespace {

namespace fs = std::filesystem;

struct CommandResult {
  int status = 1;
  std::string output;
};

using Environment = std::map<std::string, std::string>;

std::uint64_t ProcessId() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

bool IsExecutableFile(const fs::path &path) {
#ifdef _WIN32
  return fs::is_regular_file(path);
#else
  return fs::is_regular_file(path) && access(path.c_str(), X_OK) == 0;
#endif
}

#ifdef _WIN32
std::wstring Widen(std::string_view value) {
  if (value.empty())
    return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       value.data(),
                                       static_cast<int>(value.size()), nullptr,
                                       0);
  if (size <= 0)
    throw std::runtime_error("failed to convert UTF-8 command argument");
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::wstring QuoteWindowsArgument(std::wstring_view argument) {
  if (argument.empty())
    return L"\"\"";
  if (argument.find_first_of(L" \t\"") == std::wstring_view::npos)
    return std::wstring(argument);

  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(character);
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

std::wstring BuildWindowsCommandLine(
    const std::vector<std::string> &arguments) {
  std::wstring command_line;
  for (const auto &argument : arguments) {
    if (!command_line.empty())
      command_line.push_back(L' ');
    command_line += QuoteWindowsArgument(Widen(argument));
  }
  return command_line;
}

std::vector<wchar_t> BuildWindowsEnvironment(const Environment &overrides) {
  std::map<std::wstring, std::wstring, std::less<>> values;
  wchar_t *environment = GetEnvironmentStringsW();
  if (environment == nullptr)
    throw std::system_error(GetLastError(), std::system_category(),
                            "GetEnvironmentStringsW");
  for (const wchar_t *entry = environment; *entry != L'\0';) {
    const std::wstring_view record(entry);
    const auto separator = record.find(L'=', record.starts_with(L'=') ? 1 : 0);
    if (separator != std::wstring_view::npos)
      values[std::wstring(record.substr(0, separator))] =
          std::wstring(record.substr(separator + 1));
    entry += record.size() + 1;
  }
  FreeEnvironmentStringsW(environment);
  for (const auto &[name, value] : overrides)
    values[Widen(name)] = Widen(value);

  std::vector<wchar_t> block;
  for (const auto &[name, value] : values) {
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}
#endif

std::string ShellQuote(std::string_view value) {
  if (value.find_first_of(" \t\n'\"") == std::string_view::npos)
    return std::string(value);
  std::string result = "'";
  for (const char character : value) {
    if (character == '\'')
      result += "'\\''";
    else
      result.push_back(character);
  }
  result.push_back('\'');
  return result;
}

void PrintCommand(const std::vector<std::string> &arguments) {
  static std::mutex mutex;
  const std::lock_guard lock(mutex);
  std::cerr << "+";
  for (const auto &argument : arguments)
    std::cerr << ' ' << ShellQuote(argument);
  std::cerr << '\n';
}

CommandResult RunCommand(const std::vector<std::string> &arguments,
                         const Environment &environment = {},
                         bool capture = false,
                         const std::optional<fs::path> &working_directory = {}) {
  if (arguments.empty())
    throw std::runtime_error("attempted to execute an empty command");
  PrintCommand(arguments);

#ifdef _WIN32
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (capture) {
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
        !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0))
      throw std::system_error(GetLastError(), std::system_category(),
                              "CreatePipe");
  }

  auto command_line = BuildWindowsCommandLine(arguments);
  std::vector<wchar_t> environment_block;
  if (!environment.empty())
    environment_block = BuildWindowsEnvironment(environment);
  std::wstring working_directory_path;
  if (working_directory)
    working_directory_path = working_directory->wstring();
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  if (capture) {
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
  }
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, capture,
                      CREATE_UNICODE_ENVIRONMENT,
                      environment_block.empty() ? nullptr
                                                : environment_block.data(),
                      working_directory
                          ? working_directory_path.c_str()
                          : nullptr,
                      &startup, &process)) {
    const auto error = GetLastError();
    if (read_pipe)
      CloseHandle(read_pipe);
    if (write_pipe)
      CloseHandle(write_pipe);
    throw std::system_error(error, std::system_category(), "CreateProcessW");
  }
  CloseHandle(process.hThread);
  if (write_pipe)
    CloseHandle(write_pipe);

  CommandResult result;
  if (capture) {
    std::array<char, 4096> buffer{};
    DWORD size = 0;
    while (ReadFile(read_pipe, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &size, nullptr)) {
      if (size != 0)
        result.output.append(buffer.data(), size);
    }
    const auto error = GetLastError();
    CloseHandle(read_pipe);
    if (error != ERROR_BROKEN_PIPE)
      throw std::system_error(error, std::system_category(), "ReadFile");
  }
  if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
    const auto error = GetLastError();
    CloseHandle(process.hProcess);
    throw std::system_error(error, std::system_category(),
                            "WaitForSingleObject");
  }
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
    const auto error = GetLastError();
    CloseHandle(process.hProcess);
    throw std::system_error(error, std::system_category(),
                            "GetExitCodeProcess");
  }
  CloseHandle(process.hProcess);
  result.status = static_cast<int>(exit_code);
  return result;
#else
  int pipe_fds[2] = {-1, -1};
  if (capture && pipe(pipe_fds) != 0)
    throw std::system_error(errno, std::generic_category(), "pipe");

  const pid_t child = fork();
  if (child < 0)
    throw std::system_error(errno, std::generic_category(), "fork");
  if (child == 0) {
    if (capture) {
      close(pipe_fds[0]);
      dup2(pipe_fds[1], STDOUT_FILENO);
      dup2(pipe_fds[1], STDERR_FILENO);
      close(pipe_fds[1]);
    }
    if (working_directory && chdir(working_directory->c_str()) != 0)
      _exit(126);
    for (const auto &[name, value] : environment)
      setenv(name.c_str(), value.c_str(), 1);

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  CommandResult result;
  if (capture) {
    close(pipe_fds[1]);
    std::array<char, 4096> buffer{};
    while (true) {
      const auto size = read(pipe_fds[0], buffer.data(), buffer.size());
      if (size > 0) {
        result.output.append(buffer.data(), static_cast<std::size_t>(size));
      } else if (size == 0) {
        break;
      } else if (errno != EINTR) {
        close(pipe_fds[0]);
        throw std::system_error(errno, std::generic_category(), "read");
      }
    }
    close(pipe_fds[0]);
  }

  int wait_status = 0;
  while (waitpid(child, &wait_status, 0) < 0) {
    if (errno != EINTR)
      throw std::system_error(errno, std::generic_category(), "waitpid");
  }
  if (WIFEXITED(wait_status))
    result.status = WEXITSTATUS(wait_status);
  else if (WIFSIGNALED(wait_status))
    result.status = 128 + WTERMSIG(wait_status);
  return result;
#endif
}

void RequireSuccess(const CommandResult &result, std::string_view operation) {
  if (result.status == 0)
    return;
  std::ostringstream message;
  message << operation << " failed with status " << result.status;
  if (!result.output.empty())
    message << ":\n" << result.output;
  throw std::runtime_error(message.str());
}

std::optional<fs::path> FindExecutable(std::string_view name) {
#ifdef _WIN32
  const auto requested = Widen(name);
  std::vector<wchar_t> buffer(1024);
  while (true) {
    const DWORD size = SearchPathW(nullptr, requested.c_str(), L".exe",
                                   static_cast<DWORD>(buffer.size()),
                                   buffer.data(), nullptr);
    if (size == 0)
      return std::nullopt;
    if (size < buffer.size())
      return fs::weakly_canonical(fs::path(buffer.data()));
    buffer.resize(static_cast<std::size_t>(size) + 1);
  }
#else
  const fs::path requested(name);
  if (requested.has_parent_path()) {
    if (access(requested.c_str(), X_OK) == 0)
      return fs::canonical(requested);
    return std::nullopt;
  }
  const char *path_value = std::getenv("PATH");
  if (path_value == nullptr)
    return std::nullopt;
  std::string_view paths(path_value);
  while (true) {
    const auto separator = paths.find(':');
    const auto directory = paths.substr(0, separator);
    const auto candidate = fs::path(directory.empty() ? "." : std::string(directory)) /
                           requested;
    if (access(candidate.c_str(), X_OK) == 0)
      return fs::canonical(candidate);
    if (separator == std::string_view::npos)
      break;
    paths.remove_prefix(separator + 1);
  }
  return std::nullopt;
#endif
}

fs::path RequireExecutable(std::string_view name) {
  if (const auto path = FindExecutable(name))
    return *path;
  throw std::runtime_error("required executable not found: " + std::string(name));
}

void AppendFileIdentity(std::ostringstream &identity, std::string_view name,
                        const fs::path &path) {
  identity << '\n' << name << "_path=" << path.string()
           << '\n' << name << "_size=" << fs::file_size(path)
           << '\n' << name << "_mtime="
           << static_cast<long long>(
                  fs::last_write_time(path).time_since_epoch().count())
           << '\n' << name << "_sha256=" << Sha256File(path);
}

void WriteFileAtomic(const fs::path &path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  if (fs::is_regular_file(path)) {
    std::ifstream existing(path, std::ios::binary);
    const std::string current{std::istreambuf_iterator<char>(existing),
                              std::istreambuf_iterator<char>()};
    if (current == contents)
      return;
  }
  const auto temporary =
      path.string() + ".tmp-" + std::to_string(ProcessId());
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      throw std::runtime_error("failed to create " + temporary);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output)
      throw std::runtime_error("failed to write " + temporary);
  }
  std::error_code error;
  fs::rename(temporary, path, error);
  if (error) {
    fs::remove(path, error);
    error.clear();
    fs::rename(temporary, path, error);
  }
  if (error) {
    fs::remove(temporary);
    throw std::system_error(error, "failed to publish " + path.string());
  }
}

std::string ReadFile(const fs::path &path);

// ---------------------------------------------------------------------------
// Analyzer result cache
//
// clang-tidy has no cache of its own, so every audit re-analyzes every
// translation unit even when nothing it depends on changed.  The path-
// sensitive checks dominate audit wall time, so caching their verdict keyed by
// everything that can change it turns a routine re-run into a no-op.
//
// The key covers: the clang-tidy binary identity, the config file, the exact
// command line, and the content of every project header the unit can reach.
// Header discovery is a deliberately conservative textual scan -- over-
// approximating dependencies only costs a needless miss, while missing one
// would serve a stale verdict.
// ---------------------------------------------------------------------------
std::uint64_t FnvHashUpdate(std::uint64_t hash, std::string_view data) {
  for (const unsigned char byte : data) {
    hash ^= byte;
    hash *= 0x100000001b3ull;
  }
  return hash;
}

constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;

// Conservative transitive scan of quoted/angled includes that resolve inside
// the repository.  Unresolved names are system headers and are covered by the
// compiler identity in the key instead.
std::vector<fs::path>
CollectProjectDependencies(const fs::path &translation_unit,
                           const fs::path &repo_root) {
  static const std::regex include_pattern(
      R"re(^[ \t]*#[ \t]*include[ \t]*[<"]([^">]+)[">])re",
      std::regex::multiline);

  std::vector<fs::path> search_roots;
  search_roots.push_back(repo_root / "include");
  for (const auto &root : {repo_root / "src", repo_root / "libs"}) {
    std::error_code error;
    if (!fs::is_directory(root, error))
      continue;
    search_roots.push_back(root);
    for (const auto &entry : fs::recursive_directory_iterator(root, error)) {
      if (entry.is_directory())
        search_roots.push_back(entry.path());
    }
  }

  std::set<fs::path> visited;
  std::vector<fs::path> ordered;
  std::vector<fs::path> queue = {translation_unit};
  while (!queue.empty()) {
    const auto current = queue.back();
    queue.pop_back();
    std::error_code error;
    const auto canonical = fs::weakly_canonical(current, error);
    const auto key = error ? current : canonical;
    if (!visited.insert(key).second)
      continue;
    if (!fs::is_regular_file(key, error))
      continue;
    ordered.push_back(key);
    const auto source = ReadFile(key);
    for (std::sregex_iterator match(source.begin(), source.end(),
                                    include_pattern), end;
         match != end; ++match) {
      const auto name = (*match)[1].str();
      std::vector<fs::path> candidates;
      candidates.push_back(key.parent_path() / name);
      for (const auto &root : search_roots)
        candidates.push_back(root / name);
      for (const auto &candidate : candidates) {
        std::error_code candidate_error;
        if (!fs::is_regular_file(candidate, candidate_error))
          continue;
        if (!IsPathWithin(candidate, repo_root))
          continue;
        queue.push_back(candidate);
        break;
      }
    }
  }
  std::sort(ordered.begin(), ordered.end());
  return ordered;
}

std::string AuditCacheKey(const fs::path &translation_unit,
                          const fs::path &repo_root,
                          const std::vector<std::string> &command,
                          std::string_view tool_identity,
                          std::string_view config_contents) {
  auto hash = FnvHashUpdate(kFnvOffsetBasis, tool_identity);
  for (const auto &argument : command)
    hash = FnvHashUpdate(hash, argument);
  // The command line names the config by path, so the path alone cannot tell a
  // stale verdict from a fresh one: editing the check list leaves every
  // argument byte-identical.  Enabling a check would then serve cached results
  // produced without it -- an audit that reports "clean" for files the new
  // check never saw.  Hash the config contents so any edit to it is a miss.
  hash = FnvHashUpdate(hash, config_contents);
  auto content_hash = kFnvOffsetBasis;
  for (const auto &dependency :
       CollectProjectDependencies(translation_unit, repo_root)) {
    std::error_code error;
    const auto relative = fs::relative(dependency, repo_root, error);
    content_hash = FnvHashUpdate(
        content_hash, error ? dependency.string() : relative.generic_string());
    content_hash = FnvHashUpdate(content_hash, ReadFile(dependency));
  }
  std::ostringstream key;
  key << std::hex << std::setw(16) << std::setfill('0') << hash
      << std::setw(16) << std::setfill('0') << content_hash;
  return key.str();
}

std::string ReadFile(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("failed to read " + path.string());
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::size_t LineNumber(std::string_view source, std::size_t offset) {
  return 1 + static_cast<std::size_t>(
                 std::count(source.begin(), source.begin() + offset, '\n'));
}

std::pair<std::size_t, std::size_t>
BracedBody(std::string_view source, std::string_view marker) {
  const auto start = source.find(marker);
  if (start == std::string_view::npos)
    throw std::runtime_error("missing source marker: " + std::string(marker));
  const auto brace = source.find('{', start);
  if (brace == std::string_view::npos)
    throw std::runtime_error("missing body after source marker: " +
                             std::string(marker));
  std::size_t depth = 0;
  for (auto position = brace; position < source.size(); ++position) {
    if (source[position] == '{')
      ++depth;
    else if (source[position] == '}' && --depth == 0)
      return {brace, position + 1};
  }
  throw std::runtime_error("unterminated body after source marker: " +
                           std::string(marker));
}

std::size_t CountOccurrences(std::string_view source, std::string_view literal) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = source.find(literal, offset)) != std::string_view::npos) {
    ++count;
    offset += literal.size();
  }
  return count;
}

bool IsSourceFile(const fs::path &path) {
  static const std::set<std::string> extensions = {
      ".c", ".cc", ".cpp", ".h", ".hpp", ".m", ".mm"};
  return extensions.contains(path.extension().string());
}

// `literal` normally names a code construct, so matches inside #include lines
// and comments are filename/documentation collisions rather than violations.
// Rules that deliberately target directives (include-closure checks) pass
// scan_directives = true.
void AppendLiteralViolations(
    std::vector<std::string> &errors, const fs::path &repo_root,
    const fs::path &path, std::string_view source, std::string_view literal,
    std::string_view message, bool scan_directives = false) {
  std::size_t offset = 0;
  while ((offset = source.find(literal, offset)) != std::string_view::npos) {
    // A banned construct names code, not files.  As modules get extracted the
    // resulting header names legitimately contain those same words, so an
    // #include line matching a ban is a filename collision rather than a
    // violation -- flagging it would force modules to be named around the
    // policy text.
    const auto line_begin = source.rfind('\n', offset);
    const auto line_start =
        line_begin == std::string_view::npos ? 0 : line_begin + 1;
    const auto line_end = source.find('\n', offset);
    const auto line = source.substr(
        line_start, (line_end == std::string_view::npos ? source.size()
                                                       : line_end) -
                        line_start);
    const auto first = line.find_first_not_of(" \t");
    const bool is_include =
        first != std::string_view::npos && line[first] == '#' &&
        line.find("include", first) != std::string_view::npos;
    // Same reasoning for comments: documentation that names a banned
    // construct (or a module named after one) is describing the rule, not
    // breaking it.
    const bool is_comment =
        first != std::string_view::npos && line.compare(first, 2, "//") == 0;
    if (!scan_directives && (is_include || is_comment)) {
      offset += literal.size();
      continue;
    }
    errors.push_back(path.lexically_relative(repo_root).generic_string() + ":" +
                     std::to_string(LineNumber(source, offset)) + ": " +
                     std::string(message));
    offset += literal.size();
  }
}

// Value-domain module isolation is a build-graph fact, not a code-content rule.
// CommandQueueImpl and its nested replay types (ReplayState, Compiled*,
// Submitted*, Frozen*) live in the anonymous namespace of
// d3d12_command_queue.cpp and its .inc fragments, which are never installed as
// headers. An independent translation unit therefore cannot name them, and no
// literal ban is needed to prove it. The one thing such a module can still get
// wrong is pulling the queue's private headers back into its own include
// closure, which is what this rejects. Ownership, locking, and lambda-capture
// discipline are enforced by clang-tidy over the same translation units, not by
// scanning for spellings.
void AppendQueueIndependenceViolations(std::vector<std::string> &errors,
                                       const fs::path &repo_root,
                                       const fs::path &path) {
  if (!fs::is_regular_file(path))
    return;
  const auto source = ReadFile(path);
  for (const auto forbidden : {"#include \"d3d12_command_queue.hpp\"",
                               "#include \"d3d12_command_queue_"}) {
    AppendLiteralViolations(
        errors, repo_root, path, source, forbidden,
        "value-domain module must not pull the command queue into its "
        "include closure",
        /*scan_directives=*/true);
  }
}

// The concurrency-carrying tree, scanned as a whole by the structural rules
// below so that a newly extracted module is covered without touching Builder.
constexpr std::array<std::string_view, 2> kConcurrencyDirectories = {
    "src/d3d12",
    "src/dxmt",
};

std::vector<fs::path> ConcurrencySourceFiles(const fs::path &repo_root) {
  std::vector<fs::path> sources;
  for (const auto directory : kConcurrencyDirectories) {
    const auto root = repo_root / fs::path(std::string(directory));
    if (!fs::is_directory(root))
      continue;
    for (const auto &entry : fs::directory_iterator(root)) {
      if (!entry.is_regular_file())
        continue;
      const auto extension = entry.path().extension();
      if (extension != ".cpp" && extension != ".hpp" && extension != ".inc")
        continue;
      sources.push_back(entry.path());
    }
  }
  std::sort(sources.begin(), sources.end());
  return sources;
}

// A reviewed site where an analyzer escape hatch is allowed to appear, and how
// many occurrences were reviewed there. The count is an upper bound: removing
// an escape hatch is always allowed, adding one is not.
struct EscapeHatchAllowance {
  std::string_view path;
  std::string_view literal;
  std::size_t occurrences;
};

// Silencing the thread-safety analyzer is a structural property of the whole
// concurrency tree, not of the two files that happened to carry the rule first:
// whatever file an escape hatch is written in, it stops the analyzer from
// reasoning about a lock. The scan therefore covers src/d3d12 and src/dxmt
// wholesale and what is enumerated here is the *exception* list -- the
// individual reviewed sites, with their occurrence counts -- so a new module is
// constrained the moment it is added, and a new escape hatch has to be argued
// for in Builder instead of appearing silently.
//
// Unlike the code-construct bans elsewhere in this file these literals are
// scanned including comments: a clang-tidy suppression is only ever spelled
// inside a comment, so a comment-blind scan would see none of them.
void AppendThreadSafetyEscapeHatchViolations(std::vector<std::string> &errors,
                                             const fs::path &repo_root) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 3>
      escape_hatches = {{
          {"NOLINT", "inline clang-tidy suppression is forbidden"},
          {"DXMT_NO_THREAD_SAFETY_ANALYSIS",
           "thread-safety suppression is forbidden"},
          {"DXMT_ASSERT_CAPABILITY",
           "asserting a capability tells the analyzer the lock is held without "
           "proving it"},
      }};
  // The lock-ownership assertions that bridge std::unique_lock and lambda
  // bodies, which the analyzer cannot follow. Each is backed by a run-time
  // ownership check, which the rule below verifies separately.
  static constexpr std::array<EscapeHatchAllowance, 2> allowances = {{
      {"src/d3d12/d3d12_submission_service.cpp", "DXMT_ASSERT_CAPABILITY", 2},
      {"src/d3d12/d3d12_command_queue_submission.inc",
       "DXMT_ASSERT_CAPABILITY", 1},
  }};
  for (const auto &path : ConcurrencySourceFiles(repo_root)) {
    const auto relative = path.lexically_relative(repo_root).generic_string();
    const auto source = ReadFile(path);
    for (const auto &[literal, message] : escape_hatches) {
      std::size_t reviewed = 0;
      for (const auto &allowance : allowances) {
        if (allowance.path == relative && allowance.literal == literal)
          reviewed = allowance.occurrences;
      }
      std::vector<std::string> hits;
      AppendLiteralViolations(hits, repo_root, path, source, literal, message,
                              /*scan_directives=*/true);
      if (hits.size() <= reviewed)
        continue;
      for (auto &hit : hits)
        errors.push_back(
            hit + " (reviewed exceptions for this file: " +
            std::to_string(reviewed) +
            "; a new escape hatch has to be added to the exception list in "
            "tools/dxmt-builder/src/builder.cpp)");
    }
    // An asserted capability that is not paid for by a run-time ownership check
    // is an unconditional promise to the analyzer, which is the failure mode the
    // exception list exists to contain.
    const auto asserted = CountOccurrences(source, "DXMT_ASSERT_CAPABILITY");
    if (asserted != 0 && CountOccurrences(source, "assert_held(") < asserted)
      errors.push_back(
          relative +
          ": every asserted lock capability must be backed by a run-time "
          "dxmt::mutex ownership check");
  }
}

// Number of normalized lines that make a shared block a duplicated
// implementation rather than shared boilerplate. The largest legitimate block
// the tree shares today is 24 lines, so 30 keeps a margin without being so
// large that a copied function slips through.
constexpr std::size_t kDuplicateImplementationLines = 30;

// Blank lines and indentation are formatting, so a copy that was only reindented
// is still a copy. Comments are kept: they are part of what was copied, and
// dropping them makes unrelated boilerplate collide.
std::vector<std::pair<std::string, std::size_t>>
NormalizeSourceLines(std::string_view source) {
  std::vector<std::pair<std::string, std::size_t>> normalized;
  std::size_t line = 0;
  std::size_t offset = 0;
  while (offset <= source.size()) {
    const auto end = source.find('\n', offset);
    const auto raw = source.substr(
        offset, (end == std::string_view::npos ? source.size() : end) - offset);
    ++line;
    const auto first = raw.find_first_not_of(" \t\r");
    if (first != std::string_view::npos) {
      const auto last = raw.find_last_not_of(" \t\r");
      normalized.emplace_back(std::string(raw.substr(first, last - first + 1)),
                              line);
    }
    if (end == std::string_view::npos)
      break;
    offset = end + 1;
  }
  return normalized;
}

// A copied implementation is a structural defect regardless of which pair of
// files it lands in, so this walks the whole concurrency tree rather than
// naming the files that are known to have diverged copies today. Windows are
// hashed once per file and only compared across files, which keeps the check
// linear in the size of the tree.
void AppendDuplicatedImplementationViolations(std::vector<std::string> &errors,
                                              const fs::path &repo_root) {
  const auto paths = ConcurrencySourceFiles(repo_root);
  std::vector<std::string> names;
  std::vector<std::vector<std::pair<std::string, std::size_t>>> lines;
  names.reserve(paths.size());
  lines.reserve(paths.size());
  for (const auto &path : paths) {
    names.push_back(path.lexically_relative(repo_root).generic_string());
    lines.push_back(NormalizeSourceLines(ReadFile(path)));
  }

  struct Window {
    std::size_t file = 0;
    std::size_t index = 0;
  };
  std::map<std::string, Window> seen;
  std::set<std::pair<std::size_t, std::size_t>> reported;
  for (std::size_t file = 0; file < lines.size(); ++file) {
    const auto &file_lines = lines[file];
    if (file_lines.size() < kDuplicateImplementationLines)
      continue;
    for (std::size_t index = 0;
         index + kDuplicateImplementationLines <= file_lines.size(); ++index) {
      std::string block;
      for (std::size_t offset = 0; offset < kDuplicateImplementationLines;
           ++offset) {
        block += file_lines[index + offset].first;
        block += '\n';
      }
      const auto existing = seen.find(block);
      if (existing == seen.end()) {
        seen.emplace(std::move(block), Window{file, index});
        continue;
      }
      const auto &other = existing->second;
      if (other.file == file)
        continue;
      // One report per pair of files: a copied function produces one hit per
      // shared window, and the first one already names both sides.
      if (!reported.emplace(other.file, file).second)
        continue;
      errors.push_back(
          names[file] + ":" + std::to_string(file_lines[index].second) + ": " +
          std::to_string(kDuplicateImplementationLines) +
          " consecutive lines are a duplicate of " + names[other.file] + ":" +
          std::to_string(lines[other.file][other.index].second) +
          "; extract the shared implementation instead of copying it");
    }
  }
}

std::vector<std::string>
AuditDx12Metal4PolicyImpl(const fs::path &repo_root) {
  std::vector<std::string> errors;
  const auto metal_path = repo_root / "src/winemetal4/unix/winemetal_unix.c";
  const auto metal = ReadFile(metal_path);

  static constexpr std::array<std::string_view, 7>
      residency_direct_helpers = {
      "dxmt_residency_set_add_allocation_direct(",
      "dxmt_residency_set_remove_allocation_direct(",
      "dxmt_residency_set_commit_direct(",
      "dxmt_residency_set_request_direct(",
      "dxmt_residency_set_end_direct(",
      "dxmt_residency_set_contains_allocation_direct(",
      "dxmt_residency_set_allocation_count_direct(",
  };
  std::vector<std::pair<std::size_t, std::size_t>> helper_ranges;
  for (const auto marker : residency_direct_helpers) {
    try {
      const auto range = BracedBody(metal, marker);
      helper_ranges.push_back(range);
      const auto body = std::string_view(metal).substr(
          range.first, range.second - range.first);
      if (body.find("dxmt_nslock_scope_acquire(") !=
              std::string_view::npos ||
          body.find("dxmt_residency_lock_acquire(") !=
              std::string_view::npos)
        errors.push_back(
            "src/winemetal4/unix/winemetal_unix.c:" +
            std::to_string(LineNumber(metal, range.first)) +
            ": direct residency backend helper must not acquire a lock");
    } catch (const std::runtime_error &error) {
      errors.push_back(error.what());
    }
  }
  static constexpr std::array<
      std::pair<std::string_view, std::string_view>, 4>
      external_residency_thunks = {{
          {"_MTLResidencySet_addAllocation(",
           "dxmt_residency_set_add_allocation_direct("},
          {"_MTLResidencySet_removeAllocation(",
           "dxmt_residency_set_remove_allocation_direct("},
          {"_MTLResidencySet_commit(",
           "dxmt_residency_set_commit_direct("},
          {"_MTLResidencySet_requestResidency(",
           "dxmt_residency_set_request_direct("},
      }};
  std::vector<std::pair<std::size_t, std::size_t>> external_thunk_ranges;
  for (const auto &[marker, direct_call] : external_residency_thunks) {
    try {
      const auto range = BracedBody(metal, marker);
      external_thunk_ranges.push_back(range);
      const auto body = std::string_view(metal).substr(
          range.first, range.second - range.first);
      if (body.find(direct_call) == std::string_view::npos)
        errors.push_back(
            "src/winemetal4/unix/winemetal_unix.c:" +
            std::to_string(LineNumber(metal, range.first)) +
            ": legacy external residency thunk must remain synchronous and "
            "delegate directly");
      static constexpr std::array<std::string_view, 4>
          forbidden_thunk_lock_markers = {
              "dxmt_nslock_scope_acquire(",
              "dxmt_owned_residency_set_",
              "dxmt_residency_lock_",
              "dxmt_external_residency_",
          };
      for (const auto forbidden : forbidden_thunk_lock_markers) {
        if (body.find(forbidden) != std::string_view::npos)
          errors.push_back(
              "src/winemetal4/unix/winemetal_unix.c:" +
              std::to_string(LineNumber(metal, range.first)) +
              ": external residency thunk must not create a second "
              "synchronization domain");
      }
    } catch (const std::runtime_error &error) {
      errors.push_back(error.what());
    }
  }

  static constexpr std::array<std::string_view, 4>
      forbidden_bulk_residency_contracts = {
          "addAllocations:", "removeAllocations:",
          "_MTLResidencySet_applyDelta(", "MTLResidencySet_applyDelta("};
  for (const auto forbidden : forbidden_bulk_residency_contracts) {
    if (metal.find(forbidden) != std::string_view::npos)
      errors.push_back(
          "src/winemetal4/unix/winemetal_unix.c: bulk residency mutation is "
          "forbidden; use lifecycle-granular add/remove operations: " +
          std::string(forbidden));
  }

  static constexpr std::array<std::string_view, 5>
      forbidden_external_dispatch_helpers = {
          "struct dxmt_external_residency_context",
          "dxmt_external_residency_enqueue(",
          "dxmt_external_residency_execute(",
          "dxmt_external_residency_dispatch(",
          "dxmt_external_residency_active_context",
      };
  for (const auto forbidden : forbidden_external_dispatch_helpers) {
    if (metal.find(forbidden) != std::string_view::npos)
      errors.push_back(
          "src/winemetal4/unix/winemetal_unix.c: deferred external residency "
          "execution is forbidden: " +
          std::string(forbidden));
  }

  static constexpr std::array<std::string_view, 4>
      forbidden_residency_lock_registry = {
          "DXMTResidencySetLockState",
          "dxmt_residency_set_lock_registry",
          "dxmt_residency_set_locks",
          "dxmt_residency_lock_acquire",
      };
  for (const auto forbidden : forbidden_residency_lock_registry) {
    if (metal.find(forbidden) != std::string_view::npos)
      errors.push_back(
          "src/winemetal4/unix/winemetal_unix.c: universal residency-set "
          "lock registry is forbidden: " +
          std::string(forbidden));
  }

  static const std::array<std::regex, 3> residency_expressions = {
      std::regex(R"(\[[^\n;]*\b(?:addAllocation:|removeAllocation:|requestResidency\]|endResidency\]|containsAllocation:))"),
      std::regex(R"(\[(?:set|[^\]\n]*(?:ResidencySet|residencySet))\s+commit\])"),
      std::regex(R"((?:set|layerSet|[A-Za-z_.]*ResidencySet)\.allocationCount\b)"),
  };
  for (const auto &expression : residency_expressions) {
    for (std::sregex_iterator match(metal.begin(), metal.end(), expression),
         end;
         match != end; ++match) {
      const auto offset = static_cast<std::size_t>(match->position());
      if (std::any_of(helper_ranges.begin(), helper_ranges.end(),
                      [offset](const auto &range) {
                        return range.first <= offset && offset < range.second;
                      }) ||
          std::any_of(external_thunk_ranges.begin(),
                      external_thunk_ranges.end(),
                      [offset](const auto &range) {
                        return range.first <= offset && offset < range.second;
                      }))
        continue;
      errors.push_back(
          "src/winemetal4/unix/winemetal_unix.c:" +
          std::to_string(LineNumber(metal, offset)) +
          ": direct residency-set access must stay inside a direct backend "
          "helper or the externally synchronized ABI thunk");
    }
  }

  static constexpr std::array<std::string_view, 3>
      required_owned_helper_contracts = {
          "dxmt_owned_residency_set_add_allocation(",
          "dxmt_owned_residency_set_remove_allocation(",
          "dxmt_owned_residency_set_commit(",
      };
  for (const auto marker : required_owned_helper_contracts) {
    try {
      const auto range = BracedBody(metal, marker);
      const auto declaration_begin = metal.rfind('\n', range.first);
      const auto contract = std::string_view(metal).substr(
          declaration_begin == std::string::npos ? 0 : declaration_begin,
          range.first -
              (declaration_begin == std::string::npos ? 0
                                                      : declaration_begin));
      if (contract.find("DXMT_REQUIRES(owner_lock)") ==
          std::string_view::npos)
        errors.push_back(
            "src/winemetal4/unix/winemetal_unix.c:" +
            std::to_string(LineNumber(metal, range.first)) +
            ": Winemetal-owned residency mutation must declare its owner "
            "capability");
    } catch (const std::runtime_error &error) {
      errors.push_back(error.what());
    }
  }

  std::pair<std::size_t, std::size_t> argument_helper = {
      std::string::npos, std::string::npos};
  try {
    argument_helper =
        BracedBody(metal, "dxmt_metal4_argument_table_from_handle(");
  } catch (const std::runtime_error &error) {
    errors.push_back(error.what());
  }
  const std::regex argument_table_cast(
      R"((?:=|:|return)\s*\(id<MTL4ArgumentTable>\))");
  for (std::sregex_iterator match(metal.begin(), metal.end(),
                                  argument_table_cast),
       end;
       match != end; ++match) {
    const auto offset = static_cast<std::size_t>(match->position());
    if (argument_helper.first <= offset && offset < argument_helper.second)
      continue;
    errors.push_back(
        "src/winemetal4/unix/winemetal_unix.c:" +
        std::to_string(LineNumber(metal, offset)) +
        ": raw MTL4ArgumentTable cast must use "
        "dxmt_metal4_argument_table_from_handle");
  }

  const auto header = ReadFile(repo_root / "src/winemetal4/winemetal.h");
  static const std::array<std::pair<std::regex, std::string_view>, 4>
      required_abi_contracts = {{
          {std::regex(
               R"(#define\s+STATIC_ASSERT\(x\)\s+_Static_assert\(\(x\),\s*#x\))"),
           "C STATIC_ASSERT"},
          {std::regex(
               R"(offsetof\(struct WMTCommandBufferDiagnosticInfo,\s*fence_edges\)\s*==\s*296)"),
           "WMTCommandBufferDiagnosticInfo.fence_edges offset"},
          {std::regex(
               R"(offsetof\(struct WMTCommandBufferDiagnosticInfo,\s*encoders\)\s*==\s*14888)"),
           "WMTCommandBufferDiagnosticInfo.encoders offset"},
          {std::regex(
               R"(sizeof\(struct WMTCommandBufferDiagnosticInfo\)\s*==\s*23080)"),
           "WMTCommandBufferDiagnosticInfo size"},
      }};
  for (const auto &[pattern, description] : required_abi_contracts) {
    if (!std::regex_search(header, pattern))
      errors.push_back("src/winemetal4/winemetal.h: missing ABI contract: " +
                       std::string(description));
  }
  if (metal.find("DXMT_ACQUIRE(lock)") == std::string::npos ||
      metal.find("DXMT_RELEASE(lock)") == std::string::npos)
    errors.push_back(
        "src/winemetal4/unix/winemetal_unix.c: annotated NSLock scope "
        "acquire/release helpers are required");

  const auto policy_baseline =
      ReadFile(repo_root / "tools/audit/policy-baseline.json");
  std::smatch ceiling_match;
  if (!std::regex_search(
          policy_baseline, ceiling_match,
          std::regex(R"("maximum_raw_nslock_messages"\s*:\s*([0-9]+))")))
    errors.push_back(
        "tools/audit/policy-baseline.json: missing "
        "maximum_raw_nslock_messages");
  else {
    const auto ceiling = std::stoul(ceiling_match[1].str());
    const std::regex raw_nslock_expression(
        R"(\[[^\]\n]+\s(?:lock|unlock)\])");
    const auto raw_count = static_cast<std::size_t>(std::distance(
        std::sregex_iterator(metal.begin(), metal.end(),
                             raw_nslock_expression),
        std::sregex_iterator()));
    if (raw_count > ceiling)
      errors.push_back(
          "src/winemetal4/unix/winemetal_unix.c: raw NSLock messages "
          "increased from the migration ceiling " +
          std::to_string(ceiling) + " to " + std::to_string(raw_count) +
          "; use dxmt_nslock_scope");
  }

  const std::array<fs::path, 2> ownership_roots = {
      repo_root / "src/d3d12", repo_root / "src/dxmt"};
  for (const auto &root : ownership_roots) {
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file() || !IsSourceFile(entry.path()))
        continue;
      const auto relative =
          entry.path().lexically_relative(repo_root).generic_string();
      const auto source = ReadFile(entry.path());
      if (relative != "src/dxmt/dxmt_command_queue.hpp")
        AppendLiteralViolations(
            errors, repo_root, entry.path(), source,
            "ResidencyOwnership::ReplayAllocatorBlock(",
            "Replay allocator residency tokens may only be created by "
            "CommandQueue block allocators");
      if (relative != "src/d3d12/d3d12_resource.cpp" &&
          relative != "src/dxmt/dxmt_context.cpp")
        AppendLiteralViolations(
            errors, repo_root, entry.path(), source,
            "ResidencyOwnership::ReplayTemporary(",
            "Replay temporary residency tokens may only be created by "
            "explicit temporary-buffer owners");
      AppendLiteralViolations(
          errors, repo_root, entry.path(), source,
          "RegisterReplayResidency(",
          "untyped Replay residency API is forbidden");
      AppendLiteralViolations(
          errors, repo_root, entry.path(), source,
          "RetainReplayResidencyUntilGpuComplete(",
          "untyped Replay residency retention API is forbidden");
      AppendLiteralViolations(
          errors, repo_root, entry.path(), source, ".applyDelta(",
          "bulk Metal residency deltas are forbidden");
      static constexpr std::array<std::string_view, 4>
          forbidden_direct_residency_abi = {
              "MTLResidencySet_addAllocation(",
              "MTLResidencySet_removeAllocation(",
              "MTLResidencySet_commit(",
              "MTLResidencySet_requestResidency(",
          };
      for (const auto operation : forbidden_direct_residency_abi)
        AppendLiteralViolations(
            errors, repo_root, entry.path(), source, operation,
            "DXMT PE code must use the typed DeviceResidency owner");
    }
  }

  const auto queue_header =
      ReadFile(repo_root / "src/dxmt/dxmt_command_queue.hpp");
  if (queue_header.find(
          "RegisterLifetimeResidency(LifetimeResidencyAllocation allocation)") ==
      std::string::npos)
    errors.push_back(
        "src/dxmt/dxmt_command_queue.hpp: lifetime residency must require "
        "LifetimeResidencyAllocation");
  if (queue_header.find("ReplayAllocatorResidencyAllocation allocation") ==
      std::string::npos)
    errors.push_back(
        "src/dxmt/dxmt_command_queue.hpp: replay allocator registration must "
        "require ReplayAllocatorResidencyAllocation");
  if (queue_header.find("ReplayTemporaryResidencyAllocation allocation") ==
      std::string::npos)
    errors.push_back(
        "src/dxmt/dxmt_command_queue.hpp: replay temporary retention must "
        "require ReplayTemporaryResidencyAllocation");
  if (queue_header.find("friend class ResidencyOwnership;") ==
          std::string::npos ||
      queue_header.find(
          "explicit ResidencyAllocation(WMT::Object allocation,") ==
          std::string::npos)
    errors.push_back(
        "src/dxmt/dxmt_command_queue.hpp: residency token construction must "
        "remain private to ResidencyOwnership");

  const auto queue_source =
      ReadFile(repo_root / "src/dxmt/dxmt_command_queue.cpp");
  std::pair<std::size_t, std::size_t> apply_owner_body = {
      std::string::npos, std::string::npos};
  try {
    apply_owner_body =
        BracedBody(queue_source, "DeviceResidency::ApplyForSubmission(");
  } catch (const std::runtime_error &error) {
    errors.push_back(error.what());
  }
  static constexpr std::array<std::string_view, 4>
      submission_residency_operations = {
          "set_.addAllocation(", "set_.removeAllocation(", "set_.commit(",
          "set_.requestResidency("};
  for (const auto operation : submission_residency_operations) {
    std::size_t operation_offset = 0;
    bool found_operation = false;
    while ((operation_offset =
                queue_source.find(operation, operation_offset)) !=
           std::string_view::npos) {
      found_operation = true;
      if (!(apply_owner_body.first <= operation_offset &&
            operation_offset < apply_owner_body.second))
        errors.push_back(
            "src/dxmt/dxmt_command_queue.cpp:" +
            std::to_string(LineNumber(queue_source, operation_offset)) +
            ": Metal residency mutation/commit may only run under the "
            "submission owner");
      operation_offset += operation.size();
    }
    if (!found_operation)
      errors.push_back(
          "src/dxmt/dxmt_command_queue.cpp: submission owner is missing "
          "lifecycle-granular residency operation " +
          std::string(operation));
  }

  std::pair<std::size_t, std::size_t> commit_owner_body = {
      std::string::npos, std::string::npos};
  try {
    commit_owner_body =
        BracedBody(queue_source, "CommandQueue::CommitChunkInternal(");
  } catch (const std::runtime_error &error) {
    errors.push_back(error.what());
  }
  std::size_t submission_scope_offset = 0;
  bool found_submission_scope = false;
  while ((submission_scope_offset = queue_source.find(
              "DeviceResidencySubmissionScope ", submission_scope_offset)) !=
         std::string_view::npos) {
    found_submission_scope = true;
    if (!(commit_owner_body.first <= submission_scope_offset &&
          submission_scope_offset < commit_owner_body.second))
      errors.push_back(
          "src/dxmt/dxmt_command_queue.cpp:" +
          std::to_string(LineNumber(queue_source, submission_scope_offset)) +
          ": residency submission ownership token may only be acquired by "
          "CommandQueue::CommitChunkInternal");
    submission_scope_offset +=
        std::string_view("DeviceResidencySubmissionScope ").size();
  }
  if (!found_submission_scope)
    errors.push_back(
        "src/dxmt/dxmt_command_queue.cpp: submission path must acquire the "
        "residency submission ownership token");
  const auto owned_scope_offset = queue_source.find(
      "DeviceResidencySubmissionScope ", commit_owner_body.first);
  const auto metal_commit_offset =
      queue_source.find("cmdbuf.commitAndGetStats(", commit_owner_body.first);
  if (owned_scope_offset == std::string_view::npos ||
      metal_commit_offset == std::string_view::npos ||
      owned_scope_offset > metal_commit_offset ||
      metal_commit_offset >= commit_owner_body.second)
    errors.push_back(
        "src/dxmt/dxmt_command_queue.cpp: device-wide submission ownership "
        "must cover both residency mutations and the Metal command-buffer "
        "commit");
  std::size_t commit_offset = 0;
  const std::string_view residency_owner_call_marker =
      "ApplyForSubmission(";
  const auto residency_owner_definition =
      queue_source.find("DeviceResidency::ApplyForSubmission()");
  const auto residency_owner_definition_call =
      queue_source.find(residency_owner_call_marker,
                        residency_owner_definition);
  while ((commit_offset = queue_source.find(residency_owner_call_marker,
                                             commit_offset)) !=
         std::string_view::npos) {
    const bool is_definition =
        commit_offset == residency_owner_definition_call;
    const bool is_submission_owner =
        commit_owner_body.first <= commit_offset &&
        commit_offset < commit_owner_body.second;
    if (!is_definition && !is_submission_owner) {
      errors.push_back(
          "src/dxmt/dxmt_command_queue.cpp:" +
          std::to_string(LineNumber(queue_source, commit_offset)) +
          ": device residency may only be committed by "
          "CommandQueue::CommitChunkInternal");
    }
    commit_offset += residency_owner_call_marker.size();
  }

  static constexpr std::array<std::string_view, 22>
      required_residency_owner_contracts = {
          "DXMT_CAPABILITY(\"device-residency\") DeviceResidencyMutex",
          "DXMT_SCOPED_CAPABILITY DeviceResidencyLock",
          "DXMT_CAPABILITY(\"residency-submission-owner\")",
          "DXMT_SCOPED_CAPABILITY DeviceResidencySubmissionScope",
          "DXMT_GUARDED_BY(mutex_)",
          "struct RetainedAllocation",
          "using AllocationOwner = std::shared_ptr<RetainedAllocation>",
          "AddLocked(const AllocationOwner &allocation, Kind kind)",
          "RemoveLocked(WMT::Object allocation, Kind kind,",
          "QueueDesiredStateLocked(const AllocationOwner &allocation,",
          "PrepareSubmissionBatchLocked() DXMT_REQUIRES(mutex_)",
          "ApplyForSubmission()",
          "DXMT_REQUIRES(submission_owner_) DXMT_EXCLUDES(mutex_)",
          "WMT::Reference<WMT::Object> object",
          "std::unordered_map<obj_handle_t, Entry> entries_",
          "pending_mutations_",
          "backend_entries_",
          "retired_backend_owners",
          "struct SubmissionResult",
          "retired_residency_allocations",
          "applied_generation_",
          "std::atomic_uint64_t commit_count_",
      };
  for (const auto contract : required_residency_owner_contracts) {
    if (queue_header.find(contract) == std::string::npos)
      errors.push_back(
          "src/dxmt/dxmt_command_queue.hpp: missing residency ownership "
          "contract: " +
          std::string(contract));
  }
  try {
    const auto range =
        BracedBody(queue_header, "DeviceResidencySubmissionOwner final");
    const auto body = std::string_view(queue_header).substr(
        range.first, range.second - range.first);
    if (body.find("void lock() DXMT_ACQUIRE()") == std::string_view::npos ||
        body.find("void unlock() DXMT_RELEASE()") ==
            std::string_view::npos ||
        body.find("dxmt::mutex mutex_") == std::string_view::npos)
      errors.push_back(
          "src/dxmt/dxmt_command_queue.hpp: submission ownership capability "
          "must be backed by one device-wide runtime mutex");
  } catch (const std::runtime_error &error) {
    errors.push_back(error.what());
  }
  try {
    const auto range =
        BracedBody(queue_header, "DeviceResidencySubmissionScope final");
    const auto body = std::string_view(queue_header).substr(
        range.first, range.second - range.first);
    if (body.find("owner_.lock()") == std::string_view::npos ||
        body.find("owner_.unlock()") == std::string_view::npos)
      errors.push_back(
          "src/dxmt/dxmt_command_queue.hpp: submission scope must acquire and "
          "release the device-wide runtime owner");
  } catch (const std::runtime_error &error) {
    errors.push_back(error.what());
  }

  static constexpr std::array<
      std::pair<std::string_view, std::string_view>, 2>
      residency_owner_wrappers = {{
          {"DeviceResidency::Add(", "AddLocked("},
          {"DeviceResidency::Remove(", "RemoveLocked("},
      }};
  for (const auto &[wrapper, locked_call] : residency_owner_wrappers) {
    try {
      const auto range = BracedBody(queue_source, wrapper);
      const auto body = std::string_view(queue_source).substr(
          range.first, range.second - range.first);
      if (body.find("DeviceResidencyLock lock(mutex_)") ==
              std::string_view::npos ||
          body.find(locked_call) == std::string_view::npos)
        errors.push_back(
            "src/dxmt/dxmt_command_queue.cpp:" +
            std::to_string(LineNumber(queue_source, range.first)) +
            ": device residency wrapper must acquire the sole owner lock "
            "before entering its locked operation");
    } catch (const std::runtime_error &error) {
          errors.push_back(error.what());
    }
  }
  try {
    const auto range = BracedBody(queue_source, "DeviceResidency::Add(");
    const auto body = std::string_view(queue_source).substr(
        range.first, range.second - range.first);
    const auto retained_owner =
        body.find("std::make_shared<RetainedAllocation>");
    const auto owner_lock = body.find("DeviceResidencyLock lock(mutex_)");
    if (retained_owner == std::string_view::npos ||
        owner_lock == std::string_view::npos || retained_owner > owner_lock)
      errors.push_back(
          "src/dxmt/dxmt_command_queue.cpp: Metal allocation ownership must be "
          "retained before acquiring the device residency lock");
  } catch (const std::runtime_error &error) {
    errors.push_back(error.what());
  }
  if (queue_header.find("const ResidencyProvenance provenance;") ==
          std::string::npos ||
      queue_source.find(
          "std::make_shared<RetainedAllocation>(allocation, provenance)") ==
          std::string::npos)
    errors.push_back(
        "src/dxmt/dxmt_command_queue: retained Metal allocations must carry "
        "copied residency provenance");
  static constexpr std::array<std::string_view, 8>
      failure_provenance_fields = {
          "\" source=\"",    "\" owner=\"",    "\" identity=\"",
          "\" parent=\"",    "\" heapOffset=\"", "\" size=\"",
          "\" dimension=\"", "\" component=\"",
      };
  for (const auto field : failure_provenance_fields) {
    if (queue_source.find(field) == std::string::npos)
      errors.push_back(
          "src/dxmt/dxmt_command_queue.cpp: residency failure logs must "
          "preserve provenance field " +
          std::string(field));
  }
  const auto resource_source =
      ReadFile(repo_root / "src/d3d12/d3d12_resource.cpp");
  if (resource_source.find(
          "ResidencyProvenanceKind::PlacedResourceChild") ==
          std::string::npos ||
      resource_source.find(".parent = placement_heap_.handle") ==
          std::string::npos)
    errors.push_back(
        "src/d3d12/d3d12_resource.cpp: placed child residency must identify "
        "its placement heap parent in diagnostic provenance");
  static constexpr std::string_view placed_heap_owner =
      "kind_ == ResourceKind::Placed && placement_heap_residency_";
  if (std::count(resource_source.begin(), resource_source.end(), '\n') == 0 ||
      resource_source.find(placed_heap_owner) == std::string::npos ||
      resource_source.find(
          "if (kind_ == ResourceKind::Placed && "
          "placement_heap_residency_)\n"
          "      return;") == std::string::npos)
    errors.push_back(
        "src/d3d12/d3d12_resource.cpp: placed resources must inherit lifetime "
        "residency from their backing heap instead of registering child Metal "
        "allocations");
  const auto first_placed_owner = resource_source.find(placed_heap_owner);
  if (first_placed_owner == std::string::npos ||
      resource_source.find(placed_heap_owner,
                           first_placed_owner + placed_heap_owner.size()) ==
          std::string::npos)
    errors.push_back(
        "src/d3d12/d3d12_resource.cpp: HasLifetimeResidency and registration "
        "must share the placed backing-heap ownership rule");
  try {
    const auto body = std::string_view(queue_source).substr(
        apply_owner_body.first, apply_owner_body.second - apply_owner_body.first);
    const auto snapshot = body.find("PrepareSubmissionBatchLocked()");
    const auto backend_call = body.find("set_.addAllocation(");
    const auto removal_call = body.find("set_.removeAllocation(");
    const auto commit_call = body.find("set_.commit(");
    if (snapshot == std::string_view::npos ||
        backend_call == std::string_view::npos ||
        removal_call == std::string_view::npos ||
        commit_call == std::string_view::npos || snapshot > backend_call ||
        backend_call > removal_call || removal_call > commit_call)
      errors.push_back(
          "src/dxmt/dxmt_command_queue.cpp: submission owner must snapshot the "
          "PE journal, apply individual mutations, then commit once");
  } catch (const std::exception &error) {
    errors.push_back(error.what());
  }

  static constexpr std::array<std::string_view, 5>
      forbidden_residency_architecture_contracts = {
          "bool dirty_ DXMT_GUARDED_BY(mutex_)",
          "CommitForSubmissionLocked",
          "set_.applyDelta(",
          "addAllocations:",
          "submission_owner_thread_",
      };
  for (const auto contract : forbidden_residency_architecture_contracts) {
    if (queue_header.find(contract) != std::string::npos ||
        queue_source.find(contract) != std::string::npos)
      errors.push_back(
          "DXMT residency architecture contains a forbidden legacy contract: " +
          std::string(contract));
  }

  static constexpr std::array<std::string_view, 4>
      required_gpu_retention_contracts = {
          "using GpuRetainedOwner =",
          "std::variant<Rc<Sampler>",
          "RetainGpuOwner(",
          "deferred_retained_owners_",
      };
  for (const auto contract : required_gpu_retention_contracts) {
    if (queue_header.find(contract) == std::string::npos)
      errors.push_back(
          "src/dxmt/dxmt_command_queue.hpp: missing typed GPU retention "
          "contract: " +
          std::string(contract));
  }
  static constexpr std::array<std::string_view, 3>
      forbidden_gpu_retention_contracts = {
          "RetainUntilGpuComplete",
          "deferred_releases_",
          "deferred_release",
      };
  for (const auto contract : forbidden_gpu_retention_contracts) {
    AppendLiteralViolations(
        errors, repo_root, repo_root / "src/dxmt/dxmt_command_queue.hpp",
        queue_header, contract,
        "callback-style GPU retention is forbidden; retain a closed typed "
        "owner");
    AppendLiteralViolations(
        errors, repo_root, repo_root / "src/dxmt/dxmt_command_queue.cpp",
        queue_source, contract,
        "callback-style GPU retention is forbidden; retain a closed typed "
        "owner");
  }
  const auto retained_owner_begin =
      queue_header.find("using GpuRetainedOwner =");
  if (retained_owner_begin != std::string::npos) {
    const auto retained_owner_end =
        queue_header.find(';', retained_owner_begin);
    if (retained_owner_end == std::string::npos) {
      errors.push_back(
          "src/dxmt/dxmt_command_queue.hpp: GpuRetainedOwner must be a "
          "closed alias");
    } else {
      const auto retained_owner = std::string_view(queue_header).substr(
          retained_owner_begin,
          retained_owner_end - retained_owner_begin);
      for (const auto forbidden :
           {"std::function", "shared_ptr<void>", "void *", "void*", "Com<"}) {
        if (retained_owner.find(forbidden) != std::string_view::npos)
          errors.push_back(
              "src/dxmt/dxmt_command_queue.hpp:" +
              std::to_string(LineNumber(queue_header, retained_owner_begin)) +
              ": GpuRetainedOwner may contain leaf owner types only: " +
              forbidden);
      }
    }
  }

  for (const auto contract :
       {"class DeviceErrorTarget", "RegisterDeviceErrorTarget(",
        "std::weak_ptr<DeviceErrorTarget>", "device_error_targets_"}) {
    if (queue_header.find(contract) == std::string::npos)
      errors.push_back(
          "src/dxmt/dxmt_command_queue.hpp: missing typed device-error "
          "target contract: " +
          std::string(contract));
  }
  for (const auto contract :
       {"RegisterDeviceErrorCallback", "UnregisterDeviceErrorCallback",
        "device_error_callbacks_", "weak_ptr<std::function<void()>>"}) {
    AppendLiteralViolations(
        errors, repo_root, repo_root / "src/dxmt/dxmt_command_queue.hpp",
        queue_header, contract,
        "type-erased device-error callbacks are forbidden");
    AppendLiteralViolations(
        errors, repo_root, repo_root / "src/dxmt/dxmt_command_queue.cpp",
        queue_source, contract,
        "type-erased device-error callbacks are forbidden");
  }

  if (queue_header.find(
          "std::vector<DiagnosticReadback> pending_readbacks_") ==
      std::string::npos)
    errors.push_back(
        "src/dxmt/dxmt_command_queue.hpp: asynchronous diagnostic readbacks "
        "must use the closed DiagnosticReadback payload");
  for (const auto contract :
       {"deferred_readbacks",
        "std::vector<std::function<void()>> pending_readbacks_"}) {
    AppendLiteralViolations(
        errors, repo_root, repo_root / "src/dxmt/dxmt_command_queue.hpp",
        queue_header, contract,
        "type-erased asynchronous readbacks are forbidden");
    AppendLiteralViolations(
        errors, repo_root, repo_root / "src/dxmt/dxmt_command_queue.cpp",
        queue_source, contract,
        "type-erased asynchronous readbacks are forbidden");
  }
  AppendLiteralViolations(
      errors, repo_root, repo_root / "src/dxmt/dxmt_command_queue.hpp",
      queue_header, "GpuCompletionPublication",
      "secondary completion publications are forbidden; use a typed target "
      "at the Metal completion point");
  const auto query_header_path =
      repo_root / "src/dxmt/dxmt_occlusion_query.hpp";
  if (fs::is_regular_file(query_header_path)) {
    const auto query_header = ReadFile(query_header_path);
    for (const auto contract :
         {"using DiagnosticReadback = std::variant<",
          "std::is_nothrow_move_constructible_v<DiagnosticReadback>",
          "std::shared_ptr<LifetimeResidencyRegistration> "
          "residency_retirement_",
          "std::unique_ptr<TimestampSampleOwner> sample_owner_"}) {
      if (query_header.find(contract) == std::string::npos)
        errors.push_back(
            "src/dxmt/dxmt_occlusion_query.hpp: missing closed diagnostic "
            "readback contract: " +
            std::string(contract));
    }
    AppendLiteralViolations(
        errors, repo_root, query_header_path, query_header,
        "std::vector<std::function<void()>> diagnostics",
        "type-erased asynchronous readbacks are forbidden");
    AppendLiteralViolations(
        errors, repo_root, query_header_path, query_header, "std::function",
        "query and readback lifetime must not be hidden in a callback");
    for (const auto contract :
         {"setResidencyRetirement(std::function", "retire_residency_"}) {
      AppendLiteralViolations(
          errors, repo_root, query_header_path, query_header, contract,
          "visibility readback residency must be represented by typed leaf "
          "ownership");
    }
  }
  const auto d3d_query_source_path =
      repo_root / "src/d3d12/d3d12_query.cpp";
  if (fs::is_regular_file(d3d_query_source_path)) {
    const auto d3d_query_source = ReadFile(d3d_query_source_path);
    if (d3d_query_source.find(
            "class TimestampSampleOwnerImpl final : public "
            "TimestampSampleOwner") == std::string::npos)
      errors.push_back(
          "src/d3d12/d3d12_query.cpp: timestamp counter slots must be owned "
          "by a typed RAII leaf");
  }
  const auto d3d_resource_header_path =
      repo_root / "src/d3d12/d3d12_resource.hpp";
  if (fs::is_regular_file(d3d_resource_header_path)) {
    const auto d3d_resource_header = ReadFile(d3d_resource_header_path);
    for (const auto required :
         {"class CpuQueryResolveTarget",
          "std::unique_ptr<CpuQueryResolveTarget> target"}) {
      if (d3d_resource_header.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_resource.hpp: missing typed deferred CPU query "
            "resolve contract: " +
            std::string(required));
    }
    for (const auto forbidden :
         {"PendingCpuQueryResolveFn", "std::function<void(Resource *",
          "std::function<void(Resource*"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d_resource_header_path, d3d_resource_header,
          forbidden,
          "deferred CPU query resolve ownership must use a typed move-only "
          "target");
    }
  }

  const auto model_header_path =
      repo_root / "include/dxmt_d3d12_submission_model.hpp";
  const auto model_source_path =
      repo_root / "src/d3d12/d3d12_submission_model.cpp";
  const auto model_header_tu_path =
      repo_root / "src/d3d12/d3d12_submission_model_header.cpp";
  if (!fs::is_regular_file(model_header_path) ||
      !fs::is_regular_file(model_source_path) ||
      !fs::is_regular_file(model_header_tu_path)) {
    errors.push_back(
        "DX12 submission model header, source, and self-contained analysis TU "
        "must exist");
    return errors;
  }

  const auto model_header = ReadFile(model_header_path);
  const auto model_source = ReadFile(model_source_path);
  static constexpr std::array<std::string_view, 11> required_model_contracts = {
      "class D3D12SubmissionPacket final",
      "using SubmissionPayload",
      "std::variant<",
      "class GpuRetirementRecord final",
      "using RetirementPayload",
      "concept GpuRetirementPayload",
      "class SubmissionSequencer final",
      "class RetirementQueue final",
      "BackendThreadCapability final",
      "class ReplayBackend",
      "class SubmissionExecutor final",
  };
  for (const auto contract : required_model_contracts) {
    if (model_header.find(contract) == std::string::npos)
      errors.push_back(
          "include/dxmt_d3d12_submission_model.hpp: missing closed-model "
          "contract: " +
          std::string(contract));
  }

  static constexpr std::array<
      std::pair<std::string_view, std::string_view>, 8>
      forbidden_model_contracts = {{
          {"std::function", "type-erased callbacks are forbidden"},
          {"NOLINT", "inline clang-tidy suppression is forbidden"},
          {"DXMT_NO_THREAD_SAFETY_ANALYSIS",
           "thread-safety suppression is forbidden"},
          {"reinterpret_cast", "reinterpret casts are forbidden"},
          {"AddRef(", "manual COM ownership is forbidden"},
          {"Release(", "manual COM ownership is forbidden"},
          {"[&]", "implicit reference lambda capture is forbidden"},
          {"[=]", "implicit value lambda capture is forbidden"},
      }};
  for (const auto &[literal, message] : forbidden_model_contracts) {
    AppendLiteralViolations(errors, repo_root, model_header_path, model_header,
                            literal, message);
    AppendLiteralViolations(errors, repo_root, model_source_path, model_source,
                            literal, message);
  }

  try {
    const auto packet_body =
        BracedBody(model_header, "class D3D12SubmissionPacket final");
    const auto body = std::string_view(model_header).substr(
        packet_body.first, packet_body.second - packet_body.first);
    for (const auto forbidden :
         {"Com<", "IMTLD3D12Device", "CommandQueue", "void *", "void*"}) {
      if (body.find(forbidden) != std::string_view::npos)
        errors.push_back(
            "include/dxmt_d3d12_submission_model.hpp:" +
            std::to_string(LineNumber(model_header, packet_body.first)) +
            ": submission packet must not own or borrow an upper-layer "
            "runtime object: " +
            forbidden);
    }
  } catch (const std::exception &error) {
    errors.push_back(error.what());
  }

  try {
    const auto retirement_body =
        BracedBody(model_header, "class GpuRetirementRecord final");
    const auto body = std::string_view(model_header).substr(
        retirement_body.first,
        retirement_body.second - retirement_body.first);
    for (const auto forbidden :
         {"Com<", "IMTLD3D12Device", "CommandQueue", "void *", "void*"}) {
      if (body.find(forbidden) != std::string_view::npos)
        errors.push_back(
            "include/dxmt_d3d12_submission_model.hpp:" +
            std::to_string(LineNumber(model_header, retirement_body.first)) +
            ": retirement record must contain leaf state only: " +
            forbidden);
    }
  } catch (const std::exception &error) {
    errors.push_back(error.what());
  }

  // Module isolation and build-graph completeness are enforced structurally
  // over the whole d3d12 directory instead of being restated per module.
  // What is enumerated here is the *exception* list -- the upper-layer
  // components that legitimately drive a command queue -- so a newly extracted
  // value-domain module is covered automatically without touching Builder.
  static const std::set<std::string> queue_owning_components = {
      "d3d12_command_queue.cpp",
      "d3d12_command_queue.hpp",
      "d3d12_device.cpp",
      "d3d12_resource.cpp",
  };
  const auto d3d12_dir = repo_root / "src/d3d12";
  if (fs::is_directory(d3d12_dir)) {
    std::vector<fs::path> d3d12_sources;
    for (const auto &entry : fs::directory_iterator(d3d12_dir)) {
      if (!entry.is_regular_file())
        continue;
      const auto extension = entry.path().extension();
      if (extension != ".cpp" && extension != ".hpp" && extension != ".inc")
        continue;
      d3d12_sources.push_back(entry.path());
    }
    std::sort(d3d12_sources.begin(), d3d12_sources.end());
    const auto meson_text = ReadFile(repo_root / "src/d3d12/meson.build");
    for (const auto &path : d3d12_sources) {
      const auto filename = path.filename().string();
      const auto source = ReadFile(path);
      // A queue fragment is included into the class body; nobody else may pull
      // one in, and it must never be compiled as its own translation unit.
      if (path.extension() == ".inc") {
        if (meson_text.find("'" + filename + "'") != std::string::npos)
          errors.push_back(
              "src/d3d12/meson.build: queue fragment must not be compiled as "
              "a translation unit: " +
              filename);
        continue;
      }
      // Every real source file must be reachable from the build graph.
      if (path.extension() == ".cpp" &&
          meson_text.find("'" + filename + "'") == std::string::npos)
        errors.push_back(
            "src/d3d12/meson.build: source file is not compiled by any "
            "target: " +
            filename);
      // The queue source is the one legitimate place that assembles the
      // fragments into the class body; everyone else must stay out.
      if (filename != "d3d12_command_queue.cpp")
        AppendLiteralViolations(
            errors, repo_root, path, source, "#include \"d3d12_command_queue_",
            "queue fragments are included into the class body and must not be "
            "included anywhere else");
      if (queue_owning_components.contains(filename))
        continue;
      AppendQueueIndependenceViolations(errors, repo_root, path);
    }
  }

  // Both of these cover src/d3d12 and src/dxmt as a whole rather than a named
  // set of files, so extracting a module never moves code out from under them.
  AppendThreadSafetyEscapeHatchViolations(errors, repo_root);
  AppendDuplicatedImplementationViolations(errors, repo_root);

  const auto d3d12_meson = ReadFile(repo_root / "src/d3d12/meson.build");
  if (d3d12_meson.find("'d3d12_submission_model.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_submission_model_header.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_device_queue_state.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_descriptor_diagnostics.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_replay_diagnostics.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_queue_config.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_temporal_scaler.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_texture_swizzle.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_texture_view.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_shader_binding.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_argument_buffer_layout.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_pipeline_write_policy.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_submission_service.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_swapchain.cpp'") ==
          std::string::npos ||
      d3d12_meson.find("'d3d12_tile_mapping.cpp'") ==
          std::string::npos)
    errors.push_back(
        "src/d3d12/meson.build: production DX12 target must compile the "
        "submission model, device queue state, descriptor diagnostics, replay "
        "diagnostics, queue configuration, sequencer service, isolated "
        "swapchain, tile mapping, temporal scaler, texture swizzle, "
        "texture view, shader binding, argument buffer layout, and pipeline "
        "write policy domains");
  if (d3d12_meson.find("'d3d12-submission-model'") == std::string::npos ||
      d3d12_meson.find("'../../tools/audit/d3d12_submission_model_spec.cpp'") ==
          std::string::npos)
    errors.push_back(
        "src/d3d12/meson.build: native submission model test must remain "
        "registered outside the product-test public API boundary");
  if (d3d12_meson.find("'d3d12-copy-geometry'") == std::string::npos ||
      d3d12_meson.find("'../../tools/audit/d3d12_copy_geometry_spec.cpp'") ==
          std::string::npos)
    errors.push_back(
        "src/d3d12/meson.build: native copy-geometry boundary spec must remain "
        "registered; the footprint, box and tile-offset range checks are only "
        "solvable natively and have no coverage in the product tests");

  const auto fence_header_path = repo_root / "src/d3d12/d3d12_fence.hpp";
  const auto fence_source_path = repo_root / "src/d3d12/d3d12_fence.cpp";
  const auto device_source_path = repo_root / "src/d3d12/d3d12_device.cpp";
  const auto d3d12_queue_source_path =
      repo_root / "src/d3d12/d3d12_command_queue.cpp";
  // CommandQueueImpl is split across domain fragments that are included inside
  // the class body. Architecture rules must see the whole class, so every
  // fragment is concatenated into one logical queue translation unit.
  std::vector<fs::path> d3d12_queue_fragment_paths;
  if (fs::is_directory(repo_root / "src/d3d12")) {
    for (const auto &entry :
         fs::directory_iterator(repo_root / "src/d3d12")) {
      if (!entry.is_regular_file())
        continue;
      const auto filename = entry.path().filename().string();
      if (filename.rfind("d3d12_command_queue_", 0) == 0 &&
          entry.path().extension() == ".inc")
        d3d12_queue_fragment_paths.push_back(entry.path());
    }
    std::sort(d3d12_queue_fragment_paths.begin(),
              d3d12_queue_fragment_paths.end());
  }
  // Contract checks below ask "does the queue still express this design?".
  // That question must follow the code, not a file path: replay types have
  // been migrating out of the queue source into fragments and then into real
  // headers, and a check bound to one location silently turns into a false
  // alarm (or worse, a false pass) on every move.  So the unit under review is
  // the queue source plus its fragments plus the d3d12 headers it pulls in.
  const auto ReadQueueTranslationUnit = [&]() {
    std::string combined = ReadFile(d3d12_queue_source_path);
    for (const auto &fragment : d3d12_queue_fragment_paths) {
      combined += "\n// ==== queue fragment: ";
      combined += fragment.filename().string();
      combined += " ====\n";
      combined += ReadFile(fragment);
    }
    return combined;
  };
  // Two different questions need two different scopes.  "Does this construct
  // still live in the queue?" must look only at the queue itself, or an
  // extracted module would be reported as a violation of the very rule that
  // sent it there.  "Does the design still exist?" must also look at the
  // headers the queue pulls in, because that is where the replay types moved.
  const auto ReadQueueContractUnit = [&]() {
    const auto queue_source = ReadFile(d3d12_queue_source_path);
    std::string combined = ReadQueueTranslationUnit();
    static const std::regex queue_header_include(
        R"re(^[ \t]*#[ \t]*include[ \t]*"(d3d12_[A-Za-z0-9_]+\.hpp)")re",
        std::regex::multiline);
    for (std::sregex_iterator match(queue_source.begin(), queue_source.end(),
                                    queue_header_include), end;
         match != end; ++match) {
      const auto header =
          d3d12_queue_source_path.parent_path() / (*match)[1].str();
      std::error_code error;
      if (!fs::is_regular_file(header, error))
        continue;
      combined += "\n// ==== queue header: ";
      combined += header.filename().string();
      combined += " ====\n";
      combined += ReadFile(header);
    }
    return combined;
  };
  const auto submission_service_source_path =
      repo_root / "src/d3d12/d3d12_submission_service.cpp";
  const auto swapchain_source_path =
      repo_root / "src/d3d12/d3d12_swapchain.cpp";
  const auto swapchain_header_path =
      repo_root / "src/d3d12/d3d12_swapchain.hpp";
  const auto tile_mapping_source_path =
      repo_root / "src/d3d12/d3d12_tile_mapping.cpp";
  const auto tile_mapping_header_path =
      repo_root / "src/d3d12/d3d12_tile_mapping.hpp";
  const auto device_queue_state_source_path =
      repo_root / "src/d3d12/d3d12_device_queue_state.cpp";
  const auto device_queue_state_header_path =
      repo_root / "src/d3d12/d3d12_device_queue_state.hpp";
  const auto replay_diagnostics_source_path =
      repo_root / "src/d3d12/d3d12_replay_diagnostics.cpp";
  const auto replay_diagnostics_header_path =
      repo_root / "src/d3d12/d3d12_replay_diagnostics.hpp";
  const auto descriptor_diagnostics_source_path =
      repo_root / "src/d3d12/d3d12_descriptor_diagnostics.cpp";
  const auto descriptor_diagnostics_header_path =
      repo_root / "src/d3d12/d3d12_descriptor_diagnostics.hpp";
  const auto queue_config_source_path =
      repo_root / "src/d3d12/d3d12_queue_config.cpp";
  const auto queue_config_header_path =
      repo_root / "src/d3d12/d3d12_queue_config.hpp";
  const auto temporal_scaler_source_path =
      repo_root / "src/d3d12/d3d12_temporal_scaler.cpp";
  const auto temporal_scaler_header_path =
      repo_root / "src/d3d12/d3d12_temporal_scaler.hpp";
  const auto texture_swizzle_source_path =
      repo_root / "src/d3d12/d3d12_texture_swizzle.cpp";
  const auto texture_swizzle_header_path =
      repo_root / "src/d3d12/d3d12_texture_swizzle.hpp";
  const auto texture_view_source_path =
      repo_root / "src/d3d12/d3d12_texture_view.cpp";
  const auto texture_view_header_path =
      repo_root / "src/d3d12/d3d12_texture_view.hpp";
  const auto shader_binding_source_path =
      repo_root / "src/d3d12/d3d12_shader_binding.cpp";
  const auto shader_binding_header_path =
      repo_root / "src/d3d12/d3d12_shader_binding.hpp";
  const auto argument_buffer_layout_source_path =
      repo_root / "src/d3d12/d3d12_argument_buffer_layout.cpp";
  const auto argument_buffer_layout_header_path =
      repo_root / "src/d3d12/d3d12_argument_buffer_layout.hpp";
  const auto pipeline_write_policy_source_path =
      repo_root / "src/d3d12/d3d12_pipeline_write_policy.cpp";
  const auto pipeline_write_policy_header_path =
      repo_root / "src/d3d12/d3d12_pipeline_write_policy.hpp";
  if (!fs::is_regular_file(submission_service_source_path)) {
    errors.push_back(
        "src/d3d12/d3d12_submission_service.cpp: device-owned sequencer "
        "must remain an independently analyzable translation unit");
  } else {
    const auto submission_service_source =
        ReadFile(submission_service_source_path);
    for (const auto required :
         {"class DeviceSubmissionService::Impl final",
          "class DeviceQueueWorkRequest final",
          "\"dxmt-d3d12-sequencer\""}) {
      if (submission_service_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_submission_service.cpp: missing isolated "
            "device-owned submission sequencer contract: " +
            std::string(required));
    }
    static const std::regex guarded_queues(
        R"(std::vector<std::shared_ptr<SubmissionQueueEndpoint>>\s+queues_\s+DXMT_GUARDED_BY\(mutex_\))");
    static const std::regex guarded_device_work(
        R"(std::deque<std::shared_ptr<DeviceQueueWorkRequest>>\s+device_work_\s+DXMT_GUARDED_BY\(mutex_\))");
    if (!std::regex_search(submission_service_source, guarded_queues) ||
        !std::regex_search(submission_service_source, guarded_device_work))
      errors.push_back(
          "src/d3d12/d3d12_submission_service.cpp: sequencer queues must "
          "remain guarded by their admission mutex");
    try {
      const auto service_body =
          BracedBody(submission_service_source,
                     "class DeviceSubmissionService::Impl final");
      const auto body = std::string_view(submission_service_source).substr(
          service_body.first, service_body.second - service_body.first);
      if (body.find("std::function") != std::string_view::npos)
        errors.push_back(
            "src/d3d12/d3d12_submission_service.cpp:" +
            std::to_string(LineNumber(submission_service_source,
                                      service_body.first)) +
            ": device submission sequencing must use a typed endpoint, not "
            "a type-erased callback");
    } catch (const std::exception &error) {
      errors.push_back(error.what());
    }
  }
  if (!fs::is_regular_file(swapchain_source_path) ||
      !fs::is_regular_file(swapchain_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_swapchain.cpp: swapchain/present must remain an "
        "independently analyzable translation unit");
  } else {
    const auto swapchain_source = ReadFile(swapchain_source_path);
    const auto swapchain_header = ReadFile(swapchain_header_path);
    for (const auto required :
         {"class D3D12SwapChainHost",
          "struct D3D12PresentSubmission final",
          "D3D12PresentSubmission(const D3D12PresentSubmission &) = delete",
          "static_assert(!std::is_copy_constructible_v<"
          "D3D12PresentSubmission>)"}) {
      if (swapchain_header.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_swapchain.hpp: missing typed swapchain "
            "ownership contract: " +
            std::string(required));
    }
    for (const auto required :
         {"class SwapChainImpl final",
          "D3D12SwapChainHost &host_",
          "host_.SubmitPresent(",
          "host_.PrepareSwapChainResize("}) {
      if (swapchain_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_swapchain.cpp: missing isolated swapchain host "
            "contract: " +
            std::string(required));
    }
    for (const auto forbidden :
         {"CommandQueueImpl", "PendingOperation", "QueueWorkSubmission",
          "std::mutex", "dxmt::mutex", "std::function", "[&]", "[=]"}) {
      AppendLiteralViolations(
          errors, repo_root, swapchain_source_path, swapchain_source,
          forbidden,
          "swapchain must cross the queue boundary only through a typed "
          "host contract");
    }
  }
  if (!fs::is_regular_file(tile_mapping_source_path) ||
      !fs::is_regular_file(tile_mapping_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_tile_mapping.cpp: sparse tile mapping must remain "
        "an independently analyzable translation unit");
  } else {
    const auto tile_mapping_source = ReadFile(tile_mapping_source_path);
    const auto tile_mapping_header = ReadFile(tile_mapping_header_path);
    for (const auto required :
         {"class TileRangeView final",
          "FromAbi(UINT count",
          "std::span<const WMTSparseTextureMappingOperation>",
          "CollectLogicalTilesInRegion",
          "CollectTilesInRegion"}) {
      if (tile_mapping_header.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_tile_mapping.hpp: missing checked tile mapping "
            "boundary: " +
            std::string(required));
    }
    for (const auto required :
         {"struct D3D12TileMappingCounters",
          "ApplySparseTileMappingOpsToResource",
          "RecordTileMappingMetalFailure"}) {
      if (tile_mapping_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_tile_mapping.cpp: missing isolated tile "
            "mapping state: " +
            std::string(required));
    }
  }
  if (!fs::is_regular_file(device_queue_state_source_path) ||
      !fs::is_regular_file(device_queue_state_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_device_queue_state.cpp: device queue lifetime state "
        "must remain an independently analyzable translation unit");
  } else {
    const auto state_source = ReadFile(device_queue_state_source_path);
    const auto state_header = ReadFile(device_queue_state_header_path);
    for (const auto required :
         {"class D3D12DeviceQueueState final",
          "using ReplayResourceStateMap",
          "BackendResourceStates() noexcept",
          "AcquireD3D12DeviceQueueState(IMTLD3D12Device &device)"}) {
      if (state_header.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_device_queue_state.hpp: missing typed device "
            "queue state contract: " +
            std::string(required));
    }
    for (const auto required :
         {"class D3D12DeviceQueueStateRegistry final",
          "std::weak_ptr<D3D12DeviceQueueState>",
          "if (entry->second.expired())",
          "std::make_shared<D3D12DeviceQueueState>()"}) {
      if (state_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_device_queue_state.cpp: missing weak registry "
            "lifetime contract: " +
            std::string(required));
    }
    for (const auto forbidden :
         {"CommandQueueImpl", "PendingOperation", "QueueWorkSubmission",
          "ID3D12CommandQueue", "std::function", "[&]", "[=]"}) {
      AppendLiteralViolations(
          errors, repo_root, device_queue_state_source_path, state_source,
          forbidden,
          "device queue state registry must own no logical queue or replay "
          "work");
    }
    AppendLiteralViolations(
        errors, repo_root, device_queue_state_header_path, state_header,
        "std::mutex",
        "device queue state payload is backend-thread confined; only the "
        "private registry may lock");
  }
  if (!fs::is_regular_file(replay_diagnostics_source_path) ||
      !fs::is_regular_file(replay_diagnostics_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_replay_diagnostics.cpp: diagnostic retirement "
        "leaves must remain an independently analyzable translation unit");
  } else {
    const auto diagnostics_source = ReadFile(replay_diagnostics_source_path);
    const auto diagnostics_header = ReadFile(replay_diagnostics_header_path);
    for (const auto required :
         {"class DiagnosticReadbackBuffer final",
          "DiagnosticReadbackBuffer(const DiagnosticReadbackBuffer &) = "
          "delete",
          "~DiagnosticReadbackBuffer() noexcept",
          "struct IndexReadbackRetirementWork final",
          "struct VertexReadbackRetirementWork final",
          "struct ConstantBufferReadbackRetirementWork final"}) {
      if (diagnostics_header.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_replay_diagnostics.hpp: missing typed "
            "diagnostic lifetime contract: " +
            std::string(required));
    }
    for (const auto forbidden :
         {"CommandQueueImpl", "ReplayState", "PendingOperation",
          "QueueWorkSubmission", "ID3D12CommandQueue", "IMTLD3D12Device",
          "std::mutex", "dxmt::mutex", "std::function", "[&]", "[=]"}) {
      AppendLiteralViolations(
          errors, repo_root, replay_diagnostics_source_path,
          diagnostics_source, forbidden,
          "diagnostic retirement leaves may only release their owned "
          "readback allocation");
    }
  }
  if (!fs::is_regular_file(descriptor_diagnostics_source_path) ||
      !fs::is_regular_file(descriptor_diagnostics_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_descriptor_diagnostics.cpp: descriptor view "
        "diagnostics must remain an independently analyzable translation "
        "unit");
  } else {
    const auto diagnostics_source =
        ReadFile(descriptor_diagnostics_source_path);
    const auto diagnostics_header =
        ReadFile(descriptor_diagnostics_header_path);
    for (const auto required :
         {"D3D12DiagDescriptorFormat(",
          "DescriptorRecordTypeName(",
          "D3D12DiagLogTextureView(",
          "D3D12DiagLogDSVReplayDescriptor("}) {
      if (diagnostics_header.find(required) == std::string::npos ||
          diagnostics_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_descriptor_diagnostics.cpp: missing typed "
            "descriptor diagnostic contract: " +
            std::string(required));
    }
  }
  if (!fs::is_regular_file(queue_config_source_path) ||
      !fs::is_regular_file(queue_config_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_queue_config.cpp: queue descriptor validation must "
        "remain an independently analyzable translation unit");
  } else {
    const auto config_source = ReadFile(queue_config_source_path);
    const auto config_header = ReadFile(queue_config_header_path);
    if (config_header.find("NormalizeQueueDesc(") == std::string::npos ||
        config_source.find("IsSupportedQueueType(") == std::string::npos ||
        config_source.find("IsSupportedQueuePriority(") ==
            std::string::npos ||
        config_source.find("IsSupportedQueueFlags(") == std::string::npos)
      errors.push_back(
          "src/d3d12/d3d12_queue_config.cpp: missing closed queue "
          "descriptor validation contract");
  }
  if (!fs::is_regular_file(temporal_scaler_source_path) ||
      !fs::is_regular_file(temporal_scaler_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_temporal_scaler.cpp: temporal scaler cache and "
        "format policy must remain an independently analyzable translation "
        "unit");
  } else {
    const auto scaler_source = ReadFile(temporal_scaler_source_path);
    const auto scaler_header = ReadFile(temporal_scaler_header_path);
    for (const auto required :
         {"struct CachedTemporalScaler final",
          "TemporalUpscaleMotionVectorSourceFormat(WMTPixelFormat format) "
          "noexcept",
          "bool motion_vector_in_display_resolution) noexcept"}) {
      if (scaler_header.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_temporal_scaler.hpp: missing isolated temporal "
            "scaler contract: " +
            std::string(required));
    }
    for (const auto required :
         {"TemporalUpscaleMotionVectorSourceFormat(WMTPixelFormat format) "
          "noexcept",
          "TemporalUpscaleMotionTextureFormat(",
          "bool motion_vector_in_display_resolution) noexcept"}) {
      if (scaler_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_temporal_scaler.cpp: missing isolated temporal "
            "format policy: " +
            std::string(required));
    }
  }
  if (!fs::is_regular_file(texture_swizzle_source_path) ||
      !fs::is_regular_file(texture_swizzle_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_texture_swizzle.cpp: descriptor component mapping "
        "must remain an independently analyzable translation unit");
  } else {
    const auto swizzle_source = ReadFile(texture_swizzle_source_path);
    const auto swizzle_header = ReadFile(texture_swizzle_header_path);
    if (swizzle_header.find(
            "ShaderResourceViewSwizzle(WMTPixelFormat format, "
            "UINT component_mapping)") == std::string::npos)
      errors.push_back(
          "src/d3d12/d3d12_texture_swizzle.hpp: missing typed descriptor "
          "swizzle boundary");
    for (const auto required :
         {"BaseShaderReadSwizzleForFormat(",
          "TextureSwizzleFromD3D12Component(",
          "ComposeTextureSwizzleComponent(",
          "ShaderResourceViewSwizzle("}) {
      if (swizzle_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_texture_swizzle.cpp: missing descriptor "
            "component mapping rule: " +
            std::string(required));
    }
  }
  if (!fs::is_regular_file(texture_view_source_path) ||
      !fs::is_regular_file(texture_view_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_texture_view.cpp: descriptor texture view "
        "materialization must remain an independently analyzable translation "
        "unit");
  } else {
    const auto view_source = ReadFile(texture_view_source_path);
    const auto view_header = ReadFile(texture_view_header_path);
    for (const auto required :
         {"ResolveTextureViewFormat(",
          "ResolveRenderTargetTextureViewFormat(",
          "ResolveDepthStencilViewFormat(",
          "CreateRenderTargetView(",
          "CreateDepthStencilView("}) {
      if (view_header.find(required) == std::string::npos ||
          view_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_texture_view.cpp: missing typed descriptor "
            "texture view contract: " +
            std::string(required));
    }
  }
  if (!fs::is_regular_file(shader_binding_source_path) ||
      !fs::is_regular_file(shader_binding_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_shader_binding.cpp: shader argument resolution must "
        "remain an independently analyzable translation unit");
  } else {
    const auto binding_source = ReadFile(shader_binding_source_path);
    const auto binding_header = ReadFile(shader_binding_header_path);
    for (const auto required :
         {"ForEachVisibleStage(", "FindShaderForStage(",
          "BindingTypeForRange(", "ShaderBindingSlotCapacity(",
          "ShaderArgumentQwordStride(", "ShaderArgumentRangeCount(",
          "IntersectDescriptorRangeWithShaderArgument(",
          "ShaderArgumentAtRangeOffset(", "ResolveShaderBindingSlot(",
          "ResolveShaderBindingArgument(",
          "ResolveShaderBindingArgumentBySlot("}) {
      if (binding_header.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_shader_binding.hpp: missing shader binding "
            "resolution contract: " +
            std::string(required));
    }
  }
  if (!fs::is_regular_file(argument_buffer_layout_source_path) ||
      !fs::is_regular_file(argument_buffer_layout_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_argument_buffer_layout.cpp: argument buffer "
        "sub-allocation and size estimation must remain an independently "
        "analyzable translation unit");
  } else {
    const auto layout_source = ReadFile(argument_buffer_layout_source_path);
    const auto layout_header = ReadFile(argument_buffer_layout_header_path);
    for (const auto required :
         {"AllocateArgumentBuffer(", "AlignArgumentBufferSize(",
          "AdvanceArgumentBufferEstimate(",
          "EstimateShaderArgumentBufferSize(",
          "EstimateGraphicsArgumentBufferSize(",
          "EstimateComputeArgumentBufferSize("}) {
      if (layout_header.find(required) == std::string::npos ||
          layout_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_argument_buffer_layout.cpp: missing argument "
            "buffer layout contract: " +
            std::string(required));
    }
  }
  if (!fs::is_regular_file(pipeline_write_policy_source_path) ||
      !fs::is_regular_file(pipeline_write_policy_header_path)) {
    errors.push_back(
        "src/d3d12/d3d12_pipeline_write_policy.cpp: depth/stencil write "
        "policy and dispatch validation must remain an independently "
        "analyzable translation unit");
  } else {
    const auto policy_source = ReadFile(pipeline_write_policy_source_path);
    const auto policy_header = ReadFile(pipeline_write_policy_header_path);
    for (const auto required :
         {"StencilOpWrites(", "StencilFaceWrites(", "PipelineWritesDepth(",
          "PipelineWritesStencil(", "AccessForDepthStencilPlane(",
          "DepthStencilResourceStateForAccess(",
          "ValidateComputeDispatch("}) {
      if (policy_header.find(required) == std::string::npos ||
          policy_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_pipeline_write_policy.cpp: missing pipeline "
            "write policy contract: " +
            std::string(required));
    }
  }
  if (fs::is_regular_file(d3d12_queue_source_path)) {
    const auto d3d12_queue_source = ReadQueueTranslationUnit();
    const auto d3d12_queue_contract = ReadQueueContractUnit();
    const auto queue_source_text = ReadFile(d3d12_queue_source_path);
    for (const auto &fragment : d3d12_queue_fragment_paths) {
      const auto include_directive =
          "#include \"" + fragment.filename().string() + "\"";
      if (queue_source_text.find(include_directive) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_command_queue.cpp: orphaned queue fragment is "
            "never included: " +
            fragment.filename().string());
    }
    for (const auto forbidden :
         {"struct TileRangeCursor", "struct SparseTileCoordinate",
          "struct D3D12TileMappingCounters",
          "ResolveSparseTileCoordinate("}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "tile mapping state and coordinate parsing belong in "
          "d3d12_tile_mapping");
    }
    for (const auto forbidden :
         {"struct CommandQueueResourceStates",
          "g_resource_states_by_device", "g_resource_states_mutex",
          "struct ReplaySubresourceState",
          "struct ReplayResourceStateEntry"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "device queue lifetime state belongs in d3d12_device_queue_state");
    }
    for (const auto forbidden :
         {"class DiagnosticReadbackBuffer final",
          "struct DrawVisibilityRetirementWork final",
          "struct IndexReadbackRetirementWork final",
          "struct VertexReadbackRetirementWork final",
          "struct ConstantBufferReadbackRetirementWork final"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "diagnostic ownership leaves belong in d3d12_replay_diagnostics");
    }
    for (const auto forbidden :
         {"static void ForEachVisibleStage(",
          "FindShaderForStage(const PipelineState",
          "BindingTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE",
          "ShaderBindingSlotCapacity(SM50BindingType",
          "ShaderArgumentQwordStride(const DXMT12_MTL4_SHADER_ARGUMENT",
          "ShaderArgumentRangeCount(const DXMT12_MTL4_SHADER_ARGUMENT",
          "struct DescriptorShaderRangeOverlap",
          "IntersectDescriptorRangeWithShaderArgument(UINT",
          "ShaderArgumentAtRangeOffset(const DXMT12_MTL4_SHADER_ARGUMENT",
          "ResolveShaderBindingSlot(const PipelineState",
          "ResolveShaderBindingArgumentBySlot(const PipelineState"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "shader argument resolution belongs in d3d12_shader_binding");
    }
    for (const auto forbidden :
         {"AllocateArgumentBuffer(uint64_t &cursor",
          "AlignArgumentBufferSize(uint64_t",
          "AdvanceArgumentBufferEstimate(uint64_t",
          "EstimateShaderArgumentBufferSize(",
          "EstimateGraphicsArgumentBufferSize(PipelineState",
          "EstimateComputeArgumentBufferSize(PipelineState"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "argument buffer sub-allocation and size estimation belong in "
          "d3d12_argument_buffer_layout");
    }
    for (const auto forbidden :
         {"StencilOpWrites(D3D12_STENCIL_OP",
          "StencilFaceWrites(const D3D12_DEPTH_STENCILOP_DESC",
          "PipelineWritesDepth(const PipelineGraphicsState",
          "PipelineWritesStencil(const PipelineGraphicsState",
          "AccessForDepthStencilPlane(const DescriptorRecord",
          "DepthStencilResourceStateForAccess(int",
          "ValidateComputeDispatch(const WMTSize"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "depth/stencil write policy and dispatch validation belong in "
          "d3d12_pipeline_write_policy");
    }
    for (const auto forbidden :
         {"D3D12DiagDescriptorFormat(const DescriptorRecord",
          "DescriptorRecordTypeName(DescriptorRecordType",
          "D3D12DiagLogTextureView(const char",
          "D3D12DiagLogDSVReplayDescriptor(const char"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "descriptor view diagnostics belong in "
          "d3d12_descriptor_diagnostics");
    }
    for (const auto forbidden :
         {"struct CachedTemporalScaler",
          "GetTemporalUpscaleMotionVectorSourceFormat(",
          "GetTemporalUpscaleMotionTextureFormat("}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "temporal scaler cache and format policy belong in "
          "d3d12_temporal_scaler");
    }
    for (const auto forbidden :
         {"DefaultTextureViewSwizzle(",
          "BaseShaderReadSwizzleForFormat(",
          "TextureSwizzleFromD3D12Component(",
          "ComposeTextureSwizzleComponent("}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "descriptor component mapping belongs in d3d12_texture_swizzle");
    }
    for (const auto forbidden :
         {"ResolveRenderTargetTextureViewFormat(WMT::Device",
          "ResolveDepthStencilViewFormat(WMT::Device",
          "CreateRenderTargetView(WMT::Device",
          "CreateDepthStencilView(WMT::Device"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "descriptor attachment view materialization belongs in "
          "d3d12_texture_view");
    }
    for (const auto forbidden :
         {"IsSupportedQueueType(", "IsSupportedQueuePriority(",
          "IsSupportedQueueFlags("}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "queue descriptor validation belongs in d3d12_queue_config");
    }
    for (const auto required :
         {"class QueueSubmissionEndpoint final",
          "device_queue_state_->SubmissionService()->RegisterQueue(",
          "device_queue_state_->SubmissionService()->UnregisterQueue(",
          "ProcessReadySubmission() noexcept"}) {
      if (d3d12_queue_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_command_queue.cpp: missing device-owned "
            "submission sequencer contract: " +
            std::string(required));
    }
    for (const auto forbidden :
         {"SubmissionWorkerMain(", "dxmt::thread submission_worker_",
          "\"dxmt-d3d12-submit\"", "RetirementWorkerMain(",
          "dxmt::thread retirement_worker_", "GpuCompletionPublication"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "per-queue submission/retirement workers and secondary completion "
          "publications are forbidden");
    }
    for (const auto forbidden :
         {"ReplayGraphicsCompiledPayloadArena",
          "ReplayGraphicsCompiledEncodeFn", "compiled_payload",
          "void *payload", "std::function"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "replay packets must use explicit owned encoders; manual payload "
          "arenas and type-erased callbacks are forbidden");
    }
    for (const auto required :
         {"class RetirementRecord final : public GpuCompletionTarget",
          "chunk->addCompletionTarget(",
          "class DeferredCpuQueryResolveTarget final : public "
          "CpuQueryResolveTarget",
          "struct ExecuteSubmission final",
          "struct QueueWorkSubmission final",
          "struct FenceSignalSubmission final",
          "struct FenceWaitSubmission final",
          "struct StopSubmission final",
          "using PendingOperationPayload =",
          "std::variant<ExecuteSubmission, QueueWorkSubmission,",
          "FencePrivateReference fence;",
          "std::deque<PendingOperation> pending_operations_ "
          "DXMT_GUARDED_BY(mutex_);",
          "bool submission_worker_active_ DXMT_GUARDED_BY(mutex_)",
          "bool submission_worker_waiting_for_wait_ "
          "DXMT_GUARDED_BY(mutex_)",
          "bool submission_worker_stopping_ DXMT_GUARDED_BY(mutex_)",
          "bool submission_service_stopped_ DXMT_GUARDED_BY(mutex_)",
          "SubmitDxmtQueueWork(",
          "struct SwapChainResizeQueueWork final",
          "class ReplayPassEncodeCommand",
          "class ReplayCompiledEncodeCommand final",
          "std::shared_ptr<ReplayPassEncodeCommand> encoder",
          "class ReplayBlitEncodeCommand",
          "std::shared_ptr<ReplayBlitEncodeCommand> encoder",
          "struct ReplayCommandStorage final",
          "std::shared_ptr<ReplayEncoderArena> arena"}) {
      if (d3d12_queue_contract.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_command_queue.cpp: missing direct typed Metal "
            "completion contract: " +
            std::string(required));
    }
    try {
      const auto packet_body =
          BracedBody(d3d12_queue_contract, "struct PendingOperation final");
      const auto body = std::string_view(d3d12_queue_contract).substr(
          packet_body.first, packet_body.second - packet_body.first);
      for (const auto forbidden :
           {"PendingOperationType type =", "Fence *", "Fence*",
            "AddRefPrivate", "ReleasePrivate", "std::function"}) {
        if (body.find(forbidden) != std::string_view::npos)
          errors.push_back(
              // The packet type has moved out of the queue source into a real
              // header, so the offset is into the concatenated contract unit
              // and no single file path names it.
              "src/d3d12 logical command queue (contract unit offset " +
              std::to_string(
                  LineNumber(d3d12_queue_contract, packet_body.first)) + ")" +
              ": production submission packet must use a closed typed "
              "payload with RAII ownership: " +
              forbidden);
      }
    } catch (const std::exception &error) {
      errors.push_back(error.what());
    }
    try {
      const auto retirement_body =
          BracedBody(d3d12_queue_source,
                     "class RetirementRecord final : public "
                     "GpuCompletionTarget");
      const auto body = std::string_view(d3d12_queue_source).substr(
          retirement_body.first,
          retirement_body.second - retirement_body.first);
      for (const auto forbidden :
           {"CommandQueueImpl", "IMTLD3D12Device", "Com<", "std::function",
            "void *", "void*"}) {
        if (body.find(forbidden) != std::string_view::npos)
          errors.push_back(
              "src/d3d12/d3d12_command_queue.cpp:" +
              std::to_string(
                  LineNumber(d3d12_queue_source, retirement_body.first)) +
              ": production retirement target must contain leaf payload "
              "only: " +
              forbidden);
      }
    } catch (const std::exception &error) {
      errors.push_back(error.what());
    }
  }
  const auto queue_header_path =
      repo_root / "src/d3d12/d3d12_command_queue.hpp";
  const auto resource_source_path =
      repo_root / "src/d3d12/d3d12_resource.cpp";
  std::vector<fs::path> queue_lock_scan_paths = {
      d3d12_queue_source_path, queue_header_path, resource_source_path,
      device_source_path, submission_service_source_path,
      swapchain_source_path, tile_mapping_source_path,
      device_queue_state_source_path, replay_diagnostics_source_path,
      descriptor_diagnostics_source_path, queue_config_source_path,
      temporal_scaler_source_path, texture_swizzle_source_path,
      texture_view_source_path, shader_binding_source_path,
      argument_buffer_layout_source_path, pipeline_write_policy_source_path};
  queue_lock_scan_paths.insert(queue_lock_scan_paths.end(),
                               d3d12_queue_fragment_paths.begin(),
                               d3d12_queue_fragment_paths.end());
  for (const auto &path : queue_lock_scan_paths) {
    if (!fs::is_regular_file(path))
      continue;
    const auto source = ReadFile(path);
    for (const auto forbidden :
         {"dxmt_queue_mutex", "DxmtQueueSubmissionGuard",
          "AcquireDxmtQueueSubmissionGuard"}) {
      AppendLiteralViolations(
          errors, repo_root, path, source, forbidden,
          "direct DXMT queue locking is forbidden; submit typed work to the "
          "device sequencer");
    }
  }
  if (fs::is_regular_file(resource_source_path)) {
    const auto resource_source = ReadFile(resource_source_path);
    for (const auto required :
         {"struct DepthStencilUploadCommand final",
          "struct TextureUploadCommand final",
          "struct DepthStencilReadbackCommand final",
          "struct TextureReadbackCommand final",
          "using SynchronousBlitPayload =",
          "class SynchronousBlitSubmission final"}) {
      if (resource_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_resource.cpp: synchronous DXMT blits must use "
            "closed typed sequencer work: " +
            std::string(required));
    }
  }
  if (fs::is_regular_file(device_source_path)) {
    const auto device_source = ReadFile(device_source_path);
    if (device_source.find("class SetEventQueueSubmission final") ==
        std::string::npos)
      errors.push_back(
          "src/d3d12/d3d12_device.cpp: EnqueueSetEvent must use typed device "
          "sequencer work");
  }
  if (fs::is_regular_file(fence_header_path) &&
      fs::is_regular_file(fence_source_path) &&
      fs::is_regular_file(device_source_path) &&
      fs::is_regular_file(d3d12_queue_source_path)) {
    const auto fence_header = ReadFile(fence_header_path);
    const auto fence_source = ReadFile(fence_source_path);
    const auto device_source = ReadFile(device_source_path);
    const auto d3d12_queue_source = ReadQueueTranslationUnit();
    const auto d3d12_queue_contract = ReadQueueContractUnit();
    static constexpr std::array<
        std::pair<std::string_view, std::string_view>, 4>
        forbidden_completion_contracts = {{
            {"CompletionCallbackRunner",
             "global completion callback runner is forbidden"},
            {"AddCompletionCallback",
             "type-erased fence completion callbacks are forbidden"},
            {"FencePendingCallback",
             "type-erased pending fence callbacks are forbidden"},
            {".detach()", "D3D12 workers must be joined, never detached"},
        }};
    for (const auto &[literal, message] : forbidden_completion_contracts) {
      AppendLiteralViolations(errors, repo_root, fence_header_path,
                              fence_header, literal, message);
      AppendLiteralViolations(errors, repo_root, fence_source_path,
                              fence_source, literal, message);
      AppendLiteralViolations(errors, repo_root, device_source_path,
                              device_source, literal, message);
      AppendLiteralViolations(errors, repo_root, d3d12_queue_source_path,
                              d3d12_queue_source, literal, message);
    }
    for (const auto required :
         {"class FenceWaitTarget", "std::weak_ptr<FenceWaitTarget>"}) {
      if (fence_header.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_fence.hpp: missing typed fence wait contract: " +
            std::string(required));
    }
    if (device_source.find("class MultipleFenceWaitState final") ==
            std::string::npos ||
        device_source.find("class FencePrivateReference final") ==
            std::string::npos)
      errors.push_back(
          "src/d3d12/d3d12_device.cpp: multiple-fence waits must use a typed "
          "target with explicit private-reference ownership");
    if (d3d12_queue_source.find("RegisterQueueWait(") == std::string::npos)
      errors.push_back(
          "src/d3d12/d3d12_command_queue.cpp: unresolved CPU/external waits "
          "must use typed fence wait registration");
    if (fence_source.find(
            "struct FenceCompletionState final : DeviceErrorTarget") ==
        std::string::npos)
      errors.push_back(
          "src/d3d12/d3d12_fence.cpp: device removal must target the typed "
          "fence completion leaf");
    for (const auto forbidden :
         {"RegisterDeviceErrorCallback", "device_error_callback_",
          "std::make_shared<std::function<void()>>"}) {
      AppendLiteralViolations(
          errors, repo_root, fence_source_path, fence_source, forbidden,
          "fence device-error handling must not use a type-erased callback");
    }
    AppendLiteralViolations(
        errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
        "std::function<bool(CommandChunk",
        "type-erased queue work is forbidden; use QueueWorkPayload");
    AppendLiteralViolations(
        errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
        "std::vector<std::function<void(CommandChunk",
        "type-erased sparse replay groups are forbidden; use typed payloads");
    // The variant alias now lives in d3d12_queue_work_types.hpp; the visitor
    // still has to sit in the class. Ask the contract unit for the alias so the
    // rule keeps following the code out of the queue source.
    if (d3d12_queue_contract.find("using QueueWorkPayload =") ==
            std::string::npos ||
        d3d12_queue_source.find("class QueueWorkReplayVisitor final") ==
            std::string::npos)
      errors.push_back(
          "src/d3d12/d3d12_command_queue.cpp: queue work must remain a "
          "closed variant handled by a named visitor");
    for (const auto forbidden :
         {"addCompletionCallback", "completion_callbacks",
          "CompletionCallbackRunner", "deferred_readbacks"}) {
      AppendLiteralViolations(
          errors, repo_root, d3d12_queue_source_path, d3d12_queue_source,
          forbidden,
          "generic command-chunk completion callbacks are forbidden");
      AppendLiteralViolations(
          errors, repo_root, repo_root / "src/dxmt/dxmt_command_queue.hpp",
          queue_header, forbidden,
          "generic command-chunk completion callbacks are forbidden");
      AppendLiteralViolations(
          errors, repo_root, repo_root / "src/dxmt/dxmt_command_queue.cpp",
          queue_source, forbidden,
          "generic command-chunk completion callbacks are forbidden");
    }
    for (const auto required :
         {"DrawVisibilityRetirementWork", "IndexReadbackRetirementWork",
          "VertexReadbackRetirementWork",
          "ConstantBufferReadbackRetirementWork"}) {
      if (d3d12_queue_source.find(required) == std::string::npos)
        errors.push_back(
            "src/d3d12/d3d12_command_queue.cpp: missing typed retirement "
            "readback payload: " +
            std::string(required));
    }
    if (queue_header.find("class GpuCompletionTarget") ==
            std::string::npos ||
        d3d12_queue_contract.find("using RetirementPayload =") ==
            std::string::npos ||
        d3d12_queue_source.find("class RetirementVisitor final") ==
            std::string::npos ||
        d3d12_queue_source.find("EnqueueRetirement(") ==
            std::string::npos)
      errors.push_back(
          "D3D12 retirement must use the Metal completion point directly and "
          "a closed typed retirement payload");
  }

  const auto allocator_header_path =
      repo_root / "src/d3d12/d3d12_command_allocator.hpp";
  if (fs::is_regular_file(allocator_header_path)) {
    const auto allocator_header = ReadFile(allocator_header_path);
    if (allocator_header.find("class CommandAllocatorSubmissionState final") ==
        std::string::npos)
      errors.push_back(
          "src/d3d12/d3d12_command_allocator.hpp: allocator submission "
          "lifetime must be represented by an independent leaf state");
    try {
      const auto use_body =
          BracedBody(allocator_header, "struct SubmittedCommandAllocatorUse final");
      const auto body = std::string_view(allocator_header).substr(
          use_body.first, use_body.second - use_body.first);
      for (const auto forbidden :
           {"Com<", "CommandAllocatorObject", "CommandAllocator *",
            "CommandAllocator*"}) {
        if (body.find(forbidden) != std::string_view::npos)
          errors.push_back(
              "src/d3d12/d3d12_command_allocator.hpp:" +
              std::to_string(LineNumber(allocator_header, use_body.first)) +
              ": submitted allocator use must retain leaf state only: " +
              forbidden);
      }
    } catch (const std::exception &error) {
      errors.push_back(error.what());
    }
  }
  return errors;
}

class JsonStringObjectParser {
public:
  explicit JsonStringObjectParser(std::string_view contents)
      : contents_(contents) {}

  std::map<std::string, std::string> Parse() {
    std::map<std::string, std::string> result;
    SkipWhitespace();
    Expect('{');
    SkipWhitespace();
    if (Consume('}')) {
      RequireEnd();
      return result;
    }
    while (true) {
      const auto key = ParseString();
      SkipWhitespace();
      Expect(':');
      SkipWhitespace();
      const auto value = ParseString();
      if (!result.emplace(key, value).second)
        Fail("duplicate key: " + key);
      SkipWhitespace();
      if (Consume('}'))
        break;
      Expect(',');
      SkipWhitespace();
    }
    RequireEnd();
    return result;
  }

private:
  [[noreturn]] void Fail(const std::string &message) const {
    throw std::runtime_error("invalid builder config JSON at byte " +
                             std::to_string(position_) + ": " + message);
  }

  void SkipWhitespace() {
    while (position_ < contents_.size() &&
           std::isspace(static_cast<unsigned char>(contents_[position_])) != 0)
      ++position_;
  }

  bool Consume(char expected) {
    if (position_ >= contents_.size() || contents_[position_] != expected)
      return false;
    ++position_;
    return true;
  }

  void Expect(char expected) {
    if (!Consume(expected))
      Fail(std::string("expected '") + expected + "'");
  }

  static unsigned HexDigit(char character) {
    if (character >= '0' && character <= '9')
      return static_cast<unsigned>(character - '0');
    if (character >= 'a' && character <= 'f')
      return static_cast<unsigned>(character - 'a' + 10);
    if (character >= 'A' && character <= 'F')
      return static_cast<unsigned>(character - 'A' + 10);
    return 16;
  }

  void AppendUnicode(std::string &result) {
    unsigned codepoint = 0;
    for (int index = 0; index < 4; ++index) {
      if (position_ >= contents_.size())
        Fail("incomplete Unicode escape");
      const auto digit = HexDigit(contents_[position_++]);
      if (digit >= 16)
        Fail("invalid Unicode escape");
      codepoint = codepoint * 16 + digit;
    }
    if (codepoint >= 0xd800 && codepoint <= 0xdfff)
      Fail("UTF-16 surrogate escapes are not supported");
    if (codepoint <= 0x7f) {
      result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
      result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
      result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
      result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
      result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
  }

  std::string ParseString() {
    Expect('"');
    std::string result;
    while (position_ < contents_.size()) {
      const char character = contents_[position_++];
      if (character == '"')
        return result;
      if (static_cast<unsigned char>(character) < 0x20)
        Fail("unescaped control character");
      if (character != '\\') {
        result.push_back(character);
        continue;
      }
      if (position_ >= contents_.size())
        Fail("incomplete escape sequence");
      switch (contents_[position_++]) {
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      case 'u': AppendUnicode(result); break;
      default: Fail("unsupported escape sequence");
      }
    }
    Fail("unterminated string");
  }

  void RequireEnd() {
    SkipWhitespace();
    if (position_ != contents_.size())
      Fail("trailing content");
  }

  std::string_view contents_;
  std::size_t position_ = 0;
};

std::string JsonStringField(std::string_view object, std::string_view key) {
  const std::regex expression(
      "\"" + std::string(key) +
      R"("\s*:\s*("(?:\\.|[^"\\])*"))");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(object.begin(), object.end(), match, expression))
    throw std::runtime_error("compile command is missing string field: " +
                             std::string(key));
  return JsonStringObjectParser("{\"value\":" + match[1].str() + "}")
      .Parse()
      .at("value");
}

std::vector<std::string> JsonArrayObjects(std::string_view contents) {
  std::vector<std::string> objects;
  bool quoted = false;
  bool escaped = false;
  std::size_t depth = 0;
  std::size_t object_begin = std::string_view::npos;
  for (std::size_t index = 0; index < contents.size(); ++index) {
    const char character = contents[index];
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (character == '\\')
        escaped = true;
      else if (character == '"')
        quoted = false;
      continue;
    }
    if (character == '"') {
      quoted = true;
      continue;
    }
    if (character == '{') {
      if (depth++ == 0)
        object_begin = index;
    } else if (character == '}') {
      if (depth == 0)
        throw std::runtime_error("invalid compile_commands.json");
      if (--depth == 0) {
        objects.emplace_back(contents.substr(object_begin,
                                             index - object_begin + 1));
        object_begin = std::string_view::npos;
      }
    }
  }
  if (quoted || depth != 0)
    throw std::runtime_error("invalid compile_commands.json");
  return objects;
}

struct AuditCompilationDatabase {
  std::vector<fs::path> files;
  fs::path directory;
};

AuditCompilationDatabase PrepareAuditCompilationDatabase(
    const fs::path &repo_root, const fs::path &build_dir) {
  struct SelectedEntry {
    int score;
    std::string json;
  };
  std::map<fs::path, SelectedEntry> selected;
  const auto database =
      JsonArrayObjects(ReadFile(build_dir / "compile_commands.json"));
  const std::array<fs::path, 3> prefixes = {
      repo_root / "src/d3d12", repo_root / "src/dxmt",
      repo_root / "src/winemetal4"};
  for (const auto &entry : database) {
    const fs::path directory = JsonStringField(entry, "directory");
    fs::path path = JsonStringField(entry, "file");
    if (!path.is_absolute())
      path = directory / path;
    std::error_code error;
    path = fs::weakly_canonical(path, error);
    if (error || !fs::is_regular_file(path))
      continue;
    if (!std::any_of(prefixes.begin(), prefixes.end(),
                     [&path](const auto &prefix) {
                       return IsPathWithin(path, prefix);
                     }))
      continue;
    int score = 0;
    if (entry.find("DXMT_DX12_METAL4=1") != std::string::npos)
      score += 100;
    if (IsPathWithin(path, repo_root / "src/winemetal4"))
      score += 50;
    // Tie-break on the *target*, not on the compiler's name.
    //
    // Meson emits one entry per (source, target), so every source that a
    // host-native test target also compiles -- d3d12_copy_footprint.cpp,
    // d3d12_subresource_geometry.cpp, d3d12_tile_copy_plan.cpp,
    // d3d12_tile_mapping.cpp, d3d12_indirect_topology.cpp, dxmt_format.cpp --
    // appears twice: once for the shipped Windows DLL through llvm-mingw, once
    // for the macOS test binary through /usr/bin/clang++.  Preferring whichever
    // command mentioned "clang" picked the host entry, and the audit's
    // clang-tidy is the managed llvm-mingw one, which has no implicit macOS
    // sysroot: replaying a host command line made <type_traits>, <cstdint> and
    // <cstddef> unreachable, so each of those units died on a
    // clang-diagnostic-error at its first standard include and was never
    // analysed at all.  The only host command lines the audit can replay are
    // the src/winemetal4/unix ones, because that is where run_tidy adds
    // -isysroot.
    const bool host_command = entry.find("mingw32") == std::string::npos;
    const bool host_is_replayable =
        IsPathWithin(path, repo_root / "src/winemetal4/unix");
    if (host_command == host_is_replayable)
      score += 10;
    const auto current = selected.find(path);
    if (current == selected.end() || score > current->second.score)
      selected[path] = {score, entry};
  }

  AuditCompilationDatabase result;
  result.directory = build_dir / "dxmt-audit";
  fs::create_directories(result.directory);
  std::ostringstream filtered;
  filtered << "[\n";
  bool first = true;
  for (const auto &[path, entry] : selected) {
    if (!first)
      filtered << ",\n";
    first = false;
    filtered << "  " << entry.json;
    result.files.push_back(path);
  }
  filtered << "\n]\n";
  WriteFileAtomic(result.directory / "compile_commands.json", filtered.str());
  return result;
}

// compile_commands.json stores one shell-quoted string, not an argv array, so
// it has to be taken apart the way a shell would before any argument can be
// swapped out.
std::vector<std::string> SplitShellCommand(std::string_view command) {
  std::vector<std::string> arguments;
  std::string current;
  bool started = false;
  char quote = '\0';
  for (std::size_t index = 0; index < command.size(); ++index) {
    const char character = command[index];
    if (quote != '\0') {
      if (character == quote)
        quote = '\0';
      else if (character == '\\' && quote == '"' && index + 1 < command.size())
        current.push_back(command[++index]);
      else
        current.push_back(character);
      continue;
    }
    if (character == '\'' || character == '"') {
      quote = character;
      started = true;
      continue;
    }
    if (character == '\\' && index + 1 < command.size()) {
      current.push_back(command[++index]);
      started = true;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(character))) {
      if (started)
        arguments.push_back(current);
      current.clear();
      started = false;
      continue;
    }
    current.push_back(character);
    started = true;
  }
  if (started)
    arguments.push_back(current);
  return arguments;
}

// Rule: every src/d3d12/*.hpp must compile as its own translation unit.
//
// Splitting the queue into 163 headers only buys anything if a header can be
// included by somebody other than d3d12_command_queue.cpp -- that unit includes
// practically everything, so a header that forgot an include still compiles
// there and the omission surfaces only when the next module tries to reuse it.
// The probe is therefore behavioural: synthesise a one-line unit that includes
// nothing but the header, compile it with the queue's own command line and
// -fsyntax-only, and require zero errors.
//
// The capability boundary below is measured, not assumed, and matters because
// a green result here is easily misread as "this header was fully checked":
//
//   * Caught: any non-template use of a name whose declaring header is missing,
//     and a call inside a *template* body whose arguments do not depend on a
//     template parameter -- unqualified lookup for those happens at definition
//     time. Removing d3d12_sampler.hpp from d3d12_bindless_root_offsets.hpp is
//     reported straight away (no arguments to CreateD3D12StaticSampler depend
//     on a template parameter).
//
//   * NOT caught: a call inside a template body whose arguments *do* depend on
//     a template parameter. Lookup is deferred to instantiation, and a unit
//     that only includes the header never instantiates anything, so this rule
//     is green on that class by construction. The real
//     d3d12_replay_binding_encode.hpp / EncodeNativeArgumentTables defect was
//     exactly this shape. Only a unit that actually instantiates the template
//     can see it, which is what the whole-directory clang-tidy scan below
//     provides: it compiles every src/d3d12/*.cpp, and a transitive
//     instantiation counts, so a template reached from any non-queue unit is
//     covered without anyone enumerating call sites. The residual gap is a
//     template header reached from d3d12_command_queue.cpp and nothing else.
std::vector<std::string> AuditHeaderSelfContainmentViolations(
    const fs::path &repo_root, const fs::path &audit_database,
    const std::map<std::string, std::string> &environment, std::size_t jobs) {
  std::vector<std::string> violations;
  const auto entries =
      JsonArrayObjects(ReadFile(audit_database / "compile_commands.json"));
  const std::string queue_source = "d3d12_command_queue.cpp";
  std::string selected;
  fs::path working_directory;
  for (const auto &entry : entries) {
    const auto file = JsonStringField(entry, "file");
    if (file.size() < queue_source.size() || !file.ends_with(queue_source))
      continue;
    selected = JsonStringField(entry, "command");
    working_directory = JsonStringField(entry, "directory");
    break;
  }
  if (selected.empty())
    return {"the audit compilation database has no entry for " + queue_source +
            "; header self-containment cannot be verified"};

  auto arguments = SplitShellCommand(selected);
  std::vector<std::string> base;
  std::size_t source_index = 0;
  bool skip_value = false;
  for (const auto &argument : arguments) {
    if (skip_value) {
      skip_value = false;
      continue;
    }
    if (argument == "-o" || argument == "-MF" || argument == "-MQ" ||
        argument == "-MT") {
      skip_value = true;
      continue;
    }
    if (argument == "-MD" || argument == "-MMD")
      continue;
    if (argument.ends_with(queue_source))
      source_index = base.size();
    base.push_back(argument);
  }
  if (source_index == 0)
    return {"the compile command for " + queue_source +
            " does not name its own source; header self-containment cannot be "
            "verified"};
  // Meson hands out -fdiagnostics-color=always; the escape codes would end up
  // inside the reported policy string and in the JSON report.
  base.push_back("-fno-diagnostics-color");
  base.push_back("-fsyntax-only");

  const auto scratch = audit_database / "self-contained";
  fs::create_directories(scratch);
  std::vector<fs::path> headers;
  for (const auto &entry : fs::directory_iterator(repo_root / "src/d3d12")) {
    if (entry.is_regular_file() && entry.path().extension() == ".hpp")
      headers.push_back(entry.path());
  }
  std::sort(headers.begin(), headers.end());

  struct SelfContainmentResult {
    std::string header;
    CommandResult result;
  };
  auto check = [&](const fs::path &header) {
    const auto name = header.filename().string();
    const auto unit = scratch / (header.stem().string() + ".cpp");
    WriteFileAtomic(unit, "#include \"" + name + "\"\n");
    auto command = base;
    command[source_index] = unit.string();
    return SelfContainmentResult{
        name, RunCommand(command, environment, true, working_directory)};
  };
  std::vector<std::future<SelfContainmentResult>> pending;
  std::vector<SelfContainmentResult> results;
  for (const auto &header : headers) {
    pending.push_back(std::async(std::launch::async, check, header));
    if (pending.size() >= jobs) {
      for (auto &future : pending)
        results.push_back(future.get());
      pending.clear();
    }
  }
  for (auto &future : pending)
    results.push_back(future.get());

  for (const auto &result : results) {
    if (result.result.status == 0)
      continue;
    std::string detail;
    std::istringstream lines(result.result.output);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.find("error:") == std::string::npos)
        continue;
      const auto begin = line.find_first_not_of(" \t\r\n");
      const auto end = line.find_last_not_of(" \t\r\n");
      detail = begin == std::string::npos
                   ? line
                   : line.substr(begin, end - begin + 1);
      break;
    }
    violations.push_back("src/d3d12/" + result.header +
                         " is not self-contained: " +
                         (detail.empty() ? "compilation failed" : detail));
  }
  std::sort(violations.begin(), violations.end());
  return violations;
}

struct AuditDiagnostic {
  std::string path;
  std::size_t line = 0;
  std::size_t column = 0;
  std::string level;
  std::string message;
  std::string check;
  // How many translation units reported this exact location.  Reported for
  // information -- it measures how far a header reaches -- and deliberately
  // never used as a threshold.
  std::size_t reporting_units = 1;

  std::string Fingerprint() const {
    return check + "|" + path + "|" + message;
  }

  std::tuple<const std::string &, std::size_t, std::size_t,
             const std::string &, const std::string &>
  Location() const {
    return std::tie(path, line, column, check, message);
  }
};

// One source location is one diagnostic, however many units saw it.
//
// clang-tidy analyses a translation unit at a time, so a finding inside a
// header comes back once per unit that includes the header: three lines in
// src/dxmt/dxmt_context.hpp produced 402 of the 664 reports in a 175-unit deep
// scan.  Left uncollapsed that multiplier defeats the "no new diagnostics"
// gate for headers exactly where it matters most -- one new line in a widely
// included header buries every other finding under hundreds of copies of
// itself, and the reviewer reading the tail sees noise instead of signal.
// Collapse to unique (path, line, column, check, message) and keep the
// multiplicity as reporting_units.
std::vector<AuditDiagnostic>
DeduplicateAuditDiagnostics(std::vector<AuditDiagnostic> diagnostics) {
  std::sort(diagnostics.begin(), diagnostics.end(),
            [](const AuditDiagnostic &left, const AuditDiagnostic &right) {
              return left.Location() < right.Location();
            });
  std::vector<AuditDiagnostic> unique;
  unique.reserve(diagnostics.size());
  for (auto &diagnostic : diagnostics) {
    if (!unique.empty() && unique.back().Location() == diagnostic.Location()) {
      ++unique.back().reporting_units;
      continue;
    }
    unique.push_back(std::move(diagnostic));
  }
  return unique;
}

struct AuditTidyOutput {
  fs::path path;
  CommandResult result;
};

bool AuditDiagnosticMayBeBaselinedImpl(std::string_view path,
                                       std::string_view check) {
  if (path == "include/dxmt_d3d12_submission_model.hpp" ||
      path == "src/d3d12/d3d12_submission_model.cpp" ||
      path == "src/d3d12/d3d12_submission_model_header.cpp" ||
      path == "tools/audit/d3d12_submission_model_spec.cpp" ||
      path == "tools/audit/d3d12_copy_geometry_spec.cpp")
    return false;
  if (check.find("thread-safety") != std::string_view::npos)
    return false;
  return true;
}

std::vector<std::string> RequiredAuditChecks(bool model, bool deep) {
  std::vector<std::string> checks = {
      "bugprone-dangling-handle",
      "bugprone-exception-escape",
      "bugprone-sizeof-expression",
      "bugprone-unused-raii",
      "bugprone-use-after-move",
      "performance-noexcept-move-constructor",
  };
  if (model) {
    const std::array model_checks = {
        "bugprone-narrowing-conversions",
        "cppcoreguidelines-pro-bounds-array-to-pointer-decay",
        "cppcoreguidelines-pro-bounds-pointer-arithmetic",
        "cppcoreguidelines-pro-type-reinterpret-cast",
        "readability-function-cognitive-complexity",
        "readability-function-size",
    };
    checks.insert(checks.end(), model_checks.begin(), model_checks.end());
  }
  if (deep) {
    const std::array deep_checks = {
        "clang-analyzer-core.CallAndMessage",
        "clang-analyzer-cplusplus.Move",
        "clang-analyzer-cplusplus.NewDelete",
        "clang-analyzer-cplusplus.NewDeleteLeaks",
        "clang-analyzer-cplusplus.SmartPtrModeling",
        "clang-analyzer-deadcode.DeadStores",
        "clang-analyzer-unix.Malloc",
        "clang-analyzer-unix.MismatchedDeallocator",
    };
    checks.insert(checks.end(), deep_checks.begin(), deep_checks.end());
  }
  return checks;
}

std::vector<std::string>
ValidateAuditChecks(const fs::path &clang_tidy, const fs::path &config,
                    bool model, bool deep,
                    const std::map<std::string, std::string> &environment,
                    const fs::path &repo_root) {
  const auto result = RunCommand(
      {clang_tidy.string(), "--list-checks",
       "--config-file=" + config.string()},
      environment, true, repo_root);
  RequireSuccess(result, "list clang-tidy checks");
  const auto required = RequiredAuditChecks(model, deep);
  std::vector<std::string> missing;
  for (const auto &check : required) {
    if (result.output.find("\n    " + check + "\n") == std::string::npos &&
        result.output.find("\n    " + check + "\r\n") ==
            std::string::npos)
      missing.push_back(check);
  }
  if (!missing.empty()) {
    std::ostringstream message;
    message << "managed clang-tidy does not provide required checks:";
    for (const auto &check : missing)
      message << ' ' << check;
    throw std::runtime_error(message.str());
  }
  return required;
}

std::optional<AuditDiagnostic>
ParseAuditDiagnostic(const fs::path &repo_root, std::string_view line) {
  static const std::regex expression(
      R"(^(.+?):([0-9]+):([0-9]+): (warning|error): (.*?) \[([^\]]+)\]$)");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_match(line.begin(), line.end(), match, expression))
    return std::nullopt;
  fs::path path = match[1].str();
  if (!path.is_absolute())
    path = repo_root / path;
  if (!fs::exists(path)) {
    const auto value = path.generic_string();
    for (const auto directory : {"src", "include"}) {
      const auto marker = "/" + std::string(directory) + "/";
      const auto position = value.find(marker);
      if (position == std::string::npos)
        continue;
      const auto candidate =
          repo_root / directory / value.substr(position + marker.size());
      if (fs::exists(candidate)) {
        path = candidate;
        break;
      }
    }
  }
  std::error_code error;
  const auto relative = fs::relative(path, repo_root, error);
  return AuditDiagnostic{
      error ? path.generic_string() : relative.generic_string(),
      static_cast<std::size_t>(std::stoull(match[2].str())),
      static_cast<std::size_t>(std::stoull(match[3].str())),
      match[4].str(),
      match[5].str(),
      match[6].str(),
  };
}

// `count` is a number of *unique source locations* sharing this fingerprint --
// not a number of clang-tidy reports.  The fingerprint deliberately omits the
// line and column so that shifting code does not invalidate an accepted
// finding, so a fingerprint can still cover several locations; what it can no
// longer do is grow with the number of translation units that happen to
// include the header the finding lives in.  (Both baselines were empty when
// the semantics changed, so no entry was ever written under the old meaning
// and there is nothing to migrate.)
struct AuditBaselineEntry {
  std::size_t count = 0;
  std::string reason;
};

std::map<std::string, AuditBaselineEntry>
LoadAuditBaseline(const fs::path &path) {
  std::map<std::string, AuditBaselineEntry> result;
  if (!fs::is_regular_file(path))
    return result;
  const auto source = ReadFile(path);
  if (source.find("\"diagnostics\"") == std::string::npos)
    throw std::runtime_error("invalid audit baseline: " + path.string());
  if (source.find("\"schema\": 1") != std::string::npos) {
    static const std::regex legacy_entry_expression(
        R"REGEX("((?:\\.|[^"\\])*)"\s*:\s*([0-9]+))REGEX");
    for (std::sregex_iterator match(source.begin(), source.end(),
                                    legacy_entry_expression),
         end;
         match != end; ++match) {
      if ((*match)[1].str() == "schema")
        continue;
      const auto key =
          JsonStringObjectParser("{\"value\":\"" + (*match)[1].str() + "\"}")
              .Parse()
              .at("value");
      result[key] = {
          static_cast<std::size_t>(std::stoull((*match)[2].str())),
          "legacy schema-1 baseline; review before the next update"};
    }
    return result;
  }
  if (source.find("\"schema\": 2") == std::string::npos)
    throw std::runtime_error("unsupported audit baseline schema: " +
                             path.string());

  static const std::regex entry_expression(
      R"REGEX("((?:\\.|[^"\\])*)"\s*:\s*\{\s*"count"\s*:\s*([0-9]+)\s*,\s*"reason"\s*:\s*"((?:\\.|[^"\\])*)"\s*\})REGEX");
  for (std::sregex_iterator match(source.begin(), source.end(),
                                  entry_expression),
       end;
       match != end; ++match) {
    const auto fields =
        JsonStringObjectParser("{\"key\":\"" + (*match)[1].str() +
                               "\",\"reason\":\"" + (*match)[3].str() + "\"}")
            .Parse();
    result[fields.at("key")] = {
        static_cast<std::size_t>(std::stoull((*match)[2].str())),
        fields.at("reason")};
  }
  return result;
}

void WriteAuditBaseline(const fs::path &path,
                        const std::map<std::string, std::size_t> &counts,
                        std::string_view reason) {
  if (reason.empty())
    throw std::runtime_error(
        "audit baseline updates require a non-empty review reason");
  std::ostringstream output;
  output << "{\n  \"schema\": 2,\n  \"diagnostics\": {";
  bool first = true;
  for (const auto &[fingerprint, count] : counts) {
    output << (first ? "\n" : ",\n") << "    \"" << JsonEscape(fingerprint)
           << "\": {\"count\": " << count << ", \"reason\": \""
           << JsonEscape(reason) << "\"}";
    first = false;
  }
  if (!first)
    output << '\n';
  output << "  }\n}\n";
  WriteFileAtomic(path, output.str());
}

std::map<std::string, std::string> ReadProperties(const fs::path &path) {
  if (!fs::is_regular_file(path))
    return {};
  return testing::ParseProperties(ReadFile(path));
}

class FileLock {
public:
  explicit FileLock(const fs::path &path) {
    fs::create_directories(path.parent_path());
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
      throw std::system_error(GetLastError(), std::system_category(),
                              "CreateFileW lock");
    OVERLAPPED overlapped = {};
    if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                    &overlapped)) {
      const auto error = GetLastError();
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      throw std::system_error(error, std::system_category(), "LockFileEx");
    }
#else
    descriptor_ = open(path.c_str(), O_CREAT | O_RDWR, 0644);
    if (descriptor_ < 0)
      throw std::system_error(errno, std::generic_category(), "open lock");
    if (flock(descriptor_, LOCK_EX) != 0) {
      const auto error = errno;
      close(descriptor_);
      throw std::system_error(error, std::generic_category(), "flock");
    }
#endif
  }

  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;

  ~FileLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped = {};
      UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
      CloseHandle(handle_);
    }
#else
    if (descriptor_ >= 0) {
      flock(descriptor_, LOCK_UN);
      close(descriptor_);
    }
#endif
  }

private:
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int descriptor_ = -1;
#endif
};

std::string Trim(std::string value) {
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
    value.pop_back();
  return value;
}

// clang-tidy accepts an unknown check name in complete silence.  A typo, or a
// `#` line written inside the `Checks:` folded block scalar -- where YAML does
// not treat it as a comment but folds it into the check-name string, swallowing
// the neighbouring entries -- disables checks without a word of complaint, and
// the audit then reports a clean tree from a scanner that is quietly missing
// them.  Compare what each config asks for against what clang-tidy actually
// enables.  This deliberately reads the file rather than --dump-config, and
// deliberately does not strip `#`: reproducing clang-tidy's own literal view of
// the block scalar is the whole point.
std::vector<std::string>
ValidateAuditCheckConfig(const fs::path &clang_tidy, const fs::path &config,
                         const std::map<std::string, std::string> &environment,
                         const fs::path &repo_root) {
  const auto trim = [](std::string value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])))
      ++begin;
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])))
      --end;
    return value.substr(begin, end - begin);
  };
  std::vector<std::string> errors;
  const auto label = config.filename().string();
  if (!fs::is_regular_file(config)) {
    errors.push_back("audit config is missing: " + config.string());
    return errors;
  }
  const auto source = ReadFile(config);
  std::string checks;
  bool found = false;
  {
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (!found) {
        if (!line.starts_with("Checks:"))
          continue;
        found = true;
        const auto value = trim(line.substr(std::string_view("Checks:").size()));
        if (value != ">" && value != "|" && value != ">-" && value != "|-")
          checks += " " + value;
        continue;
      }
      if (line.empty())
        continue;
      if (!std::isspace(static_cast<unsigned char>(line.front())))
        break;
      checks += " " + trim(line);
    }
  }
  if (!found) {
    errors.push_back("audit config has no Checks entry: " + label);
    return errors;
  }
  const auto listed =
      RunCommand({clang_tidy.string(), "--list-checks",
                  "--config-file=" + config.string()},
                 environment, true, repo_root);
  RequireSuccess(listed, "list clang-tidy checks for " + label);
  std::vector<std::string> enabled;
  {
    std::istringstream stream(listed.output);
    std::string line;
    while (std::getline(stream, line)) {
      const auto name = trim(line);
      if (name.empty() || name.ends_with(':'))
        continue;
      enabled.push_back(name);
    }
  }
  if (enabled.empty()) {
    errors.push_back("clang-tidy enabled no checks at all for " + label);
    return errors;
  }
  std::size_t position = 0;
  while (position <= checks.size()) {
    const auto comma = checks.find(',', position);
    const auto piece = trim(checks.substr(
        position, comma == std::string::npos ? std::string::npos
                                             : comma - position));
    position = comma == std::string::npos ? checks.size() + 1 : comma + 1;
    if (piece.empty() || piece == "*" || piece == "-*")
      continue;
    const bool negated = piece.front() == '-';
    const auto name = negated ? piece.substr(1) : piece;
    if (name.empty() ||
        name.find_first_of(" \t#") != std::string::npos) {
      errors.push_back(
          "audit config entry is not a check name (a '#' line inside the "
          "folded Checks block is literal text, not a comment): " +
          label + ": " + piece);
      continue;
    }
    bool matched = false;
    if (name.back() == '*') {
      const auto prefix = name.substr(0, name.size() - 1);
      matched = std::any_of(enabled.begin(), enabled.end(),
                            [&](const std::string &candidate) {
                              return candidate.starts_with(prefix);
                            });
    } else {
      matched = std::find(enabled.begin(), enabled.end(), name) !=
                enabled.end();
    }
    if (!negated && !matched)
      errors.push_back("audit config names a check clang-tidy does not "
                       "enable: " +
                       label + ": " + name);
    if (negated && matched)
      errors.push_back("audit config disables a check that is still "
                       "enabled: " +
                       label + ": " + name);
  }
  return errors;
}

struct BuilderConfiguration {
  fs::path source;
  fs::path managed_root;
  std::string profile_namespace;
};

void ValidateProfileNamespace(std::string_view value) {
  if (value.empty())
    return;
  const fs::path path(value);
  if (path.is_absolute() || path.has_root_path())
    throw std::runtime_error("profile_namespace must be a relative path");
  for (const auto &component : path) {
    if (component.empty() || component == "." || component == "..")
      throw std::runtime_error(
          "profile_namespace contains an unsafe path component");
  }
}

std::string ResolveGitProfileNamespace(const fs::path &repo_root) {
  const auto symbolic = RunCommand(
      {"git", "-C", repo_root.string(), "symbolic-ref", "--quiet", "--short",
       "HEAD"},
      {}, true);
  if (symbolic.status == 0 && !Trim(symbolic.output).empty())
    return Trim(symbolic.output);

  const auto remote_refs = RunCommand(
      {"git", "-C", repo_root.string(), "for-each-ref",
       "--format=%(refname:short)", "--points-at=HEAD", "refs/remotes/origin"},
      {}, true);
  if (remote_refs.status == 0) {
    std::istringstream lines(remote_refs.output);
    std::vector<std::string> matches;
    for (std::string line; std::getline(lines, line);) {
      line = Trim(line);
      if (line.empty() || line == "origin/HEAD")
        continue;
      constexpr std::string_view prefix = "origin/";
      if (line.starts_with(prefix))
        line.erase(0, prefix.size());
      matches.push_back(std::move(line));
    }
    if (!matches.empty()) {
      std::sort(matches.begin(), matches.end());
      return matches.front();
    }
  }

  const auto commit = RunCommand(
      {"git", "-C", repo_root.string(), "rev-parse", "HEAD"}, {}, true);
  RequireSuccess(commit, "Git profile namespace resolution");
  const auto result = Trim(commit.output);
  if (result.empty())
    throw std::runtime_error("Git profile namespace resolved to an empty value");
  return result;
}

std::optional<fs::path> DiscoverConfigPath(
    const fs::path &repo_root,
    const std::optional<fs::path> &requested) {
  if (requested) {
    const auto path = fs::absolute(*requested).lexically_normal();
    if (!fs::is_regular_file(path))
      throw std::runtime_error("builder config does not exist: " + path.string());
    return path;
  }
  const auto local = repo_root / ".dxmt-builder/config.json";
  if (fs::is_regular_file(local))
    return fs::absolute(local).lexically_normal();
  return std::nullopt;
}

BuilderConfiguration LoadBuilderConfiguration(
    const fs::path &repo_root,
    const std::optional<fs::path> &requested) {
  BuilderConfiguration result;
  result.managed_root = repo_root / ".cache/managed";
  const auto config_path = DiscoverConfigPath(repo_root, requested);
  if (!config_path)
    return result;

  result.source = *config_path;
  const auto values =
      testing::ParseJsonStringObject(ReadFile(result.source));
  for (const auto &[name, value] : values) {
    if (name != "cache_root" && name != "profile_namespace")
      throw std::runtime_error("unknown builder config key: " + name);
  }
  if (const auto found = values.find("cache_root"); found != values.end()) {
    if (found->second.empty())
      throw std::runtime_error("cache_root must not be empty");
    const fs::path configured_root(found->second);
    result.managed_root = configured_root.is_absolute()
                              ? configured_root.lexically_normal()
                              : (repo_root / configured_root).lexically_normal();
  }
  if (const auto found = values.find("profile_namespace");
      found != values.end()) {
    if (found->second == "git")
      result.profile_namespace = ResolveGitProfileNamespace(repo_root);
    else if (found->second != "none")
      result.profile_namespace = found->second;
  }
  ValidateProfileNamespace(result.profile_namespace);
  return result;
}

std::string EnvironmentValue(std::string_view name,
                             std::string_view fallback = {}) {
  if (const char *value = std::getenv(std::string(name).c_str()))
    return value;
  return std::string(fallback);
}

bool EnvironmentFlag(std::string_view name) {
  const auto value = EnvironmentValue(name);
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool IsIncompleteCacheName(std::string_view name) {
  return name.starts_with(".tmp-") ||
         name.find(".incomplete-") != std::string_view::npos;
}

std::uintmax_t DirectorySize(const fs::path &root,
                             const std::optional<fs::path> &exclude = {}) {
  if (!fs::exists(root))
    return 0;
  std::uintmax_t total = 0;
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(
           root, fs::directory_options::skip_permission_denied, error), end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }
    if (exclude && IsPathWithin(iterator->path(), *exclude)) {
      if (iterator->is_directory())
        iterator.disable_recursion_pending();
      continue;
    }
    if (iterator->is_regular_file(error))
      total += iterator->file_size(error);
  }
  return total;
}

std::string HumanSize(std::uintmax_t bytes) {
  constexpr std::array<std::string_view, 5> units = {"B", "KiB", "MiB", "GiB", "TiB"};
  double size = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (size >= 1024.0 && unit + 1 < units.size()) {
    size /= 1024.0;
    ++unit;
  }
  std::ostringstream output;
  output.setf(std::ios::fixed);
  output.precision(unit == 0 ? 0 : 2);
  output << size << ' ' << units[unit];
  return output.str();
}

std::string DirectoryDigest(const fs::path &root) {
  if (!fs::is_directory(root))
    throw std::runtime_error("directory to hash does not exist: " + root.string());
  std::vector<fs::path> files;
  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_regular_file())
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  std::ostringstream manifest;
  for (const auto &file : files)
    manifest << fs::relative(file, root).generic_string() << '='
             << Sha256File(file) << '\n';
  return Sha256(manifest.str());
}

std::string MesonArray(std::initializer_list<fs::path> values) {
  std::string result = "[";
  for (const auto &value : values) {
    if (result.size() > 1)
      result += ", ";
    result += "'" + value.string() + "'";
  }
  result += "]";
  return result;
}

std::string Join(const std::vector<std::string> &values,
                 std::string_view separator) {
  std::ostringstream result;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0)
      result << separator;
    result << values[index];
  }
  return result.str();
}

struct ResolvedProfile {
  const Profile *profile = nullptr;
  fs::path root;
  fs::path build;
  fs::path install;
  fs::path stage;
  fs::path prefix;
  fs::path meta;
  fs::path cross_file;
  fs::path native_file;
  fs::path ccache;
  fs::path target_c;
  fs::path target_cpp;
  fs::path target_ar;
  fs::path target_strip;
  fs::path target_windres;
  fs::path native_c;
  fs::path native_cpp;
  fs::path native_llvm;
  fs::path windows_llvm;
  fs::path wine_root;
  std::string fingerprint;
};

class Driver {
public:
  Driver(fs::path repo_root, fs::path managed_root,
         std::string profile_namespace, fs::path config_path)
      : repo_root_(std::move(repo_root)), managed_root_(std::move(managed_root)),
        profile_namespace_(std::move(profile_namespace)),
        config_path_(std::move(config_path)) {}

  int Run(std::span<const std::string> arguments) {
    if (arguments.empty() || arguments.front() == "help" ||
        arguments.front() == "--help" || arguments.front() == "-h") {
      PrintUsage();
      return arguments.empty() ? 2 : 0;
    }
    if (arguments.front() == "bootstrap")
      return Bootstrap(arguments.subspan(1));
    if (arguments.front() == "configure")
      return ConfigureCommand(arguments.subspan(1));
    if (arguments.front() == "audit")
      return AuditCommand(arguments.subspan(1));
    if (arguments.front() == "build")
      return BuildCommand(arguments.subspan(1));
    if (arguments.front() == "test")
      return TestCommand(arguments.subspan(1));
    if (arguments.front() == "package")
      return PackageCommand(arguments.subspan(1));
    if (arguments.front() == "wine-exec")
      return WineExecCommand(arguments.subspan(1));
    if (arguments.front() == "install")
      return InstallCommand(arguments.subspan(1));
    if (arguments.front() == "restore-package")
      return RestorePackageCommand(arguments.subspan(1));
    if (arguments.front() == "config")
      return ConfigCommand(arguments.subspan(1));
    if (arguments.front() == "cache")
      return CacheCommand(arguments.subspan(1));
    if (arguments.front() == "internal")
      return InternalCommand(arguments.subspan(1));
    throw std::runtime_error("unknown command: " + arguments.front());
  }

private:
  void RecordTelemetry(
      std::string_view operation, std::string_view subject,
      std::string_view result,
      std::chrono::steady_clock::time_point started) const {
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream json;
    json << "{\"operation\":\"" << JsonEscape(operation)
         << "\",\"subject\":\"" << JsonEscape(subject)
         << "\",\"result\":\"" << JsonEscape(result)
         << "\",\"duration_ms\":" << duration << "}\n";
    WriteFileAtomic(managed_root_ / "telemetry" /
                        (std::to_string(timestamp) + "-" +
                        std::to_string(ProcessId()) + ".json"),
                    json.str());
  }

  void PrintUsage() const {
    std::cout
        << "usage: scripts/dxmt-builder [--config FILE] <command> [options]\n\n"
        << "commands:\n"
        << "  bootstrap [all|host|wine-x64|llvm-mingw|llvm-project|llvm-darwin-x64|llvm-win]...\n"
        << "  configure [--profile NAME]\n"
        << "  audit [--profile NAME] [--scope dx12-metal4] [--jobs N] [--policy-only] [--checker-fixtures-only] [--deep] [--sanitizers] [--no-cache] [--report PATH] [--update-baseline --baseline-reason TEXT]\n"
        << "  build [--profile NAME] <runtime|d3d9|d3d10|d3d11|d3d12|tests-*|benchmarks>...\n"
        << "  test [--profile NAME] [all|unit|integration|performance] [--suite NAME] [--test-args ARG]\n"
        << "  package [--profile NAME] windows-oracle [--dest PATH]\n"
        << "  wine-exec [--] <wine-args...>   # Meson/benchmark Wine launcher\n"
        << "  install [--profile NAME] [--component NAME] [--dest PATH]\n"
        << "  restore-package --source PATH --dest PATH --backup PATH\n"
        << "  config <cache-root|namespace|profile-path --profile NAME KIND>\n"
        << "  cache <status [--json]|verify|prune [--dry-run|--apply]|clean --profile NAME>\n";
  }

  fs::path ProfilesRoot() const {
    auto root = managed_root_ / "profiles";
    if (!profile_namespace_.empty())
      root /= profile_namespace_;
    return root;
  }

  fs::path ProfileRoot(std::string_view name) const {
    return ProfilesRoot() / name;
  }

  fs::path ProfileLockPath(std::string_view name) const {
    if (profile_namespace_.empty())
      return managed_root_ / "locks" / (std::string(name) + ".lock");
    return managed_root_ / "locks" /
           ("profile-" + Sha256(profile_namespace_).substr(0, 16) + "-" +
            std::string(name) + ".lock");
  }

  fs::path CcacheRoot() const {
    return testing::CcacheRoot(managed_root_, profile_namespace_);
  }

  int ConfigCommand(std::span<const std::string> arguments) const {
    if (arguments.empty())
      throw std::runtime_error("config requires a subcommand");
    if (arguments.front() == "cache-root") {
      if (arguments.size() != 1)
        throw std::runtime_error("config cache-root does not accept arguments");
      std::cout << managed_root_.string() << '\n';
      return 0;
    }
    if (arguments.front() == "namespace") {
      if (arguments.size() != 1)
        throw std::runtime_error("config namespace does not accept arguments");
      std::cout << profile_namespace_ << '\n';
      return 0;
    }
    if (arguments.front() == "profile-path") {
      std::string profile;
      std::string kind;
      for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--profile") {
          if (++index >= arguments.size())
            throw std::runtime_error("--profile requires a value");
          profile = arguments[index];
        } else if (arguments[index].starts_with("--profile=")) {
          profile = arguments[index].substr(std::string("--profile=").size());
        } else if (kind.empty()) {
          kind = arguments[index];
        } else {
          throw std::runtime_error("unexpected config profile-path argument: " +
                                   arguments[index]);
        }
      }
      if (FindProfile(profile) == nullptr)
        throw std::runtime_error("config profile-path requires a valid --profile");
      auto path = ProfileRoot(profile);
      if (kind == "root" || kind.empty()) {
      } else if (kind == "build") {
        path /= "build";
      } else if (kind == "install") {
        path /= "install";
      } else if (kind == "stage") {
        path /= "stage";
      } else if (kind == "prefix") {
        path /= "prefix";
      } else if (kind == "meta") {
        path /= "meta";
      } else {
        throw std::runtime_error("unknown profile path kind: " + kind);
      }
      std::cout << path.string() << '\n';
      return 0;
    }
    throw std::runtime_error("unknown config subcommand: " + arguments.front());
  }

  std::string ParseProfile(
      std::span<const std::string> arguments,
      std::vector<std::string> *remaining,
      // Clang is the default toolchain: GCC silently ignores every
      // -Wthread-safety contract in this codebase (capabilities, GUARDED_BY,
      // and the residency lock ordering), so building with it means those
      // invariants are only ever checked during an audit.  Clang enforces them
      // on every build, and it already caught real defects GCC missed
      // (missing returns after assert, dangling `this` captures).
      std::string_view default_profile = "llvm-mingw-x64-release") const {
    std::string profile = std::string(default_profile);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (arguments[index] == "--profile") {
        if (++index >= arguments.size())
          throw std::runtime_error("--profile requires a value");
        profile = arguments[index];
      } else if (arguments[index].starts_with("--profile=")) {
        profile = arguments[index].substr(std::string("--profile=").size());
      } else {
        remaining->push_back(arguments[index]);
      }
    }
    if (FindProfile(profile) == nullptr)
      throw std::runtime_error("unknown profile: " + profile);
    return profile;
  }

  Environment BuildEnvironment() const {
    const auto ccache_root = CcacheRoot();
    const auto config = ccache_root / "ccache.conf";
    Environment environment = {
        {"CCACHE_CONFIGPATH", config.string()},
        {"CCACHE_DIR", (ccache_root / "data").string()},
        {"CCACHE_BASEDIR", repo_root_.string()},
        {"DXMT_REPO_ROOT", repo_root_.string()},
        {"DXMT_MANAGED_CACHE_ROOT", managed_root_.string()},
        {"DXMT_APITRACE_CAS", (managed_root_ / "cas/apitrace").string()},
    };
    return environment;
  }

  void EnsureManagedLayout() const {
    const auto ccache_root = CcacheRoot();
    for (const auto &path : {
             managed_root_ / "profiles", ccache_root / "data",
             managed_root_ / "cas/metal", managed_root_ / "cas/apitrace",
             managed_root_ / "deps", managed_root_ / "artifacts",
             managed_root_ / "locks", managed_root_ / "telemetry"})
      fs::create_directories(path);
    std::ostringstream config;
    config << "cache_dir = " << (ccache_root / "data").string() << '\n'
           << "base_dir = " << repo_root_.string() << '\n'
           << "compression = true\n"
           << "compiler_check = content\n"
           << "hash_dir = false\n";
    WriteFileAtomic(ccache_root / "ccache.conf", config.str());
  }

  fs::path ResolveWine(const Profile &profile) const {
    if (!profile.cross)
      return {};
    if (const char *path = std::getenv("DXMT_WINE_ROOT")) {
      if (fs::is_directory(path))
        return fs::canonical(path);
    }
    const auto deps = managed_root_ / "deps";
    const auto prefix = "wine-x86_64-";
    std::vector<fs::path> matches;
    if (fs::is_directory(deps)) {
      for (const auto &entry : fs::directory_iterator(deps)) {
        if (entry.is_directory() &&
            entry.path().filename().string().starts_with(prefix) &&
            fs::is_regular_file(entry.path() / ".dxmt-builder-dependency"))
          matches.push_back(entry.path());
      }
    }
    if (!matches.empty()) {
      std::sort(matches.begin(), matches.end());
      return fs::canonical(matches.back());
    }
    const auto sibling = repo_root_.parent_path().parent_path() / "wine-proton-macos/install";
    if (fs::is_directory(sibling))
      return fs::canonical(sibling);
    throw std::runtime_error(
        "no managed Wine development cache is available; run 'scripts/dxmt-builder bootstrap wine-x64' or set DXMT_WINE_ROOT");
  }

  fs::path ResolveNativeLlvm() const {
    if (const char *path = std::getenv("DXMT_NATIVE_LLVM_PATH")) {
      if (fs::is_directory(path))
        return fs::absolute(path).lexically_normal();
    }
    const auto deps = managed_root_ / "deps";
    constexpr std::string_view arch = "x86_64";
    std::vector<fs::path> matches;
    std::vector<std::string> seen;
    if (fs::is_directory(deps)) {
      for (const auto &entry : fs::directory_iterator(deps)) {
        const auto name = entry.path().filename().string();
        if (!entry.is_directory() || !name.starts_with("llvm-darwin-"))
          continue;
        seen.push_back(name);
        if (IsIncompleteCacheName(name))
          continue;
        if (name.ends_with("-" + std::string(arch)) &&
            fs::is_regular_file(entry.path() / ".dxmt-builder-dependency") &&
            fs::is_regular_file(entry.path() / "lib/cmake/llvm/LLVMConfig.cmake"))
          matches.push_back(entry.path());
      }
    }
    if (!matches.empty()) {
      std::sort(matches.begin(), matches.end());
      return fs::canonical(matches.back());
    }
    const fs::path homebrew = "/usr/local/opt/llvm@15";
    if (fs::is_directory(homebrew))
      // Keep the stable Homebrew opt path: the main Meson project uses this
      // prefix to add the matching zstd and unwind static dependencies.
      return homebrew;
    std::ostringstream detail;
    detail << "LLVM 15 native libraries were not found for " << arch
           << " under " << deps.string();
    if (seen.empty()) {
      detail << " (no llvm-darwin-* directories)";
    } else {
      detail << " (found:";
      for (const auto &name : seen)
        detail << ' ' << name;
      detail << ')';
    }
    detail << "; run 'scripts/dxmt-builder bootstrap llvm-darwin-x64'";
    throw std::runtime_error(detail.str());
  }

  std::vector<std::string> NativeLlvmLinkArgs(const fs::path &prefix) const {
    static constexpr std::array<std::string_view, 2> headers = {
        "llvm/ADT/StringRef.h", "llvm/IR/Constants.h"};
    static constexpr std::array<std::string_view, 7> libraries = {
        "LLVMBitReader", "LLVMCore", "LLVMRemarks", "LLVMBinaryFormat",
        "LLVMBitstreamReader", "LLVMSupport", "LLVMDemangle"};
    std::vector<std::string> arguments;
    for (const auto header : headers) {
      const auto include = prefix / "include" / header;
      if (!fs::is_regular_file(include))
        throw std::runtime_error("native LLVM prefix is incomplete: missing " +
                                 include.string());
    }
    for (const auto library : libraries) {
      const auto archive =
          prefix / "lib" / ("lib" + std::string(library) + ".a");
      if (!fs::is_regular_file(archive))
        throw std::runtime_error("native LLVM prefix is incomplete: missing " +
                                 archive.string());
      arguments.emplace_back("-l" + std::string(library));
    }
    arguments.insert(arguments.end(), {"-lm", "-lz"});
    const auto prefix_string = prefix.string();
    if (prefix_string.starts_with("/usr/local/opt/") ||
        prefix_string.starts_with("/opt/homebrew/opt/"))
      arguments.emplace_back("-lzstd");
    arguments.insert(arguments.end(), {"-lcurses", "-lxml2"});
    return arguments;
  }

  fs::path ResolveCompiler(const Profile &profile, std::string_view executable) const {
    if (profile.compiler_family == "llvm-mingw") {
      const auto deps = managed_root_ / "deps";
      for (const auto &entry : fs::directory_iterator(deps)) {
        const auto candidate = entry.path() / "bin" / executable;
        if (entry.path().filename().string().starts_with("llvm-mingw-") &&
            fs::is_regular_file(entry.path() / ".dxmt-builder-dependency") &&
            IsExecutableFile(candidate))
          // llvm-mingw dispatches from the invoked symlink name. Resolving it
          // to clang-target-wrapper.sh loses the target triple.
          return fs::absolute(candidate).lexically_normal();
      }
    }
    return RequireExecutable(executable);
  }

  fs::path ResolveWindowsLlvm() const {
    if (const char *path = std::getenv("DXMT_WINDOWS_LLVM_PATH")) {
      if (fs::is_directory(path))
        return fs::absolute(path).lexically_normal();
    }
    std::vector<fs::path> matches;
    const auto deps = managed_root_ / "deps";
    if (fs::is_directory(deps)) {
      for (const auto &entry : fs::directory_iterator(deps)) {
        if (entry.is_directory() &&
            entry.path().filename().string().starts_with("llvm-win-") &&
            fs::is_regular_file(entry.path() / ".dxmt-builder-dependency"))
          matches.push_back(entry.path());
      }
    }
    if (matches.empty())
      throw std::runtime_error(
          "managed LLVM Windows libraries are missing; run 'scripts/dxmt-builder bootstrap llvm-win'");
    std::sort(matches.begin(), matches.end());
    return fs::canonical(matches.back());
  }

  fs::path ResolveClangTidy() const {
    if (const char *path = std::getenv("DXMT_CLANG_TIDY")) {
      fs::path candidate = path;
      if (!candidate.is_absolute()) {
        if (const auto resolved = FindExecutable(path))
          candidate = *resolved;
      }
      if (IsExecutableFile(candidate))
        return fs::absolute(candidate).lexically_normal();
      throw std::runtime_error("DXMT_CLANG_TIDY is not executable");
    }
    if (const auto system = FindExecutable("clang-tidy"))
      return *system;

    const auto deps = managed_root_ / "deps";
    std::vector<fs::path> matches;
    if (fs::is_directory(deps)) {
      for (const auto &entry : fs::directory_iterator(deps)) {
        if (!entry.is_directory() ||
            !entry.path().filename().string().starts_with("llvm-mingw-"))
          continue;
        const auto candidate = entry.path() / "bin/clang-tidy";
        if (IsExecutableFile(candidate))
          matches.push_back(candidate);
      }
    }
    if (!matches.empty()) {
      std::sort(matches.begin(), matches.end());
      return fs::absolute(matches.back()).lexically_normal();
    }
    throw std::runtime_error(
        "clang-tidy was not found; run 'scripts/dxmt-builder bootstrap llvm-mingw' "
        "or set DXMT_CLANG_TIDY");
  }

  ResolvedProfile Resolve(std::string_view name) const {
    EnsureManagedLayout();
    const auto *profile = FindProfile(name);
    if (profile == nullptr)
      throw std::runtime_error("unknown profile: " + std::string(name));

    ResolvedProfile result;
    result.profile = profile;
    result.root = ProfileRoot(profile->name);
    result.build = result.root / "build";
    result.install = result.root / "install";
    result.stage = result.root / "stage";
    result.prefix = result.root / "prefix";
    result.meta = result.root / "meta";
    result.cross_file = result.meta / "cross.ini";
    result.native_file = result.meta / "native.ini";
    result.ccache = RequireExecutable("ccache");
    result.native_c = RequireExecutable("clang");
    result.native_cpp = RequireExecutable("clang++");
    result.native_llvm = ResolveNativeLlvm();
    if (profile->airconv_windows)
      result.windows_llvm = ResolveWindowsLlvm();
    result.wine_root = ResolveWine(*profile);

    if (profile->cross) {
      std::string prefix;
      if (profile->target_arch == "x64")
        prefix = "x86_64-w64-mingw32-";
      else if (profile->target_arch == "x86")
        prefix = "i686-w64-mingw32-";
      else
        throw std::runtime_error("unsupported cross target architecture: " +
                                 std::string(profile->target_arch));
      result.target_c = ResolveCompiler(*profile, prefix + "gcc");
      result.target_cpp = ResolveCompiler(*profile, prefix + "g++");
      result.target_ar = ResolveCompiler(*profile, prefix + "ar");
      result.target_strip = ResolveCompiler(*profile, prefix + "strip");
      result.target_windres = ResolveCompiler(*profile, prefix + "windres");
    }

    const bool apitrace = EnvironmentFlag("DXMT_BUILDER_APITRACE");
    std::ostringstream identity;
    identity << "schema=1\nname=" << profile->name
             << "\narch=" << profile->target_arch
             << "\ncompiler_family=" << profile->compiler_family
             << "\nbuildtype=" << profile->buildtype
             << "\ntests=" << profile->tests << "\nnvapi=" << profile->nvapi
             << "\nnvngx=" << profile->nvngx
             << "\nairconv_windows=" << profile->airconv_windows
             << "\napitrace_builtin=" << apitrace
             << "\nccache=" << result.ccache
             << "\nnative_c=" << result.native_c
             << "\nnative_cpp=" << result.native_cpp
             << "\nnative_llvm=" << result.native_llvm
             << "\nwindows_llvm=" << result.windows_llvm
             << "\nwine=" << result.wine_root;
    identity << "\nnative_arch_flags=-arch x86_64";
    if (profile->cross)
      identity << "\ntarget_c=" << result.target_c
               << "\ntarget_cpp=" << result.target_cpp
               << "\ntarget_ar=" << result.target_ar;
    AppendFileIdentity(identity, "ccache", result.ccache);
    AppendFileIdentity(identity, "native_c", result.native_c);
    AppendFileIdentity(identity, "native_cpp", result.native_cpp);
    const auto llvm_config = result.native_llvm / "lib/cmake/llvm/LLVMConfig.cmake";
    if (fs::is_regular_file(llvm_config))
      AppendFileIdentity(identity, "native_llvm_config", llvm_config);
    if (profile->cross) {
      const auto wine_marker = result.wine_root / ".dxmt-builder-dependency";
      const auto wine_executable = result.wine_root / "bin/wine";
      if (fs::is_regular_file(wine_marker))
        AppendFileIdentity(identity, "wine_marker", wine_marker);
      if (fs::is_regular_file(wine_executable))
        AppendFileIdentity(identity, "wine_executable", wine_executable);
    }
    if (!result.windows_llvm.empty()) {
      const auto marker = result.windows_llvm / ".dxmt-builder-dependency";
      if (fs::is_regular_file(marker))
        AppendFileIdentity(identity, "windows_llvm_marker", marker);
    }
    if (profile->cross) {
      AppendFileIdentity(identity, "target_c", result.target_c);
      AppendFileIdentity(identity, "target_cpp", result.target_cpp);
      AppendFileIdentity(identity, "target_ar", result.target_ar);
    }
    const auto meson = RunCommand({"meson", "--version"}, {}, true);
    RequireSuccess(meson, "Meson identity");
    identity << "\nmeson=" << Trim(meson.output);
    const auto sdk = RunCommand(
        {"xcrun", "--sdk", "macosx", "--show-sdk-version"}, {}, true);
    RequireSuccess(sdk, "macOS SDK identity");
    identity << "\nmacos_sdk=" << Trim(sdk.output);
    result.fingerprint = Sha256(identity.str());
    return result;
  }

  void WriteMachineFiles(const ResolvedProfile &profile) const {
    fs::create_directories(profile.meta);
    std::ostringstream native;
    native << "[binaries]\n";
    native << "c = ['" << profile.ccache.string() << "', '"
           << profile.native_c.string() << "', '-arch', 'x86_64']\n"
           << "cpp = ['" << profile.ccache.string() << "', '"
           << profile.native_cpp.string() << "', '-arch', 'x86_64']\n";
    WriteFileAtomic(profile.native_file, native.str());

    if (!profile.profile->cross)
      return;
    std::string family;
    std::string cpu;
    if (profile.profile->target_arch == "x64") {
      family = "x86_64";
      cpu = "x86_64";
    } else if (profile.profile->target_arch == "x86") {
      family = "x86";
      cpu = "x86";
    } else {
      throw std::runtime_error("unsupported cross target architecture: " +
                               std::string(profile.profile->target_arch));
    }
    std::ostringstream cross;
    cross << "[binaries]\n"
          << "c = " << MesonArray({profile.ccache, profile.target_c}) << '\n'
          << "cpp = " << MesonArray({profile.ccache, profile.target_cpp}) << '\n'
          << "ar = '" << profile.target_ar.string() << "'\n"
          << "strip = '" << profile.target_strip.string() << "'\n"
          << "windres = '" << profile.target_windres.string() << "'\n\n"
          << "[properties]\nneeds_exe_wrapper = true\n\n"
          << "[host_machine]\nsystem = 'windows'\n"
          << "cpu_family = '" << family << "'\n"
          << "cpu = '" << cpu << "'\nendian = 'little'\n";
    WriteFileAtomic(profile.cross_file, cross.str());
  }

  fs::path BuilderBinary() const {
    if (const char *path = std::getenv("DXMT_BUILDER_BINARY"))
      return fs::canonical(path);
    throw std::runtime_error("DXMT_BUILDER_BINARY was not provided by the launcher");
  }

  void Configure(const ResolvedProfile &profile) const {
    WriteMachineFiles(profile);

    const auto properties_path = profile.meta / "profile.properties";
    const auto existing = ReadProperties(properties_path);
    const bool configured = fs::is_regular_file(profile.build / "meson-private/coredata.dat");
    const bool compatible = existing.contains("fingerprint") &&
                            existing.at("fingerprint") == profile.fingerprint;
    if (configured && !compatible) {
      if (!IsPathWithin(profile.build, managed_root_))
        throw std::runtime_error("refusing to replace unmanaged build directory");
      fs::remove_all(profile.build);
    }

    const auto version = EnvironmentValue("DXMT_BUILDER_DXMT_VERSION");
    const auto native_llvm_link_args =
        Join(NativeLlvmLinkArgs(profile.native_llvm), ",");
    const auto native_llvm_link_args_digest = Sha256(native_llvm_link_args);
    const bool native_llvm_link_args_compatible =
        existing.contains("native_llvm_link_args_digest") &&
        existing.at("native_llvm_link_args_digest") ==
            native_llvm_link_args_digest;
    const auto wine_source =
        profile.profile->cross ? FindManagedWineSource() : fs::path{};
    const auto existing_wine_source =
        existing.contains("wine_source") ? existing.at("wine_source") : "";
    if (!fs::is_regular_file(profile.build / "meson-private/coredata.dat")) {
      std::vector<std::string> command = {
          "meson", "setup", profile.build.string(), repo_root_.string(),
          "--native-file", profile.native_file.string()};
      if (profile.profile->cross) {
        command.push_back("--cross-file");
        command.push_back(profile.cross_file.string());
      }
      command.push_back("--buildtype");
      command.emplace_back(profile.profile->buildtype);
      command.push_back("--prefix");
      command.push_back("/usr/local");
      command.push_back("-Dnative_llvm_path=" + profile.native_llvm.string());
      command.push_back("-Dnative_llvm_link_args=" + native_llvm_link_args);
      command.push_back("-Denable_tests=" + std::string(profile.profile->tests ? "true" : "false"));
      command.push_back("-Denable_nvapi=" + std::string(profile.profile->nvapi ? "true" : "false"));
      command.push_back("-Denable_nvngx=" + std::string(profile.profile->nvngx ? "true" : "false"));
      command.push_back("-Dbuild_airconv_for_windows=" +
                        std::string(profile.profile->airconv_windows ? "true" : "false"));
      if (!profile.windows_llvm.empty())
        command.push_back("-Dwindows_llvm_path=" + profile.windows_llvm.string());
      command.push_back("-Dapitrace_builtin=" +
                        std::string(EnvironmentFlag("DXMT_BUILDER_APITRACE") ? "true" : "false"));
      if (EnvironmentFlag("DXMT_BUILDER_APITRACE"))
        command.push_back("-Dapitrace_source_path=" +
                          (repo_root_ / "external/apitrace").string());
      command.push_back("-Ddxmt_version=" + version);
      command.push_back("-Ddxmt_builder_path=" + BuilderBinary().string());
      command.push_back("-Ddxmt_builder_config_path=" + config_path_.string());
      command.push_back("-Ddxmt_cache_root=" + managed_root_.string());
      if (profile.profile->cross) {
        command.push_back("-Dwine_build_path=");
        command.push_back("-Dwine_install_path=" + profile.wine_root.string());
        // Wine's own d3d9 conformance modules build from the Wine SOURCE tree,
        // which carries include/wine/test.h and the test sources. The install
        // does not, so the two paths are both needed and are not the same
        // directory. Passing nothing simply leaves those modules unbuilt.
        if (!wine_source.empty())
          command.push_back("-Dwine_source_path=" + wine_source.string());
      }
      RequireSuccess(RunCommand(command, BuildEnvironment()), "Meson configure");
    } else if (!existing.contains("dxmt_version") ||
               existing.at("dxmt_version") != version ||
               !native_llvm_link_args_compatible ||
               !existing.contains("builder_config_path") ||
               existing.at("builder_config_path") != config_path_.string() ||
               existing_wine_source != wine_source.string()) {
      std::vector<std::string> reconfigure = {
          "meson", "configure", profile.build.string(),
          "-Ddxmt_version=" + version,
          "-Dnative_llvm_link_args=" + native_llvm_link_args,
          "-Ddxmt_builder_config_path=" + config_path_.string()};
      // A build directory configured before the Wine source was discoverable
      // would otherwise keep the stale value for its whole life.
      if (profile.profile->cross && !wine_source.empty())
        reconfigure.push_back("-Dwine_source_path=" + wine_source.string());
      RequireSuccess(RunCommand(reconfigure, BuildEnvironment()),
                     "DXMT option reconfigure");
    }

    std::ostringstream properties;
    properties << "schema=1\nprofile=" << profile.profile->name
               << "\nfingerprint=" << profile.fingerprint
               << "\ndxmt_version=" << version
               << "\nnative_llvm_link_args_digest="
               << native_llvm_link_args_digest
               << "\nbuilder_config_path=" << config_path_.string()
               << "\nwine_root=" << profile.wine_root.string()
               << "\nwine_source=" << wine_source.string() << '\n';
    WriteFileAtomic(properties_path, properties.str());
  }

  ResolvedProfile EnsureConfigured(std::string_view name) const {
    auto profile = Resolve(name);
    Configure(profile);
    return profile;
  }

  int Bootstrap(std::span<const std::string> arguments) const {
    EnsureManagedLayout();
    std::vector<std::string> components(arguments.begin(), arguments.end());
    if (components.empty() || std::find(components.begin(), components.end(), "all") != components.end())
      components = {"host", "wine-x64", "llvm-mingw", "llvm-project",
                    "llvm-darwin-x64", "llvm-win"};

    const auto helper = (repo_root_ / "scripts/ci-self-hosted.sh").string();
    const Environment environment = {
        {"DXMT_MANAGED_CACHE_ROOT", managed_root_.string()},
        {"DXMT_REPO_ROOT", repo_root_.string()},
    };
    for (const auto &component : components) {
      FileLock component_lock(managed_root_ / "locks" /
                              ("bootstrap-" + component + ".lock"));
      std::vector<std::string> command;
      if (component == "host")
        command = {helper, "setup-host"};
      else if (component == "wine-x64")
        command = {helper, "ensure-wine", "x86_64"};
      else if (component == "llvm-mingw")
        command = {helper, "ensure-llvm-mingw"};
      else if (component == "llvm-project")
        command = {helper, "ensure-llvm-project"};
      else if (component == "llvm-darwin-x64")
        command = {helper, "ensure-llvm-darwin", "x86_64"};
      else if (component == "llvm-win")
        command = {helper, "ensure-llvm-win"};
      else
        throw std::runtime_error("unknown bootstrap component: " + component);
      if (!config_path_.empty()) {
        command.insert(command.begin() + 1, config_path_.string());
        command.insert(command.begin() + 1, "--config");
      }
      RequireSuccess(RunCommand(command, environment), "bootstrap " + component);
    }
    return 0;
  }

  int ConfigureCommand(std::span<const std::string> arguments) const {
    std::vector<std::string> remaining;
    const auto name = ParseProfile(arguments, &remaining);
    if (!remaining.empty())
      throw std::runtime_error("unexpected configure argument: " + remaining.front());
    FileLock lock(ProfileLockPath(name));
    const auto profile = EnsureConfigured(name);
    std::cout << "configured " << name << " at " << profile.build << '\n';
    return 0;
  }

  int AuditCommand(std::span<const std::string> arguments) const {
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::string> audit_arguments;
    const auto name = ParseProfile(arguments, &audit_arguments,
                                   "llvm-mingw-x64-debugoptimized");
    bool expects_value = false;
    bool policy_only = false;
    bool checker_fixtures_only = false;
    bool use_audit_cache = true;
    bool deep = false;
    bool sanitizers = false;
    bool update_baseline = false;
    std::string baseline_reason;
    std::optional<fs::path> requested_report;
    // The analyzer is CPU bound and this host has far more cores than the old
    // hard cap of 4 used.  Leave two cores for the rest of the system and let
    // --jobs override when memory pressure matters.
    std::size_t jobs = std::max<std::size_t>(
        1, std::thread::hardware_concurrency() > 2
               ? std::thread::hardware_concurrency() - 2
               : 1);
    std::string scope = "dx12-metal4";
    for (std::size_t index = 0; index < audit_arguments.size(); ++index) {
      const auto &argument = audit_arguments[index];
      if (expects_value) {
        if (audit_arguments[index - 1] == "--scope")
          scope = argument;
        else if (audit_arguments[index - 1] == "--jobs")
          jobs = static_cast<std::size_t>(std::stoul(argument));
        else if (audit_arguments[index - 1] == "--baseline-reason")
          baseline_reason = argument;
        else if (audit_arguments[index - 1] == "--report")
          requested_report = fs::absolute(argument).lexically_normal();
        expects_value = false;
        continue;
      }
      if (argument == "--no-cache")
        continue;
      if (argument == "--scope" || argument == "--jobs" ||
          argument == "--baseline-reason" || argument == "--report") {
        expects_value = true;
      } else if (argument.starts_with("--scope=")) {
        scope = argument.substr(std::string("--scope=").size());
      } else if (argument.starts_with("--jobs=")) {
        jobs = static_cast<std::size_t>(
            std::stoul(argument.substr(std::string("--jobs=").size())));
      } else if (argument == "--deep") {
        deep = true;
      } else if (argument == "--sanitizers") {
        sanitizers = true;
      } else if (argument == "--update-baseline") {
        update_baseline = true;
      } else if (argument.starts_with("--baseline-reason=")) {
        baseline_reason =
            argument.substr(std::string("--baseline-reason=").size());
      } else if (argument.starts_with("--report=")) {
        requested_report = fs::absolute(
                               argument.substr(std::string("--report=").size()))
                               .lexically_normal();
      } else if (argument == "--policy-only") {
        policy_only = true;
        continue;
      } else if (argument == "--checker-fixtures-only") {
        checker_fixtures_only = true;
        continue;
      } else if (argument == "--no-cache") {
        use_audit_cache = false;
        continue;
      } else {
        throw std::runtime_error("unexpected audit argument: " + argument);
      }
    }
    if (expects_value)
      throw std::runtime_error("audit option requires a value");
    if (scope != "dx12-metal4")
      throw std::runtime_error("unsupported audit scope: " + scope);
    if (jobs == 0)
      throw std::runtime_error("--jobs must be greater than zero");
    if (update_baseline && baseline_reason.empty())
      throw std::runtime_error(
          "--update-baseline requires --baseline-reason");
    if (policy_only && sanitizers)
      throw std::runtime_error(
          "--policy-only cannot be combined with --sanitizers");
    if (checker_fixtures_only && (deep || sanitizers || update_baseline))
      throw std::runtime_error(
          "--checker-fixtures-only cannot be combined with deep, sanitizer, "
          "or baseline modes");

    auto policy_errors = testing::AuditDx12Metal4Policy(repo_root_);
    // --list-checks compiles nothing, so config integrity is verified in every
    // audit mode, including --policy-only and --checker-fixtures-only.  A
    // fixture proves one check is alive; this proves the config as a whole did
    // not silently lose entries.
    for (const auto *audit_config :
         {".clang-tidy", ".clang-tidy-deep", ".clang-tidy-model"}) {
      auto config_errors =
          ValidateAuditCheckConfig(ResolveClangTidy(), repo_root_ / audit_config,
                                   BuildEnvironment(), repo_root_);
      policy_errors.insert(policy_errors.end(), config_errors.begin(),
                           config_errors.end());
    }
    for (const auto &error : policy_errors)
      std::cerr << "policy error: " << error << '\n';
    if (policy_only) {
      if (!policy_errors.empty())
        return 1;
      std::cout << "DXMT audit policy checks passed\n";
      RecordTelemetry("audit", name, "success", started);
      return 0;
    }

    FileLock lock(ProfileLockPath(name));
    const auto profile = EnsureConfigured(name);
    RequireSuccess(
        RunCommand({"meson", "compile", "-C", profile.build.string(),
                    "dxmt-d3d12"},
                   BuildEnvironment()),
        "DX12/Metal4 audit prerequisite build");
    const auto clang_tidy = ResolveClangTidy();
    auto database =
        PrepareAuditCompilationDatabase(repo_root_, profile.build);
    // Structural, not stylistic: this needs the queue's real compile command,
    // so it lives here rather than in the text-only --policy-only pass, and it
    // reports through policy_errors so a non-self-contained header fails the
    // audit exactly like any other policy break.
    {
      auto self_containment_errors = AuditHeaderSelfContainmentViolations(
          repo_root_, database.directory, BuildEnvironment(), jobs);
      for (const auto &error : self_containment_errors)
        std::cerr << "policy error: " << error << '\n';
      policy_errors.insert(policy_errors.end(), self_containment_errors.begin(),
                           self_containment_errors.end());
    }
    // The companion guarantee, which is why there is no separate "every
    // template header needs an instantiation point" rule: the scan below runs
    // over the whole compilation database, and that database contains every
    // src/d3d12/*.cpp (146 of 146 at the time of writing) plus every
    // src/dxmt/*.cpp. Compiling a unit instantiates everything it reaches, and
    // *transitive* instantiation counts -- BuildBindlessRootOffsets has no
    // direct caller yet is instantiated through d3d12_replay_binding_encode.cpp
    // -- so a grep for call sites would be the wrong probe and a rule built on
    // one would be worse than no rule. Any template header reachable from a
    // unit other than d3d12_command_queue.cpp is therefore already covered
    // behaviourally, and the residual gap is exactly the set of headers only
    // that one giant unit reaches. Measured over the include closure of every
    // non-queue unit, that set is a single header today,
    // d3d12_replay_perf_record_buckets.hpp, whose template body names nothing
    // that depends on its template parameter, so the self-containment rule
    // above already resolves all of it at definition time. Re-measure before
    // assuming the set is still that small.

    if (deep) {
      // Every extracted d3d12 module is deep-analyzed automatically: these
      // are the units small enough for the path-sensitive analyzer to finish,
      // which is the entire point of pulling them out of the queue. Only the
      // cross-cutting non-d3d12 units stay enumerated.
      std::set<std::string> deep_paths = {
          "src/winemetal4/unix/cache.c",
          "src/winemetal4/unix/metal4_cpp.cpp",
          "src/winemetal4/unix/winemetal_unix.c",
      };
      // Enumerating src/dxmt by hand meant a unit added there was silently
      // quick-only: std::erase_if drops it with no diagnostic, so the audit
      // reports success for a file the path-sensitive checks never opened.
      // Sweep both directories instead, and keep the enumeration only for
      // winemetal4/unix, where the deep scope is a deliberate subset.
      for (const auto *directory : {"src/d3d12", "src/dxmt"}) {
        for (const auto &entry :
             fs::directory_iterator(repo_root_ / directory)) {
          if (entry.is_regular_file() && entry.path().extension() == ".cpp")
            deep_paths.insert(std::string(directory) + "/" +
                              entry.path().filename().string());
        }
      }
      std::erase_if(database.files, [&](const fs::path &path) {
        std::error_code error;
        const auto relative = fs::relative(path, repo_root_, error);
        return error || !deep_paths.contains(relative.generic_string());
      });
    }
    if (database.files.empty())
      throw std::runtime_error(
          "no translation units matched the DX12/Metal4 audit scope");
    const std::array model_sources = {
        fs::weakly_canonical(
            repo_root_ / "src/d3d12/d3d12_submission_model.cpp"),
        fs::weakly_canonical(
            repo_root_ / "src/d3d12/d3d12_submission_model_header.cpp"),
    };
    for (const auto &model_source : model_sources) {
      if (std::find(database.files.begin(), database.files.end(),
                    model_source) == database.files.end())
        throw std::runtime_error(
            "DX12 submission model source is missing from the audit "
            "compilation database: " +
            model_source.string());
    }
    const auto native_model_database =
        profile.meta / "audit-native-d3d12-model";
    fs::create_directories(native_model_database);
    const auto sdk_result = RunCommand(
        {"xcrun", "--sdk", "macosx", "--show-sdk-path"}, {}, true);
    RequireSuccess(sdk_result, "resolve macOS SDK");
    const auto sdk = Trim(sdk_result.output);
    const auto libcxx_include =
        fs::path(sdk) / "usr/include/c++/v1";
    if (!fs::is_directory(libcxx_include))
      throw std::runtime_error(
          "Xcode libc++ headers are missing under " +
          libcxx_include.string());
    std::ostringstream native_model_commands;
    native_model_commands << "[\n";
    for (std::size_t index = 0; index < model_sources.size(); ++index) {
      const auto object =
          native_model_database /
          (model_sources[index].filename().string() + ".o");
      std::vector<std::string> native_arguments = {
          profile.native_cpp.string(),
          "-std=c++20",
          "-arch",
          "x86_64",
          "-stdlib=libc++",
          "-isysroot",
          sdk,
          "-isystem",
          libcxx_include.string(),
          "-Wthread-safety",
          "-Wthread-safety-beta",
          "-Werror=thread-safety",
          "-I" + (repo_root_ / "include").string(),
          "-c",
          model_sources[index].string(),
          "-o",
          object.string(),
      };
      std::ostringstream command;
      for (const auto &argument : native_arguments) {
        if (command.tellp() > 0)
          command << ' ';
        command << ShellQuote(argument);
      }
      native_model_commands
          << "  {\"directory\":\"" << JsonEscape(repo_root_.string())
          << "\",\"command\":\"" << JsonEscape(command.str())
          << "\",\"file\":\"" << JsonEscape(model_sources[index].string())
          << "\"}";
      if (index + 1 != model_sources.size())
        native_model_commands << ',';
      native_model_commands << '\n';
    }
    native_model_commands << "]\n";
    WriteFileAtomic(native_model_database / "compile_commands.json",
                    native_model_commands.str());

    const auto config =
        repo_root_ / (deep ? ".clang-tidy-deep" : ".clang-tidy");
    const auto environment = BuildEnvironment();
    const auto model_config = repo_root_ / ".clang-tidy-model";
    auto enabled_checks =
        ValidateAuditChecks(clang_tidy, config, false, deep, environment,
                            repo_root_);
    auto model_checks =
        ValidateAuditChecks(clang_tidy, model_config, true, false, environment,
                            repo_root_);
    enabled_checks.insert(enabled_checks.end(), model_checks.begin(),
                          model_checks.end());
    std::sort(enabled_checks.begin(), enabled_checks.end());
    enabled_checks.erase(
        std::unique(enabled_checks.begin(), enabled_checks.end()),
        enabled_checks.end());

    const auto checker_fixture =
        repo_root_ / "tools/audit/fixtures/clang_tidy_positive.cpp";
    const std::array<std::string_view, 4> fixture_expectations = {
        "bugprone-use-after-move",
        "clang-analyzer-cplusplus.NewDelete",
        "cppcoreguidelines-pro-bounds-pointer-arithmetic",
        "clang-diagnostic-thread-safety-analysis",
    };
    // The generic thread-safety category above also fires for an unguarded
    // read, so it cannot prove that lock *ordering* is still being checked.
    // Require the specific inversion message: without -Wthread-safety-beta
    // Clang silently stops verifying member lock order, and every
    // DXMT_ACQUIRED_BEFORE annotation would degrade into a comment.
    static constexpr std::string_view lock_order_expectation =
        "must be acquired before";
    const auto fixture_result = RunCommand(
        {clang_tidy.string(),
         checker_fixture.string(),
         "--config-file=" + model_config.string(),
         "--quiet",
         "--header-filter=.*",
         "--extra-arg=-Wthread-safety",
         "--extra-arg=-Wthread-safety-beta",
         "--",
         "-std=c++20",
         "-I" + (repo_root_ / "include").string()},
        environment, true, repo_root_);
    const auto thread_fixture_result = RunCommand(
        {clang_tidy.string(),
         checker_fixture.string(),
         "--checks=-*,clang-analyzer-core.CallAndMessage",
         "--quiet",
         "--extra-arg=-Wthread-safety",
         "--extra-arg=-Wthread-safety-beta",
         "--extra-arg=-Werror=thread-safety",
         "--",
         "-std=c++20",
         "-I" + (repo_root_ / "include").string()},
        environment, true, repo_root_);
    const auto fixture_output =
        fixture_result.output + thread_fixture_result.output;
    // A deep check that reports nothing is indistinguishable from one that
    // never ran.  Several of them are legitimately clean across the tree, so
    // prove their liveness against a fixture instead of reading zero findings
    // as safety.
    const auto deep_fixture =
        repo_root_ / "tools/audit/fixtures/clang_tidy_deep_positive.cpp";
    const auto deep_config = repo_root_ / ".clang-tidy-deep";
    if (fs::is_regular_file(deep_fixture) && fs::is_regular_file(deep_config)) {
      const auto deep_fixture_result = RunCommand(
          {clang_tidy.string(), deep_fixture.string(),
           "--config-file=" + deep_config.string(), "--quiet", "--",
           "-std=c++20", "-isysroot", sdk},
          environment, true, repo_root_);
      for (const auto expectation :
           {"bugprone-integer-division", "bugprone-too-small-loop-variable",
            "bugprone-inc-dec-in-conditions",
            "clang-analyzer-optin.cplusplus.UninitializedObject",
            "clang-analyzer-optin.cplusplus.VirtualCall",
            "bugprone-assert-side-effect", "bugprone-dangling-handle",
            "bugprone-exception-escape", "bugprone-infinite-loop",
            "bugprone-misplaced-widening-cast",
            "bugprone-implicit-widening-of-multiplication-result",
            "bugprone-multiple-statement-macro", "bugprone-sizeof-container",
            "bugprone-sizeof-expression", "bugprone-suspicious-memset-usage",
            "bugprone-unused-raii", "bugprone-signed-char-misuse",
            "bugprone-unchecked-optional-access", "misc-no-recursion",
            "concurrency-mt-unsafe", "clang-analyzer-deadcode.DeadStores",
            "clang-analyzer-nullability.NullPassedToNonnull",
            "clang-analyzer-unix.Malloc",
            "clang-analyzer-unix.MismatchedDeallocator",
            "clang-analyzer-unix.cstring.BadSizeArg",
            "clang-analyzer-unix.cstring.NullArg",
            "clang-analyzer-osx.cocoa.RetainCount",
            "performance-noexcept-move-constructor"}) {
        if (deep_fixture_result.output.find("[" + std::string(expectation)) !=
            std::string::npos)
          continue;
        policy_errors.push_back(
            "checker fixture did not trigger required deep diagnostic: " +
            std::string(expectation));
      }
    }
    // The fixture above is also a positive fixture for the quick config, which
    // otherwise had none: clang_tidy_positive.cpp runs under .clang-tidy-model,
    // so nothing proved .clang-tidy itself still selects anything.  Assert on
    // checks that live only in the quick config, never in the model one.
    const auto quick_config = repo_root_ / ".clang-tidy";
    if (fs::is_regular_file(deep_fixture) && fs::is_regular_file(quick_config)) {
      const auto quick_fixture_result = RunCommand(
          {clang_tidy.string(), deep_fixture.string(),
           "--config-file=" + quick_config.string(), "--quiet", "--",
           "-std=c++20", "-isysroot", sdk},
          environment, true, repo_root_);
      for (const auto expectation :
           {"bugprone-assert-side-effect", "bugprone-infinite-loop",
            "bugprone-misplaced-widening-cast",
            "bugprone-multiple-statement-macro", "bugprone-sizeof-container",
            "bugprone-suspicious-memset-usage"}) {
        if (quick_fixture_result.output.find("[" + std::string(expectation)) !=
            std::string::npos)
          continue;
        policy_errors.push_back(
            "checker fixture did not trigger required quick diagnostic: " +
            std::string(expectation));
      }
    }
    if (fixture_output.find(lock_order_expectation) == std::string::npos)
      policy_errors.push_back(
          "checker fixture did not trigger lock-ordering diagnostic; "
          "member lock order analysis requires -Wthread-safety-beta");
    for (const auto expectation : fixture_expectations) {
      const auto marker = "[" + std::string(expectation);
      if (fixture_output.find(marker) != std::string::npos)
        continue;
      policy_errors.push_back(
          "checker fixture did not trigger required diagnostic: " +
          std::string(expectation));
    }
    if (std::any_of(policy_errors.begin(), policy_errors.end(),
                    [](const std::string &error) {
                      return error.starts_with("checker fixture");
                    })) {
      std::cerr << "checker fixture output:\n" << fixture_output;
      if (!fixture_output.ends_with('\n'))
        std::cerr << '\n';
    }
    for (const auto &error : policy_errors)
      if (error.starts_with("checker fixture"))
        std::cerr << "policy error: " << error << '\n';
    if (checker_fixtures_only) {
      if (!policy_errors.empty())
        return 1;
      std::cout << "DXMT audit checker fixtures passed\n";
      RecordTelemetry("audit", name + ":checker-fixtures", "success",
                      started);
      return 0;
    }

    // Cache verdicts per (tool, config contents, command line, reachable
    // sources).  Keyed by content rather than timestamps so a branch switch or
    // a rebuild that restores identical files still hits.
    fs::path audit_cache_directory;
    std::string tool_identity;
    // Read once here rather than per unit: the two configs are fixed for the
    // whole run, and a mid-run edit must not split the key space.
    std::map<fs::path, std::string> audit_config_contents;
    if (use_audit_cache) {
      const auto version =
          RunCommand({clang_tidy.string(), "--version"}, environment, true,
                     repo_root_);
      tool_identity = clang_tidy.string() + "\n" + version.output;
      for (const auto &selected : {config, model_config})
        audit_config_contents.emplace(selected, ReadFile(selected));
      audit_cache_directory =
          managed_root_ / "audit-cache" / (deep ? "deep" : "quick");
      std::error_code error;
      fs::create_directories(audit_cache_directory, error);
      if (error)
        audit_cache_directory.clear();
    }

    auto run_tidy = [&](const fs::path &path) {
      const bool is_model =
          std::find(model_sources.begin(), model_sources.end(), path) !=
          model_sources.end();
      const auto &selected_config = is_model ? model_config : config;
      const auto &selected_database =
          is_model ? native_model_database : database.directory;
      std::vector<std::string> command = {
          clang_tidy.string(),
          "-p",
          selected_database.string(),
          path.string(),
          "--config-file=" + selected_config.string(),
          "--quiet",
          "--extra-arg=-DINFINITY=__builtin_inff()",
          "--extra-arg=-Wthread-safety",
         "--extra-arg=-Wthread-safety-beta",
          "--extra-arg=-Werror=thread-safety",
      };
      if (IsPathWithin(path, repo_root_ / "src/winemetal4/unix")) {
        command.push_back("--extra-arg=-isysroot");
        command.push_back("--extra-arg=" + sdk);
      }
      if (!audit_cache_directory.empty()) {
        const auto config_entry = audit_config_contents.find(selected_config);
        const auto key = AuditCacheKey(
            path, repo_root_, command, tool_identity,
            config_entry == audit_config_contents.end() ? std::string_view{}
                                                        : config_entry->second);
        const auto entry = audit_cache_directory / (key + ".cache");
        std::error_code error;
        if (fs::is_regular_file(entry, error)) {
          const auto cached = ReadFile(entry);
          // Stored as "<status>\n<output>"; a malformed entry is simply
          // treated as a miss rather than trusted.
          const auto newline = cached.find('\n');
          if (newline != std::string::npos) {
            try {
              CommandResult result;
              result.status = std::stoi(cached.substr(0, newline));
              result.output = cached.substr(newline + 1);
              return AuditTidyOutput{path, result};
            } catch (const std::exception &) {
              // fall through to a real run
            }
          }
        }
        auto result = RunCommand(command, environment, true, repo_root_);
        // Only cache verdicts the tool actually produced.  A crashed or
        // killed clang-tidy must never be remembered as a clean result.
        if (result.status == 0 || !result.output.empty())
          WriteFileAtomic(entry, std::to_string(result.status) + "\n" +
                                     result.output);
        return AuditTidyOutput{path, result};
      }
      return AuditTidyOutput{
          path, RunCommand(command, environment, true, repo_root_)};
    };

    // Longest-processing-time first: a translation unit that pulls in queue
    // fragments costs far more analyzer time than its own line count suggests,
    // so weigh each file by itself plus every fragment it includes.  Starting
    // with the heaviest keeps the tail from being one lone giant TU.
    auto analysis_weight = [&](const fs::path &path) {
      std::uintmax_t weight = 0;
      std::error_code error;
      weight += fs::file_size(path, error);
      if (error)
        weight = 0;
      const auto source = ReadFile(path);
      static const std::regex fragment_include(
          R"re(#include "([A-Za-z0-9_]+\.inc)")re");
      for (std::sregex_iterator match(source.begin(), source.end(),
                                      fragment_include), end;
           match != end; ++match) {
        std::error_code fragment_error;
        const auto fragment = path.parent_path() / (*match)[1].str();
        const auto size = fs::file_size(fragment, fragment_error);
        if (!fragment_error)
          weight += size;
      }
      return weight;
    };
    std::vector<std::pair<std::uintmax_t, fs::path>> ordered;
    ordered.reserve(database.files.size());
    for (const auto &path : database.files)
      ordered.emplace_back(analysis_weight(path), path);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &left, const auto &right) {
                if (left.first != right.first)
                  return left.first > right.first;
                return left.second < right.second;
              });

    std::vector<AuditTidyOutput> outputs;
    std::vector<std::future<AuditTidyOutput>> pending;
    // Harvest whichever unit finishes first.  Waiting on the front of the
    // queue instead would let a single slow TU block every free slot behind
    // it, collapsing real parallelism far below --jobs.
    auto harvest_ready = [&](bool block) {
      for (;;) {
        for (auto it = pending.begin(); it != pending.end();) {
          if (it->wait_for(std::chrono::milliseconds(0)) ==
              std::future_status::ready) {
            outputs.push_back(it->get());
            it = pending.erase(it);
            if (!block)
              return;
          } else {
            ++it;
          }
        }
        if (!block || pending.size() < jobs)
          return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    };
    for (const auto &[weight, path] : ordered) {
      pending.push_back(std::async(std::launch::async, run_tidy, path));
      if (pending.size() >= jobs)
        harvest_ready(true);
    }
    for (auto &future : pending)
      outputs.push_back(future.get());
    std::sort(outputs.begin(), outputs.end(),
              [](const auto &left, const auto &right) {
                return left.path < right.path;
              });

    std::vector<AuditDiagnostic> diagnostics;
    bool tool_errors = false;
    for (const auto &output : outputs) {
      bool parsed = false;
      std::istringstream lines(output.result.output);
      std::string line;
      while (std::getline(lines, line)) {
        auto diagnostic = ParseAuditDiagnostic(repo_root_, line);
        if (!diagnostic)
          continue;
        parsed = true;
        if (diagnostic->level == "error")
          tool_errors = true;
        diagnostics.push_back(std::move(*diagnostic));
      }
      if (output.result.status != 0 && !parsed) {
        tool_errors = true;
        std::cerr << "clang-tidy failed for " << output.path << ":\n"
                  << output.result.output;
        if (!output.result.output.ends_with('\n'))
          std::cerr << '\n';
      }
    }

    std::size_t diagnostic_reports = diagnostics.size();
    diagnostics = DeduplicateAuditDiagnostics(std::move(diagnostics));
    // Errors are printed once per location too: a missing include reported by
    // twenty units is one broken include path, not twenty.
    for (const auto &diagnostic : diagnostics) {
      if (diagnostic.level != "error")
        continue;
      std::cerr << diagnostic.path << ":" << diagnostic.line << ":"
                << diagnostic.column << ": error: " << diagnostic.message
                << " [" << diagnostic.check << "]";
      if (diagnostic.reporting_units > 1)
        std::cerr << " (" << diagnostic.reporting_units
                  << " translation units)";
      std::cerr << '\n';
    }

    // Counted over unique locations, so a header finding weighs the same as a
    // finding in a .cpp instead of once per including unit.
    std::map<std::string, std::size_t> counts;
    for (const auto &diagnostic : diagnostics) {
      if (diagnostic.level != "error" &&
          AuditDiagnosticMayBeBaselinedImpl(diagnostic.path,
                                             diagnostic.check))
        ++counts[diagnostic.Fingerprint()];
    }
    const auto baseline_path =
        repo_root_ / "tools/audit" /
        (deep ? "clang-tidy-deep-baseline.json"
              : "clang-tidy-baseline.json");
    if (update_baseline) {
      WriteAuditBaseline(baseline_path, counts, baseline_reason);
      std::cout << "updated "
                << baseline_path.lexically_relative(repo_root_).generic_string()
                << " with " << diagnostics.size()
                << " unique diagnostics (" << diagnostic_reports
                << " reports)\n";
      return !policy_errors.empty() || tool_errors ? 1 : 0;
    }

    auto baseline = LoadAuditBaseline(baseline_path);
    std::vector<AuditDiagnostic> new_diagnostics;
    std::size_t baseline_hits = 0;
    // Already ordered by (path, line, column, check, message): the
    // deduplication sorts on that key and nothing has been appended since.
    for (const auto &diagnostic : diagnostics) {
      if (diagnostic.level == "error")
        continue;
      if (AuditDiagnosticMayBeBaselinedImpl(diagnostic.path,
                                             diagnostic.check)) {
        auto found = baseline.find(diagnostic.Fingerprint());
        if (found != baseline.end() && found->second.count != 0) {
          --found->second.count;
          ++baseline_hits;
          continue;
        }
      }
      new_diagnostics.push_back(diagnostic);
    }
    for (const auto &diagnostic : new_diagnostics) {
      std::cout << diagnostic.path << ":" << diagnostic.line << ":"
                << diagnostic.column << ": " << diagnostic.level << ": "
                << diagnostic.message << " [" << diagnostic.check << "]";
      if (diagnostic.reporting_units > 1)
        std::cout << " (" << diagnostic.reporting_units
                  << " translation units)";
      std::cout << '\n';
    }

    constexpr uint64_t kAuditScheduleSeed = 0xd3124d4554414c34ull;
    std::vector<std::string> sanitizer_errors;
    std::vector<std::string> sanitizer_passed;
    if (sanitizers) {
      const auto sanitizer_root = profile.root / "audit-sanitizers";
      fs::create_directories(sanitizer_root);
      // What gets sanitized is *data*, not code: every entry below is compiled
      // and run once per sanitizer configuration, so covering a new audit spec
      // means appending one target here and never touching the loop. Paths are
      // repository-relative and must stay in sync with the matching native test
      // executable in src/d3d12/meson.build -- that target is the source of
      // truth for the file and include list; this one only adds sanitizers.
      // Everything shared by all targets (C++ standard, -arch x86_64, -O1 -g,
      // the thread-safety warnings) stays out of the table because it follows
      // from the native toolchain -- ResolveNativeLlvm only ever accepts an
      // x86_64 LLVM -- and not from any one spec.
      struct SanitizerTarget {
        std::string name;
        std::vector<std::string> sources;
        std::vector<std::string> includes;
        std::vector<std::string> arguments;
        std::map<std::string, std::string> environment;
      };
      const std::vector<SanitizerTarget> targets = {
          SanitizerTarget{
              "d3d12-submission-model",
              {"src/d3d12/d3d12_submission_model.cpp",
               "tools/audit/d3d12_submission_model_spec.cpp"},
              {"include"},
              {},
              {}},
          // Mirrors d3d12_copy_geometry_native_test in src/d3d12/meson.build.
          // The spec asserts on returned geometry; the sanitizers are what
          // prove the clamping and overflow paths never actually stepped
          // outside a buffer while computing it.
          SanitizerTarget{
              "d3d12-copy-geometry",
              {"src/d3d12/d3d12_copy_footprint.cpp",
               "src/d3d12/d3d12_subresource_geometry.cpp",
               "src/d3d12/d3d12_tile_copy_plan.cpp",
               "src/d3d12/d3d12_indirect_topology.cpp",
               "src/d3d12/d3d12_tile_mapping.cpp",
               "src/dxmt/dxmt_format.cpp", "src/util/log/log.cpp",
               "src/util/util_env.cpp", "src/util/thread.cpp",
               "tools/audit/d3d12_copy_geometry_spec.cpp"},
              {"src/d3d12", "src/dxmt", "src/dxgi", "src/util",
               "src/winemetal4", "src/airconv", "include",
               "include/native/windows", "include/native/directx", "libs"},
              {"-DDXMT_DX12_METAL4=1", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00",
               // Narrow, deliberate carve-out: -fsanitize=enum only. D3D12
               // hands IndirectArgumentByteSize a raw application-supplied
               // D3D12_INDIRECT_ARGUMENT_DESC::Type, and the spec exercises
               // exactly that -- an out-of-range type value that must fall to
               // the default arm. Merely loading such a value is UB by the
               // letter of the standard, so -fsanitize=enum fires on the
               // defensive switch itself in d3d12_indirect_topology.cpp. Every
               // other UBSan check and all of ASan stay on; drop this flag
               // again once Type is read through an unsigned integer instead
               // of the enum.
               "-fno-sanitize=enum"},
              {{"DXMT_LOG_PATH", "none"}}},
      };
      struct SanitizerConfiguration {
        std::string name;
        std::string flags;
        std::map<std::string, std::string> environment;
      };
#ifdef __APPLE__
      constexpr auto kAsanOptions =
          "halt_on_error=1:detect_leaks=0:strict_string_checks=1";
#else
      constexpr auto kAsanOptions =
          "halt_on_error=1:detect_leaks=1:strict_string_checks=1";
#endif
      const std::array configurations = {
          SanitizerConfiguration{
              "asan-ubsan", "-fsanitize=address,undefined",
              {{"ASAN_OPTIONS", kAsanOptions},
               {"UBSAN_OPTIONS",
                "halt_on_error=1:print_stacktrace=1"}}},
          SanitizerConfiguration{
              "tsan", "-fsanitize=thread",
              {{"TSAN_OPTIONS",
                "halt_on_error=1:second_deadlock_stack=1"}}},
      };
      for (const auto &target : targets) {
      for (const auto &configuration : configurations) {
        const auto label = target.name + "-" + configuration.name;
        const auto binary = sanitizer_root / label;
        std::vector<std::string> compile = {
            profile.native_cpp.string(),
            "-std=c++20",
            "-arch",
            "x86_64",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            "-pthread",
            configuration.flags,
            "-Wthread-safety",
          "-Wthread-safety-beta",
            "-Werror=thread-safety",
        };
        for (const auto &argument : target.arguments)
          compile.push_back(argument);
        for (const auto &include : target.includes)
          compile.push_back("-I" + (repo_root_ / include).string());
        for (const auto &source : target.sources)
          compile.push_back((repo_root_ / source).string());
        compile.push_back("-o");
        compile.push_back(binary.string());
        const auto compile_result =
            RunCommand(compile, BuildEnvironment(), true, repo_root_);
        if (compile_result.status != 0) {
          sanitizer_errors.push_back(label + " compilation failed");
          std::cerr << label
                    << " compilation failed:\n"
                    << compile_result.output;
          continue;
        }
        auto run_environment = BuildEnvironment();
        run_environment.insert(configuration.environment.begin(),
                               configuration.environment.end());
        run_environment.insert(target.environment.begin(),
                               target.environment.end());
        run_environment["DXMT_TEST_SEED"] =
            std::to_string(kAuditScheduleSeed);
        const auto run_result =
            RunCommand({binary.string()}, run_environment, true, repo_root_);
        if (run_result.status != 0) {
          sanitizer_errors.push_back(label + " test failed");
          std::cerr << label << " test failed:\n"
                    << run_result.output;
        } else {
          sanitizer_passed.push_back(label);
          std::cout << "DXMT " << label << " test passed\n";
        }
      }
      }
    }

    const auto commit =
        RunCommand({"git", "-C", repo_root_.string(), "rev-parse", "HEAD"},
                   {}, true);
    const auto status =
        RunCommand({"git", "-C", repo_root_.string(), "status",
                    "--porcelain"},
                   {}, true);
    const auto report_path = requested_report.value_or(
        profile.meta /
        (deep ? "audit-dx12-metal4-deep.json"
              : "audit-dx12-metal4.json"));
    std::ostringstream report;
    // schema 2: "diagnostics" holds one entry per unique source location with
    // a "reporting_units" multiplicity, where schema 1 held one entry per
    // clang-tidy report.  Anything counting the array length means something
    // different now, so the version has to say so.
    report << "{\n"
           << "  \"schema\": 2,\n"
           << "  \"scope\": \"dx12-metal4\",\n"
           << "  \"profile\": \"" << JsonEscape(name) << "\",\n"
           << "  \"toolchain_fingerprint\": \""
           << JsonEscape(profile.fingerprint) << "\",\n"
           << "  \"clang_tidy_config_sha256\": \""
           << Sha256File(config) << "\",\n"
           << "  \"model_config_sha256\": \""
           << Sha256File(model_config) << "\",\n"
           << "  \"commit\": \"" << JsonEscape(Trim(commit.output))
           << "\",\n"
           << "  \"dirty\": " << (!Trim(status.output).empty() ? "true" : "false")
           << ",\n"
           << "  \"deep\": " << (deep ? "true" : "false") << ",\n"
           << "  \"sanitizers\": " << (sanitizers ? "true" : "false")
           << ",\n"
           << "  \"translation_units\": [";
    for (std::size_t index = 0; index < database.files.size(); ++index) {
      std::error_code error;
      const auto relative = fs::relative(database.files[index], repo_root_,
                                         error);
      report << (index == 0 ? "\n" : ",\n") << "    \""
             << JsonEscape(error ? database.files[index].generic_string()
                                 : relative.generic_string())
             << "\"";
    }
    if (!database.files.empty())
      report << '\n';
    report << "  ],\n  \"required_checks\": [";
    for (std::size_t index = 0; index < enabled_checks.size(); ++index)
      report << (index == 0 ? "\n" : ",\n") << "    \""
             << JsonEscape(enabled_checks[index]) << "\"";
    if (!enabled_checks.empty())
      report << '\n';
    report << "  ],\n  \"checker_fixtures\": [";
    for (std::size_t index = 0; index < fixture_expectations.size(); ++index)
      report << (index == 0 ? "\n" : ",\n")
             << "    {\"check\":\""
             << JsonEscape(std::string(fixture_expectations[index]))
             << "\",\"triggered\":true}";
    if (!fixture_expectations.empty())
      report << '\n';
    report << "  ],\n  \"policy_errors\": [";
    for (std::size_t index = 0; index < policy_errors.size(); ++index)
      report << (index == 0 ? "\n" : ",\n") << "    \""
             << JsonEscape(policy_errors[index]) << "\"";
    if (!policy_errors.empty())
      report << '\n';
    report << "  ],\n  \"diagnostics\": [";
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
      const auto &diagnostic = diagnostics[index];
      report << (index == 0 ? "\n" : ",\n")
             << "    {\"path\":\"" << JsonEscape(diagnostic.path)
             << "\",\"line\":" << diagnostic.line
             << ",\"column\":" << diagnostic.column
             << ",\"level\":\"" << JsonEscape(diagnostic.level)
             << "\",\"check\":\"" << JsonEscape(diagnostic.check)
             << "\",\"message\":\"" << JsonEscape(diagnostic.message)
             // Informational: how many units reported this one location, i.e.
             // how far the header holding it reaches.  Never a threshold.
             << "\",\"reporting_units\":" << diagnostic.reporting_units
             << "}";
    }
    if (!diagnostics.empty())
      report << '\n';
    const bool success = policy_errors.empty() && !tool_errors &&
                         new_diagnostics.empty() &&
                         sanitizer_errors.empty();
    // Total clang-tidy reports behind the deduplicated list above.  Kept as a
    // coverage number only; the gate counts unique locations.
    report << "  ],\n  \"diagnostic_reports\": " << diagnostic_reports
           << ",\n  \"baseline_hits\": " << baseline_hits
           << ",\n  \"sanitizer_seed\": " << kAuditScheduleSeed
           << ",\n  \"sanitizer_profiles_passed\": [";
    for (std::size_t index = 0; index < sanitizer_passed.size(); ++index)
      report << (index == 0 ? "\n" : ",\n") << "    \""
             << JsonEscape(sanitizer_passed[index]) << "\"";
    if (!sanitizer_passed.empty())
      report << '\n';
    report << "  ],\n  \"sanitizer_errors\": [";
    for (std::size_t index = 0; index < sanitizer_errors.size(); ++index)
      report << (index == 0 ? "\n" : ",\n") << "    \""
             << JsonEscape(sanitizer_errors[index]) << "\"";
    if (!sanitizer_errors.empty())
      report << '\n';
    report << "  ],\n  \"success\": " << (success ? "true" : "false")
           << "\n}\n";
    WriteFileAtomic(report_path, report.str());
    std::cout << "DXMT audit report: " << report_path << '\n';

    std::cout << "DXMT audit scanned " << database.files.size()
              << " translation units: " << diagnostics.size()
              << " unique diagnostics (" << diagnostic_reports
              << " reports), " << new_diagnostics.size() << " new, "
              << policy_errors.size() << " policy errors\n";
    if (!success)
      return 1;
    RecordTelemetry("audit", name, "success", started);
    return 0;
  }

  std::vector<std::string> MapTargets(const std::vector<std::string> &targets) const {
    if (targets.empty())
      return {"runtime"};
    static const std::set<std::string> supported = {
        "runtime", "d3d9", "d3d10", "d3d11", "d3d12", "tests-framework",
        "tests-d3d9", "tests-d3d10", "tests-d3d11", "tests-d3d12",
        "tests-all", "benchmarks"};
    static const std::map<std::string, std::string> meson_targets = {
        {"runtime", "dxmt-runtime"},
        {"d3d9", "dxmt-d3d9"},
        {"d3d10", "dxmt-d3d10"},
        {"d3d11", "dxmt-d3d11"},
        {"d3d12", "dxmt-d3d12"},
        {"tests-framework", "dxmt-wine-tests-framework"},
        {"tests-d3d9", "dxmt-wine-tests-d3d9"},
        {"tests-d3d10", "dxmt-wine-tests-d3d10"},
        {"tests-d3d11", "dxmt-wine-tests-d3d11"},
        {"tests-d3d12", "dxmt-wine-tests-d3d12"},
        {"tests-all", "dxmt-wine-tests"},
        {"benchmarks", "dxmt-benchmarks"},
    };
    std::vector<std::string> result;
    result.reserve(targets.size());
    for (const auto &target : targets) {
      if (!supported.contains(target))
        throw std::runtime_error("unsupported target group: " + target);
      result.push_back(meson_targets.at(target));
    }
    return result;
  }

  int BuildCommand(std::span<const std::string> arguments) const {
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::string> targets;
    const auto name = ParseProfile(arguments, &targets);
    FileLock lock(ProfileLockPath(name));
    const auto profile = EnsureConfigured(name);
    auto command = std::vector<std::string>{"meson", "compile", "-C", profile.build.string()};
    const auto mapped = MapTargets(targets);
    command.insert(command.end(), mapped.begin(), mapped.end());
    RequireSuccess(RunCommand(command, BuildEnvironment()), "Meson build");
    RecordTelemetry("build", name, "success", started);
    return 0;
  }

  static bool PathIsExecutable(const fs::path &path) {
    return IsExecutableFile(path);
  }

  static std::optional<fs::path> FindWineLauncher(const fs::path &root) {
    for (const auto &name : {"wine", "wine64"}) {
      const auto candidate = root / "bin" / name;
      if (PathIsExecutable(candidate))
        return candidate;
    }
    return std::nullopt;
  }

  fs::path ResolveWineRootForTests() const {
    if (const char *path = std::getenv("DXMT_TEST_WINE_ROOT")) {
      if (fs::is_directory(path))
        return fs::canonical(path);
    }
    if (const char *path = std::getenv("DXMT_WINE_ROOT")) {
      if (fs::is_directory(path))
        return fs::canonical(path);
    }
    const auto deps = managed_root_ / "deps";
    if (fs::is_directory(deps)) {
      std::vector<fs::path> matches;
      for (const auto &entry : fs::directory_iterator(deps)) {
        if (!entry.is_directory())
          continue;
        if (!entry.path().filename().string().starts_with("wine-x86_64-"))
          continue;
        if (FindWineLauncher(entry.path()))
          matches.push_back(entry.path());
      }
      if (!matches.empty()) {
        std::sort(matches.begin(), matches.end());
        return fs::canonical(matches.back());
      }
    }
    throw std::runtime_error(
        "no runnable Wine cache found; bootstrap wine-x64 or set DXMT_WINE_ROOT");
  }

  static bool WinePrefixReady(const fs::path &prefix) {
    std::error_code error;
    const auto non_empty = [&](const fs::path &path) {
      return fs::is_regular_file(path, error) && fs::file_size(path, error) > 0 &&
             !error;
    };
    return non_empty(prefix / "system.reg") && non_empty(prefix / "user.reg") &&
           non_empty(prefix / "userdef.reg") &&
           fs::is_regular_file(prefix / ".update-timestamp", error) &&
           fs::exists(prefix / "dosdevices/c:", error);
  }

  // The managed cache keeps the Wine source it built the install from. Match on
  // the header the tests need rather than on a directory name, so a branch
  // rename does not silently disable them.
  fs::path FindManagedWineSource() const {
    const auto sources = managed_root_ / "sources";
    std::error_code error;
    if (!fs::is_directory(sources, error))
      return {};
    for (const auto &entry : fs::directory_iterator(sources, error)) {
      if (!entry.is_directory(error))
        continue;
      if (fs::is_regular_file(entry.path() / "include/wine/test.h", error) &&
          fs::is_directory(entry.path() / "dlls/d3d9/tests", error))
        return entry.path();
    }
    return {};
  }

  Environment WineTestEnvironment(const fs::path &wine_root,
                                  const fs::path &runtime_root,
                                  bool require_runtime_deps) const {
    Environment environment = BuildEnvironment();
    environment["WINEARCH"] = EnvironmentValue("WINEARCH", "win64");
    environment["WINEDEBUG"] = EnvironmentValue("WINEDEBUG", "-all");
    environment["DXMT_EXPERIMENT_DX12_SUPPORT"] =
        EnvironmentValue("DXMT_EXPERIMENT_DX12_SUPPORT", "1");
    // Wine's own conformance modules read this. Their todo_wine marks record
    // where Wine deviates, and a todo block that succeeds counts as a failure,
    // so running as "wine" would score this frontend being more correct than
    // Wine as a regression. Hold the assertions at their strict meaning and let
    // the baseline carry what is not yet met.
    environment["WINETEST_PLATFORM"] =
        EnvironmentValue("WINETEST_PLATFORM", "windows");
    const auto dll_overrides = EnvironmentValue(
        "WINEDLLOVERRIDES",
        "d3d9,d3d10core,d3d11,d3d11_dxmt,d3d12,dxgi,winemetal,winemetal4=n,b");
    // Managed test prefixes must initialize without interactive dependency
    // installers. Wine's mscoree registration can launch
    // `control.exe appwiz.cpl install_mono` while wineboot updates a prefix, so
    // disable both the trigger DLLs and the installer entry points. Keep this
    // rule last so a caller-provided override cannot re-enable the prompts.
    environment["WINEDLLOVERRIDES"] =
        dll_overrides + ";mscoree,mshtml,control.exe,appwiz.cpl=";
    environment["DXMT_TEST_WINE_ROOT"] = wine_root.string();

    if (require_runtime_deps) {
      for (const auto *dylib :
           {"libfreetype.6.dylib", "libgcrypt.20.dylib", "libgmp.10.dylib",
            "libgnutls.30.dylib", "libSDL2-2.0.0.dylib", "libMoltenVK.dylib"}) {
        if (!fs::is_regular_file(wine_root / "lib" / dylib))
          throw std::runtime_error(std::string("Wine runtime missing ") + dylib +
                                   " under " + wine_root.string());
      }
    }

    std::string library_path =
        (wine_root / "lib").string() + ":" +
        (wine_root / "lib/wine/x86_64-unix").string();
    if (!runtime_root.empty()) {
      if (!fs::is_directory(runtime_root / "x86_64-windows") ||
          !fs::is_directory(runtime_root / "x86_64-unix"))
        throw std::runtime_error("invalid DXMT test runtime root: " +
                                 runtime_root.string());
      environment["DXMT_TEST_RUNTIME_ROOT"] = runtime_root.string();
      const char *existing = std::getenv("WINEDLLPATH");
      environment["WINEDLLPATH"] =
          runtime_root.string() +
          (existing && *existing ? std::string(":") + existing : std::string());
      library_path =
          (runtime_root / "x86_64-unix").string() + ":" + library_path;
    }
    const char *fallback = std::getenv("DYLD_FALLBACK_LIBRARY_PATH");
    environment["DYLD_FALLBACK_LIBRARY_PATH"] =
        library_path +
        (fallback && *fallback ? std::string(":") + fallback : std::string());
    if (fs::is_regular_file(wine_root / "lib/libMoltenVK.dylib"))
      environment["WINE_SONAME_LIBVULKAN"] =
          (wine_root / "lib/libMoltenVK.dylib").string();
    return environment;
  }

  // Wine launcher used by Meson tests and benchmarks:
  //   scripts/dxmt-builder wine-exec [--] <exe> [args...]
  int WineExecCommand(std::span<const std::string> arguments) const {
    std::vector<std::string> wine_args;
    for (const auto &argument : arguments) {
      if (argument == "--")
        continue;
      wine_args.push_back(argument);
    }
    if (wine_args.empty())
      throw std::runtime_error("wine-exec requires a command to run under Wine");

    fs::path wine_root;
    fs::path wine_binary;
    if (const char *explicit_wine = std::getenv("DXMT_TEST_WINE")) {
      wine_binary = explicit_wine;
      if (!PathIsExecutable(wine_binary)) {
        if (const auto resolved = FindExecutable(explicit_wine))
          wine_binary = *resolved;
      }
      if (!PathIsExecutable(wine_binary))
        throw std::runtime_error("DXMT_TEST_WINE is not executable");
      wine_root = wine_binary.parent_path().parent_path();
    } else {
      wine_root = ResolveWineRootForTests();
      if (const auto launcher = FindWineLauncher(wine_root))
        wine_binary = *launcher;
      else
        throw std::runtime_error("Wine launcher missing under " +
                                 wine_root.string());
    }
    if (!PathIsExecutable(wine_root / "bin/wineserver"))
      throw std::runtime_error("incomplete Wine root (no wineserver): " +
                               wine_root.string());

    fs::path runtime_root;
    if (const char *path = std::getenv("DXMT_TEST_RUNTIME_ROOT"))
      runtime_root = path;
    const bool require_runtime =
        EnvironmentFlag("DXMT_TEST_REQUIRE_RUNTIME");
    auto environment =
        WineTestEnvironment(wine_root, runtime_root, require_runtime);
    if (const char *prefix = std::getenv("DXMT_TEST_WINEPREFIX"))
      environment["WINEPREFIX"] = prefix;
    else if (const char *prefix = std::getenv("WINEPREFIX"))
      environment["WINEPREFIX"] = prefix;

    std::vector<std::string> command = {wine_binary.string()};
    command.insert(command.end(), wine_args.begin(), wine_args.end());
    const auto result = RunCommand(command, environment);
    return result.status == 0 ? 0 : result.status;
  }

  fs::path StageWineTestRuntime(const ResolvedProfile &profile,
                                std::string_view suite,
                                std::string_view mode) const {
    std::string build_targets;
    std::string install_tags;
    if (suite == "all") {
      build_targets = "dxmt-runtime dxmt-wine-tests";
      install_tags = "runtime-common,runtime-metal3,runtime-metal4,nvext";
    } else if (suite == "framework") {
      build_targets = "dxmt-wine-tests-framework";
    } else if (suite == "d3d9") {
      build_targets = "dxmt-d3d9 dxmt-wine-tests-d3d9";
      install_tags = "runtime-common,runtime-metal3";
    } else if (suite == "d3d10") {
      build_targets = "dxmt-d3d10 dxmt-wine-tests-d3d10";
      install_tags = "runtime-common,runtime-metal3";
    } else if (suite == "d3d11") {
      build_targets = "dxmt-d3d11 dxmt-wine-tests-d3d11";
      install_tags = "runtime-common,runtime-metal3";
    } else if (suite == "d3d12") {
      build_targets = "dxmt-d3d12 dxmt-wine-tests-d3d12";
      install_tags = "runtime-common,runtime-metal4";
    } else {
      throw std::runtime_error("unsupported test suite: " + std::string(suite));
    }
    if (mode == "all" || mode == "integration" || mode == "performance")
      build_targets += " dxmt-benchmarks";

    std::vector<std::string> compile = {"meson", "compile", "-C",
                                        profile.build.string()};
    {
      std::istringstream input{build_targets};
      std::string target;
      while (input >> target)
        compile.push_back(target);
    }
    RequireSuccess(RunCommand(compile, BuildEnvironment()),
                   "compile Wine test targets");

    const auto stage_dir = profile.build / "wine-test-runtime-stage";
    fs::remove_all(stage_dir);
    fs::create_directories(stage_dir);
    if (!install_tags.empty()) {
      auto install_env = BuildEnvironment();
      install_env["DESTDIR"] = stage_dir.string();
      RequireSuccess(
          RunCommand({"meson", "install", "-C", profile.build.string(),
                      "--no-rebuild", "--tags", install_tags},
                     install_env),
          "stage Wine test runtime");
    }

    // Resolve Meson prefix (usually /usr/local).
    const auto prefix_result = RunCommand(
        {"/bin/sh", "-c",
         "meson introspect " + ShellQuote(profile.build.string()) +
             " --buildoptions | python3 -c \""
             "import json,sys\n"
             "for o in json.load(sys.stdin):\n"
             "  if o.get('name')=='prefix':\n"
             "    print(o.get('value') or ''); break\n"
             "\""},
        BuildEnvironment(), true);
    RequireSuccess(prefix_result, "resolve meson prefix");
    std::string prefix = Trim(prefix_result.output);
    if (prefix.empty())
      prefix = "/usr/local";

    const auto runtime_root = testing::StagedInstallRoot(stage_dir, prefix);
    fs::create_directories(runtime_root / "x86_64-windows");
    fs::create_directories(runtime_root / "x86_64-unix");

    std::vector<std::string> required;
    if (suite == "all") {
      required = {"x86_64-windows/d3d9.dll", "x86_64-windows/d3d11.dll",
                  "x86_64-windows/d3d12.dll",
                  "x86_64-windows/dxgi.dll", "x86_64-windows/winemetal.dll",
                  "x86_64-windows/winemetal4.dll", "x86_64-unix/winemetal.so",
                  "x86_64-unix/winemetal4.so"};
    } else if (suite == "d3d9") {
      required = {"x86_64-windows/d3d9.dll", "x86_64-unix/winemetal.so"};
    } else if (suite == "d3d10") {
      required = {"x86_64-windows/d3d10core.dll", "x86_64-windows/d3d11.dll",
                  "x86_64-windows/dxgi.dll", "x86_64-unix/winemetal.so"};
    } else if (suite == "d3d11") {
      required = {"x86_64-windows/d3d11.dll", "x86_64-windows/dxgi.dll",
                  "x86_64-unix/winemetal.so"};
    } else if (suite == "d3d12") {
      required = {"x86_64-windows/d3d12.dll", "x86_64-windows/dxgi.dll",
                  "x86_64-windows/winemetal4.dll", "x86_64-unix/winemetal4.so"};
    }
    for (const auto &relative : required) {
      if (!fs::is_regular_file(runtime_root / relative))
        throw std::runtime_error("staged runtime is missing " + relative);
    }
    return runtime_root;
  }

  void EnsureWinePrefix(const fs::path &wine_root, const fs::path &prefix,
                        const fs::path &runtime_root) const {
    fs::create_directories(prefix);
    const auto wine = FindWineLauncher(wine_root);
    if (!wine)
      throw std::runtime_error("Wine launcher missing under " +
                               wine_root.string());
    const auto version = RunCommand({wine->string(), "--version"},
                                    {{"WINEPREFIX", prefix.string()}}, true);
    RequireSuccess(version, "wine --version");
    const auto identity = Trim(version.output);
    const auto marker = prefix / ".dxmt-test-ready";
    std::string previous;
    if (fs::is_regular_file(marker)) {
      std::ifstream input(marker);
      std::getline(input, previous);
    }
    if (WinePrefixReady(prefix) && previous == identity) {
      RunCommand({(wine_root / "bin/wineserver").string(), "-k"},
                 {{"WINEPREFIX", prefix.string()}}, true);
      RunCommand({(wine_root / "bin/wineserver").string(), "-w"},
                 {{"WINEPREFIX", prefix.string()}}, true);
      return;
    }

    auto environment = WineTestEnvironment(wine_root, runtime_root, false);
    environment["WINEPREFIX"] = prefix.string();
    environment["DXMT_TEST_REQUIRE_RUNTIME"] = "0";
    // Initialize prefix via wineboot under wine-exec semantics.
    std::vector<std::string> boot = {wine->string(), "wineboot", "-u"};
    // Run wineboot synchronously; wineserver stays up for the test suite.
    RequireSuccess(RunCommand(boot, environment), "wineboot -u");
    WriteFileAtomic(marker, identity + "\n");
    RunCommand({(wine_root / "bin/wineserver").string(), "-w"},
               {{"WINEPREFIX", prefix.string()}}, true);
  }

#ifdef _WIN32
  fs::path BuildWindowsOracle(std::string_view profile_name) const {
    FileLock lock(ProfileLockPath(profile_name));
    const auto build = managed_root_ / "windows-oracle" /
                       std::string(profile_name) / "build-meson-clang";
    const auto meson = RequireExecutable("meson");
    const Environment toolchain = {
        {"CC", "clang-cl"},       {"CXX", "clang-cl"},
        {"CC_LD", "lld-link"},   {"CXX_LD", "lld-link"},
        {"AR", "llvm-lib"},
    };
    std::vector<std::string> setup = {
        meson.string(), "setup", build.string(), repo_root_.string(),
        "--backend=ninja", "--buildtype=release",
        "-Dwindows_oracle_only=true",
    };
    if (fs::is_regular_file(build / "meson-private/coredata.dat"))
      setup.emplace_back("--reconfigure");
    RequireSuccess(RunCommand(setup, toolchain),
                   "native Windows oracle configuration");
    RequireSuccess(
        RunCommand({meson.string(), "compile", "-C", build.string(),
                    "dxmt-wine-d3d9-tests", "dxmt-wine-d3d10-tests",
                    "dxmt-wine-d3d11-tests", "dxmt-wine-d3d12-tests"},
                   toolchain),
        "native Windows oracle build");

    const auto output = build / "tests";
    for (const auto &name : {"dxmt-wine-d3d9-tests.exe",
                             "dxmt-wine-d3d10-tests.exe",
                             "dxmt-wine-d3d11-tests.exe",
                             "dxmt-wine-d3d12-tests.exe",
                             "shader_oracle_baseline.txt",
                             "run-windows-oracle.bat"}) {
      if (!fs::is_regular_file(output / name))
        throw std::runtime_error("native Windows oracle output is missing: " +
                                 (output / name).string());
    }
    return output;
  }

  int TestWindowsOracle(std::span<const std::string> arguments) const {
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::string> remaining;
    const auto name = ParseProfile(arguments, &remaining);
    if (remaining.size() != 1 ||
        (remaining.front() != "unit" && remaining.front() != "all"))
      throw std::runtime_error(
          "native Windows builder tests require the existing unit or all mode");

    const auto output = BuildWindowsOracle(name);
    const auto cmd = RequireExecutable("cmd");
    const auto result = RunCommand(
        {cmd.string(), "/d", "/c", "call run-windows-oracle.bat"}, {}, false,
        output);
    RequireSuccess(result, "native Windows D3D oracle suites");
    RecordTelemetry("test", name + ":unit:windows-oracle", "success",
                    started);
    std::cout << (output / "windows-oracle-results").string() << '\n';
    return 0;
  }

  int PackageWindowsOracle(
      const std::string &name, const std::optional<fs::path> &destination,
      std::chrono::steady_clock::time_point started) const {
    const auto oracle = BuildWindowsOracle(name);
    const auto commit = RunCommand(
        {"git", "-C", repo_root_.string(), "rev-parse", "HEAD"}, {}, true);
    RequireSuccess(commit, "Windows oracle source identity");
    const auto short_commit = Trim(commit.output).substr(0, 12);
    const auto output = destination.value_or(
        managed_root_ / "artifacts/local-1" / name /
        ("dxmt-windows-oracle-" + short_commit + ".zip"));
    if (output.extension() != ".zip")
      throw std::runtime_error("Windows oracle destination must end in .zip");
    fs::create_directories(output.parent_path());

    const auto staging = managed_root_ / "artifacts/.staging" /
                         ("windows-oracle-" + std::to_string(ProcessId()) +
                          "-" + std::to_string(std::chrono::steady_clock::now()
                                                   .time_since_epoch()
                                                   .count()));
    fs::create_directories(staging);
    for (const auto &name : {"dxmt-wine-d3d9-tests.exe",
                             "dxmt-wine-d3d10-tests.exe",
                             "dxmt-wine-d3d11-tests.exe",
                             "dxmt-wine-d3d12-tests.exe",
                             "shader_oracle_baseline.txt",
                             "run-windows-oracle.bat"}) {
      fs::copy_file(oracle / name, staging / name,
                    fs::copy_options::overwrite_existing);
    }

    std::ostringstream instructions;
    instructions
        << "DXMT native Windows D3D behavior oracle\n\n"
        << "Suite schema: public-api-v1\n"
        << "Source commit: " << Trim(commit.output) << "\n"
        << "Builder profile: " << name << "\n\n"
        << "Run run-windows-oracle.bat from a Windows command prompt.\n";
    WriteFileAtomic(staging / "README.txt", instructions.str());

    std::ostringstream digests;
    for (const auto &file : {"dxmt-wine-d3d9-tests.exe",
                             "dxmt-wine-d3d10-tests.exe",
                             "dxmt-wine-d3d11-tests.exe",
                             "dxmt-wine-d3d12-tests.exe",
                             "run-windows-oracle.bat"}) {
      digests << Sha256File(staging / file) << "  " << file << '\n';
    }
    WriteFileAtomic(staging / "SHA256SUMS.txt", digests.str());

    const auto incomplete = output.parent_path() /
                            ("." + output.stem().string() + ".incomplete-" +
                             std::to_string(ProcessId()) + ".zip");
    const auto tar = RequireExecutable("tar");
    RequireSuccess(
        RunCommand({tar.string(), "-a", "-c", "-f", incomplete.string(),
                    "dxmt-wine-d3d9-tests.exe",
                    "dxmt-wine-d3d10-tests.exe",
                    "dxmt-wine-d3d11-tests.exe",
                    "dxmt-wine-d3d12-tests.exe", "shader_oracle_baseline.txt",
                    "run-windows-oracle.bat", "README.txt", "SHA256SUMS.txt"},
                   {}, false, staging),
        "Windows oracle archive");
    RequireSuccess(RunCommand({tar.string(), "-t", "-f",
                               incomplete.string()}),
                   "Windows oracle archive verification");
    std::error_code error;
    fs::remove(output, error);
    fs::rename(incomplete, output);
    fs::remove_all(staging);
    RecordTelemetry("package", name + ":windows-oracle", "success", started);
    std::cout << output.string() << '\n';
    return 0;
  }
#endif

  int TestCommand(std::span<const std::string> arguments) const {
#ifdef _WIN32
    return TestWindowsOracle(arguments);
#else
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::string> remaining;
    const auto name = ParseProfile(arguments, &remaining);
    FileLock lock(ProfileLockPath(name));
    const auto profile = EnsureConfigured(name);
    if (!profile.profile->tests)
      throw std::runtime_error("profile does not enable tests: " + name);

    std::string mode = "all";
    std::string suite = "all";
    std::vector<std::string> forwarded;
    for (std::size_t index = 0; index < remaining.size(); ++index) {
      if (remaining[index] == "all" || remaining[index] == "unit" ||
          remaining[index] == "integration" ||
          remaining[index] == "performance") {
        mode = remaining[index];
      } else if (remaining[index] == "--suite") {
        if (++index >= remaining.size())
          throw std::runtime_error("--suite requires a value");
        suite = remaining[index];
      } else if (remaining[index].starts_with("--suite=")) {
        suite = remaining[index].substr(8);
      } else if (remaining[index] == "--test-args") {
        if (++index >= remaining.size())
          throw std::runtime_error("--test-args requires a value");
        forwarded.push_back("--test-args=" + remaining[index]);
      } else if (remaining[index].starts_with("--test-args=")) {
        forwarded.push_back(remaining[index]);
      } else {
        throw std::runtime_error("unexpected test argument: " + remaining[index]);
      }
    }
    static const std::set<std::string> suites = {
        "all", "builder", "framework", "d3d9", "d3d10", "d3d11", "d3d12",
        "d3d12-model"};
    if (!suites.contains(suite))
      throw std::runtime_error("unsupported test suite: " + suite);
    if (suite == "builder") {
      if (mode != "unit" && mode != "all")
        throw std::runtime_error("builder supports only unit or all mode");
      const auto builder_tests = BuilderBinary().parent_path() /
                                 "dxmt-builder-tests";
      RequireSuccess(RunCommand({builder_tests.string()}, BuildEnvironment()),
                     "DXMT Builder self-test");
      RecordTelemetry("test", name + ":unit:builder", "success", started);
      return 0;
    }
    if (suite == "d3d12-model") {
      if (mode != "unit" && mode != "all")
        throw std::runtime_error(
            "d3d12-model supports only unit or all mode");
      RequireSuccess(
          // The d3d12-model suite contains both native test binaries; meson test
          // runs the whole suite with --no-rebuild, so every member has to be
          // compiled here or the run aborts on the missing executable.
          RunCommand({"meson", "compile", "-C", profile.build.string(),
                      "src/d3d12/dxmt-d3d12-submission-model-tests",
                      "src/d3d12/dxmt-d3d12-copy-geometry-tests"},
                     BuildEnvironment()),
          "native D3D12 model test build");
      RequireSuccess(
          RunCommand({"meson", "test", "-C", profile.build.string(),
                      "--no-rebuild", "--suite", "d3d12-model",
                      "--print-errorlogs"},
                     BuildEnvironment()),
          "native D3D12 submission model test");
      RecordTelemetry("test", name + ":unit:d3d12-model", "success",
                      started);
      return 0;
    }

    const auto runtime_root = StageWineTestRuntime(profile, suite, mode);
    const auto wine_root =
        profile.wine_root.empty() ? ResolveWineRootForTests() : profile.wine_root;
    EnsureWinePrefix(wine_root, profile.prefix, runtime_root);

    auto environment = BuildEnvironment();
    environment["DXMT_TEST_WINEPREFIX"] = profile.prefix.string();
    environment["DXMT_TEST_WINE_ROOT"] = wine_root.string();
    environment["DXMT_TEST_RUNTIME_ROOT"] = runtime_root.string();
    environment["DXMT_TEST_REQUIRE_RUNTIME"] = "1";
    environment["WINEPREFIX"] = profile.prefix.string();

    if (mode == "all" || mode == "unit") {
      RequireSuccess(
          // The d3d12-model suite contains both native test binaries; meson test
          // runs the whole suite with --no-rebuild, so every member has to be
          // compiled here or the run aborts on the missing executable.
          RunCommand({"meson", "compile", "-C", profile.build.string(),
                      "src/d3d12/dxmt-d3d12-submission-model-tests",
                      "src/d3d12/dxmt-d3d12-copy-geometry-tests"},
                     BuildEnvironment()),
          "native D3D12 model test build");
      RequireSuccess(
          RunCommand({"meson", "test", "-C", profile.build.string(),
                      "--no-rebuild", "--suite", "d3d12-model",
                      "--print-errorlogs"},
                     BuildEnvironment()),
          "native D3D12 submission model test");
      RequireSuccess(
          RunCommand({"meson", "test", "-C", profile.build.string(),
                      "--no-rebuild", "--suite", "repository",
                      "--print-errorlogs"}),
          "repository policy tests");

      std::string scheduler_args = "--dxmt-test-suite=" + suite;
      for (const auto &argument : forwarded) {
        if (argument.starts_with("--test-args="))
          scheduler_args += " " + argument.substr(std::string("--test-args=").size());
      }
      std::vector<std::string> command = {
          "meson", "test", "-C", profile.build.string(), "--no-rebuild",
          "--suite", "wine", "--print-errorlogs",
          "--test-args=" + scheduler_args,
      };
      RequireSuccess(RunCommand(command, environment), "Wine unit tests");
    }
    if (mode == "all" || mode == "integration" || mode == "performance") {
      std::string benchmark_suite = "wine";
      if (mode == "performance")
        benchmark_suite = "performance";
      else if (mode == "integration" && suite != "all")
        benchmark_suite = suite;
      std::vector<std::string> command = {
          "meson", "test", "-C", profile.build.string(), "--no-rebuild",
          "--benchmark", "--suite", benchmark_suite, "--print-errorlogs",
      };
      command.insert(command.end(), forwarded.begin(), forwarded.end());
      RequireSuccess(RunCommand(command, environment), "Wine benchmark tests");
    }

    if (mode == "all" && suite == "all") {
      const auto commit = RunCommand(
          {"git", "-C", repo_root_.string(), "rev-parse", "HEAD"}, {}, true);
      RequireSuccess(commit, "source commit identity");
      std::ostringstream properties;
      properties << "schema=1\ncommit=" << Trim(commit.output)
                 << "\nprofile=" << name
                 << "\nfingerprint=" << profile.fingerprint
                 << "\nruntime_sha256=" << DirectoryDigest(runtime_root)
                 << "\ntests_passed=true\n";
      WriteFileAtomic(profile.meta / "tests-passed.properties", properties.str());
    }
    RecordTelemetry("test", name + ":" + mode + ":" + suite, "success",
                    started);
    return 0;
#endif
  }

  int PackageCommand(std::span<const std::string> arguments) const {
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::string> remaining;
    const auto name = ParseProfile(arguments, &remaining);
    if (remaining.empty() || remaining.front() != "windows-oracle")
      throw std::runtime_error("package requires the windows-oracle subject");

    std::optional<fs::path> destination;
    for (std::size_t index = 1; index < remaining.size(); ++index) {
      if (remaining[index] == "--dest") {
        if (++index >= remaining.size())
          throw std::runtime_error("--dest requires a value");
        destination = fs::absolute(remaining[index]).lexically_normal();
      } else {
        throw std::runtime_error("unexpected package argument: " +
                                 remaining[index]);
      }
    }

#ifdef _WIN32
    return PackageWindowsOracle(name, destination, started);
#else
    FileLock lock(ProfileLockPath(name));
    const auto profile = EnsureConfigured(name);
    const auto compile = std::vector<std::string>{
        "meson", "compile", "-C", profile.build.string(),
        "dxmt-wine-tests-d3d9", "dxmt-wine-tests-d3d10",
        "dxmt-wine-tests-d3d11", "dxmt-wine-tests-d3d12"};
    RequireSuccess(RunCommand(compile, BuildEnvironment()),
                   "Windows oracle prerequisite build");

    const std::array executables = {
        profile.build / "tests/dxmt-wine-d3d9-tests.exe",
        profile.build / "tests/dxmt-wine-d3d10-tests.exe",
        profile.build / "tests/dxmt-wine-d3d11-tests.exe",
        profile.build / "tests/dxmt-wine-d3d12-tests.exe",
    };
    const auto script = profile.build / "tests/run-windows-oracle.bat";
    for (const auto &required :
         {executables[0], executables[1], executables[2], executables[3],
          script}) {
      if (!fs::is_regular_file(required))
        throw std::runtime_error("Windows oracle artifact is missing: " +
                                 required.string());
    }

    const auto commit = RunCommand(
        {"git", "-C", repo_root_.string(), "rev-parse", "HEAD"}, {}, true);
    RequireSuccess(commit, "Windows oracle source identity");
    const auto short_commit = Trim(commit.output).substr(0, 12);
    const auto artifact_root = managed_root_ / "artifacts/local-1" / name;
    const auto output = destination.value_or(
        artifact_root /
        ("dxmt-windows-oracle-" + short_commit + ".zip"));
    if (output.extension() != ".zip")
      throw std::runtime_error("Windows oracle destination must end in .zip");
    fs::create_directories(output.parent_path());

    const auto staging =
        managed_root_ / "artifacts/.staging" /
        ("windows-oracle-" + std::to_string(ProcessId()) + "-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
    fs::create_directories(staging);
    const auto readme = staging / "README.txt";
    const auto checksums = staging / "SHA256SUMS.txt";
    std::ostringstream instructions;
    instructions
        << "DXMT D3D9, D3D10, D3D11, and D3D12 Windows behavior oracle\n\n"
        << "Suite schema: public-api-v1\n\n"
        << "Keep all four EXEs, the baseline, and the batch script in the same\n"
        << "directory.\n"
        << "Do not copy DXMT Direct3D or DXGI DLLs beside the EXEs.\n\n"
        << "Default run (complete D3D10/D3D11/D3D12 suites):\n"
        << "  run-windows-oracle.bat\n\n"
        << "Complete D3D12 suite with the Debug Layer:\n"
        << "  run-windows-oracle.bat --debug\n\n"
        << "Use the default hardware adapter for D3D12:\n"
        << "  run-windows-oracle.bat --hardware\n\n"
        << "Source commit: " << Trim(commit.output) << "\n"
        << "Builder profile: " << name << "\n";
    WriteFileAtomic(readme, instructions.str());

    std::ostringstream digest_manifest;
    digest_manifest << Sha256File(executables[0])
                    << "  dxmt-wine-d3d9-tests.exe\n"
                    << Sha256File(executables[1])
                    << "  dxmt-wine-d3d10-tests.exe\n"
                    << Sha256File(executables[2])
                    << "  dxmt-wine-d3d11-tests.exe\n"
                    << Sha256File(executables[3])
                    << "  dxmt-wine-d3d12-tests.exe\n"
                    << Sha256File(script) << "  run-windows-oracle.bat\n";
    WriteFileAtomic(checksums, digest_manifest.str());

    const auto incomplete =
        output.parent_path() /
        ("." + output.stem().string() + ".incomplete-" +
         std::to_string(ProcessId()) + ".zip");
    const std::vector<std::string> zip_command = {
        "/usr/bin/zip", "-j", "-9", incomplete.string(),
        executables[0].string(), executables[1].string(),
        executables[2].string(), script.string(), readme.string(),
        checksums.string()};
    RequireSuccess(RunCommand(zip_command), "Windows oracle archive");
    if (!fs::is_regular_file(incomplete) || fs::file_size(incomplete) == 0)
      throw std::runtime_error("Windows oracle archive was not created");
    RequireSuccess(RunCommand({"/usr/bin/unzip", "-tq", incomplete.string()}),
                   "Windows oracle archive verification");
    fs::rename(incomplete, output);
    fs::remove_all(staging);

    RecordTelemetry("package", name + ":windows-oracle", "success", started);
    std::cout << output.string() << '\n';
    return 0;
#endif
  }

  int InstallCommand(std::span<const std::string> arguments) const {
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::string> remaining;
    const auto name = ParseProfile(arguments, &remaining);
    FileLock lock(ProfileLockPath(name));
    auto profile = EnsureConfigured(name);
    std::string component = "runtime";
    std::optional<fs::path> destination;
    for (std::size_t index = 0; index < remaining.size(); ++index) {
      if (remaining[index] == "--component") {
        if (++index >= remaining.size())
          throw std::runtime_error("--component requires a value");
        component = remaining[index];
      } else if (remaining[index] == "--dest") {
        if (++index >= remaining.size())
          throw std::runtime_error("--dest requires a value");
        destination = fs::absolute(remaining[index]);
      } else {
        throw std::runtime_error("unexpected install argument: " + remaining[index]);
      }
    }
    const auto targets = MapTargets({component});
    auto compile = std::vector<std::string>{"meson", "compile", "-C", profile.build.string()};
    compile.insert(compile.end(), targets.begin(), targets.end());
    RequireSuccess(RunCommand(compile, BuildEnvironment()), "install prerequisite build");

    const auto dest = destination.value_or(profile.install);
    if (!destination) {
      if (!IsPathWithin(dest, managed_root_))
        throw std::runtime_error("internal install path escaped managed cache");
      fs::remove_all(dest);
    }
    fs::create_directories(dest);
    std::string tags;
    if (component == "d3d12")
      tags = "runtime-common,runtime-metal4";
    else if (component == "d3d9" || component == "d3d10" ||
             component == "d3d11")
      tags = "runtime-common,runtime-metal3";
    else
      tags = "runtime-common,runtime-metal3,runtime-metal4,nvext";
    const std::vector<std::string> command = {
        "meson", "install", "-C", profile.build.string(), "--no-rebuild",
        "--destdir", dest.string(), "--tags", tags,
    };
    RequireSuccess(RunCommand(command, BuildEnvironment()), "Meson install");
    const auto runtime_root = dest / "usr/local";
    const auto runtime_digest = DirectoryDigest(runtime_root);
    if (EnvironmentFlag("DXMT_BUILDER_REQUIRE_TESTED")) {
      const auto tested = ReadProperties(profile.meta / "tests-passed.properties");
      const auto commit = RunCommand(
          {"git", "-C", repo_root_.string(), "rev-parse", "HEAD"}, {}, true);
      RequireSuccess(commit, "source commit identity");
      if (!tested.contains("tests_passed") || tested.at("tests_passed") != "true" ||
          !tested.contains("commit") || tested.at("commit") != Trim(commit.output) ||
          !tested.contains("fingerprint") ||
          tested.at("fingerprint") != profile.fingerprint ||
          !tested.contains("runtime_sha256") ||
          tested.at("runtime_sha256") != runtime_digest)
        throw std::runtime_error(
            "installed runtime does not match the successfully tested commit/profile");
    }
    const auto run_key = EnvironmentValue("GITHUB_RUN_ID", "local") + "-" +
                         EnvironmentValue("GITHUB_RUN_ATTEMPT", "1");
    const auto artifact_meta = managed_root_ / "artifacts" / run_key /
                               std::string(profile.profile->name) / "runtime.properties";
    std::ostringstream artifact;
    artifact << "schema=1\nprofile=" << profile.profile->name
             << "\nfingerprint=" << profile.fingerprint
             << "\nruntime_sha256=" << runtime_digest << '\n';
    WriteFileAtomic(artifact_meta, artifact.str());
    RecordTelemetry("install", name + ":" + component, "success", started);
    std::cout << (dest / "usr/local").string() << '\n';
    return 0;
  }

  int RestorePackageCommand(std::span<const std::string> arguments) const {
    const auto started = std::chrono::steady_clock::now();
    std::optional<fs::path> source_argument;
    std::optional<fs::path> destination_argument;
    std::optional<fs::path> backup_argument;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto parse_path = [&](std::optional<fs::path> &output,
                                  std::string_view option) {
        if (++index >= arguments.size())
          throw std::runtime_error(std::string(option) + " requires a value");
        if (output)
          throw std::runtime_error(std::string(option) + " was specified twice");
        output = fs::absolute(arguments[index]).lexically_normal();
      };
      if (arguments[index] == "--source")
        parse_path(source_argument, "--source");
      else if (arguments[index] == "--dest")
        parse_path(destination_argument, "--dest");
      else if (arguments[index] == "--backup")
        parse_path(backup_argument, "--backup");
      else
        throw std::runtime_error("unexpected restore-package argument: " +
                                 arguments[index]);
    }
    if (!source_argument || !destination_argument || !backup_argument)
      throw std::runtime_error(
          "restore-package requires --source, --dest, and --backup");

    const auto source = *source_argument;
    const auto destination = *destination_argument;
    const auto backup = *backup_argument;
    if (!fs::is_directory(source))
      throw std::runtime_error("restore source is not a directory: " +
                               source.string());
    if (source == destination || IsPathWithin(source, destination) ||
        IsPathWithin(destination, source))
      throw std::runtime_error("restore source and destination overlap");
    if (backup.parent_path() != destination.parent_path())
      throw std::runtime_error(
          "restore backup must be a sibling of the destination");
    if (backup == destination)
      throw std::runtime_error("restore backup equals destination");
    if (fs::exists(backup))
      throw std::runtime_error("restore backup already exists: " +
                               backup.string());

    const std::array<fs::path, 13> required_files = {
        "manifest.json",
        "wine/x86_64-unix/winemetal.so",
        "wine/x86_64-unix/winemetal4.so",
        "wine/x86_64-windows/d3d10core.dll",
        "wine/x86_64-windows/d3d11.dll",
        "wine/x86_64-windows/d3d11_dxmt.dll",
        "wine/x86_64-windows/d3d12.dll",
        "wine/x86_64-windows/d3d12core.dll",
        "wine/x86_64-windows/dxgi.dll",
        "wine/x86_64-windows/nvapi64.dll",
        "wine/x86_64-windows/nvngx.dll",
        "wine/x86_64-windows/winemetal.dll",
        "wine/x86_64-windows/winemetal4.dll",
    };
    for (const auto &relative : required_files) {
      if (!fs::is_regular_file(source / relative))
        throw std::runtime_error("restore package is incomplete; missing " +
                                 relative.generic_string());
    }

    fs::create_directories(destination.parent_path());
    const auto staging =
        destination.parent_path() /
        ("." + destination.filename().string() + ".restore-incomplete-" +
         std::to_string(ProcessId()));
    if (fs::exists(staging))
      throw std::runtime_error("restore staging path already exists: " +
                               staging.string());

    const auto source_digest = DirectoryDigest(source);
    bool destination_moved = false;
    try {
      fs::copy(source, staging,
               fs::copy_options::recursive | fs::copy_options::copy_symlinks);
      if (DirectoryDigest(staging) != source_digest)
        throw std::runtime_error("staged restore package digest mismatch");
      if (fs::exists(destination)) {
        fs::rename(destination, backup);
        destination_moved = true;
      }
      fs::rename(staging, destination);
    } catch (...) {
      std::error_code ignored;
      fs::remove_all(staging, ignored);
      if (destination_moved && !fs::exists(destination))
        fs::rename(backup, destination, ignored);
      throw;
    }
    if (DirectoryDigest(destination) != source_digest)
      throw std::runtime_error("restored package digest mismatch");

    RecordTelemetry("restore-package", destination.string(), "success", started);
    std::cout << "source=" << source.string()
              << "\ndestination=" << destination.string()
              << "\nbackup="
              << (destination_moved ? backup.string() : std::string("<none>"))
              << "\nruntime_sha256=" << source_digest << '\n';
    return 0;
  }

  int CacheCommand(std::span<const std::string> arguments) const {
    if (arguments.empty())
      throw std::runtime_error("cache requires a subcommand");
    EnsureManagedLayout();
    if (arguments.front() == "status") {
      const bool json = arguments.size() > 1 && arguments[1] == "--json";
      const auto managed = DirectorySize(managed_root_);
      if (json) {
        std::cout << "{\"managed_bytes\":" << managed
                  << ",\"scope\":\"" << JsonEscape(managed_root_.string())
                  << "\",\"profile_namespace\":\""
                  << JsonEscape(profile_namespace_) << "\",\"profiles\":[";
        bool first = true;
        for (const auto &profile : Profiles()) {
          if (!first)
            std::cout << ',';
          first = false;
          const auto size = DirectorySize(ProfileRoot(profile.name));
          std::cout << "{\"name\":\"" << JsonEscape(profile.name)
                    << "\",\"bytes\":" << size << '}';
        }
        std::cout << "]}\n";
      } else {
        std::cout << "managed cache: " << HumanSize(managed) << '\n';
        if (!profile_namespace_.empty())
          std::cout << "profile namespace: " << profile_namespace_ << '\n';
        for (const auto &profile : Profiles()) {
          const auto size = DirectorySize(ProfileRoot(profile.name));
          if (size != 0)
            std::cout << "  " << profile.name << ": " << HumanSize(size) << '\n';
        }
        if (const auto ccache = FindExecutable("ccache")) {
          auto environment = BuildEnvironment();
          const auto stats = RunCommand({ccache->string(), "--show-stats"}, environment, true);
          if (stats.status == 0)
            std::cout << stats.output;
        }
      }
      return 0;
    }
    if (arguments.front() == "verify") {
      std::size_t incomplete = 0;
      for (fs::recursive_directory_iterator iterator(
               managed_root_, fs::directory_options::skip_permission_denied), end;
           iterator != end; ++iterator) {
        const auto name = iterator->path().filename().string();
        if (IsIncompleteCacheName(name)) {
          std::cerr << "incomplete cache entry: " << iterator->path() << '\n';
          ++incomplete;
        }
      }
      if (incomplete != 0)
        return 1;
      std::cout << "managed cache verified\n";
      return 0;
    }
    if (arguments.front() == "prune") {
      const bool apply = std::find(arguments.begin(), arguments.end(), "--apply") != arguments.end();
      std::size_t candidates = 0;
      std::vector<fs::path> paths;
      for (fs::recursive_directory_iterator iterator(
               managed_root_, fs::directory_options::skip_permission_denied), end;
           iterator != end; ++iterator) {
        const auto name = iterator->path().filename().string();
        if (IsIncompleteCacheName(name)) {
          paths.push_back(iterator->path());
          if (iterator->is_directory())
            iterator.disable_recursion_pending();
        }
      }
      std::sort(paths.begin(), paths.end(), [](const auto &left, const auto &right) {
        return left.native().size() > right.native().size();
      });
      for (const auto &path : paths) {
        std::cout << (apply ? "remove " : "would remove ") << path << '\n';
        if (apply)
          fs::remove_all(path);
        ++candidates;
      }
      std::cout << candidates << " incomplete cache entries "
                << (apply ? "removed" : "found") << '\n';
      return 0;
    }
    if (arguments.front() == "clean") {
      std::string profile;
      for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--profile" && ++index < arguments.size())
          profile = arguments[index];
      }
      if (FindProfile(profile) == nullptr)
        throw std::runtime_error("cache clean requires a valid --profile");
      const auto path = ProfileRoot(profile);
      if (!IsPathWithin(path, managed_root_))
        throw std::runtime_error("refusing to clean an unmanaged path");
      FileLock lock(ProfileLockPath(profile));
      fs::remove_all(path);
      std::cout << "removed managed profile " << profile << '\n';
      return 0;
    }
    throw std::runtime_error("unknown cache subcommand: " + arguments.front());
  }

  int InternalCommand(std::span<const std::string> arguments) const {
    if (arguments.empty())
      throw std::runtime_error("unknown internal command");
    if (arguments.front() == "lock-command") {
      std::string name;
      std::vector<std::string> command;
      bool after_separator = false;
      for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (after_separator) {
          command.push_back(arguments[index]);
        } else if (arguments[index] == "--") {
          after_separator = true;
        } else if (arguments[index] == "--name" && ++index < arguments.size()) {
          name = arguments[index];
        } else {
          throw std::runtime_error("invalid lock-command argument");
        }
      }
      if (name.empty() || command.empty() ||
          name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") !=
              std::string::npos)
        throw std::runtime_error("lock-command requires a safe name and a command");
      FileLock lock(managed_root_ / "locks" / (name + ".lock"));
      RequireSuccess(RunCommand(command, BuildEnvironment()), "locked command");
      return 0;
    }
    if (arguments.front() != "cache-command")
      throw std::runtime_error("unknown internal command");
    std::string cache_namespace;
    fs::path output;
    fs::path input;
    std::vector<std::string> command;
    bool after_separator = false;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
      if (after_separator) {
        command.push_back(arguments[index]);
      } else if (arguments[index] == "--") {
        after_separator = true;
      } else if (arguments[index] == "--namespace" && ++index < arguments.size()) {
        cache_namespace = arguments[index];
      } else if (arguments[index] == "--output" && ++index < arguments.size()) {
        output = arguments[index];
      } else if (arguments[index] == "--input" && ++index < arguments.size()) {
        input = arguments[index];
      } else {
        throw std::runtime_error("invalid cache-command argument");
      }
    }
    if (cache_namespace.empty() || output.empty() || input.empty() || command.empty())
      throw std::runtime_error("cache-command requires namespace, input, output and command");
    if (!fs::is_regular_file(input))
      throw std::runtime_error("cache-command input is missing: " + input.string());

    std::ostringstream identity;
    identity << "schema=1\nnamespace=" << cache_namespace
             << "\ninput_sha256=" << Sha256File(input);
    for (const auto &argument : command) {
      if (argument == output.string())
        identity << "\narg=<output>";
      else if (argument == input.string())
        identity << "\narg=<input>";
      else
        identity << "\narg=" << argument;
    }
    if (const auto tool = FindExecutable(command.front())) {
      const auto status = fs::status(*tool);
      identity << "\ntool=" << tool->string()
               << "\ntool_size=" << fs::file_size(*tool)
               << "\ntool_mtime="
               << static_cast<long long>(
                      fs::last_write_time(*tool).time_since_epoch().count())
               << "\ntool_type=" << static_cast<int>(status.type());
    }
    if (fs::path(command.front()).filename() == "xcrun") {
      std::string sdk = "macosx";
      std::string program;
      for (std::size_t index = 1; index < command.size(); ++index) {
        if ((command[index] == "-sdk" || command[index] == "--sdk") &&
            index + 1 < command.size()) {
          sdk = command[++index];
        } else if (!command[index].starts_with('-')) {
          program = command[index];
          break;
        }
      }
      if (!program.empty()) {
        const auto resolved = RunCommand(
            {"xcrun", "--sdk", sdk, "--find", program}, {}, true);
        RequireSuccess(resolved, "Xcode tool identity");
        const fs::path tool_path = Trim(resolved.output);
        if (fs::is_regular_file(tool_path))
          AppendFileIdentity(identity, "xcode_tool", tool_path);
      }
    }
    const auto key = Sha256(identity.str());
    const auto cache_dir = managed_root_ / "cas" / cache_namespace;
    const auto entry = cache_dir / key;
    const auto started = std::chrono::steady_clock::now();
    FileLock lock(managed_root_ / "locks" / ("cas-" + key + ".lock"));
    fs::create_directories(output.parent_path());
    if (fs::is_regular_file(entry)) {
      fs::copy_file(entry, output, fs::copy_options::overwrite_existing);
      RecordTelemetry("cas", cache_namespace, "hit", started);
      return 0;
    }

    fs::create_directories(cache_dir);
    RequireSuccess(RunCommand(command, BuildEnvironment()), "cached command");
    if (!fs::is_regular_file(output))
      throw std::runtime_error("cached command did not produce " + output.string());
    const auto temporary =
        entry.string() + ".tmp-" + std::to_string(ProcessId());
    fs::copy_file(output, temporary, fs::copy_options::overwrite_existing);
    fs::rename(temporary, entry);
    RecordTelemetry("cas", cache_namespace, "miss", started);
    return 0;
  }

  fs::path repo_root_;
  fs::path managed_root_;
  std::string profile_namespace_;
  fs::path config_path_;
};

} // namespace

const std::vector<Profile> &Profiles() {
  // Product policy: Windows PE is x86_64 only (Wine WoW64 for 32-bit apps).
  // No native i386 DXMT profiles.
  static const std::vector<Profile> profiles = {
      {"gcc-x64-release-full", "x64", "gcc", "release", true, true, true, true, false},
      {"llvm-mingw-x64-release", "x64", "llvm-mingw", "release", true, true, true, true, false},
      {"llvm-mingw-x64-debugoptimized", "x64", "llvm-mingw", "debugoptimized", true, false, true, true, true},
      {"apple-clang-x86_64-release", "x86_64", "apple-clang", "release", false, false, false, false, false},
  };
  return profiles;
}

const Profile *FindProfile(std::string_view name) {
  const auto &profiles = Profiles();
  const auto match = std::find_if(profiles.begin(), profiles.end(),
                                  [name](const Profile &profile) {
                                    return profile.name == name;
                                  });
  return match == profiles.end() ? nullptr : &*match;
}

std::string JsonEscape(std::string_view value) {
  std::string result;
  for (const char character : value) {
    switch (character) {
    case '\\': result += "\\\\"; break;
    case '"': result += "\\\""; break;
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    default: result.push_back(character); break;
    }
  }
  return result;
}

bool IsPathWithin(const fs::path &path, const fs::path &root) {
  const auto normalized_path = fs::absolute(path).lexically_normal();
  const auto normalized_root = fs::absolute(root).lexically_normal();
  auto path_iterator = normalized_path.begin();
  for (auto root_iterator = normalized_root.begin(); root_iterator != normalized_root.end();
       ++root_iterator, ++path_iterator) {
    if (path_iterator == normalized_path.end() || *path_iterator != *root_iterator)
      return false;
  }
  return true;
}

namespace testing {

fs::path CcacheRoot(const fs::path &managed_root,
                    std::string_view profile_namespace) {
  auto root = managed_root / "ccache";
  if (!profile_namespace.empty())
    root /= fs::path(profile_namespace);
  return root;
}

std::map<std::string, std::string> ParseProperties(std::string_view contents) {
  std::map<std::string, std::string> values;
  std::istringstream input{std::string(contents)};
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#')
      continue;
    const auto separator = line.find('=');
    if (separator != std::string::npos)
      values[line.substr(0, separator)] = line.substr(separator + 1);
  }
  return values;
}

std::map<std::string, std::string> ParseJsonStringObject(
    std::string_view contents) {
  return JsonStringObjectParser(contents).Parse();
}

std::optional<std::filesystem::path> DiscoverConfigPath(
    const std::filesystem::path &repo_root,
    const std::optional<std::filesystem::path> &requested) {
  return dxmt::builder::DiscoverConfigPath(repo_root, requested);
}

std::filesystem::path StagedInstallRoot(
    const std::filesystem::path &stage_dir,
    const std::filesystem::path &install_prefix) {
  const auto relative_prefix = install_prefix.is_absolute()
                                   ? install_prefix.relative_path()
                                   : install_prefix;
  return (stage_dir / relative_prefix).lexically_normal();
}

void WriteFileAtomic(const fs::path &path, std::string_view contents) {
  dxmt::builder::WriteFileAtomic(path, contents);
}

void WithFileLock(const fs::path &path,
                  const std::function<void()> &operation) {
  FileLock lock(path);
  operation();
}

std::vector<std::string>
AuditDx12Metal4Policy(const fs::path &repo_root) {
  return AuditDx12Metal4PolicyImpl(repo_root);
}

bool AuditDiagnosticMayBeBaselined(std::string_view path,
                                   std::string_view check) {
  return AuditDiagnosticMayBeBaselinedImpl(path, check);
}

std::string AuditCacheKey(const fs::path &translation_unit,
                          const fs::path &repo_root,
                          const std::vector<std::string> &command,
                          std::string_view tool_identity,
                          std::string_view config_contents) {
  return dxmt::builder::AuditCacheKey(translation_unit, repo_root, command,
                                      tool_identity, config_contents);
}

std::vector<std::string>
DeduplicateAuditDiagnosticLines(const fs::path &repo_root,
                                const std::vector<std::string> &lines) {
  std::vector<AuditDiagnostic> parsed;
  for (const auto &line : lines) {
    if (auto diagnostic = ParseAuditDiagnostic(repo_root, line))
      parsed.push_back(std::move(*diagnostic));
  }
  std::vector<std::string> collapsed;
  for (const auto &diagnostic : DeduplicateAuditDiagnostics(std::move(parsed)))
    collapsed.push_back(diagnostic.path + ":" + std::to_string(diagnostic.line) +
                        ":" + std::to_string(diagnostic.column) + ": " +
                        diagnostic.level + ": " + diagnostic.message + " [" +
                        diagnostic.check + "] x" +
                        std::to_string(diagnostic.reporting_units));
  return collapsed;
}

} // namespace testing

Application::Application(fs::path repo_root)
    : repo_root_(fs::canonical(std::move(repo_root))) {}

int Application::Run(std::span<const std::string> arguments) {
  std::optional<fs::path> requested_config;
  std::size_t index = 0;
  while (index < arguments.size()) {
    if (arguments[index] == "--config") {
      if (++index >= arguments.size())
        throw std::runtime_error("--config requires a value");
      if (requested_config)
        throw std::runtime_error("--config may only be provided once");
      requested_config = arguments[index++];
    } else if (arguments[index].starts_with("--config=")) {
      if (requested_config)
        throw std::runtime_error("--config may only be provided once");
      requested_config =
          arguments[index++].substr(std::string("--config=").size());
    } else {
      break;
    }
  }
  const auto configuration =
      LoadBuilderConfiguration(repo_root_, requested_config);
  return Driver(repo_root_, configuration.managed_root,
                configuration.profile_namespace, configuration.source)
      .Run(arguments.subspan(index));
}

} // namespace dxmt::builder
