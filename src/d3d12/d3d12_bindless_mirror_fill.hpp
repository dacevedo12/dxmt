#pragma once

namespace dxmt::d3d12 {

/**
 * Outcome of one deferred bindless-mirror slot fill attempt.
 *
 * Ordinary texture/sampler/null descriptors are materialized at descriptor
 * write time; only typed texture-buffer views and fallback repairs reach the
 * deferred fill path as actual writes.
 */
enum class BindlessMirrorFillKind {
  None,
  Texture,
  Sampler,
  TextureBuffer,
  Null,
};

// Master gate for the bindless-mirror verify path. Kept inline and constant so
// the verify code stays foldable at the call sites on the encode hot path --
// the project ships without LTO, so an out-of-line definition would leave a
// real call in every per-slot fill.
[[nodiscard]] inline bool
BindlessMirrorVerifyEnabled() {
  return false;
}

// Rate limiter for verify mismatch reports. Returns false once the per-process
// report budget is exhausted, and never advances the counter while verify is
// disabled.
[[nodiscard]] bool BindlessMirrorVerifyShouldLog();

} // namespace dxmt::d3d12
