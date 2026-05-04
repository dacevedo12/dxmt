// DYNAMIC DEFAULT-pool texture LockRect (issue #314, the other half).
//
// MSDN: a D3DUSAGE_DYNAMIC texture is lockable regardless of pool. dxmt
// previously gave a DYNAMIC + D3DPOOL_DEFAULT texture a Private texture
// with no mirror, so LockRect returned D3DERR_INVALIDCALL. COD4's
// cinematic decoder creates DYNAMIC DEFAULT L8 planes (and dynamic water
// normal maps) and LockRect(DISCARD)s them every frame to stream decoded
// data — a broken lock means black video / stale maps.
//
// Validates: LockRect(DISCARD) succeeds with a non-null pointer + sane
// pitch; the write survives Unlock + re-Lock (the sysmem mirror persists);
// a per-frame DISCARD loop keeps succeeding (the upload-ring snapshot makes
// the mirror safe to re-lock each frame — no rename-ring); double-lock
// rejects.

#include "../dx9_smoke.h"

void
test_dynamic_texture_lock(void) {
  Dx9Fixture fx;
  if (!fx.create(320, 240, D3DFMT_X8R8G8B8))
    return;

  // ---- 64x64 A8R8G8B8 single-level DYNAMIC DEFAULT texture. ----
  IDirect3DTexture9 *tex = nullptr;
  check_hr(fx.dev->CreateTexture(64, 64, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr));
  if (!tex)
    return;

  // ---- Per-frame DISCARD loop: each lock must succeed with a valid
  // pointer, and the bytes written this "frame" must read back (the
  // mirror is the CPU master; the upload-ring snapshots on Unlock so the
  // re-lock is safe without a rename-ring). ----
  for (DWORD frame = 0; frame < 4; ++frame) {
    D3DLOCKED_RECT lr = {};
    check_hr(tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD));
    check_true(lr.pBits != nullptr);
    check_true(lr.Pitch >= 64 * 4);
    if (lr.pBits) {
      DWORD marker = 0xC0DE0000u | frame;
      ((DWORD *)lr.pBits)[0] = marker;
      ((DWORD *)((char *)lr.pBits + (size_t)63 * lr.Pitch))[63] = marker ^ 0xFFFFu;
    }
    check_hr(tex->UnlockRect(0));

    // Re-lock and confirm this frame's write persisted in the mirror.
    D3DLOCKED_RECT lr2 = {};
    check_hr(tex->LockRect(0, &lr2, nullptr, 0));
    if (lr2.pBits) {
      check_eq_u32(((DWORD *)lr2.pBits)[0], 0xC0DE0000u | frame);
      check_eq_u32(((DWORD *)((char *)lr2.pBits + (size_t)63 * lr2.Pitch))[63], (0xC0DE0000u | frame) ^ 0xFFFFu);
    }
    check_hr(tex->UnlockRect(0));
  }

  // ---- Double-lock rejected. ----
  D3DLOCKED_RECT lr_a = {}, lr_b = {};
  check_hr(tex->LockRect(0, &lr_a, nullptr, D3DLOCK_DISCARD));
  check_hr_eq(tex->LockRect(0, &lr_b, nullptr, 0), D3DERR_INVALIDCALL);
  check_hr(tex->UnlockRect(0));

  tex->Release();
}
