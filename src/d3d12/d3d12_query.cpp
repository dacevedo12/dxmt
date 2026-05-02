#include "d3d12_query.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <cstring>

namespace dxmt::d3d12 {
namespace {

class QueryHeapImpl final : public ComObjectWithInitialRef<ID3D12QueryHeap>,
                            public QueryHeap {
public:
  QueryHeapImpl(IMTLD3D12Device *device, const D3D12_QUERY_HEAP_DESC &desc)
      : device_(device), desc_(desc), queries_(desc.Count) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12QueryHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12QueryHeap), riid))
      WARN("D3D12QueryHeap: unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                   const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  const D3D12_QUERY_HEAP_DESC &GetDesc() const override { return desc_; }

  Rc<VisibilityResultQuery> BeginVisibility(D3D12_QUERY_TYPE type,
                                            UINT index) override {
    if (!ValidateAccess(type, index))
      return {};
    if (desc_.Type != D3D12_QUERY_HEAP_TYPE_OCCLUSION) {
      WARN("D3D12QueryHeap: BeginQuery unsupported for heap type ",
           desc_.Type);
      return {};
    }
    auto &query = queries_[index];
    query.visibility = new VisibilityResultQuery();
    query.began = true;
    query.valid = false;
    return query.visibility;
  }

  Rc<VisibilityResultQuery> EndVisibility(D3D12_QUERY_TYPE type,
                                          UINT index) override {
    if (!ValidateAccess(type, index))
      return {};
    if (desc_.Type != D3D12_QUERY_HEAP_TYPE_OCCLUSION) {
      WARN("D3D12QueryHeap: EndQuery visibility unsupported for heap type ",
           desc_.Type);
      return {};
    }
    auto &query = queries_[index];
    if (!query.began || !query.visibility) {
      WARN("D3D12QueryHeap: EndQuery without matching BeginQuery");
      return {};
    }
    query.began = false;
    query.valid = true;
    return query.visibility;
  }

  Rc<TimestampQuery> EndTimestamp(D3D12_QUERY_TYPE type, UINT index) override {
    if (!ValidateAccess(type, index))
      return {};
    if (desc_.Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP ||
        type != D3D12_QUERY_TYPE_TIMESTAMP) {
      WARN("D3D12QueryHeap: unsupported timestamp end query type ", type);
      return {};
    }
    auto &query = queries_[index];
    query.timestamp = new TimestampQuery();
    query.valid = true;
    return query.timestamp;
  }

  bool BeginStatistics(D3D12_QUERY_TYPE type, UINT index) override {
    if (!ValidateAccess(type, index))
      return false;
    if (desc_.Type != D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS &&
        desc_.Type != D3D12_QUERY_HEAP_TYPE_SO_STATISTICS) {
      WARN("D3D12QueryHeap: BeginQuery statistics unsupported for heap type ",
           desc_.Type);
      return false;
    }
    auto &query = queries_[index];
    query.began = true;
    query.valid = false;
    return true;
  }

  bool EndStatistics(D3D12_QUERY_TYPE type, UINT index) override {
    if (!ValidateAccess(type, index))
      return false;
    if (desc_.Type != D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS &&
        desc_.Type != D3D12_QUERY_HEAP_TYPE_SO_STATISTICS) {
      WARN("D3D12QueryHeap: EndQuery statistics unsupported for heap type ",
           desc_.Type);
      return false;
    }
    auto &query = queries_[index];
    if (!query.began) {
      WARN("D3D12QueryHeap: EndQuery statistics without matching BeginQuery");
      return false;
    }
    query.began = false;
    query.valid = true;
    return true;
  }

  bool Resolve(D3D12_QUERY_TYPE type, UINT start_index, UINT query_count,
               std::vector<uint8_t> &data) const override {
    const auto stride = ResolveStride(type);
    if (!stride)
      return false;
    if (!query_count)
      return true;
    if (start_index >= queries_.size() || query_count > queries_.size() - start_index) {
      WARN("D3D12QueryHeap: resolve range exceeds heap size");
      return false;
    }
    data.resize(size_t(query_count) * stride);
    for (UINT i = 0; i < query_count; i++) {
      const auto &query = queries_[start_index + i];
      const auto offset = size_t(i) * stride;
      switch (type) {
      case D3D12_QUERY_TYPE_OCCLUSION:
      case D3D12_QUERY_TYPE_BINARY_OCCLUSION: {
        uint64_t value = 0;
        if (query.valid && query.visibility)
          query.visibility->getValue(&value);
        if (type == D3D12_QUERY_TYPE_BINARY_OCCLUSION && value)
          value = 1;
        std::memcpy(data.data() + offset, &value, sizeof(value));
        break;
      }
      case D3D12_QUERY_TYPE_TIMESTAMP: {
        uint64_t value = 0;
        if (query.valid && query.timestamp)
          query.timestamp->getValue(&value);
        std::memcpy(data.data() + offset, &value, sizeof(value));
        break;
      }
      case D3D12_QUERY_TYPE_PIPELINE_STATISTICS: {
        D3D12_QUERY_DATA_PIPELINE_STATISTICS stats = {};
        std::memcpy(data.data() + offset, &stats, sizeof(stats));
        break;
      }
      case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0:
      case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
      case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2:
      case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3: {
        D3D12_QUERY_DATA_SO_STATISTICS stats = {};
        std::memcpy(data.data() + offset, &stats, sizeof(stats));
        break;
      }
      default:
        WARN("D3D12QueryHeap: unsupported resolve query type ", type);
        return false;
      }
    }
    return true;
  }

private:
  struct QueryData {
    bool began = false;
    bool valid = false;
    Rc<VisibilityResultQuery> visibility;
    Rc<TimestampQuery> timestamp;
  };

  bool ValidateAccess(D3D12_QUERY_TYPE type, UINT index) const {
    if (index >= desc_.Count) {
      WARN("D3D12QueryHeap: query index out of range index=", index,
           " count=", desc_.Count);
      return false;
    }
    switch (desc_.Type) {
    case D3D12_QUERY_HEAP_TYPE_OCCLUSION:
      return type == D3D12_QUERY_TYPE_OCCLUSION ||
             type == D3D12_QUERY_TYPE_BINARY_OCCLUSION;
    case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
      return type == D3D12_QUERY_TYPE_TIMESTAMP;
    case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
      return type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
    case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS:
      return type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 ||
             type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1 ||
             type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2 ||
             type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3;
    default:
      return false;
    }
  }

  static size_t ResolveStride(D3D12_QUERY_TYPE type) {
    switch (type) {
    case D3D12_QUERY_TYPE_OCCLUSION:
    case D3D12_QUERY_TYPE_BINARY_OCCLUSION:
    case D3D12_QUERY_TYPE_TIMESTAMP:
      return sizeof(uint64_t);
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS:
      return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3:
      return sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
    default:
      return 0;
    }
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_QUERY_HEAP_DESC desc_ = {};
  std::vector<QueryData> queries_;
  std::string name_;
};

} // namespace

Com<ID3D12QueryHeap>
CreateQueryHeap(IMTLD3D12Device *device, const D3D12_QUERY_HEAP_DESC *desc) {
  return Com<ID3D12QueryHeap>::transfer(new QueryHeapImpl(device, *desc));
}

} // namespace dxmt::d3d12
