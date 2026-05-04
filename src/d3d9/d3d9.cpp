#include "d3d9.h"
#include "d3d9_interface.hpp"
#include "d3d9_trace.hpp"
#include "log/log.hpp"

#include <atomic>

namespace dxmt {
Logger Logger::s_instance("d3d9.log");
}

#ifdef _WIN32
extern "C" BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(instance);
  return TRUE;
}
#endif

extern "C" IDirect3D9 *WINAPI
Direct3DCreate9(UINT SDKVersion) {
  D9_TRACE("Direct3DCreate9");
  auto *iface = new dxmt::MTLD3D9Interface(SDKVersion, /*isEx=*/false);
  iface->AddRef();
  return static_cast<IDirect3D9 *>(iface);
}

extern "C" HRESULT WINAPI
Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D) {
  D9_TRACE("Direct3DCreate9Ex");
  if (!ppD3D)
    return D3DERR_INVALIDCALL;
  auto *iface = new dxmt::MTLD3D9Interface(SDKVersion, /*isEx=*/true);
  iface->AddRef();
  *ppD3D = static_cast<IDirect3D9Ex *>(iface);
  return D3D_OK;
}

// PIX event nesting counter. Mirrors wined3d d3d9_main.c:225/235 —
// BeginEvent returns the level the new event sits at (post-increment
// of the prior depth); EndEvent returns the level after popping
// (pre-decrement). Apps inspect the returned level to validate
// nesting; returning 0 unconditionally would silently break that
// contract. Atomic because PIX events are documented as callable
// from any thread holding the device.
static std::atomic<int> s_d3dperfEventLevel{0};

extern "C" int WINAPI
D3DPERF_BeginEvent(D3DCOLOR, const WCHAR *) {
  D9_TRACE("D3DPERF_BeginEvent");
  return s_d3dperfEventLevel.fetch_add(1, std::memory_order_relaxed);
}
extern "C" int WINAPI
D3DPERF_EndEvent(void) {
  D9_TRACE("D3DPERF_EndEvent");
  return s_d3dperfEventLevel.fetch_sub(1, std::memory_order_relaxed) - 1;
}
extern "C" DWORD WINAPI
D3DPERF_GetStatus(void) {
  D9_TRACE("D3DPERF_GetStatus");
  return 0;
}
extern "C" BOOL WINAPI
D3DPERF_QueryRepeatFrame(void) {
  D9_TRACE("D3DPERF_QueryRepeatFrame");
  return FALSE;
}
extern "C" void WINAPI
D3DPERF_SetMarker(D3DCOLOR, const WCHAR *) {
  D9_TRACE("D3DPERF_SetMarker");
}
extern "C" void WINAPI
D3DPERF_SetOptions(DWORD) {
  D9_TRACE("D3DPERF_SetOptions");
}
extern "C" void WINAPI
D3DPERF_SetRegion(D3DCOLOR, const WCHAR *) {
  D9_TRACE("D3DPERF_SetRegion");
}

extern "C" void WINAPI
DebugSetLevel(DWORD) {
  D9_TRACE("DebugSetLevel");
}
extern "C" BOOL WINAPI
DebugSetMute(void) {
  D9_TRACE("DebugSetMute");
  return TRUE;
}

// Direct3DShaderValidatorCreate9 — undocumented MS export used by the
// SDK's fxc.exe and a handful of games. The returned object's
// Begin/Instruction/End methods always succeed; no validation is
// actually performed. Callers that fail to find the export at
// GetProcAddress time can fall into a bytecode-validation path that
// triggers a CRT memcpy/memset on data they don't own — observed in
// the wild as a c0000005 inside ucrtbase right after
// CreatePixelShader. Wine d3d9_main.c uses the same stub shape.
//
// The vtable is a static singleton — every Direct3DShaderValidatorCreate9
// call hands out the same object and its AddRef/Release are pinned at
// 1, matching wined3d's behaviour. dxmt has no shader-validator role
// of its own (we run our own DXSO walker in CreatePixelShader /
// CreateVertexShader); this exists purely to keep games' GetProcAddress
// probe happy.
namespace {

typedef HRESULT(WINAPI *shader_validator_cb)(
    const char *file, int line, DWORD_PTR arg3, DWORD_PTR message_id, const char *message, void *context
);

struct IDirect3DShaderValidator9;

struct IDirect3DShaderValidator9Vtbl {
  HRESULT(WINAPI *QueryInterface)(IDirect3DShaderValidator9 *iface, REFIID iid, void **out);
  ULONG(WINAPI *AddRef)(IDirect3DShaderValidator9 *iface);
  ULONG(WINAPI *Release)(IDirect3DShaderValidator9 *iface);
  HRESULT(WINAPI *Begin)(IDirect3DShaderValidator9 *iface, shader_validator_cb callback, void *context, DWORD_PTR arg3);
  HRESULT(WINAPI *Instruction)(
      IDirect3DShaderValidator9 *iface, const char *file, int line, const DWORD *tokens, unsigned int token_count
  );
  HRESULT(WINAPI *End)(IDirect3DShaderValidator9 *iface);
};

struct IDirect3DShaderValidator9 {
  const struct IDirect3DShaderValidator9Vtbl *vtbl;
};

HRESULT WINAPI
shader_validator_QueryInterface(IDirect3DShaderValidator9 *, REFIID, void **out) {
  *out = nullptr;
  return E_NOINTERFACE;
}
ULONG WINAPI
shader_validator_AddRef(IDirect3DShaderValidator9 *) {
  return 2;
}
ULONG WINAPI
shader_validator_Release(IDirect3DShaderValidator9 *) {
  return 1;
}
HRESULT WINAPI
shader_validator_Begin(IDirect3DShaderValidator9 *, shader_validator_cb, void *, DWORD_PTR) {
  return S_OK;
}
HRESULT WINAPI
shader_validator_Instruction(IDirect3DShaderValidator9 *, const char *, int, const DWORD *, unsigned int) {
  return S_OK;
}
HRESULT WINAPI
shader_validator_End(IDirect3DShaderValidator9 *) {
  return S_OK;
}

const IDirect3DShaderValidator9Vtbl shader_validator_vtbl = {
    shader_validator_QueryInterface, shader_validator_AddRef,      shader_validator_Release,
    shader_validator_Begin,          shader_validator_Instruction, shader_validator_End,
};

IDirect3DShaderValidator9 shader_validator = {&shader_validator_vtbl};

} // namespace

extern "C" IDirect3DShaderValidator9 *WINAPI
Direct3DShaderValidatorCreate9(void) {
  D9_TRACE("Direct3DShaderValidatorCreate9");
  return &shader_validator;
}
