// SetMaterial / SetLight / LightEnable smoke. FFP fixed-function
// lighting bookkeeping: pre-Set defaults, round-trips, sparse-index
// growth, NULL-pointer rejection, Light::Type range validation, and
// GetLight on an index that's never been Set returning INVALIDCALL.

#include "../dx9_smoke.h"
void
test_setmaterial_setlight(void) {
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

  // Default material — wined3d defaults: Diffuse=(1,1,1,1), all
  // others zero, Power=0.
  D3DMATERIAL9 m = {};
  HRESULT hr = dev->GetMaterial(&m);
  printf(
      "Default Material: hr=0x%08lx Diffuse=%.0f,%.0f,%.0f,%.0f\n", (unsigned long)hr, m.Diffuse.r, m.Diffuse.g,
      m.Diffuse.b, m.Diffuse.a
  );
  printf("Default Material Power=%.1f Ambient.r=%.1f\n", m.Power, m.Ambient.r);

  // Round-trip a material with non-default values.
  D3DMATERIAL9 setM = {};
  setM.Diffuse = {0.25f, 0.50f, 0.75f, 1.00f};
  setM.Ambient = {0.10f, 0.10f, 0.10f, 0.10f};
  setM.Specular = {0.30f, 0.30f, 0.30f, 0.30f};
  setM.Emissive = {0.05f, 0.05f, 0.05f, 0.05f};
  setM.Power = 16.0f;
  hr = dev->SetMaterial(&setM);
  printf("SetMaterial: hr=0x%08lx\n", (unsigned long)hr);
  D3DMATERIAL9 getM = {};
  hr = dev->GetMaterial(&getM);
  printf(
      "GetMaterial: hr=0x%08lx Power=%.1f match=%s\n", (unsigned long)hr, getM.Power,
      memcmp(&setM, &getM, sizeof(setM)) == 0 ? "yes" : "no"
  );

  // NULL rejection.
  hr = dev->SetMaterial(NULL);
  printf("SetMaterial(NULL): hr=0x%08lx (must reject)\n", (unsigned long)hr);
  hr = dev->GetMaterial(NULL);
  printf("GetMaterial(NULL): hr=0x%08lx (must reject)\n", (unsigned long)hr);

  // GetLight on an index that's never been Set.
  D3DLIGHT9 gl = {};
  hr = dev->GetLight(0, &gl);
  printf("GetLight(0) pre-Set: hr=0x%08lx (must reject)\n", (unsigned long)hr);

  // Round-trip a light at index 3 — sparse growth.
  D3DLIGHT9 sl = {};
  sl.Type = D3DLIGHT_POINT;
  sl.Diffuse = {1.0f, 0.5f, 0.25f, 1.0f};
  sl.Position = {10.0f, 20.0f, 30.0f};
  sl.Range = 100.0f;
  hr = dev->SetLight(3, &sl);
  printf("SetLight(3, POINT): hr=0x%08lx\n", (unsigned long)hr);
  D3DLIGHT9 rl = {};
  hr = dev->GetLight(3, &rl);
  printf(
      "GetLight(3): hr=0x%08lx Type=%d Range=%.1f match=%s\n", (unsigned long)hr, (int)rl.Type, rl.Range,
      memcmp(&sl, &rl, sizeof(sl)) == 0 ? "yes" : "no"
  );

  // GetLight at index 0/1/2 still INVALIDCALL — sparse growth doesn't
  // implicitly Set lower indices.
  hr = dev->GetLight(0, &gl);
  printf("GetLight(0) after Set(3): hr=0x%08lx (must reject)\n", (unsigned long)hr);

  // LightEnable on an index never Set — wined3d implicitly creates
  // a default directional light at that index.
  hr = dev->LightEnable(5, TRUE);
  printf("LightEnable(5, TRUE) on unset: hr=0x%08lx\n", (unsigned long)hr);
  BOOL le = FALSE;
  hr = dev->GetLightEnable(5, &le);
  printf("GetLightEnable(5): hr=0x%08lx enable=%d (want 1)\n", (unsigned long)hr, (int)le);
  hr = dev->GetLight(5, &rl);
  printf(
      "GetLight(5) after LightEnable: hr=0x%08lx Type=%d (want %d=DIR)\n", (unsigned long)hr, (int)rl.Type,
      D3DLIGHT_DIRECTIONAL
  );

  // Disable a previously-enabled light.
  hr = dev->LightEnable(5, FALSE);
  printf("LightEnable(5, FALSE): hr=0x%08lx\n", (unsigned long)hr);
  hr = dev->GetLightEnable(5, &le);
  printf("GetLightEnable(5): hr=0x%08lx enable=%d (want 0)\n", (unsigned long)hr, (int)le);

  // Light::Type out of range — INVALIDCALL.
  D3DLIGHT9 bad = sl;
  bad.Type = (D3DLIGHTTYPE)0;
  hr = dev->SetLight(7, &bad);
  printf("SetLight(7, Type=0): hr=0x%08lx (must reject)\n", (unsigned long)hr);
  bad.Type = (D3DLIGHTTYPE)0xFF;
  hr = dev->SetLight(7, &bad);
  printf("SetLight(7, Type=0xFF): hr=0x%08lx (must reject)\n", (unsigned long)hr);

  // NULL rejection.
  hr = dev->SetLight(0, NULL);
  printf("SetLight(0, NULL): hr=0x%08lx (must reject)\n", (unsigned long)hr);
  hr = dev->GetLight(0, NULL);
  printf("GetLight(0, NULL): hr=0x%08lx (must reject)\n", (unsigned long)hr);
  hr = dev->GetLightEnable(0, NULL);
  printf("GetLightEnable(0, NULL): hr=0x%08lx (must reject)\n", (unsigned long)hr);

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
