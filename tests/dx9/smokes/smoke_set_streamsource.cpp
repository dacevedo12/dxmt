// SetStreamSource / GetStreamSource / SetIndices / GetIndices smoke.
// Validates: stream-index range gate, NULL-buffer offset/stride
// preservation, round-trip, cross-device rejection on both VB and
// IB paths, and the priv-pin lifetime (release while bound, slot
// keeps it alive).

#include "../dx9_smoke.h"
void
test_set_streamsource(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("Direct3DCreate9: NULL\n");
    return;
  }

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 320;
  pp.BackBufferHeight = 240;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

  IDirect3DDevice9 *dev = NULL;
  HRESULT cdhr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  printf("CreateDevice: hr=0x%08lx\n", (unsigned long)cdhr);
  if (FAILED(cdhr) || !dev) {
    d3d->Release();
    return;
  }

  // Initial state: nothing bound on stream 0.
  IDirect3DVertexBuffer9 *got = NULL;
  UINT off = 0xdeadbeef, str = 0xdeadbeef;
  HRESULT hr = dev->GetStreamSource(0, &got, &off, &str);
  printf(
      "GetStreamSource(0) initial: hr=0x%08lx out=%s off=%u str=%u\n", (unsigned long)hr, got ? "non-null" : "null",
      off, str
  );
  if (got)
    got->Release();

  IDirect3DIndexBuffer9 *gotib = NULL;
  hr = dev->GetIndices(&gotib);
  printf("GetIndices initial: hr=0x%08lx out=%s\n", (unsigned long)hr, gotib ? "non-null" : "null");
  if (gotib)
    gotib->Release();

  // Make a couple of vertex buffers and an index buffer.
  IDirect3DVertexBuffer9 *vb0 = NULL;
  dev->CreateVertexBuffer(1024, 0, 0, D3DPOOL_DEFAULT, &vb0, NULL);
  IDirect3DVertexBuffer9 *vb_high = NULL;
  dev->CreateVertexBuffer(2048, 0, 0, D3DPOOL_DEFAULT, &vb_high, NULL);
  IDirect3DIndexBuffer9 *ib = NULL;
  dev->CreateIndexBuffer(512, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL);
  printf("Created vb0=%s vb_high=%s ib=%s\n", vb0 ? "ok" : "null", vb_high ? "ok" : "null", ib ? "ok" : "null");

  // Round-trip on stream 0.
  HRESULT s = dev->SetStreamSource(0, vb0, 16, 32);
  printf("SetStreamSource(0, vb0, off=16, str=32): hr=0x%08lx\n", (unsigned long)s);
  got = NULL;
  off = str = 0;
  dev->GetStreamSource(0, &got, &off, &str);
  printf("GetStreamSource(0): same=%s off=%u str=%u\n", got == vb0 ? "yes" : "no", off, str);
  if (got)
    got->Release();

  // Round-trip on the highest valid stream (15).
  s = dev->SetStreamSource(15, vb_high, 8, 64);
  printf("SetStreamSource(15, vb_high, off=8, str=64): hr=0x%08lx\n", (unsigned long)s);
  got = NULL;
  off = str = 0;
  dev->GetStreamSource(15, &got, &off, &str);
  printf("GetStreamSource(15): same=%s off=%u str=%u\n", got == vb_high ? "yes" : "no", off, str);
  if (got)
    got->Release();

  // Out-of-range stream — Set + Get both INVALIDCALL.
  s = dev->SetStreamSource(16, vb0, 0, 0);
  printf("SetStreamSource(16, ...): hr=0x%08lx (must reject)\n", (unsigned long)s);
  hr = dev->GetStreamSource(16, &got, &off, &str);
  printf("GetStreamSource(16): hr=0x%08lx (must reject)\n", (unsigned long)hr);

  // NULL-buffer Set preserves previous offset/stride. Sequence:
  //   1) Set(0, vb0, 100, 200)
  //   2) Set(0, NULL, 999, 999)  — args ignored, offset/stride preserved
  //   3) Get(0) → buffer=NULL, offset=100, stride=200
  dev->SetStreamSource(0, vb0, 100, 200);
  s = dev->SetStreamSource(0, NULL, 999, 999);
  printf("SetStreamSource(0, NULL, 999, 999): hr=0x%08lx (preserves)\n", (unsigned long)s);
  got = NULL;
  off = str = 0;
  dev->GetStreamSource(0, &got, &off, &str);
  printf(
      "GetStreamSource(0) post-NULL-set: out=%s off=%u str=%u "
      "(want NULL,100,200)\n",
      got ? "non-null" : "null", off, str
  );
  if (got)
    got->Release();

  // Index buffer round-trip.
  s = dev->SetIndices(ib);
  printf("SetIndices(ib): hr=0x%08lx\n", (unsigned long)s);
  gotib = NULL;
  dev->GetIndices(&gotib);
  printf("GetIndices: same=%s\n", gotib == ib ? "yes" : "no");
  if (gotib)
    gotib->Release();

  // Unbind index buffer.
  dev->SetIndices(NULL);
  gotib = (IDirect3DIndexBuffer9 *)0xdeadbeef;
  dev->GetIndices(&gotib);
  printf("GetIndices post-unbind: out=%s\n", gotib ? "non-null" : "null");

  // ---- Cross-device rejection. ----
  IDirect3DDevice9 *dev2 = NULL;
  HRESULT cd2 = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev2);
  printf("CreateDevice(2): hr=0x%08lx\n", (unsigned long)cd2);
  if (SUCCEEDED(cd2) && dev2) {
    IDirect3DVertexBuffer9 *foreign_vb = NULL;
    dev2->CreateVertexBuffer(64, 0, 0, D3DPOOL_DEFAULT, &foreign_vb, NULL);
    if (foreign_vb) {
      HRESULT bad = dev->SetStreamSource(1, foreign_vb, 0, 0);
      printf("SetStreamSource(foreign-vb): hr=0x%08lx\n", (unsigned long)bad);
      foreign_vb->Release();
    }
    IDirect3DIndexBuffer9 *foreign_ib = NULL;
    dev2->CreateIndexBuffer(32, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &foreign_ib, NULL);
    if (foreign_ib) {
      HRESULT bad = dev->SetIndices(foreign_ib);
      printf("SetIndices(foreign-ib): hr=0x%08lx\n", (unsigned long)bad);
      foreign_ib->Release();
    }
    dev2->Release();
  }

  // ---- Lifetime: release vb0 publicly while still bound at stream 0
  // (after the NULL-set sequence we re-bind it explicitly first).
  // Device priv ref keeps vb0 alive until the next SetStreamSource
  // replaces it.
  dev->SetStreamSource(0, vb0, 0, 16);
  ULONG vb0_ref = vb0->Release();
  printf("Release(vb0) while bound: %lu (priv ref keeps it alive)\n", (unsigned long)vb0_ref);
  got = NULL;
  off = str = 0;
  HRESULT still = dev->GetStreamSource(0, &got, &off, &str);
  printf("GetStreamSource(0) post-release: hr=0x%08lx ptr_match=%s\n", (unsigned long)still, got == vb0 ? "yes" : "no");
  if (got)
    got->Release();

  // ---- Index-buffer release-while-bound. ----
  dev->SetIndices(ib);
  ULONG ib_ref = ib->Release();
  printf("Release(ib) while bound: %lu\n", (unsigned long)ib_ref);
  gotib = NULL;
  dev->GetIndices(&gotib);
  printf("GetIndices post-release: ptr_match=%s\n", gotib == ib ? "yes" : "no");
  if (gotib)
    gotib->Release();

  if (vb_high)
    vb_high->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
