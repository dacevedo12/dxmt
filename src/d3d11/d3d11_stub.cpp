#include "util_fh4_bypass.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>

namespace {

constexpr const char *BackendDllName = "d3d11_dxmt.dll";

std::once_flag g_backend_once;
HMODULE g_backend_module = nullptr;
HRESULT g_backend_status = E_FAIL;

HRESULT
LoadBackend() {
  std::call_once(g_backend_once, [] {
    g_backend_module = LoadLibraryA(BackendDllName);
    if (!g_backend_module) {
      auto error = GetLastError();
      g_backend_status = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
      return;
    }
    g_backend_status = S_OK;
  });
  return g_backend_status;
}

FARPROC
GetBackendProc(const char *name) {
  if (FAILED(LoadBackend()))
    return nullptr;
  auto proc = GetProcAddress(g_backend_module, name);
  if (!proc)
    g_backend_status = HRESULT_FROM_WIN32(GetLastError());
  return proc;
}

HRESULT
BackendFailure() {
  return FAILED(g_backend_status) ? g_backend_status : E_NOINTERFACE;
}

} // namespace

extern "C" HRESULT WINAPI
D3D11CoreCreateDevice(IDXGIFactory *pFactory, IDXGIAdapter *pAdapter,
                      UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
                      UINT FeatureLevels, ID3D11Device **ppDevice) {
  using Fn = HRESULT (WINAPI *)(IDXGIFactory *, IDXGIAdapter *, UINT,
                               const D3D_FEATURE_LEVEL *, UINT,
                               ID3D11Device **);
  auto proc = reinterpret_cast<Fn>(GetBackendProc("D3D11CoreCreateDevice"));
  return proc ? proc(pFactory, pAdapter, Flags, pFeatureLevels, FeatureLevels,
                     ppDevice)
              : BackendFailure();
}

extern "C" HRESULT WINAPI
D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {
  using Fn = HRESULT (WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                               const D3D_FEATURE_LEVEL *, UINT, UINT,
                               const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
                               ID3D11Device **, D3D_FEATURE_LEVEL *,
                               ID3D11DeviceContext **);
  auto proc = reinterpret_cast<Fn>(GetBackendProc("D3D11CreateDeviceAndSwapChain"));
  return proc ? proc(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                     FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
                     ppDevice, pFeatureLevel, ppImmediateContext)
              : BackendFailure();
}

extern "C" HRESULT WINAPI
D3D11CreateDevice(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType,
                  HMODULE Software, UINT Flags,
                  const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
                  UINT SDKVersion, ID3D11Device **ppDevice,
                  D3D_FEATURE_LEVEL *pFeatureLevel,
                  ID3D11DeviceContext **ppImmediateContext) {
  using Fn = HRESULT (WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                               const D3D_FEATURE_LEVEL *, UINT, UINT,
                               ID3D11Device **, D3D_FEATURE_LEVEL *,
                               ID3D11DeviceContext **);
  auto proc = reinterpret_cast<Fn>(GetBackendProc("D3D11CreateDevice"));
  return proc ? proc(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                     FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
                     ppImmediateContext)
              : BackendFailure();
}

extern "C" HRESULT __stdcall
D3D11On12CreateDevice(IUnknown *pDevice, UINT Flags,
                      const D3D_FEATURE_LEVEL *pFeatureLevels,
                      UINT FeatureLevels, IUnknown *const *ppCommandQueues,
                      UINT NumQueues, UINT NodeMask, ID3D11Device **ppDevice,
                      ID3D11DeviceContext **ppImmediateContext,
                      D3D_FEATURE_LEVEL *pChosenFeatureLevel) {
  using Fn = HRESULT (__stdcall *)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *,
                                  UINT, IUnknown *const *, UINT, UINT,
                                  ID3D11Device **, ID3D11DeviceContext **,
                                  D3D_FEATURE_LEVEL *);
  auto proc = reinterpret_cast<Fn>(GetBackendProc("D3D11On12CreateDevice"));
  return proc ? proc(pDevice, Flags, pFeatureLevels, FeatureLevels,
                     ppCommandQueues, NumQueues, NodeMask, ppDevice,
                     ppImmediateContext, pChosenFeatureLevel)
              : BackendFailure();
}

#ifdef _WIN32

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  dxmt::fh4bypass::ApplyBadFiberDataBypass();
  DisableThreadLibraryCalls(instance);
  return TRUE;
}

#endif
