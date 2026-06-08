#include "dxgi_interfaces.h"
#include "log/log.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace dxmt {
namespace {

std::mutex &
BackendMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<DxgiBackendProvider> &
Backends() {
  static std::vector<DxgiBackendProvider> backends;
  return backends;
}

bool
ProviderValid(const DxgiBackendProvider &provider) {
  return provider.kind != DxgiBackendKind::Unknown &&
         provider.adapter_count && provider.get_adapter_info;
}

} // namespace

extern "C" HRESULT __stdcall
DXMTDXGIRegisterBackend(const DxgiBackendProvider *provider) {
  if (!provider || !ProviderValid(*provider))
    return E_INVALIDARG;

  std::lock_guard lock(BackendMutex());
  auto &backends = Backends();
  auto existing = std::find_if(
      backends.begin(), backends.end(),
      [&](const DxgiBackendProvider &entry) {
        return entry.kind == provider->kind;
      });
  if (existing != backends.end())
    *existing = *provider;
  else
    backends.push_back(*provider);

  std::sort(backends.begin(), backends.end(),
            [](const DxgiBackendProvider &a, const DxgiBackendProvider &b) {
              return a.priority > b.priority;
            });
  return S_OK;
}

std::vector<DxgiBackendProvider>
CopyRegisteredBackends() {
  std::lock_guard lock(BackendMutex());
  return Backends();
}

} // namespace dxmt
