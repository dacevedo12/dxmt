// CreateVertexBuffer + CreateIndexBuffer smoke: a few buffers across
// pools/usages, hit GetDesc + Lock/Unlock, then validate the
// rejection paths the runtime is expected to gate (SCRATCH, RT/DS
// usage on a buffer, zero size, non-index format on an IB).

#include "../dx9_smoke.h"
static void
DescribeVB(IDirect3DVertexBuffer9 *vb, const char *label) {
  D3DVERTEXBUFFER_DESC d = {};
  HRESULT hr = vb->GetDesc(&d);
  printf(
      "  %s GetDesc: hr=0x%08lx fmt=0x%08lx type=%u usage=0x%08lx "
      "pool=%u size=%u fvf=0x%08lx\n",
      label, (unsigned long)hr, (unsigned long)d.Format, (unsigned)d.Type, (unsigned long)d.Usage, (unsigned)d.Pool,
      (unsigned)d.Size, (unsigned long)d.FVF
  );
  void *ptr = NULL;
  HRESULT lhr = vb->Lock(0, 0, &ptr, 0);
  printf("  %s Lock: hr=0x%08lx ptr=%s\n", label, (unsigned long)lhr, ptr ? "non-null" : "null");
  if (SUCCEEDED(lhr)) {
    HRESULT uhr = vb->Unlock();
    printf("  %s Unlock: hr=0x%08lx\n", label, (unsigned long)uhr);
  }
}

static void
DescribeIB(IDirect3DIndexBuffer9 *ib, const char *label) {
  D3DINDEXBUFFER_DESC d = {};
  HRESULT hr = ib->GetDesc(&d);
  printf(
      "  %s GetDesc: hr=0x%08lx fmt=0x%08lx type=%u usage=0x%08lx "
      "pool=%u size=%u\n",
      label, (unsigned long)hr, (unsigned long)d.Format, (unsigned)d.Type, (unsigned long)d.Usage, (unsigned)d.Pool,
      (unsigned)d.Size
  );
  void *ptr = NULL;
  HRESULT lhr = ib->Lock(0, 0, &ptr, 0);
  printf("  %s Lock: hr=0x%08lx ptr=%s\n", label, (unsigned long)lhr, ptr ? "non-null" : "null");
  if (SUCCEEDED(lhr)) {
    HRESULT uhr = ib->Unlock();
    printf("  %s Unlock: hr=0x%08lx\n", label, (unsigned long)uhr);
  }
}

void
test_create_buffer(void) {
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

  // Static DEFAULT vertex buffer.
  IDirect3DVertexBuffer9 *vb_def = NULL;
  HRESULT r0 = dev->CreateVertexBuffer(4096, 0, D3DFVF_XYZ | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &vb_def, NULL);
  printf(
      "CreateVertexBuffer(4096/DEFAULT/static): hr=0x%08lx out=%s\n", (unsigned long)r0, vb_def ? "non-null" : "null"
  );
  if (vb_def)
    DescribeVB(vb_def, "vb_def");

  // Dynamic + WRITEONLY DEFAULT vertex buffer (most common shape for
  // streaming geometry).
  IDirect3DVertexBuffer9 *vb_dyn = NULL;
  HRESULT r1 = dev->CreateVertexBuffer(8192, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb_dyn, NULL);
  printf(
      "CreateVertexBuffer(8192/DEFAULT/DYN+WO): hr=0x%08lx out=%s\n", (unsigned long)r1, vb_dyn ? "non-null" : "null"
  );
  if (vb_dyn)
    DescribeVB(vb_dyn, "vb_dyn");

  // SYSTEMMEM vertex buffer.
  IDirect3DVertexBuffer9 *vb_sys = NULL;
  HRESULT r2 = dev->CreateVertexBuffer(1024, 0, 0, D3DPOOL_SYSTEMMEM, &vb_sys, NULL);
  printf("CreateVertexBuffer(1024/SYSTEMMEM): hr=0x%08lx out=%s\n", (unsigned long)r2, vb_sys ? "non-null" : "null");
  if (vb_sys)
    DescribeVB(vb_sys, "vb_sys");

  // ---- Failure paths ----
  // SCRATCH not allowed for buffers.
  IDirect3DVertexBuffer9 *vb_bad1 = (IDirect3DVertexBuffer9 *)0xdead;
  HRESULT rb1 = dev->CreateVertexBuffer(1024, 0, 0, D3DPOOL_SCRATCH, &vb_bad1, NULL);
  printf(
      "CreateVertexBuffer(SCRATCH): hr=0x%08lx out=%s "
      "(SCRATCH on buffer must reject)\n",
      (unsigned long)rb1, vb_bad1 == NULL ? "null" : "non-null"
  );

  // RT usage not allowed on a buffer.
  IDirect3DVertexBuffer9 *vb_bad2 = (IDirect3DVertexBuffer9 *)0xdead;
  HRESULT rb2 = dev->CreateVertexBuffer(1024, D3DUSAGE_RENDERTARGET, 0, D3DPOOL_DEFAULT, &vb_bad2, NULL);
  printf(
      "CreateVertexBuffer(RT usage): hr=0x%08lx out=%s\n", (unsigned long)rb2, vb_bad2 == NULL ? "null" : "non-null"
  );

  // DS usage not allowed on a buffer.
  IDirect3DVertexBuffer9 *vb_bad3 = (IDirect3DVertexBuffer9 *)0xdead;
  HRESULT rb3 = dev->CreateVertexBuffer(1024, D3DUSAGE_DEPTHSTENCIL, 0, D3DPOOL_DEFAULT, &vb_bad3, NULL);
  printf(
      "CreateVertexBuffer(DS usage): hr=0x%08lx out=%s\n", (unsigned long)rb3, vb_bad3 == NULL ? "null" : "non-null"
  );

  // Zero-size buffer.
  IDirect3DVertexBuffer9 *vb_bad4 = (IDirect3DVertexBuffer9 *)0xdead;
  HRESULT rb4 = dev->CreateVertexBuffer(0, 0, 0, D3DPOOL_DEFAULT, &vb_bad4, NULL);
  printf("CreateVertexBuffer(size=0): hr=0x%08lx out=%s\n", (unsigned long)rb4, vb_bad4 == NULL ? "null" : "non-null");

  // ---- Lifetime: VB->GetDevice after device release. VB pins
  // device; chain release should also tear the device down.
  ULONG vb_ref = 0;
  if (vb_def) {
    IDirect3DDevice9 *back = NULL;
    HRESULT bhr = vb_def->GetDevice(&back);
    printf(
        "vb_def->GetDevice (with dev still held): hr=0x%08lx same=%s\n", (unsigned long)bhr,
        (back == dev) ? "yes" : "no"
    );
    if (back)
      back->Release();
  }
  // ---- Index buffers ----
  // INDEX16 DEFAULT static.
  IDirect3DIndexBuffer9 *ib_def = NULL;
  HRESULT ri0 = dev->CreateIndexBuffer(2048, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib_def, NULL);
  printf(
      "CreateIndexBuffer(2048/INDEX16/DEFAULT/static): hr=0x%08lx out=%s\n", (unsigned long)ri0,
      ib_def ? "non-null" : "null"
  );
  if (ib_def)
    DescribeIB(ib_def, "ib_def");

  // INDEX32 DEFAULT DYN+WO.
  IDirect3DIndexBuffer9 *ib_dyn = NULL;
  HRESULT ri1 = dev->CreateIndexBuffer(
      4096, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &ib_dyn, NULL
  );
  printf(
      "CreateIndexBuffer(4096/INDEX32/DEFAULT/DYN+WO): hr=0x%08lx out=%s\n", (unsigned long)ri1,
      ib_dyn ? "non-null" : "null"
  );
  if (ib_dyn)
    DescribeIB(ib_dyn, "ib_dyn");

  // ---- IB failure paths ----
  // Non-index format.
  IDirect3DIndexBuffer9 *ib_bad1 = (IDirect3DIndexBuffer9 *)0xdead;
  HRESULT rib1 = dev->CreateIndexBuffer(1024, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ib_bad1, NULL);
  printf(
      "CreateIndexBuffer(A8R8G8B8 fmt): hr=0x%08lx out=%s "
      "(non-index format must reject)\n",
      (unsigned long)rib1, ib_bad1 == NULL ? "null" : "non-null"
  );

  // SCRATCH not allowed.
  IDirect3DIndexBuffer9 *ib_bad2 = (IDirect3DIndexBuffer9 *)0xdead;
  HRESULT rib2 = dev->CreateIndexBuffer(1024, 0, D3DFMT_INDEX16, D3DPOOL_SCRATCH, &ib_bad2, NULL);
  printf("CreateIndexBuffer(SCRATCH): hr=0x%08lx out=%s\n", (unsigned long)rib2, ib_bad2 == NULL ? "null" : "non-null");

  // RT usage.
  IDirect3DIndexBuffer9 *ib_bad3 = (IDirect3DIndexBuffer9 *)0xdead;
  HRESULT rib3 = dev->CreateIndexBuffer(1024, D3DUSAGE_RENDERTARGET, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib_bad3, NULL);
  printf(
      "CreateIndexBuffer(RT usage): hr=0x%08lx out=%s\n", (unsigned long)rib3, ib_bad3 == NULL ? "null" : "non-null"
  );

  // Zero size.
  IDirect3DIndexBuffer9 *ib_bad4 = (IDirect3DIndexBuffer9 *)0xdead;
  HRESULT rib4 = dev->CreateIndexBuffer(0, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib_bad4, NULL);
  printf("CreateIndexBuffer(size=0): hr=0x%08lx out=%s\n", (unsigned long)rib4, ib_bad4 == NULL ? "null" : "non-null");

  if (ib_def)
    ib_def->Release();
  if (ib_dyn)
    ib_dyn->Release();

  if (vb_dyn)
    vb_dyn->Release();
  if (vb_sys)
    vb_sys->Release();

  ULONG dev_after = dev->Release();
  printf("Release(dev) while vb_def held: %lu (expect non-zero — vb pins it)\n", (unsigned long)dev_after);

  if (vb_def) {
    IDirect3DDevice9 *back2 = NULL;
    HRESULT bhr = vb_def->GetDevice(&back2);
    printf("vb_def->GetDevice after dev release: hr=0x%08lx dev=%s\n", (unsigned long)bhr, back2 ? "non-null" : "null");
    if (back2)
      back2->Release();
    vb_ref = vb_def->Release();
    printf("Release(vb_def): %lu\n", (unsigned long)vb_ref);
  }

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
