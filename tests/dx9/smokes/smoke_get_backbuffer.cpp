// GetBackBuffer + the implicit-RT auto-bind. Three-way identity check
// (chain BB, device BB, device GetRenderTarget(0)) plus rejection
// paths and refcount stepping.

#include "../dx9_smoke.h"
void
test_get_backbuffer(void) {
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

  // ---- Identity: device GetBackBuffer == swapchain GetBackBuffer
  // == device GetRenderTarget(0). ----
  IDirect3DSwapChain9 *chain = NULL;
  HRESULT gsc = dev->GetSwapChain(0, &chain);
  printf("GetSwapChain(0): hr=0x%08lx\n", (unsigned long)gsc);

  IDirect3DSurface9 *bb_chain = NULL;
  HRESULT gbc = chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb_chain);
  printf("chain->GetBackBuffer(0,MONO): hr=0x%08lx out=%s\n", (unsigned long)gbc, bb_chain ? "ok" : "null");

  IDirect3DSurface9 *bb_dev = NULL;
  HRESULT gbd = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb_dev);
  printf(
      "dev->GetBackBuffer(0,0,MONO): hr=0x%08lx out=%s same=%s\n", (unsigned long)gbd, bb_dev ? "ok" : "null",
      bb_dev == bb_chain ? "yes" : "no"
  );

  IDirect3DSurface9 *rt0 = NULL;
  HRESULT grt = dev->GetRenderTarget(0, &rt0);
  printf(
      "dev->GetRenderTarget(0): hr=0x%08lx out=%s same_as_bb=%s\n", (unsigned long)grt, rt0 ? "ok" : "null",
      rt0 == bb_chain ? "yes" : "no"
  );

  // Backbuffer's GetDesc reflects the requested params.
  if (bb_chain) {
    D3DSURFACE_DESC desc = {};
    bb_chain->GetDesc(&desc);
    printf(
        "backbuffer desc: %lux%lu fmt=0x%lx pool=0x%lx usage=0x%lx\n", (unsigned long)desc.Width,
        (unsigned long)desc.Height, (unsigned long)desc.Format, (unsigned long)desc.Pool, (unsigned long)desc.Usage
    );
  }

  // ---- Rejection paths. ----
  IDirect3DSurface9 *poison = (IDirect3DSurface9 *)0xdeadbeef;
  HRESULT bad = chain->GetBackBuffer(1, D3DBACKBUFFER_TYPE_MONO, &poison);
  printf(
      "chain->GetBackBuffer(1,MONO): hr=0x%08lx out=%s "
      "(want INVALIDCALL, NULL)\n",
      (unsigned long)bad, poison ? "non-null" : "null"
  );

  poison = (IDirect3DSurface9 *)0xdeadbeef;
  bad = chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_LEFT, &poison);
  printf(
      "chain->GetBackBuffer(0,LEFT): hr=0x%08lx out=%s "
      "(want INVALIDCALL, NULL)\n",
      (unsigned long)bad, poison ? "non-null" : "null"
  );

  bad = chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, NULL);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  bad = dev->GetBackBuffer(1, 0, D3DBACKBUFFER_TYPE_MONO, &poison);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  poison = (IDirect3DSurface9 *)0xdeadbeef;
  bad = dev->GetBackBuffer(0, 1, D3DBACKBUFFER_TYPE_MONO, &poison);
  printf(
      "dev->GetBackBuffer(swap=0, ibb=1): hr=0x%08lx out=%s "
      "(want INVALIDCALL via chain forwarder)\n",
      (unsigned long)bad, poison ? "non-null" : "null"
  );

  // We created via the non-Ex CreateDevice path, so QI to the Ex
  // interface must reject. (The Ex-side identity check belongs in a
  // separate smoke once we have an Ex-device fixture.)
  IDirect3DSwapChain9Ex *chainEx = (IDirect3DSwapChain9Ex *)0xdeadbeef;
  HRESULT qiEx = chain->QueryInterface(__uuidof(IDirect3DSwapChain9Ex), (void **)&chainEx);
  printf(
      "chain->QI(SwapChain9Ex) on non-Ex chain: hr=0x%08lx out=%s "
      "(want E_NOINTERFACE, NULL)\n",
      (unsigned long)qiEx, chainEx ? "non-null" : "null"
  );

  // ---- Lifetime: drop our refs in reverse, watch refcount step. ----
  if (rt0) {
    ULONG r = rt0->Release();
    printf("Release(rt0): %lu (drops the GetRenderTarget addref)\n", (unsigned long)r);
  }
  if (bb_dev) {
    ULONG r = bb_dev->Release();
    printf("Release(bb_dev): %lu\n", (unsigned long)r);
  }
  if (bb_chain) {
    ULONG r = bb_chain->Release();
    printf("Release(bb_chain): %lu (chain priv pin still alive)\n", (unsigned long)r);
  }

  if (chain)
    chain->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
