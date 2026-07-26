#include "d3d12_query.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>

namespace dxmt::d3d12 {
namespace {

static bool
D3D12QueryDiagEnabled() {
  static const bool enabled = []() {
    auto value = env::getEnvVar("DXMT_DIAG_D3D12_QUERY");
    if (value.empty())
      value = env::getEnvVar("DXMT_DIAG_COMMAND_QUEUE");
    return value == "1" || value == "true" || value == "yes" ||
           value == "trace";
  }();
  return enabled;
}

static bool
D3D12QueryDiagShouldLog(std::atomic<uint32_t> &counter) {
  if (!D3D12QueryDiagEnabled())
    return false;
  counter.fetch_add(1, std::memory_order_relaxed);
  return true;
}

static size_t
QueryResolveStride(D3D12_QUERY_TYPE type) {
  switch (type) {
  case D3D12_QUERY_TYPE_OCCLUSION:
  case D3D12_QUERY_TYPE_BINARY_OCCLUSION:
  case D3D12_QUERY_TYPE_TIMESTAMP:
    return sizeof(uint64_t);
  default:
    return 0;
  }
}

class QueryHeapImpl final : public ComObjectWithInitialRef<ID3D12QueryHeap>,
                            public QueryHeap {
public:
  QueryHeapImpl(IMTLD3D12Device *device, const D3D12_QUERY_HEAP_DESC &desc)
      : device_(device), desc_(desc)
#if DXMT_DX12_METAL4
        , timestamp_pages_(std::make_shared<TimestampPageState>())
#endif
        , queries_(desc.Count) {}

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
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  IMTLD3D12Device *GetParentDevice() const override { return device_.ptr(); }

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
    std::lock_guard<std::mutex> lock(queries_mutex_);
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
    std::lock_guard<std::mutex> lock(queries_mutex_);
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
    std::lock_guard<std::mutex> lock(queries_mutex_);
    auto &query = queries_[index];
    query.timestamp = new TimestampQuery();
#if DXMT_DX12_METAL4
    AssignTimestampSample(*query.timestamp, index);
#endif
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
    std::lock_guard<std::mutex> lock(queries_mutex_);
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
    std::lock_guard<std::mutex> lock(queries_mutex_);
    auto &query = queries_[index];
    if (!query.began) {
      WARN("D3D12QueryHeap: EndQuery statistics without matching BeginQuery");
      return false;
    }
    query.began = false;
    query.valid = true;
    return true;
  }

  bool CaptureResolve(D3D12_QUERY_TYPE type, UINT start_index, UINT query_count,
                      QueryResolveSnapshot &snapshot) const override {
    const auto stride = QueryResolveStride(type);
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12QueryDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 query diagnostic: CaptureResolve enter"
           " heap=", reinterpret_cast<uintptr_t>(this),
           " heapType=", desc_.Type,
           " type=", type,
           " start=", start_index,
           " count=", query_count,
           " stride=", stride);
    }
    if (!stride)
      return false;
    if (!query_count)
      return snapshot = QueryResolveSnapshot{type, {}}, true;
    if (start_index >= queries_.size() || query_count > queries_.size() - start_index) {
      WARN("D3D12QueryHeap: resolve range exceeds heap size");
      return false;
    }
    std::lock_guard<std::mutex> lock(queries_mutex_);
    snapshot.type = type;
    snapshot.entries.clear();
    snapshot.entries.reserve(query_count);
    for (UINT i = 0; i < query_count; i++) {
      const auto &query = queries_[start_index + i];
      if (D3D12QueryDiagShouldLog(log_count)) {
        WARN_FILE_ONLY("D3D12 query diagnostic: CaptureResolve query"
             " heap=", reinterpret_cast<uintptr_t>(this),
             " type=", type,
             " index=", start_index + i,
             " began=", query.began,
             " valid=", query.valid,
             " hasVisibility=", bool(query.visibility),
             " hasTimestamp=", bool(query.timestamp));
      }
      snapshot.entries.push_back(QueryResolveSnapshotEntry{
          query.valid, query.visibility, query.timestamp});
    }
    if (D3D12QueryDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 query diagnostic: CaptureResolve leave"
           " heap=", reinterpret_cast<uintptr_t>(this),
           " heapType=", desc_.Type,
           " type=", type,
           " start=", start_index,
           " count=", query_count,
           " bytes=", size_t(query_count) * stride);
    }
    return true;
  }

private:
#if DXMT_DX12_METAL4
  struct TimestampPage {
    WMT::Reference<WMT::CounterHeap> heap;
    uint64_t entry_size = 0;
    std::vector<bool> occupied;
  };

  struct TimestampPageState {
    std::mutex mutex;
    std::vector<std::shared_ptr<TimestampPage>> pages;
  };

  // Metal/MTL_DEBUG_LAYER rejects MTL4CounterHeapDescriptor.count > 4096
  // (assert: "entryCount must not be more than 4096"). Cap native heaps and
  // fall back for D3D indices beyond the cap.
  static constexpr uint64_t kMaxMetalTimestampCounterHeapEntries = 4096;

  class TimestampSampleOwnerImpl final : public TimestampSampleOwner {
  public:
    TimestampSampleOwnerImpl(
        std::shared_ptr<TimestampPageState> state,
        std::shared_ptr<TimestampPage> page, uint64_t slot) noexcept
        : state_(std::move(state)), page_(std::move(page)), slot_(slot) {}

    ~TimestampSampleOwnerImpl() noexcept override {
      dxmt::invokeNoexcept("timestamp sample release", [this]() {
        ReleaseTimestampSample(*state_, page_, slot_);
      });
    }

  private:
    std::shared_ptr<TimestampPageState> state_;
    std::shared_ptr<TimestampPage> page_;
    uint64_t slot_ = 0;
  };

  void AssignTimestampSample(TimestampQuery &query, UINT d3d_index) {
    if (desc_.Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP)
      return;
    if (d3d_index >= kMaxMetalTimestampCounterHeapEntries)
      return;

    auto state = timestamp_pages_;
    std::lock_guard<std::mutex> lock(state->mutex);

    auto page = FindTimestampPage(*state, d3d_index);
    if (!page)
      return;

    page->occupied[d3d_index] = true;
    query.setSampleIndex(d3d_index);

    auto resolve_heap = page->heap;
    const auto resolve_entry_size = page->entry_size;
    query.setResolveSource(
        resolve_heap, resolve_entry_size,
        std::make_unique<TimestampSampleOwnerImpl>(
            std::move(state), std::move(page), d3d_index));
  }

  static void ReleaseTimestampSample(TimestampPageState &state,
                                     const std::shared_ptr<TimestampPage> &page,
                                     uint64_t d3d_index) {
    std::lock_guard<std::mutex> lock(state.mutex);

    if (!page)
      return;

    if (d3d_index < page->occupied.size())
      page->occupied[d3d_index] = false;
    if (state.pages.size() > 1 &&
        std::none_of(page->occupied.begin(), page->occupied.end(),
                     [](bool occupied) { return occupied; })) {
      state.pages.erase(
          std::remove(state.pages.begin(), state.pages.end(), page),
          state.pages.end());
    }
  }

  std::shared_ptr<TimestampPage>
  FindTimestampPage(TimestampPageState &state, UINT d3d_index) {
    for (const auto &page : state.pages) {
      if (page && d3d_index < page->occupied.size() &&
          !page->occupied[d3d_index])
        return page;
    }

    return CreateTimestampPage(state);
  }

  std::shared_ptr<TimestampPage>
  CreateTimestampPage(TimestampPageState &state) {
    if (timestamp_heap_unavailable_)
      return {};

    WMT::Device device = device_->GetMTLDevice();
    // Metal hard-caps counter heap entries at 4096 (debug layer asserts above).
    // Support D3D heaps larger than that only for the first 4096 slots; higher
    // indices use the per-command-buffer timestamp fallback.
    const uint64_t requested = std::max<uint64_t>(desc_.Count, 2);
    const uint64_t native_count =
        std::min(requested, kMaxMetalTimestampCounterHeapEntries);
    if (requested > kMaxMetalTimestampCounterHeapEntries) {
      WARN("D3D12QueryHeap: capping Metal timestamp counter heap"
           " d3dCount=", desc_.Count, " metalCount=", native_count,
           " (Metal max ", kMaxMetalTimestampCounterHeapEntries, ")");
    }
    WMT::Error error = {};
    auto heap = device.newTimestampCounterHeap(native_count, &error);
    const uint64_t entry_size = device.timestampHeapEntrySize();
    if (!heap || entry_size < sizeof(WMTMTL4TimestampHeapEntry)) {
      // A failed allocation for this D3D heap shape is deterministic on the
      // active device. Retrying it for every EndQuery crosses the Wine/Metal
      // boundary and can turn timestamp-heavy workloads into a CPU bottleneck.
      // Keep using the existing per-command-buffer timestamp fallback instead.
      timestamp_heap_unavailable_ = true;
      // CounterHeap quota easter egg: fuck apple.
      WARN("D3D12QueryHeap: Metal timestamp counter heap unavailable"
           " d3dCount=", desc_.Count,
           " nativeCount=", native_count,
           " heap=", bool(heap),
           " entrySize=", entry_size);
      return {};
    }

    auto page = std::make_shared<TimestampPage>();
    page->heap = std::move(heap);
    page->entry_size = entry_size;
    // Occupied is indexed by Metal sample slot (== d3d index for capped range).
    page->occupied.assign(static_cast<size_t>(native_count), false);
    state.pages.push_back(page);
    return page;
  }
#endif

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
    return IsQueryTypeCompatible(desc_.Type, type);
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_QUERY_HEAP_DESC desc_ = {};
#if DXMT_DX12_METAL4
  std::shared_ptr<TimestampPageState> timestamp_pages_;
  bool timestamp_heap_unavailable_ = false;
#endif
  mutable std::mutex queries_mutex_;
  std::vector<QueryData> queries_;
  std::string name_;
};

} // namespace

bool
ResolveQuerySnapshot(const QueryResolveSnapshot &snapshot,
                     std::vector<uint8_t> &data) {
  const auto stride = QueryResolveStride(snapshot.type);
  if (!stride)
    return false;
  if (snapshot.entries.size() >
      std::numeric_limits<size_t>::max() / stride)
    return false;

  data.assign(snapshot.entries.size() * stride, 0);
  for (size_t i = 0; i < snapshot.entries.size(); i++) {
    const auto &entry = snapshot.entries[i];
    if (!entry.ended)
      continue;

    uint64_t value = 0;
    switch (snapshot.type) {
    case D3D12_QUERY_TYPE_OCCLUSION:
    case D3D12_QUERY_TYPE_BINARY_OCCLUSION:
      if (entry.visibility)
        entry.visibility->getValue(&value);
      if (snapshot.type == D3D12_QUERY_TYPE_BINARY_OCCLUSION && value)
        value = 1;
      break;
    case D3D12_QUERY_TYPE_TIMESTAMP:
      if (entry.timestamp)
        entry.timestamp->getValue(&value);
      break;
    default:
      return false;
    }
    std::memcpy(data.data() + i * stride, &value, sizeof(value));
  }
  return true;
}

Com<ID3D12QueryHeap>
CreateQueryHeap(IMTLD3D12Device *device, const D3D12_QUERY_HEAP_DESC *desc) {
  return Com<ID3D12QueryHeap>::transfer(new QueryHeapImpl(device, *desc));
}

} // namespace dxmt::d3d12
