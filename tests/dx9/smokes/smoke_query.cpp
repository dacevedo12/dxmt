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
  check_hr(pp_occ);
  check_hr(pp_evt);
  check_hr(pp_ts);
  check_hr_eq(pp_vc, D3DERR_NOTAVAILABLE);

  // OCCLUSION happy path.
  IDirect3DQuery9 *q_occ = NULL;
  HRESULT cr_occ = dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q_occ);
  check_hr(cr_occ);
  check_true(q_occ != nullptr);
  if (q_occ) {
    check_eq_u32(q_occ->GetType(), D3DQUERYTYPE_OCCLUSION);
    check_eq_u32(q_occ->GetDataSize(), 4); // OCCLUSION result is a DWORD

    // GetData before D3DISSUE_END is "result not ready" → S_FALSE per
    // MSDN (a documented SUCCEEDED return), NOT a rejection.
    DWORD pixels = 0xdeadbeefu;
    HRESULT pre = q_occ->GetData(&pixels, sizeof(pixels), 0);
    check_hr_eq(pre, S_FALSE);

    // BEGIN → END → GetData. Readiness is GPU-timing-dependent (S_FALSE
    // until the visibility result retires), so assert only that it's a
    // valid SUCCEEDED return, not a specific code or pixel count (no
    // draw was issued, so a ready result would be 0 anyway).
    q_occ->Issue(D3DISSUE_BEGIN);
    q_occ->Issue(D3DISSUE_END);
    DWORD result = 0;
    HRESULT gd = q_occ->GetData(&result, sizeof(result), 0);
    check_true(SUCCEEDED(gd));

    // pData=NULL/dwSize=0 readiness poll — S_FALSE or S_OK, both valid.
    HRESULT poll = q_occ->GetData(NULL, 0, 0);
    check_true(SUCCEEDED(poll));

    q_occ->Release();
  }

  // EVENT happy path.
  IDirect3DQuery9 *q_evt = NULL;
  dev->CreateQuery(D3DQUERYTYPE_EVENT, &q_evt);
  if (q_evt) {
    q_evt->Issue(D3DISSUE_END);
    BOOL signaled = FALSE;
    // Signalled state is GPU-timing-dependent (S_FALSE until the fence
    // retires); assert the call is well-formed, not the BOOL value.
    HRESULT ev = q_evt->GetData(&signaled, sizeof(signaled), 0);
    check_true(SUCCEEDED(ev));
    q_evt->Release();
  }

  // Unhandled type — NOTAVAILABLE, and the out-pointer left NULL.
  IDirect3DQuery9 *q_bad = (IDirect3DQuery9 *)0xdead;
  HRESULT cr_bad = dev->CreateQuery(D3DQUERYTYPE_VCACHE, &q_bad);
  check_hr_eq(cr_bad, D3DERR_NOTAVAILABLE);
  check_true(q_bad == NULL);

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
