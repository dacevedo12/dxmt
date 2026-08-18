#pragma once

// Spawning a child PE from a test running under Wine. The suite drives both
// corpora as separate processes rather than linked-in code, so a module that
// crashes or hangs costs one process instead of the run.

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt::test {

struct WineProcess {
  HANDLE handle = nullptr;
  DWORD id = 0;
};

inline std::wstring
WineExecutablePath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0)
      return {};
    if (size < buffer.size() - 1)
      return std::wstring(buffer.data(), size);
    buffer.resize(buffer.size() * 2);
  }
}

inline std::wstring
WidenWineArgument(std::string_view value) {
  if (value.empty())
    return {};

  // A test name or a path from the corpus is UTF-8; fall back to the active
  // code page rather than dropping an argument that is not valid UTF-8.
  UINT code_page = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  int size = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (size == 0) {
    code_page = CP_ACP;
    flags = 0;
    size = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
  }
  if (size == 0)
    return {};

  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), result.data(), size);
  return result;
}

// CommandLineToArgvW's quoting rules: a run of backslashes is only doubled
// when a quote follows it, so a trailing path separator does not escape the
// closing quote.
inline std::wstring
QuoteWineArgument(std::wstring_view argument) {
  if (argument.empty())
    return L"\"\"";
  if (argument.find_first_of(L" \t\"") == std::wstring_view::npos)
    return std::wstring(argument);

  std::wstring result = L"\"";
  size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      backslashes++;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      backslashes = 0;
    } else {
      result.append(backslashes, L'\\');
      backslashes = 0;
    }
    result.push_back(character);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

inline std::wstring
BuildWineCommandLine(std::wstring_view executable, const std::vector<std::string> &arguments) {
  std::wstring command_line = QuoteWineArgument(executable);
  for (const auto &argument : arguments) {
    command_line.push_back(L' ');
    command_line += QuoteWineArgument(WidenWineArgument(argument));
  }
  return command_line;
}

inline std::string
WineErrorMessage(DWORD error) {
  char *buffer = nullptr;
  const DWORD size = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
      reinterpret_cast<char *>(&buffer), 0, nullptr
  );
  if (size == 0 || buffer == nullptr)
    return "Win32 error " + std::to_string(error);

  std::string message(buffer, size);
  LocalFree(buffer);
  while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
    message.pop_back();
  return message;
}

// output, when given, receives both stdout and stderr; handle inheritance is
// enabled only then, so an ordinary spawn leaks nothing into the child.
inline std::optional<WineProcess>
StartWineProcess(
    std::wstring_view executable, const std::vector<std::string> &arguments, DWORD *error, HANDLE output = nullptr
) {
  const std::wstring executable_path(executable);
  std::wstring command_line = BuildWineCommandLine(executable, arguments);

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  if (output != nullptr) {
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = output;
    startup_info.hStdError = output;
  }

  PROCESS_INFORMATION process_info = {};
  if (!CreateProcessW(
          executable_path.c_str(), command_line.data(), nullptr, nullptr, output != nullptr, 0, nullptr, nullptr,
          &startup_info, &process_info
      )) {
    *error = GetLastError();
    return std::nullopt;
  }

  CloseHandle(process_info.hThread);
  return WineProcess{process_info.hProcess, process_info.dwProcessId};
}

} // namespace dxmt::test
