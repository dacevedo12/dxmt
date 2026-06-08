#pragma once

#include <dxgi1_6.h>
#include "util_d3dkmt.h"
#include "com/com_guid.hpp"

namespace dxmt {

enum class DxgiBackendKind : uint32_t {
  Unknown = 0,
  Metal3 = 1,
  Metal4 = 2,
};

struct DxgiBackendAdapterInfo {
  uint64_t device_handle = 0;
  uint64_t registry_id = 0;
  uint32_t backend_index = 0;
  bool has_unified_memory = false;
  uint64_t recommended_max_working_set_size = 0;
  uint64_t current_allocated_size = 0;
  WCHAR name[128] = {};
};

struct DxgiBackendProvider {
  DxgiBackendKind kind = DxgiBackendKind::Unknown;
  const char *name = nullptr;
  uint32_t priority = 0;
  bool (*set_metal_cache_path)(const char *path) = nullptr;
  uint32_t (*adapter_count)() = nullptr;
  bool (*get_adapter_info)(uint32_t index, DxgiBackendAdapterInfo *info) = nullptr;
  void (*retain_device)(uint64_t device_handle) = nullptr;
  void (*release_device)(uint64_t device_handle) = nullptr;
  bool (*is_backbuffer_format_supported)(uint64_t device_handle, DXGI_FORMAT format) = nullptr;
};

extern "C" HRESULT __stdcall DXMTDXGIRegisterBackend(
    const DxgiBackendProvider *provider);

} // namespace dxmt

DEFINE_COM_INTERFACE("acdf3ef1-b33a-4cb6-97bd-1c1974827e6d", IMTLDXGIAdapter)
    : public IDXGIAdapter4 {
  virtual dxmt::DxgiBackendKind STDMETHODCALLTYPE GetBackendKind() = 0;
  virtual uint64_t STDMETHODCALLTYPE GetMetalDeviceHandle() = 0;
  virtual uint32_t STDMETHODCALLTYPE GetBackendAdapterIndex() = 0;
  virtual bool STDMETHODCALLTYPE ResolveBackendAdapter(
      const dxmt::DxgiBackendProvider *Provider) = 0;
  virtual D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() = 0;
  virtual bool STDMETHODCALLTYPE IsBackBufferFormatSupported(DXGI_FORMAT Format) = 0;
};

DEFINE_COM_INTERFACE("6bfa1657-9cb1-471a-a4fb-7cacf8a81207", IMTLDXGIDevice)
    : public IDXGIDevice3 {
  virtual dxmt::DxgiBackendKind STDMETHODCALLTYPE GetBackendKind() = 0;
  virtual uint64_t STDMETHODCALLTYPE GetMetalDeviceHandle() = 0;
  virtual D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 * pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGISwapChain1 **ppSwapChain) = 0;
};

static constexpr IID DXMT_NVEXT_GUID = dxmt::guid::make_guid("ba0af616-4a43-4259-815c-db3b89829905");

namespace dxmt {
enum class VendorExtension {
  None,
  Nvidia,
};

extern VendorExtension g_extension_enabled;
} // namespace dxmt
