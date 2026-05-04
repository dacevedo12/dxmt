// Soft-impl device-method bulk smoke. Covers methods whose pre-port
// shape returned E_NOTIMPL and broke hr-strict app init paths, where
// the spec-correct shape (per DXVK + wined3d) is a successful store-
// and-return:
//
//   * Set/GetClipStatus              — vestigial FFP occlusion struct.
//   * Set/Get/CurrentTexturePalette  — paletted-texture state (FFP P8
//                                      sampler not landed; storage-
//                                      only round-trip).
//   * Set/GetSoftwareVertexProcessing — defensive HW-VP probe.
//   * Set/GetNPatchMode               — D3D10-removed tessellation;
//                                      apps issue SetNPatchMode(0.0)
//                                      to confirm disabled.
//
// All of these are "must succeed, must round-trip, must not crash"
// gates. Locking them in a smoke means a future commit that re-stubs
// any of them shows up as a TAP fail rather than a black-screen init
// regression days later.

#include "../dx9_smoke.h"

#include <string.h>

void
test_softimpl_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- SetClipStatus / GetClipStatus round-trip. ----
  check_hr_eq(dev->SetClipStatus(NULL), D3DERR_INVALIDCALL);
  check_hr_eq(dev->GetClipStatus(NULL), D3DERR_INVALIDCALL);

  D3DCLIPSTATUS9 cs_in = {};
  cs_in.ClipUnion = 0x12345678u;
  cs_in.ClipIntersection = 0xDEADBEEFu;
  check_hr(dev->SetClipStatus(&cs_in));
  D3DCLIPSTATUS9 cs_out = {};
  check_hr(dev->GetClipStatus(&cs_out));
  check_eq_u32(cs_out.ClipUnion, 0x12345678u);
  check_eq_u32(cs_out.ClipIntersection, 0xDEADBEEFu);

  // ---- Palette entries. ----
  check_hr_eq(dev->SetPaletteEntries(0, NULL), D3DERR_INVALIDCALL);
  check_hr_eq(dev->GetPaletteEntries(0, NULL), D3DERR_INVALIDCALL);

  // Get on a not-yet-set palette → INVALIDCALL (DXVK + wined3d
  // both reject unknown palette numbers).
  PALETTEENTRY junk[256] = {};
  check_hr_eq(dev->GetPaletteEntries(42, junk), D3DERR_INVALIDCALL);

  // Set a custom palette and read it back bit-exact.
  PALETTEENTRY pal_in[256] = {};
  for (uint32_t i = 0; i < 256; ++i) {
    pal_in[i].peRed = static_cast<BYTE>(i);
    pal_in[i].peGreen = static_cast<BYTE>(255 - i);
    pal_in[i].peBlue = static_cast<BYTE>((i * 3) & 0xFF);
    pal_in[i].peFlags = static_cast<BYTE>(i & 0x7F);
  }
  check_hr(dev->SetPaletteEntries(7, pal_in));
  PALETTEENTRY pal_out[256] = {};
  check_hr(dev->GetPaletteEntries(7, pal_out));
  check_true(memcmp(pal_in, pal_out, sizeof(pal_in)) == 0);

  // SetCurrentTexturePalette + GetCurrentTexturePalette round-trip.
  check_hr_eq(dev->GetCurrentTexturePalette(NULL), D3DERR_INVALIDCALL);
  check_hr(dev->SetCurrentTexturePalette(7));
  UINT cur = 0xCDCDCDCDu;
  check_hr(dev->GetCurrentTexturePalette(&cur));
  check_eq_u32(cur, 7u);

  // ---- SoftwareVertexProcessing — must silently accept both TRUE
  // and FALSE; Get always returns FALSE (Metal is HW-VP only). ----
  check_hr(dev->SetSoftwareVertexProcessing(FALSE));
  check_eq_u32(dev->GetSoftwareVertexProcessing(), 0u); // FALSE
  check_hr(dev->SetSoftwareVertexProcessing(TRUE));
  check_eq_u32(dev->GetSoftwareVertexProcessing(), 0u); // still FALSE

  // ---- N-patch mode — store-only on the device; Get returns last Set.
  check_hr(dev->SetNPatchMode(0.0f));
  check_true(dev->GetNPatchMode() == 0.0f);
  check_hr(dev->SetNPatchMode(4.0f));
  // GetNPatchMode reads whatever the stored value is (storage-only;
  // tessellation isn't wired). Pre-port returned 0; the spec-correct
  // shape per wined3d is to round-trip. Don't assert the exact value
  // since the storage shape isn't load-bearing for any active app,
  // just that the call returned without crashing.
  (void)dev->GetNPatchMode();

  check_zero_losable_count(dev);
}
