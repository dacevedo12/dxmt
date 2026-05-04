// SetCursorProperties spec-gate smoke. Apps init paths hr-check the
// validation result — the visible cursor is whatever macOS renders
// (no Metal-side HW cursor API), but the bitmap-shape rejection
// contract still has to hold.
//
// Contract (DXVK d3d9_device.cpp:352-386):
//   * NULL pCursorBitmap                         → D3DERR_INVALIDCALL.
//   * Bitmap format != D3DFMT_A8R8G8B8           → D3DERR_INVALIDCALL.
//   * Bitmap dimensions not powers of two        → D3DERR_INVALIDCALL.
//   * Hotspot outside the bitmap bounds          → D3DERR_INVALIDCALL.
//   * Otherwise                                  → D3D_OK.

#include "../dx9_smoke.h"

void
test_set_cursor_properties_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- NULL bitmap ----
  check_hr_eq(dev->SetCursorProperties(0, 0, NULL), D3DERR_INVALIDCALL);

  // ---- Wrong format (X8R8G8B8, not A8R8G8B8). ----
  IDirect3DSurface9 *wrong_fmt = NULL;
  check_hr(dev->CreateOffscreenPlainSurface(32, 32, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &wrong_fmt, NULL));
  if (wrong_fmt) {
    check_hr_eq(dev->SetCursorProperties(0, 0, wrong_fmt), D3DERR_INVALIDCALL);
    wrong_fmt->Release();
  }

  // ---- Non-power-of-two dimensions. ----
  IDirect3DSurface9 *bad_w = NULL;
  if (SUCCEEDED(dev->CreateOffscreenPlainSurface(48, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &bad_w, NULL)) && bad_w) {
    check_hr_eq(dev->SetCursorProperties(0, 0, bad_w), D3DERR_INVALIDCALL);
    bad_w->Release();
  }

  IDirect3DSurface9 *bad_h = NULL;
  if (SUCCEEDED(dev->CreateOffscreenPlainSurface(32, 24, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &bad_h, NULL)) && bad_h) {
    check_hr_eq(dev->SetCursorProperties(0, 0, bad_h), D3DERR_INVALIDCALL);
    bad_h->Release();
  }

  // ---- Hotspot out of bounds (>= width / height). ----
  IDirect3DSurface9 *ok = NULL;
  check_hr(dev->CreateOffscreenPlainSurface(32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &ok, NULL));
  if (ok) {
    check_hr_eq(dev->SetCursorProperties(32, 0, ok), D3DERR_INVALIDCALL);
    check_hr_eq(dev->SetCursorProperties(0, 32, ok), D3DERR_INVALIDCALL);
    check_hr_eq(dev->SetCursorProperties(100, 100, ok), D3DERR_INVALIDCALL);

    // ---- Happy path: valid 32x32 A8R8G8B8 bitmap, hotspot 0,0. ----
    check_hr(dev->SetCursorProperties(0, 0, ok));
    // ---- Happy path: hotspot at the last legal pixel (width-1). ----
    check_hr(dev->SetCursorProperties(31, 31, ok));
    ok->Release();
  }

  check_zero_losable_count(dev);
}
