/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include <fstream>
#include <string>

#include "../thread.hpp"
#include "../util_string.hpp"

namespace dxmt {

enum class LogLevel : uint32_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  None = 5,
};

using PFN_wineLogOutput = int(__cdecl *)(const char *);

/**
 * \brief Logger
 *
 * Logger for one DLL. Creates a text file and
 * writes all log messages to that file.
 */
class Logger {

public:
  Logger(const std::string &file_name);
  ~Logger();

  static void trace(const std::string &message);
  static void debug(const std::string &message);
  static void info(const std::string &message);
  static void warn(const std::string &message);
  static void err(const std::string &message);
  static void log(LogLevel level, const std::string &message);
  static void logFileOnly(LogLevel level, const std::string &message);

  static LogLevel logLevel() { return s_instance.m_minLevel; }

private:
  static Logger s_instance;

  const LogLevel m_minLevel;
  const std::string m_fileName;

  dxmt::mutex m_mutex;
  std::ofstream m_fileStream;

  bool m_initialized = false;
  PFN_wineLogOutput m_wineLogOutput = nullptr;

  void emitMsg(LogLevel level, const std::string &message, bool wineOutput);

  std::string getFileName(const std::string &base);

  static LogLevel getMinLogLevel();
};

} // namespace dxmt


#define DXMT_LOG_ENABLED(level)                                                \
  (static_cast<uint32_t>(level) >=                                             \
   static_cast<uint32_t>(dxmt::Logger::logLevel()))

#define TRACE(...)                                                             \
  do {                                                                         \
    if (DXMT_LOG_ENABLED(dxmt::LogLevel::Trace))                               \
      dxmt::Logger::trace(dxmt::str::format(__VA_ARGS__));                     \
  } while (0)

#define DEBUG(...)                                                             \
  do {                                                                         \
    if (DXMT_LOG_ENABLED(dxmt::LogLevel::Debug))                               \
      dxmt::Logger::debug(dxmt::str::format(__VA_ARGS__));                     \
  } while (0)

#define INFO(...)                                                              \
  do {                                                                         \
    if (DXMT_LOG_ENABLED(dxmt::LogLevel::Info))                                \
      dxmt::Logger::info(dxmt::str::format(__VA_ARGS__));                      \
  } while (0)

#define WARN(...)                                                              \
  do {                                                                         \
    if (DXMT_LOG_ENABLED(dxmt::LogLevel::Warn))                                \
      dxmt::Logger::warn(dxmt::str::format(__VA_ARGS__));                      \
  } while (0)

#define WARN_FILE_ONLY(...)                                                    \
  do {                                                                         \
    if (DXMT_LOG_ENABLED(dxmt::LogLevel::Warn))                                \
      dxmt::Logger::logFileOnly(dxmt::LogLevel::Warn,                          \
                                dxmt::str::format(__VA_ARGS__));               \
  } while (0)

#define ERR_E_INVALIDARG(method)                                               \
  ([&]() -> HRESULT {                                                          \
    if (DXMT_LOG_ENABLED(dxmt::LogLevel::Error))                               \
      dxmt::Logger::err(                                                       \
          dxmt::str::format(method, " returning E_INVALIDARG at ", __FILE__,   \
                            ":", __LINE__));                                   \
    return E_INVALIDARG;                                                       \
  }())

#define WARN_E_INVALIDARG(method) ERR_E_INVALIDARG(method)

#define ERR(...)                                                               \
  do {                                                                         \
    if (DXMT_LOG_ENABLED(dxmt::LogLevel::Error))                               \
      dxmt::Logger::err(dxmt::str::format(__VA_ARGS__));                       \
  } while (0)

#define ERR_ONCE(...)                                                          \
  static bool s_errorShown = false;                                            \
  if (DXMT_LOG_ENABLED(dxmt::LogLevel::Error) &&                               \
      !std::exchange(s_errorShown, true)) {                                    \
    dxmt::Logger::err(dxmt::str::format(__VA_ARGS__));                         \
  }
