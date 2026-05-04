// SetTransform / GetTransform / MultiplyTransform — bookkeeping only.
// dxmt has no FFP rasterizer, so the matrices are just stored; apps
// that defensively call these (LoL setup paths, etc.) get S_OK plus
// a faithful round-trip. Validates: identity-default, set/get
// round-trip across the dense index table (VIEW, PROJECTION, TEXTURE,
// WORLD), MultiplyTransform numeric correctness on a known case.

#include "../dx9_smoke.h"
static bool
is_identity(const D3DMATRIX &m) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      if (m.m[i][j] != (i == j ? 1.0f : 0.0f))
        return false;
  return true;
}

static bool
eq_matrix(const D3DMATRIX &a, const D3DMATRIX &b) {
  return memcmp(&a, &b, sizeof(a)) == 0;
}

void
test_settransform(void) {
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

  // ---- Default identity. ----
  D3DMATRIX got = {};
  HRESULT g0 = dev->GetTransform(D3DTS_VIEW, &got);
  printf("GetTransform(VIEW default): hr=0x%08lx identity=%s\n", (unsigned long)g0, is_identity(got) ? "yes" : "no");

  HRESULT g1 = dev->GetTransform(D3DTS_PROJECTION, &got);
  printf(
      "GetTransform(PROJECTION default): hr=0x%08lx identity=%s\n", (unsigned long)g1, is_identity(got) ? "yes" : "no"
  );

  HRESULT g2 = dev->GetTransform(D3DTS_WORLD, &got);
  printf("GetTransform(WORLD default): hr=0x%08lx identity=%s\n", (unsigned long)g2, is_identity(got) ? "yes" : "no");

  HRESULT g3 = dev->GetTransform(D3DTS_TEXTURE3, &got);
  printf(
      "GetTransform(TEXTURE3 default): hr=0x%08lx identity=%s\n", (unsigned long)g3, is_identity(got) ? "yes" : "no"
  );

  // ---- Set/Get round-trip across the dense table. ----
  D3DMATRIX viewM = {};
  viewM.m[0][0] = 2.0f;
  viewM.m[1][1] = 3.0f;
  viewM.m[2][2] = 5.0f;
  viewM.m[3][3] = 7.0f;
  viewM.m[0][3] = 11.0f;
  HRESULT s0 = dev->SetTransform(D3DTS_VIEW, &viewM);
  printf("SetTransform(VIEW): hr=0x%08lx\n", (unsigned long)s0);
  dev->GetTransform(D3DTS_VIEW, &got);
  printf("VIEW round-trip: match=%s\n", eq_matrix(got, viewM) ? "yes" : "no");

  D3DMATRIX projM = {};
  projM.m[0][0] = 13.0f;
  projM.m[1][1] = 17.0f;
  projM.m[2][2] = 19.0f;
  projM.m[3][3] = 23.0f;
  dev->SetTransform(D3DTS_PROJECTION, &projM);
  dev->GetTransform(D3DTS_PROJECTION, &got);
  printf("PROJECTION round-trip: match=%s\n", eq_matrix(got, projM) ? "yes" : "no");

  // WORLDMATRIX(255) — the highest dense index.
  D3DMATRIX wmHi = {};
  wmHi.m[0][0] = 99.0f;
  wmHi.m[3][3] = 1.0f;
  dev->SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_WORLDMATRIX(255)), &wmHi);
  dev->GetTransform(D3DTRANSFORMSTATETYPE(D3DTS_WORLDMATRIX(255)), &got);
  printf("WORLDMATRIX(255) round-trip: match=%s\n", eq_matrix(got, wmHi) ? "yes" : "no");

  // VIEW must still hold its own value — no aliasing across slots.
  dev->GetTransform(D3DTS_VIEW, &got);
  printf("VIEW unchanged after WORLD set: match=%s\n", eq_matrix(got, viewM) ? "yes" : "no");

  // ---- MultiplyTransform numeric check. ----
  // Reset VIEW to identity, then multiply by viewM; result must equal
  // viewM (identity * X = X).
  D3DMATRIX ident = {};
  ident.m[0][0] = ident.m[1][1] = ident.m[2][2] = ident.m[3][3] = 1.0f;
  dev->SetTransform(D3DTS_VIEW, &ident);
  dev->MultiplyTransform(D3DTS_VIEW, &viewM);
  dev->GetTransform(D3DTS_VIEW, &got);
  printf("MultiplyTransform(I*V): match-V=%s\n", eq_matrix(got, viewM) ? "yes" : "no");

  // Multiply by a translation, verify the (0,3)/(1,3)/(2,3) row picks
  // up the translation since pre-multiply * identity-current = translate.
  // Actually: MultiplyTransform is current = current * pMatrix. So
  // V_new = viewM * T. Predict by hand for one element to sanity-check.
  D3DMATRIX trans = {};
  trans.m[0][0] = trans.m[1][1] = trans.m[2][2] = trans.m[3][3] = 1.0f;
  trans.m[3][0] = 100.0f; // column-3-row-3 in row-major: T[3][0..2] = tx,ty,tz
  // viewM is diagonal {2,3,5,7} with viewM[0][3]=11.
  // (viewM * trans)[0][0] = 2*1 + 0 + 0 + 11*100 = 1102
  // (viewM * trans)[3][3] = 7
  dev->SetTransform(D3DTS_VIEW, &viewM);
  dev->MultiplyTransform(D3DTS_VIEW, &trans);
  dev->GetTransform(D3DTS_VIEW, &got);
  printf(
      "Multiply(viewM * trans): [0][0]=%.0f want=1102 match=%s\n", got.m[0][0], got.m[0][0] == 1102.0f ? "yes" : "no"
  );
  printf("                          [3][3]=%.0f want=7 match=%s\n", got.m[3][3], got.m[3][3] == 7.0f ? "yes" : "no");

  // ---- Rejection paths. ----
  HRESULT bad = dev->SetTransform(D3DTS_VIEW, NULL);
  check_hr_eq(bad, D3DERR_INVALIDCALL);
  bad = dev->GetTransform(D3DTS_VIEW, NULL);
  check_hr_eq(bad, D3DERR_INVALIDCALL);
  bad = dev->MultiplyTransform(D3DTS_VIEW, NULL);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  // ---- TEXTURE0..7 dense-index aliasing check. Indices 2..9 must
  // not alias each other or the VIEW/PROJECTION slots. ----
  for (int i = 0; i < 8; ++i) {
    D3DMATRIX tm = {};
    tm.m[0][0] = (float)(i + 1);
    tm.m[3][3] = 1.0f;
    dev->SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + i), &tm);
  }
  bool tex_ok = true;
  for (int i = 0; i < 8; ++i) {
    dev->GetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + i), &got);
    if (got.m[0][0] != (float)(i + 1)) {
      tex_ok = false;
      break;
    }
  }
  printf("TEXTURE0..7 no-alias: match=%s\n", tex_ok ? "yes" : "no");

  // Out-of-range state value — the unsigned-wrap path through
  // TransformIndex must hit the kMaxTransforms bounds check.
  D3DMATRIX dummy = {};
  dummy.m[0][0] = 1.0f;
  HRESULT oob = dev->SetTransform((D3DTRANSFORMSTATETYPE)0xDEAD, &dummy);
  check_hr_eq(oob, D3DERR_INVALIDCALL);

  ULONG dr = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dr);
  ULONG r = d3d->Release();
  printf("Release: %lu\n", (unsigned long)r);
}
