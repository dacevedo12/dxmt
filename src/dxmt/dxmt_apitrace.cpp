#include "dxmt_apitrace.hpp"

#include "log/log.hpp"
#include "util_env.hpp"
#include "winemetal.h"

#ifdef DXMT_APITRACE_D3D
#include "apitrace/capture_runtime.hpp"
#endif

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dxmt::apitrace {
namespace {

std::atomic<int> enabled_cache{-1};
std::atomic<int> verbose_cache{-1};
std::atomic<uint64_t> seal_after_frame_cache{std::numeric_limits<uint64_t>::max()};
std::atomic_bool seal_after_frame_configured = false;
std::atomic_bool seal_after_frame_metal_done = false;
std::atomic_bool seal_after_frame_d3d_done = false;
std::atomic_bool session_open_logged = false;
std::atomic_bool shutdown_requested = false;

bool
truthy_env_value(const std::string &value) {
  return value == "1" || value == "true" || value == "yes" || value == "trace";
}

bool
env_enabled() {
  const int cached = enabled_cache.load(std::memory_order_relaxed);
  if (cached >= 0)
    return cached != 0;

  auto value = env::getEnvVar("DXMT_APITRACE_ENBALED");
  if (value.empty())
    value = env::getEnvVar("DXMT_APITRACE_ENABLED");
  const bool enabled = truthy_env_value(value);
  enabled_cache.store(enabled ? 1 : 0, std::memory_order_relaxed);
  return enabled;
}

bool
verbose_enabled() {
  const int cached = verbose_cache.load(std::memory_order_relaxed);
  if (cached >= 0)
    return cached != 0;

  const auto value = env::getEnvVar("APITRACE_METAL_VERBOSE");
  const bool verbose = truthy_env_value(value);
  verbose_cache.store(verbose ? 1 : 0, std::memory_order_relaxed);
  return verbose;
}

void
log_verbose(const char *message, uint64_t arg0 = 0, uint64_t arg1 = 0) {
  if (!verbose_enabled())
    return;

  INFO("DXMT apitrace: ", message, " arg0=", arg0, " arg1=", arg1);
}

uint64_t
seal_after_frame() {
  if (!seal_after_frame_configured.exchange(true, std::memory_order_acq_rel)) {
    auto value = env::getEnvVar("APITRACE_METAL_SEAL_CHECKPOINT_AFTER_FRAME");
    if (value.empty())
      value = env::getEnvVar("DXMT_APITRACE_SEAL_CHECKPOINT_AFTER_FRAME");
    if (!value.empty()) {
      try {
        seal_after_frame_cache.store(std::stoull(value), std::memory_order_relaxed);
      } catch (...) {
        seal_after_frame_cache.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
      }
    }
  }
  return seal_after_frame_cache.load(std::memory_order_relaxed);
}

void
set_env_var(const char *name, const char *value) {
#ifdef _WIN32
  SetEnvironmentVariableA(name, value);
  // SetEnvironmentVariableA only updates the PEB; mingw msvcrt keeps its own
  // environ table for std::getenv. apitrace's resolve_bundle_root reads via
  // std::getenv, so without the CRT-side write the PE TraceSession would fall
  // back to the cwd-default bundle even after we mirror the value here.
  _putenv_s(name, value);

  using WineSetUnixEnvProc = LONG(WINAPI *)(const char *, const char *);
  auto set_unix_env = reinterpret_cast<WineSetUnixEnvProc>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_set_unix_env"));
  if (set_unix_env)
    set_unix_env(name, value);
#else
  setenv(name, value, 1);
#endif
}

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

  static std::atomic_bool bundle_initialized = false;
  if (!bundle_initialized.exchange(true, std::memory_order_relaxed) && env::getEnvVar("APITRACE_METAL_BUNDLE").empty()) {
    const auto base_dir = env::getUnixPath(env::getExeBaseName() + "_dxmt_apitrace");
    if (!base_dir.empty()) {
      env::createDirectory(base_dir);
      std::time_t now;
      std::time(&now);
      char timestamp[32] = {};
      std::strftime(timestamp, sizeof(timestamp), "%Y%m%dT%H%M%S", std::localtime(&now));
      const auto bundle_root = base_dir + "/trace-" + timestamp + ".apitrace";
      set_env_var("APITRACE_METAL_BUNDLE", bundle_root.c_str());
      if (verbose_enabled()) {
        INFO("DXMT apitrace: bundle root ", bundle_root);
      }
    }
  }

  static std::atomic_bool trace_bundle_synced = false;
  if (!trace_bundle_synced.exchange(true, std::memory_order_relaxed)) {
    const auto bundle_root = env::getEnvVar("APITRACE_METAL_BUNDLE");
    if (!bundle_root.empty() && env::getEnvVar("APITRACE_TRACE_BUNDLE").empty()) {
      set_env_var("APITRACE_TRACE_BUNDLE", bundle_root.c_str());
    }
    if (verbose_enabled()) {
      const char *crt_view = std::getenv("APITRACE_TRACE_BUNDLE");
      INFO("DXMT apitrace: APITRACE_TRACE_BUNDLE crt-view=", crt_view ? crt_view : "(null)");
    }
  }

  WMTApitraceSessionEnsureOpen();
  if (!session_open_logged.exchange(true, std::memory_order_relaxed) && verbose_enabled()) {
    INFO("DXMT apitrace: session open requested");
  }
}

void
seal_checkpoint() {
  if (!enabled())
    return;

  if (shutdown_requested.load(std::memory_order_acquire))
    return;

  WMTApitraceSessionSealCheckpoint();
#ifdef DXMT_APITRACE_D3D
  ::apitrace::runtime::seal_process_trace_session_checkpoint();
#endif
  log_verbose("session checkpoint sealed");
}

void
seal_metal_checkpoint() {
  if (!enabled())
    return;

  if (shutdown_requested.load(std::memory_order_acquire))
    return;

  WMTApitraceSessionSealCheckpoint();
  log_verbose("metal checkpoint sealed");
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
  log_verbose("d3d checkpoint sealed");
}

void
shutdown() {
  if (!enabled())
    return;

  if (shutdown_requested.exchange(true, std::memory_order_acq_rel))
    return;

  WMTApitraceSessionClose();
#ifdef DXMT_APITRACE_D3D
  ::apitrace::runtime::shutdown_process_trace_session();
#endif
  log_verbose("session close");
}

void
set_current_d3d_sequence(uint64_t d3d_sequence) {
  if (!enabled())
    return;

  WMTApitraceSetCurrentD3DSequence(d3d_sequence);
}

void
on_command_buffer_begin(uint64_t command_buffer_id, uint64_t frame_id) {
  if (!enabled())
    return;

  ensure_session_open();
  WMTApitraceCommandBufferBegin(command_buffer_id, frame_id);
  log_verbose("command buffer begin", command_buffer_id, frame_id);
}

void
maybe_seal_metal_checkpoint_after_frame(uint64_t frame_index) {
  const auto target = seal_after_frame();
  if (target == std::numeric_limits<uint64_t>::max() || frame_index + 1 < target)
    return;
  if (seal_after_frame_metal_done.exchange(true, std::memory_order_acq_rel))
    return;
  seal_metal_checkpoint();
}

void
maybe_seal_d3d_checkpoint_after_frame(uint64_t frame_index) {
  const auto target = seal_after_frame();
  if (target == std::numeric_limits<uint64_t>::max() || frame_index + 1 < target)
    return;
  if (seal_after_frame_d3d_done.exchange(true, std::memory_order_acq_rel))
    return;
  seal_d3d_checkpoint();
}

void
on_command_buffer_commit(uint64_t command_buffer_id) {
  if (!enabled())
    return;

  WMTApitraceCommandBufferCommit(command_buffer_id);
  log_verbose("command buffer commit", command_buffer_id, 0);
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

  WMTApitracePresentDrawable(command_buffer_id, drawable_id, frame_index, sync_interval, flags);
  maybe_seal_metal_checkpoint_after_frame(frame_index);
  if (verbose_enabled()) {
    INFO("DXMT apitrace: present commandBuffer=", command_buffer_id,
         " drawable=", drawable_id,
         " frame=", frame_index,
         " syncInterval=", sync_interval,
         " flags=", flags);
  }
}

} // namespace dxmt::apitrace
