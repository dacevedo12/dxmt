// Dynamic buffer DISCARD/NOOVERWRITE smoke. Validates: DISCARD on
// a dynamic VB returns a fresh sub-region (different pointer);
// NOOVERWRITE returns the same sub-region as the previous Lock;
// static buffers ignore DISCARD (single-slot ring). Same shape on
// the IndexBuffer side. Markers written through DISCARD-Lock'd
// region 0 must not appear in DISCARD-Lock'd region 1 (different
// sub-regions of the underlying ring).

#include "../dx9_smoke.h"
void
test_dynamic_buffer_rename(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("Direct3DCreate9: NULL\n");
    return;
  }

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

  IDirect3DDevice9 *dev = NULL;
  HRESULT cdhr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(cdhr) || !dev) {
    d3d->Release();
    return;
  }

  // Dynamic VB. DISCARD twice — sub-region pointers must differ.
  IDirect3DVertexBuffer9 *dyn_vb = NULL;
  HRESULT cr = dev->CreateVertexBuffer(256, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &dyn_vb, NULL);
  printf("CreateVertexBuffer(DYNAMIC|WRITEONLY/256): hr=0x%08lx\n", (unsigned long)cr);
  if (dyn_vb) {
    void *p0 = NULL, *p1 = NULL, *p1b = NULL;
    dyn_vb->Lock(0, 256, &p0, D3DLOCK_DISCARD);
    if (p0)
      memset(p0, 0xa5, 4);
    dyn_vb->Unlock();

    dyn_vb->Lock(0, 256, &p1, D3DLOCK_DISCARD);
    if (p1)
      memset(p1, 0x5a, 4);
    dyn_vb->Unlock();

    // NOOVERWRITE — same sub-region as the previous DISCARD.
    dyn_vb->Lock(0, 256, &p1b, D3DLOCK_NOOVERWRITE);
    dyn_vb->Unlock();

    printf("Dynamic VB DISCARD bumps: %s (expect yes)\n", (p0 != p1) ? "yes" : "no");
    printf("Dynamic VB NOOVERWRITE keeps: %s (expect yes)\n", (p1 == p1b) ? "yes" : "no");
    // Cross-check: DISCARD0's marker must NOT show up at DISCARD1's
    // pointer (proves the ring slots are distinct, not aliased).
    if (p1) {
      printf("  region1[0]=0x%02x (expect 0x5a, NOT 0xa5 from region0)\n", *(uint8_t *)p1);
    }
    dyn_vb->Release();
  }

  // Static VB — no rename ring. DISCARD must be a no-op (single
  // slot); successive Locks return the same pointer.
  IDirect3DVertexBuffer9 *stat_vb = NULL;
  dev->CreateVertexBuffer(256, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &stat_vb, NULL);
  if (stat_vb) {
    void *s0 = NULL, *s1 = NULL;
    stat_vb->Lock(0, 256, &s0, D3DLOCK_DISCARD);
    stat_vb->Unlock();
    stat_vb->Lock(0, 256, &s1, D3DLOCK_DISCARD);
    stat_vb->Unlock();
    printf("Static VB DISCARD same: %s (expect yes — single-slot ring)\n", (s0 == s1) ? "yes" : "no");
    stat_vb->Release();
  }

  // Dynamic IB. Same rename behaviour.
  IDirect3DIndexBuffer9 *dyn_ib = NULL;
  HRESULT crib = dev->CreateIndexBuffer(
      256, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &dyn_ib, NULL
  );
  printf("CreateIndexBuffer(DYNAMIC/256): hr=0x%08lx\n", (unsigned long)crib);
  if (dyn_ib) {
    void *q0 = NULL, *q1 = NULL;
    dyn_ib->Lock(0, 256, &q0, D3DLOCK_DISCARD);
    dyn_ib->Unlock();
    dyn_ib->Lock(0, 256, &q1, D3DLOCK_DISCARD);
    dyn_ib->Unlock();
    printf("Dynamic IB DISCARD bumps: %s (expect yes)\n", (q0 != q1) ? "yes" : "no");
    dyn_ib->Release();
  }

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
