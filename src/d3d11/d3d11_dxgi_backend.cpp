#include "d3d11_dxgi_backend.hpp"

#include "dxmt_format.hpp"
#include "dxmt_shader_cache.hpp"
#include "log/log.hpp"
#include "util_env.hpp"

#include <exception>

namespace dxmt {
namespace {

std::string
ResolveMetalCachePath(const char *path) {
  std::string cache_path = path ? path : "";
  if (cache_path.empty())
    return {};

  try {
    if (cache_path.starts_with("/"))
      return cache_path;

    if (cache_path.size() >= 2 && cache_path[1] == ':')
      return env::getUnixPath(cache_path);

    return cache_path;
  } catch (const std::exception &) {
    Logger::warn("Failed to resolve Metal cache path");
  } catch (...) {
    Logger::warn("Failed to resolve Metal cache path");
  }

  return {};
}

bool
SetMetalCachePath(const char *path) {
  auto unix_path = ResolveMetalCachePath(path);
  if (unix_path.empty())
    return false;
  return WMTSetMetalShaderCachePath(unix_path.c_str());
}

uint32_t
AdapterCount() {
  return uint32_t(WMT::CopyAllDevices().count());
}

bool
GetAdapterInfo(uint32_t index, DxgiBackendAdapterInfo *info) {
  if (!info)
    return false;
  auto devices = WMT::CopyAllDevices();
  if (index >= devices.count())
    return false;
  auto device = devices.object(index);
  *info = {};
  info->device_handle = device.handle;
  info->registry_id = device.registryID();
  info->backend_index = index;
  info->has_unified_memory = device.hasUnifiedMemory();
  info->recommended_max_working_set_size = device.recommendedMaxWorkingSetSize();
  info->current_allocated_size = device.currentAllocatedSize();
  device.name().getCString(reinterpret_cast<char *>(info->name),
                           sizeof(info->name), WMTUTF16StringEncoding);
  return true;
}

void
RetainDevice(uint64_t device_handle) {
  WMT::Object{device_handle}.retain();
}

void
ReleaseDevice(uint64_t device_handle) {
  WMT::Object{device_handle}.release();
}

bool
IsBackBufferFormatSupported(uint64_t device_handle, DXGI_FORMAT format) {
  MTL_DXGI_FORMAT_DESC desc = {};
  return SUCCEEDED(MTLQueryDXGIFormat(WMT::Device{device_handle}, format, desc)) &&
         (desc.Flag & MTL_DXGI_FORMAT_BACKBUFFER);
}

void
InitializeMetalCachePath() {
  if (env::getEnvVar("DXMT_USE_DEFAULT_METAL_CACHE") == "1")
    return;
  auto metal_cache_path = GetDXMTShaderCacheDirectory() + "com.apple.metal";
  if (!SetMetalCachePath(metal_cache_path.c_str()))
    Logger::info("Failed to set Metal cache path, fallback to system default");
}

DxgiBackendProvider
BackendProvider() {
  DxgiBackendProvider provider = {};
  provider.kind = DxgiBackendKind::Metal3;
  provider.name = "winemetal";
  provider.priority = 10;
  provider.set_metal_cache_path = SetMetalCachePath;
  provider.adapter_count = AdapterCount;
  provider.get_adapter_info = GetAdapterInfo;
  provider.retain_device = RetainDevice;
  provider.release_device = ReleaseDevice;
  provider.is_backbuffer_format_supported = IsBackBufferFormatSupported;
  return provider;
}

} // namespace

HRESULT
RegisterD3D11DxgiBackend() {
  InitializeMetalCachePath();
  auto provider = BackendProvider();
  return DXMTDXGIRegisterBackend(&provider);
}

WMT::Device
GetD3D11AdapterDevice(IMTLDXGIAdapter *adapter) {
  if (!adapter)
    return {};
  auto kind = adapter->GetBackendKind();
  if (kind != DxgiBackendKind::Unknown && kind != DxgiBackendKind::Metal3)
    return {};
  if (!adapter->GetMetalDeviceHandle()) {
    auto provider = BackendProvider();
    if (!adapter->ResolveBackendAdapter(&provider))
      return {};
  }
  return WMT::Device{adapter->GetMetalDeviceHandle()};
}

WMT::Device
GetD3D11DeviceMetalDevice(IMTLDXGIDevice *device) {
  return device ? WMT::Device{device->GetMetalDeviceHandle()} : WMT::Device{};
}

} // namespace dxmt
