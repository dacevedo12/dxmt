// CreateQuery / Query lifecycle smoke.
//
// Validates: support-probe (ppQuery=NULL), happy-path BEGIN→END→
// GetData round-trip, GetData-before-END rejection, NOTAVAILABLE
// for unhandled types, GetDataSize per-type values.

#include "../dx9_smoke.h"
void
test_query(void) {
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
  printf("CreateDevice: hr=0x%08lx\n", (unsigned long)cdhr);
  if (FAILED(cdhr) || !dev) {
    d3d->Release();
    return;
  }

  // Support probes — ppQuery=NULL means "is this type supported?".
  HRESULT pp_occ = dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, NULL);
  HRESULT pp_evt = dev->CreateQuery(D3DQUERYTYPE_EVENT, NULL);
  HRESULT pp_ts = dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, NULL);
  HRESULT pp_vc = dev->CreateQuery(D3DQUERYTYPE_VCACHE, NULL);
  printf("Probe OCCLUSION: hr=0x%08lx (expect OK)\n", (unsigned long)pp_occ);
  printf("Probe EVENT:     hr=0x%08lx (expect OK)\n", (unsigned long)pp_evt);
  printf("Probe TIMESTAMP: hr=0x%08lx (expect OK)\n", (unsigned long)pp_ts);
  printf("Probe VCACHE:    hr=0x%08lx (expect 0x8876086a NOTAVAILABLE)\n", (unsigned long)pp_vc);

  // OCCLUSION happy path.
  IDirect3DQuery9 *q_occ = NULL;
  HRESULT cr_occ = dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q_occ);
  printf("CreateQuery(OCCLUSION): hr=0x%08lx out=%s\n", (unsigned long)cr_occ, q_occ ? "non-null" : "null");
  if (q_occ) {
    printf("  GetType: 0x%lx (expect 0x9 = D3DQUERYTYPE_OCCLUSION)\n", (unsigned long)q_occ->GetType());
    printf("  GetDataSize: %lu (expect 4)\n", (unsigned long)q_occ->GetDataSize());

    // GetData before END — must reject.
    DWORD pixels = 0xdeadbeefu;
    HRESULT pre = q_occ->GetData(&pixels, sizeof(pixels), 0);
    printf("  GetData before END: hr=0x%08lx (must reject)\n", (unsigned long)pre);

    // BEGIN → END → GetData.
    q_occ->Issue(D3DISSUE_BEGIN);
    q_occ->Issue(D3DISSUE_END);
    DWORD result = 0;
    HRESULT gd = q_occ->GetData(&result, sizeof(result), 0);
    printf("  GetData after END: hr=0x%08lx pixels=%lu (expect non-zero)\n", (unsigned long)gd, (unsigned long)result);

    // Polling pData=NULL dwSize=0 — readiness probe.
    HRESULT poll = q_occ->GetData(NULL, 0, 0);
    printf("  GetData(NULL/0) poll: hr=0x%08lx (expect OK)\n", (unsigned long)poll);

    q_occ->Release();
  }

  // EVENT happy path.
  IDirect3DQuery9 *q_evt = NULL;
  dev->CreateQuery(D3DQUERYTYPE_EVENT, &q_evt);
  if (q_evt) {
    q_evt->Issue(D3DISSUE_END);
    BOOL signaled = FALSE;
    q_evt->GetData(&signaled, sizeof(signaled), 0);
    printf("EVENT GetData: signaled=%d (expect 1)\n", (int)signaled);
    q_evt->Release();
  }

  // Unhandled type — must reject.
  IDirect3DQuery9 *q_bad = (IDirect3DQuery9 *)0xdead;
  HRESULT cr_bad = dev->CreateQuery(D3DQUERYTYPE_VCACHE, &q_bad);
  printf("CreateQuery(VCACHE): hr=0x%08lx out=%s\n", (unsigned long)cr_bad, q_bad == NULL ? "null" : "non-null");

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
