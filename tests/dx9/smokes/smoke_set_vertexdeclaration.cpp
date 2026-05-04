// CreateVertexDeclaration / SetVertexDeclaration / GetVertexDeclaration
// smoke. Validates: count-with-terminator matches wined3d's
// GetDeclaration contract, the NULL-element-array query path, the
// cross-device rejection, and the priv-pin lifetime probe.

#include "../dx9_smoke.h"
void
test_set_vertexdeclaration(void) {
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

  // Initial: nothing bound.
  IDirect3DVertexDeclaration9 *got = NULL;
  HRESULT hr = dev->GetVertexDeclaration(&got);
  printf("GetVertexDeclaration initial: hr=0x%08lx out=%s\n", (unsigned long)hr, got ? "non-null" : "null");
  if (got)
    got->Release();

  // 3-element decl: position float3 + texcoord float2 + terminator.
  D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };

  IDirect3DVertexDeclaration9 *decl = NULL;
  HRESULT cv = dev->CreateVertexDeclaration(elements, &decl);
  printf("CreateVertexDeclaration: hr=0x%08lx out=%s\n", (unsigned long)cv, decl ? "ok" : "null");
  if (FAILED(cv) || !decl) {
    dev->Release();
    d3d->Release();
    return;
  }

  // Query count with NULL pElement.
  UINT count = 0;
  hr = decl->GetDeclaration(NULL, &count);
  printf("GetDeclaration(NULL, &count): hr=0x%08lx count=%u (want 3)\n", (unsigned long)hr, count);

  // Read back full element array.
  D3DVERTEXELEMENT9 readback[8] = {};
  hr = decl->GetDeclaration(readback, &count);
  printf("GetDeclaration(readback): hr=0x%08lx count=%u\n", (unsigned long)hr, count);
  printf(
      "  [0] stream=%u offset=%u type=%u usage=%u\n", readback[0].Stream, readback[0].Offset, readback[0].Type,
      readback[0].Usage
  );
  printf(
      "  [1] stream=%u offset=%u type=%u usage=%u\n", readback[1].Stream, readback[1].Offset, readback[1].Type,
      readback[1].Usage
  );
  printf("  [2] stream=0x%02x type=%u (D3DDECL_END terminator)\n", readback[2].Stream, readback[2].Type);

  // Null pNumElements rejected.
  hr = decl->GetDeclaration(readback, NULL);
  printf("GetDeclaration(_, NULL): hr=0x%08lx\n", (unsigned long)hr);

  // Round-trip through SetVertexDeclaration.
  HRESULT s = dev->SetVertexDeclaration(decl);
  printf("SetVertexDeclaration(decl): hr=0x%08lx\n", (unsigned long)s);
  got = NULL;
  hr = dev->GetVertexDeclaration(&got);
  printf("GetVertexDeclaration: hr=0x%08lx same=%s\n", (unsigned long)hr, got == decl ? "yes" : "no");
  if (got)
    got->Release();

  // Unbind.
  s = dev->SetVertexDeclaration(NULL);
  printf("SetVertexDeclaration(NULL): hr=0x%08lx\n", (unsigned long)s);
  got = (IDirect3DVertexDeclaration9 *)0xdeadbeef;
  hr = dev->GetVertexDeclaration(&got);
  printf("GetVertexDeclaration post-unbind: out=%s\n", got ? "non-null" : "null");

  // ---- Cross-device rejection. ----
  IDirect3DDevice9 *dev2 = NULL;
  HRESULT cd2 = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev2);
  printf("CreateDevice(2): hr=0x%08lx\n", (unsigned long)cd2);
  if (SUCCEEDED(cd2) && dev2) {
    IDirect3DVertexDeclaration9 *foreign = NULL;
    dev2->CreateVertexDeclaration(elements, &foreign);
    if (foreign) {
      HRESULT bad = dev->SetVertexDeclaration(foreign);
      printf("SetVertexDeclaration(foreign): hr=0x%08lx\n", (unsigned long)bad);
      foreign->Release();
    }
    dev2->Release();
  }

  // ---- Lifetime: release decl publicly while still bound. ----
  dev->SetVertexDeclaration(decl);
  ULONG decl_ref = decl->Release();
  printf("Release(decl) while bound: %lu (slot keeps it alive)\n", (unsigned long)decl_ref);
  got = NULL;
  HRESULT still = dev->GetVertexDeclaration(&got);
  printf(
      "GetVertexDeclaration post-release: hr=0x%08lx ptr_match=%s\n", (unsigned long)still, got == decl ? "yes" : "no"
  );
  if (got)
    got->Release();

  // ---- Null-out CreateVertexDeclaration param rejection. ----
  IDirect3DVertexDeclaration9 *bad_out = (IDirect3DVertexDeclaration9 *)1;
  hr = dev->CreateVertexDeclaration(elements, NULL);
  printf("CreateVertexDeclaration(_, NULL): hr=0x%08lx\n", (unsigned long)hr);
  hr = dev->CreateVertexDeclaration(NULL, &bad_out);
  printf(
      "CreateVertexDeclaration(NULL, _): hr=0x%08lx out=%s "
      "(InitReturnPtr must zero)\n",
      (unsigned long)hr, bad_out ? "non-null" : "null"
  );

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
