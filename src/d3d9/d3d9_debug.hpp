#pragma once

#include "util_env.hpp"

namespace dxmt {

// Env gates for the d3d9 diagnostic paths. Each reads its variable once and
// caches the answer, so a disabled gate costs one predictable branch at the
// call site and the tracing can stay inline in the hot paths it describes.
// Performance accounting does not live here: it goes through
// dxmt::FrameStatistics under DXMT_PERF_STATS like every other backend.
inline bool
d9EnvGateEnabled(const char *name) {
  const auto value = env::getEnvVar(name);
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

// One-shot diagnostics a normal run should never emit, such as the
// device-teardown high-water reports.
inline bool
d9DebugEnabled() {
  static const bool on = d9EnvGateEnabled("DXMT_D9_DEBUG");
  return on;
}

// Present-path tracer. Logs which backing each Present hands to the drawable
// and by which path, correlated against each backbuffer rebuild, so a
// one-frame stale present at a scene cut can be attributed to a resource.
inline bool
d9PresentDbgEnabled() {
  static const bool on = d9EnvGateEnabled("DXMT_D9_PRESENTDBG");
  return on;
}

} // namespace dxmt
