#include "d3d12_command_allocator.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

namespace dxmt::d3d12 {
namespace {

class CommandAllocatorImpl final : public ComObjectWithInitialRef<ID3D12CommandAllocator>,
                                   public CommandAllocator {
public:
  CommandAllocatorImpl(IMTLD3D12Device *device, D3D12_COMMAND_LIST_TYPE type)
      : device_(device), type_(type) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandAllocator)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12CommandAllocator), riid))
      WARN("D3D12CommandAllocator: unknown interface query ", str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE Reset() override {
    return S_OK;
  }

  D3D12_COMMAND_LIST_TYPE GetCommandListType() const override {
    return type_;
  }

private:
  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_COMMAND_LIST_TYPE type_;
  std::string name_;
};

} // namespace

Com<ID3D12CommandAllocator>
CreateCommandAllocator(IMTLD3D12Device *device, D3D12_COMMAND_LIST_TYPE type) {
  return Com<ID3D12CommandAllocator>::transfer(new CommandAllocatorImpl(device, type));
}

} // namespace dxmt::d3d12
