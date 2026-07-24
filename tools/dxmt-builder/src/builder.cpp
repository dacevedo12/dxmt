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
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
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

std::string ReadFile(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("failed to read " + path.string());
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
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
        << "  build [--profile NAME] <runtime|d3d9|d3d10|d3d11|d3d12|tests-*|benchmarks>...\n"
        << "  test [--profile NAME] [all|unit|integration|performance] [--suite NAME] [--test-args ARG]\n"
        << "  package [--profile NAME] windows-oracle [--dest PATH]\n"
        << "  wine-exec [--] <wine-args...>   # Meson/benchmark Wine launcher\n"
        << "  install [--profile NAME] [--component NAME] [--dest PATH]\n"
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

  std::string ParseProfile(std::span<const std::string> arguments,
                           std::vector<std::string> *remaining) const {
    std::string profile = "gcc-x64-release-full";
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
    static const std::set<std::string> suites = {"all",   "framework", "d3d9",
                                                 "d3d10", "d3d11",     "d3d12"};
    if (!suites.contains(suite))
      throw std::runtime_error("unsupported test suite: " + suite);

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
      {"llvm-mingw-x64-release", "x64", "llvm-mingw", "release", true, false, true, true, false},
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
