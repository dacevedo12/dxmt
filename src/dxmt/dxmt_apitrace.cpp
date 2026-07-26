#include "dxmt_apitrace.hpp"

#include "log/log.hpp"
#include "util_env.hpp"
#include "winemetal.h"

#ifdef DXMT_APITRACE_D3D
#include "apitrace/capture_runtime.hpp"
#endif

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#if DXMT_DX12_METAL4
#define DXMT_WMT_APITRACE_SESSION_FLUSH WMT4ApitraceSessionFlush
#define DXMT_WMT_APITRACE_SESSION_ENSURE_OPEN WMT4ApitraceSessionEnsureOpen
#define DXMT_WMT_APITRACE_SESSION_SEAL_CHECKPOINT WMT4ApitraceSessionSealCheckpoint
#define DXMT_WMT_APITRACE_SESSION_CLOSE WMT4ApitraceSessionClose
#define DXMT_WMT_APITRACE_SET_CURRENT_D3D_SEQUENCE WMT4ApitraceSetCurrentD3DSequence
#define DXMT_WMT_APITRACE_COMMAND_BUFFER_BEGIN WMT4ApitraceCommandBufferBegin
#define DXMT_WMT_APITRACE_COMMAND_BUFFER_COMMIT WMT4ApitraceCommandBufferCommit
#define DXMT_WMT_APITRACE_PRESENT_DRAWABLE WMT4ApitracePresentDrawable
#else
#define DXMT_WMT_APITRACE_SESSION_FLUSH WMTApitraceSessionFlush
#define DXMT_WMT_APITRACE_SESSION_ENSURE_OPEN WMTApitraceSessionEnsureOpen
#define DXMT_WMT_APITRACE_SESSION_SEAL_CHECKPOINT WMTApitraceSessionSealCheckpoint
#define DXMT_WMT_APITRACE_SESSION_CLOSE WMTApitraceSessionClose
#define DXMT_WMT_APITRACE_SET_CURRENT_D3D_SEQUENCE WMTApitraceSetCurrentD3DSequence
#define DXMT_WMT_APITRACE_COMMAND_BUFFER_BEGIN WMTApitraceCommandBufferBegin
#define DXMT_WMT_APITRACE_COMMAND_BUFFER_COMMIT WMTApitraceCommandBufferCommit
#define DXMT_WMT_APITRACE_PRESENT_DRAWABLE WMTApitracePresentDrawable
#endif

namespace dxmt::apitrace {
namespace {

std::atomic<int> enabled_cache{-1};
std::atomic_bool session_open_logged = false;
std::atomic_bool shutdown_requested = false;
#ifdef _WIN32
std::atomic_bool crash_flush_handler_installed = false;
std::atomic_bool crash_flush_running = false;
#endif

constexpr const char *kTraceBundleEnv = "APITRACE_TRACE_BUNDLE";

bool
truthy_env_value(const std::string &value) {
  return value == "1" || value == "true" || value == "yes" || value == "trace";
}

bool
env_enabled() {
  const int cached = enabled_cache.load(std::memory_order_relaxed);
  if (cached >= 0)
    return cached != 0;

  auto value = env::getEnvVar("DXMT_APITRACE_ENABLED");
  const bool enabled = truthy_env_value(value);
  enabled_cache.store(enabled ? 1 : 0, std::memory_order_relaxed);
  return enabled;
}

bool
ends_with(const std::string &value, const char *suffix) {
  const std::string suffix_string(suffix);
  return value.size() >= suffix_string.size() &&
         value.compare(value.size() - suffix_string.size(), suffix_string.size(), suffix_string) == 0;
}

// Publishes `name` into the PE-side environment only.
//
// Deliberately does NOT touch the unix-side environment (formerly via ntdll's
// __wine_set_unix_env, and plain setenv() on native builds). getenv() hands out
// interior pointers into the environment block and setenv() frees the old value
// string when it grows the block, so writing the unix environment from a DXMT
// worker thread turned every concurrent unix-side getenv() of the same variable
// into a use-after-free. winemetal's unix side now receives the bundle root as
// an explicit WMT*ApitraceSessionEnsureOpen argument instead, so nothing on that
// side reads APITRACE_TRACE_BUNDLE any more.
//
// The remaining PE-side write is still required: apitrace's own
// resolve_bundle_root() (external, not ours to change) reads it through
// std::getenv. It is ordered by the std::call_once in ensure_session_open(),
// which happens-before any thread can reach apitrace's readers.
void
set_pe_env_var([[maybe_unused]] const char *name, [[maybe_unused]] const char *value) {
#ifdef _WIN32
  SetEnvironmentVariableA(name, value);
  // SetEnvironmentVariableA only updates the PEB; mingw msvcrt keeps its own
  // environ table for std::getenv. apitrace's resolve_bundle_root reads via
  // std::getenv, so without the CRT-side write the PE TraceSession would fall
  // back to the cwd-default bundle even after we mirror the value here.
  _putenv_s(name, value);
#endif
}

std::string
default_bundle_root() {
  const auto base_dir = env::getUnixPath(env::getExeBaseName() + "_dxmt_apitrace");
  if (base_dir.empty())
    return {};

  env::createDirectory(base_dir);
  std::time_t now;
  std::time(&now);
  char timestamp[32] = {};
  std::strftime(timestamp, sizeof(timestamp), "%Y%m%dT%H%M%S", std::localtime(&now));
  return base_dir + "/trace-" + timestamp + ".apitrace";
}

std::string
bundle_root_from_env(const std::string &value) {
  if (value.empty())
    return {};

  const auto unix_path = env::getUnixPath(value);
  if (unix_path.empty())
    return {};

  if (ends_with(unix_path, ".apitrace"))
    return unix_path;

  WARN("DXMT apitrace: ignoring APITRACE_TRACE_BUNDLE because it is not a .apitrace bundle: ", unix_path.c_str());
  return {};
}

// Written exactly once, under bundle_root_once, before any reader observes it.
std::string resolved_bundle_root;
std::once_flag bundle_root_once;

void
initialize_bundle_root() {
  auto trace_bundle = bundle_root_from_env(env::getEnvVar(kTraceBundleEnv));

  if (trace_bundle.empty())
    trace_bundle = default_bundle_root();

  if (trace_bundle.empty())
    return;

  set_pe_env_var(kTraceBundleEnv, trace_bundle.c_str());
  resolved_bundle_root = std::move(trace_bundle);
}

// Returns the process-wide bundle root, initializing it on first use.
//
// std::call_once (unlike the previous atomic exchange flag) makes every caller
// block until initialization completes, so the single PE-side environment write
// inside initialize_bundle_root() happens-before any other thread can reach
// apitrace's std::getenv-based resolve_bundle_root().
const std::string &
bundle_root() {
  std::call_once(bundle_root_once, initialize_bundle_root);
  return resolved_bundle_root;
}

#ifdef _WIN32
bool
is_debug_exception(DWORD code) {
  constexpr DWORD kThreadNameException = 0x406D1388;
  return code == DBG_PRINTEXCEPTION_C ||
         code == DBG_PRINTEXCEPTION_WIDE_C ||
         code == DBG_CONTROL_C ||
         code == DBG_CONTROL_BREAK ||
         code == kThreadNameException;
}

void
flush_sessions_for_crash() {
  if (!enabled())
    return;

  if (shutdown_requested.load(std::memory_order_acquire))
    return;

  if (crash_flush_running.exchange(true, std::memory_order_acq_rel))
    return;

#ifdef DXMT_APITRACE_D3D
  ::apitrace::runtime::flush_process_trace_session();
#endif
  DXMT_WMT_APITRACE_SESSION_FLUSH();
  crash_flush_running.store(false, std::memory_order_release);
}

LONG WINAPI
apitrace_crash_flush_handler(EXCEPTION_POINTERS *exception_info) {
  if (!exception_info || !exception_info->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;

  const DWORD code = exception_info->ExceptionRecord->ExceptionCode;
  if (is_debug_exception(code))
    return EXCEPTION_CONTINUE_SEARCH;

  flush_sessions_for_crash();
  return EXCEPTION_CONTINUE_SEARCH;
}

void
install_crash_flush_handler() {
  if (crash_flush_handler_installed.exchange(true, std::memory_order_acq_rel))
    return;

  AddVectoredExceptionHandler(1, apitrace_crash_flush_handler);
}
#endif

} // namespace

bool
enabled() {
  return env_enabled();
}

void
ensure_session_open() {
  if (!enabled())
    return;

  if (shutdown_requested.load(std::memory_order_acquire))
    return;

  const std::string &root = bundle_root();

#ifdef _WIN32
  install_crash_flush_handler();
#endif

  // The unix side copies this string under its own lock; it never keeps the
  // pointer, and it never reads the environment for it.
  DXMT_WMT_APITRACE_SESSION_ENSURE_OPEN(root.empty() ? nullptr : root.c_str());
  if (!session_open_logged.exchange(true, std::memory_order_relaxed))
    INFO("DXMT apitrace: session open requested");
}

void
seal_checkpoint() {
  if (!enabled())
    return;

  if (shutdown_requested.load(std::memory_order_acquire))
    return;

  DXMT_WMT_APITRACE_SESSION_SEAL_CHECKPOINT();
#ifdef DXMT_APITRACE_D3D
  ::apitrace::runtime::seal_process_trace_session_checkpoint();
#endif
}

void
seal_metal_checkpoint() {
  if (!enabled())
    return;

  if (shutdown_requested.load(std::memory_order_acquire))
    return;

  DXMT_WMT_APITRACE_SESSION_SEAL_CHECKPOINT();
}

void
seal_d3d_checkpoint() {
  if (!enabled())
    return;

  if (shutdown_requested.load(std::memory_order_acquire))
    return;

#ifdef DXMT_APITRACE_D3D
  ::apitrace::runtime::seal_process_trace_session_checkpoint();
#endif
}

void
shutdown() {
  if (!enabled())
    return;

  if (shutdown_requested.exchange(true, std::memory_order_acq_rel))
    return;

  DXMT_WMT_APITRACE_SESSION_CLOSE();
#ifdef DXMT_APITRACE_D3D
  ::apitrace::runtime::shutdown_process_trace_session();
#endif
}

void
set_current_d3d_sequence(uint64_t d3d_sequence) {
  if (!enabled())
    return;

  ensure_session_open();
  DXMT_WMT_APITRACE_SET_CURRENT_D3D_SEQUENCE(d3d_sequence);
}

void
on_command_buffer_begin(uint64_t command_buffer_id, uint64_t frame_id) {
  if (!enabled())
    return;

  ensure_session_open();
  DXMT_WMT_APITRACE_COMMAND_BUFFER_BEGIN(command_buffer_id, frame_id);
}

void
on_command_buffer_commit(uint64_t command_buffer_id) {
  if (!enabled())
    return;

  DXMT_WMT_APITRACE_COMMAND_BUFFER_COMMIT(command_buffer_id);
}

void
on_present_drawable(
    uint64_t command_buffer_id,
    uint64_t drawable_id,
    uint64_t frame_index,
    uint32_t sync_interval,
    uint32_t flags) {
  if (!enabled())
    return;

  DXMT_WMT_APITRACE_PRESENT_DRAWABLE(command_buffer_id, drawable_id, frame_index, sync_interval, flags);
}

} // namespace dxmt::apitrace
