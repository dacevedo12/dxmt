// CreateRenderTarget smoke: stand up a windowed device, allocate a
// few RTs across formats and sizes, observe GetDesc, validate
// rejection paths, and exercise the cycle-free Surface→Device
// lifetime ordering. RTs themselves remain non-lockable (LockRect
// rejects); SYSTEMMEM/SCRATCH offscreen-plain surfaces are lockable
// via the buffer-backed path (smoke_lockrect covers that contract).

#include "../dx9_smoke.h"
static void
DescribeSurface(IDirect3DSurface9 *s, const char *label) {
  D3DSURFACE_DESC d = {};
  HRESULT hr = s->GetDesc(&d);
  printf(
      "  %s GetDesc: hr=0x%08lx fmt=0x%08lx type=%u usage=0x%08lx "
      "pool=%u msaa=%u w=%u h=%u\n",
      label, (unsigned long)hr, (unsigned long)d.Format, (unsigned)d.Type, (unsigned long)d.Usage, (unsigned)d.Pool,
      (unsigned)d.MultiSampleType, (unsigned)d.Width, (unsigned)d.Height
  );
  D3DLOCKED_RECT lr = {};
  HRESULT lockhr = s->LockRect(&lr, NULL, 0);
  printf("  %s LockRect: hr=0x%08lx\n", label, (unsigned long)lockhr);
  if (SUCCEEDED(lockhr))
    s->UnlockRect();
}

void
test_create_rendertarget(void) {
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

  IDirect3DDevice9 *dev = nullptr;
  HRESULT cdhr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  printf("CreateDevice: hr=0x%08lx\n", (unsigned long)cdhr);
  if (FAILED(cdhr) || !dev) {
    d3d->Release();
    return;
  }

  // Happy path: 256x256 X8R8G8B8 RT.
  IDirect3DSurface9 *rt0 = nullptr;
  HRESULT r0 = dev->CreateRenderTarget(256, 256, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt0, NULL);
  printf("CreateRenderTarget(X8R8G8B8/256/none): hr=0x%08lx out=%s\n", (unsigned long)r0, rt0 ? "non-null" : "null");
  if (rt0)
    DescribeSurface(rt0, "rt0");

  // A8R8G8B8 with the alpha channel actually meaningful.
  IDirect3DSurface9 *rt1 = nullptr;
  HRESULT r1 = dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt1, NULL);
  printf("CreateRenderTarget(A8R8G8B8/64): hr=0x%08lx out=%s\n", (unsigned long)r1, rt1 ? "non-null" : "null");
  if (rt1)
    DescribeSurface(rt1, "rt1");

  // 4x MSAA RT — we accept this band per CheckDeviceMultiSampleType.
  IDirect3DSurface9 *rt2 = nullptr;
  HRESULT r2 = dev->CreateRenderTarget(128, 128, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &rt2, NULL);
  printf("CreateRenderTarget(X8R8G8B8/128/4x): hr=0x%08lx out=%s\n", (unsigned long)r2, rt2 ? "non-null" : "null");
  if (rt2)
    DescribeSurface(rt2, "rt2");

  // Failure path: depth-stencil format on the RT path is invalid.
  IDirect3DSurface9 *rt_bad = (IDirect3DSurface9 *)0xdead;
  HRESULT rbad = dev->CreateRenderTarget(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt_bad, NULL);
  printf(
      "CreateRenderTarget(D24S8): hr=0x%08lx out=%s "
      "(depth as RT must reject)\n",
      (unsigned long)rbad, rt_bad == NULL ? "null" : "non-null"
  );

  // Failure path: zero-area surface.
  IDirect3DSurface9 *rt_zero = (IDirect3DSurface9 *)0xdead;
  HRESULT rzero = dev->CreateRenderTarget(0, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt_zero, NULL);
  printf("CreateRenderTarget(0x64): hr=0x%08lx out=%s\n", (unsigned long)rzero, rt_zero == NULL ? "null" : "non-null");

  // Failure path: MSAA + Lockable. Metal rejects this combination at
  // descriptor validation; D3D9 itself disallows it. Should fail
  // INVALIDCALL up front rather than reach the Metal allocator.
  IDirect3DSurface9 *rt_msaa_lock = (IDirect3DSurface9 *)0xdead;
  HRESULT rmlock =
      dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, TRUE, &rt_msaa_lock, NULL);
  printf(
      "CreateRenderTarget(MSAA+Lockable): hr=0x%08lx out=%s\n", (unsigned long)rmlock,
      rt_msaa_lock == NULL ? "null" : "non-null"
  );

  // Spec-fidelity: 8x MSAA is a valid enum but unsupported on Apple
  // Silicon — should report NOTAVAILABLE, not INVALIDCALL.
  IDirect3DSurface9 *rt_8x = (IDirect3DSurface9 *)0xdead;
  HRESULT r8x = dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_8_SAMPLES, 0, FALSE, &rt_8x, NULL);
  printf("CreateRenderTarget(8x MSAA): hr=0x%08lx out=%s\n", (unsigned long)r8x, rt_8x == NULL ? "null" : "non-null");

  // Cycle-free lifetime: rt0 holds a public ref on the device
  // (Surface::AddRef bumps device on first call). Release the device
  // first, then use the surface — the surface's stored device pointer
  // must still be valid.
  if (rt0) {
    IDirect3DDevice9 *back = nullptr;
    HRESULT bhr = rt0->GetDevice(&back);
    printf(
        "rt0->GetDevice (with dev still held): hr=0x%08lx same=%s\n", (unsigned long)bhr, (back == dev) ? "yes" : "no"
    );
    if (back)
      back->Release();
  }

  // ---- Depth-stencil surfaces ----
  IDirect3DSurface9 *ds0 = nullptr;
  HRESULT d0 = dev->CreateDepthStencilSurface(256, 256, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds0, NULL);
  printf(
      "CreateDepthStencilSurface(D24S8/256/discard): hr=0x%08lx out=%s\n", (unsigned long)d0, ds0 ? "non-null" : "null"
  );
  if (ds0)
    DescribeSurface(ds0, "ds0");

  // Non-discard depth — should land in Private storage. We can't see
  // the storage mode from the d3d9 side, but the descriptor and the
  // refcount path are still observable.
  IDirect3DSurface9 *ds1 = nullptr;
  HRESULT d1 = dev->CreateDepthStencilSurface(64, 64, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, &ds1, NULL);
  printf("CreateDepthStencilSurface(D16/64/keep): hr=0x%08lx out=%s\n", (unsigned long)d1, ds1 ? "non-null" : "null");
  if (ds1)
    DescribeSurface(ds1, "ds1");

  // Failure path: colour format on the depth-stencil path is invalid.
  IDirect3DSurface9 *ds_bad = (IDirect3DSurface9 *)0xdead;
  HRESULT dbad = dev->CreateDepthStencilSurface(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds_bad, NULL);
  printf(
      "CreateDepthStencilSurface(X8R8G8B8): hr=0x%08lx out=%s "
      "(colour as DS must reject)\n",
      (unsigned long)dbad, ds_bad == NULL ? "null" : "non-null"
  );

  // 4x MSAA depth — Memoryless+MSAA on Apple Silicon TBDR.
  IDirect3DSurface9 *ds_msaa = nullptr;
  HRESULT dmsaa =
      dev->CreateDepthStencilSurface(128, 128, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, TRUE, &ds_msaa, NULL);
  printf(
      "CreateDepthStencilSurface(D24S8/128/4x/discard): hr=0x%08lx out=%s\n", (unsigned long)dmsaa,
      ds_msaa ? "non-null" : "null"
  );
  if (ds_msaa)
    DescribeSurface(ds_msaa, "ds_msaa");

  // Spec-fidelity: 8x MSAA is a valid enum but unsupported on Apple
  // Silicon — should report NOTAVAILABLE, mirroring the RT path.
  IDirect3DSurface9 *ds_8x = (IDirect3DSurface9 *)0xdead;
  HRESULT d8x = dev->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_8_SAMPLES, 0, TRUE, &ds_8x, NULL);
  printf(
      "CreateDepthStencilSurface(8x MSAA): hr=0x%08lx out=%s\n", (unsigned long)d8x, ds_8x == NULL ? "null" : "non-null"
  );

  // D32F_LOCKABLE keeps — exercises the alternate depth Metal format
  // (Depth32Float) and the LOCKABLE-named-but-not-yet-Lock-able case.
  IDirect3DSurface9 *ds_d32f = nullptr;
  HRESULT dd32 =
      dev->CreateDepthStencilSurface(64, 64, D3DFMT_D32F_LOCKABLE, D3DMULTISAMPLE_NONE, 0, FALSE, &ds_d32f, NULL);
  printf(
      "CreateDepthStencilSurface(D32F_LOCKABLE/keep): hr=0x%08lx out=%s\n", (unsigned long)dd32,
      ds_d32f ? "non-null" : "null"
  );
  if (ds_d32f)
    DescribeSurface(ds_d32f, "ds_d32f");

  // ---- Offscreen plain surfaces ----
  // DEFAULT pool — GPU-resident, legal StretchRect dest.
  IDirect3DSurface9 *ops_def = nullptr;
  HRESULT op0 = dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ops_def, NULL);
  printf(
      "CreateOffscreenPlainSurface(A8R8G8B8/64/DEFAULT): hr=0x%08lx out=%s\n", (unsigned long)op0,
      ops_def ? "non-null" : "null"
  );
  if (ops_def)
    DescribeSurface(ops_def, "ops_def");

  // SYSTEMMEM pool — CPU-resident, legal UpdateSurface source.
  IDirect3DSurface9 *ops_sys = nullptr;
  HRESULT op1 = dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &ops_sys, NULL);
  printf(
      "CreateOffscreenPlainSurface(X8R8G8B8/64/SYSTEMMEM): hr=0x%08lx out=%s\n", (unsigned long)op1,
      ops_sys ? "non-null" : "null"
  );
  if (ops_sys)
    DescribeSurface(ops_sys, "ops_sys");

  // SCRATCH pool — CPU-only, no GPU bind allowed.
  IDirect3DSurface9 *ops_scratch = nullptr;
  HRESULT op2 = dev->CreateOffscreenPlainSurface(32, 32, D3DFMT_R5G6B5, D3DPOOL_SCRATCH, &ops_scratch, NULL);
  printf(
      "CreateOffscreenPlainSurface(R5G6B5/32/SCRATCH): hr=0x%08lx out=%s\n", (unsigned long)op2,
      ops_scratch ? "non-null" : "null"
  );
  if (ops_scratch)
    DescribeSurface(ops_scratch, "ops_scratch");

  // Failure path: D3DPOOL_MANAGED is contract-illegal here.
  IDirect3DSurface9 *ops_managed = (IDirect3DSurface9 *)0xdead;
  HRESULT opm = dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &ops_managed, NULL);
  printf(
      "CreateOffscreenPlainSurface(MANAGED): hr=0x%08lx out=%s "
      "(MANAGED on plain surface must reject)\n",
      (unsigned long)opm, ops_managed == NULL ? "null" : "non-null"
  );

  // Failure path: depth-stencil format on the plain-surface path is
  // invalid (no sampleable mapping).
  IDirect3DSurface9 *ops_dsfmt = (IDirect3DSurface9 *)0xdead;
  HRESULT opd = dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_D24S8, D3DPOOL_DEFAULT, &ops_dsfmt, NULL);
  printf(
      "CreateOffscreenPlainSurface(D24S8): hr=0x%08lx out=%s "
      "(depth fmt as plain must reject)\n",
      (unsigned long)opd, ops_dsfmt == NULL ? "null" : "non-null"
  );

  if (ops_def)
    ops_def->Release();
  if (ops_sys)
    ops_sys->Release();
  if (ops_scratch)
    ops_scratch->Release();

  if (ds0)
    ds0->Release();
  if (ds1)
    ds1->Release();
  if (ds_msaa)
    ds_msaa->Release();
  if (ds_d32f)
    ds_d32f->Release();
  if (rt1)
    rt1->Release();
  if (rt2)
    rt2->Release();

  // rt0 outlives the device: release device, then use rt0.
  ULONG dev_after_release = dev->Release();
  printf("Release(dev) while rt0 held: %lu (expect non-zero — rt0 pins it)\n", (unsigned long)dev_after_release);

  if (rt0) {
    IDirect3DDevice9 *back2 = nullptr;
    HRESULT bhr = rt0->GetDevice(&back2);
    printf("rt0->GetDevice after dev release: hr=0x%08lx dev=%s\n", (unsigned long)bhr, back2 ? "non-null" : "null");
    if (back2)
      back2->Release();
    ULONG ref = rt0->Release();
    printf("Release(rt0): %lu (chain release should also tear the device)\n", (unsigned long)ref);
  }

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
