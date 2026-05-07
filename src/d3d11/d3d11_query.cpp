#include "d3d11_query.hpp"
#include "dxmt_occlusion_query.hpp"
#include <algorithm>
#include <cstring>

namespace dxmt {

namespace {

bool
SupportsTimestampSampleBuffers(WMT::Device device) {
  return bool(device.newCounterSampleBuffer(1, true));
}

HRESULT
CopyCounterString(const char *source, LPSTR destination, UINT *length) {
  if (!length)
    return destination ? E_INVALIDARG : S_OK;

  const auto required_length = UINT(std::strlen(source) + 1);
  const auto provided_length = *length;
  *length = required_length;

  if (!destination)
    return S_OK;

  if (provided_length < required_length)
    return S_FALSE;

  std::memcpy(destination, source, required_length);
  return S_OK;
}

class MTLD3D11DeviceCounterBase
    : public MTLD3D11DeviceChild<ID3D11Counter, IMTLD3D11CounterExt> {
public:
  MTLD3D11DeviceCounterBase(
      MTLD3D11Device *pDevice,
      const D3D11DeviceCounterMetadata &metadata)
      : MTLD3D11DeviceChild<ID3D11Counter, IMTLD3D11CounterExt>(pDevice),
        desc_{metadata.counter, 0},
        metadata_(metadata),
        d3d10_(this, pDevice->GetImmediateContextPrivate()) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11Asynchronous) ||
        riid == __uuidof(ID3D11Counter)) {
      *ppvObject = ref_and_cast<ID3D11Counter>(this);
      return S_OK;
    }

    if (riid == __uuidof(IMTLD3D11CounterExt)) {
      *ppvObject = ref_and_cast<IMTLD3D11CounterExt>(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10DeviceChild) ||
        riid == __uuidof(ID3D10Asynchronous) ||
        riid == __uuidof(ID3D10Counter)) {
      *ppvObject = ref(&d3d10_);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11Counter), riid)) {
      Logger::warn("D3D11Counter::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }

  UINT STDMETHODCALLTYPE GetDataSize() final { return sizeof(UINT64); }

  void STDMETHODCALLTYPE GetDesc(D3D11_COUNTER_DESC *pDesc) final {
    if (pDesc)
      *pDesc = desc_;
  }

  void STDMETHODCALLTYPE BeginCounter() final { BeginImpl(); }

  void STDMETHODCALLTYPE EndCounter() final { EndImpl(); }

  void STDMETHODCALLTYPE
  EncodeBeginCounter(ArgumentEncodingContext *enc) final {
    EncodeBeginImpl(enc);
  }

  void STDMETHODCALLTYPE
  EncodeEndCounter(ArgumentEncodingContext *enc) final {
    EncodeEndImpl(enc);
  }

  void STDMETHODCALLTYPE
  ReplayBeginCounter(ArgumentEncodingContext *enc) final {
    BeginImpl();
    EncodeBeginImpl(enc);
  }

  void STDMETHODCALLTYPE
  ReplayEndCounter(ArgumentEncodingContext *enc) final {
    EndImpl();
    EncodeEndImpl(enc);
  }

  HRESULT STDMETHODCALLTYPE GetCounterData(void *data) final {
    return GetDataImpl(data);
  }

protected:
  virtual void BeginImpl() = 0;
  virtual void EndImpl() = 0;
  virtual void EncodeBeginImpl(ArgumentEncodingContext *enc) = 0;
  virtual void EncodeEndImpl(ArgumentEncodingContext *enc) = 0;
  virtual HRESULT GetDataImpl(void *data) = 0;

  D3D11_COUNTER_DESC desc_;
  D3D11DeviceCounterMetadata metadata_;
  QueryState state_ = QueryState::Undefined;
  MTLD3D10Counter d3d10_;
};

class MTLD3D11MemoryUsageCounter final : public MTLD3D11DeviceCounterBase {
public:
  MTLD3D11MemoryUsageCounter(
      MTLD3D11Device *pDevice,
      const D3D11DeviceCounterMetadata &metadata)
      : MTLD3D11DeviceCounterBase(pDevice, metadata) {}

protected:
  void BeginImpl() override {
    state_ = QueryState::Building;
  }

  void EndImpl() override {
    if (state_ != QueryState::Building)
      BeginImpl();

    latest_value_ = this->m_parent->GetMTLDevice().currentAllocatedSize();
    state_ = QueryState::Signaled;
  }

  void EncodeBeginImpl(ArgumentEncodingContext *enc) override {
    (void)enc;
  }

  void EncodeEndImpl(ArgumentEncodingContext *enc) override {
    (void)enc;
  }

  HRESULT GetDataImpl(void *data) override {
    if (state_ == QueryState::Undefined || state_ == QueryState::Building)
      return DXGI_ERROR_INVALID_CALL;

    *static_cast<UINT64 *>(data) = latest_value_;
    return S_OK;
  }

private:
  UINT64 latest_value_ = 0;
};

class MTLD3D11RunningTimeCounter final : public MTLD3D11DeviceCounterBase {
public:
  MTLD3D11RunningTimeCounter(
      MTLD3D11Device *pDevice,
      const D3D11DeviceCounterMetadata &metadata)
      : MTLD3D11DeviceCounterBase(pDevice, metadata) {}

protected:
  void BeginImpl() override {
    begin_query_ = new TimestampQuery();
    end_query_ = new TimestampQuery();
    latest_value_ = 0;
    state_ = QueryState::Building;
  }

  void EndImpl() override {
    if (state_ != QueryState::Building) {
      auto query = Rc<TimestampQuery>(new TimestampQuery());
      begin_query_ = query;
      end_query_ = std::move(query);
    }
    state_ = QueryState::Issued;
  }

  void EncodeBeginImpl(ArgumentEncodingContext *enc) override {
    if (enc && begin_query_)
      enc->sampleTimestamp(Rc(begin_query_));
  }

  void EncodeEndImpl(ArgumentEncodingContext *enc) override {
    if (enc && end_query_)
      enc->sampleTimestamp(Rc(end_query_));
  }

  HRESULT GetDataImpl(void *data) override {
    if (state_ == QueryState::Undefined || state_ == QueryState::Building)
      return DXGI_ERROR_INVALID_CALL;

    uint64_t begin_value = 0;
    uint64_t end_value = 0;

    if (!begin_query_ || !end_query_ ||
        !begin_query_->getValue(&begin_value) ||
        !end_query_->getValue(&end_value)) {
      return S_FALSE;
    }

    latest_value_ = end_value >= begin_value ? end_value - begin_value : 0;
    state_ = QueryState::Signaled;
    *static_cast<UINT64 *>(data) = latest_value_;
    return S_OK;
  }

private:
  Rc<TimestampQuery> begin_query_;
  Rc<TimestampQuery> end_query_;
  UINT64 latest_value_ = 0;
};

} // namespace

class OcclusionQuery : public MTLD3DQueryBase<MTLD3D11OcclusionQuery> {
  using MTLD3DQueryBase<MTLD3D11OcclusionQuery>::MTLD3DQueryBase;

  QueryState state_ = QueryState::Signaled;

  Rc<VisibilityResultQuery> query_ = new VisibilityResultQuery();
  uint64_t accumulated_value_ = 0;

  virtual UINT STDMETHODCALLTYPE
  GetDataSize() override {
    return desc_.Query == D3D11_QUERY_OCCLUSION_PREDICATE ? sizeof(BOOL) : sizeof(UINT64);
  };

  virtual HRESULT
  GetData(void *data) override {
    if (state_ == QueryState::Building) {
      return DXGI_ERROR_INVALID_CALL;
    }
    if (state_ == QueryState::Signaled || query_->getValue(&accumulated_value_)) {
      if (desc_.Query == D3D11_QUERY_OCCLUSION_PREDICATE) {
        *((BOOL *)data) = accumulated_value_ != 0;
      } else {
        *((uint64_t *)data) = accumulated_value_;
      }
      query_ = new VisibilityResultQuery();
      state_ = QueryState::Signaled;
      return S_OK;
    }
    return S_FALSE;
  }

  virtual VisibilityResultQuery *
  Begin() override {
    if (state_ == QueryState::Signaled) {
      state_ = QueryState::Building;
      return query_.ptr();
    }
    if (state_ == QueryState::Issued) {
      // discard previous issued query
      state_ = QueryState::Building;
      query_ = new VisibilityResultQuery();
      return query_.ptr();
    }
    // FIXME: it's effectively ignoring  Begin() after Begin()
    return nullptr;
  };

  virtual VisibilityResultQuery *
  End() override {
    if (state_ == QueryState::Signaled) {
      // ignore  a single End()
      accumulated_value_ = 0;
      return nullptr;
    }
    if (state_ == QueryState::Issued) {
      // FIXME: it's effectively ignoring End() after End()
      return nullptr;
    }
    state_ = QueryState::Issued;
    return query_.ptr();
  };

  virtual void DoDeferredQuery(VisibilityResultQuery *deferred_query) override{
    accumulated_value_ = 0;
    state_ = QueryState::Issued;
    query_ = deferred_query;
  };
};

HRESULT
CreateOcclusionQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, ID3D11Query1 **ppQuery) {
  if (ppQuery) {
    *ppQuery = ref(new OcclusionQuery(pDevice, pDesc));
    return S_OK;
  }
  return S_FALSE;
}

class MTLD3D11TimestampQueryImpl : public MTLD3DQueryBase<MTLD3D11TimestampQuery> {
  using MTLD3DQueryBase<MTLD3D11TimestampQuery>::MTLD3DQueryBase;

  QueryState state_ = QueryState::Signaled;

  Rc<TimestampQuery> query_ = new TimestampQuery();
  uint64_t latest_value_ = 0;

  virtual UINT STDMETHODCALLTYPE
  GetDataSize() override {
    return sizeof(UINT64);
  };

  virtual HRESULT
  GetData(void *data) override {
    if (state_ == QueryState::Signaled || query_->getValue(&latest_value_)) {
      *((uint64_t *)data) = latest_value_;
      query_ = new TimestampQuery();
      state_ = QueryState::Signaled;
      return S_OK;
    }
    return S_FALSE;
  }

  virtual TimestampQuery *
  End() override {
    if (state_ == QueryState::Issued) {
      // discard previous query
      query_ = new TimestampQuery();
    }
    state_ = QueryState::Issued;
    return query_.ptr();
  };
};

HRESULT
CreateTimestampQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, ID3D11Query1 **ppQuery) {
  if (ppQuery) {
    *ppQuery = ref(new MTLD3D11TimestampQueryImpl(pDevice, pDesc));
    return S_OK;
  }
  return S_FALSE;
}

std::vector<D3D11DeviceCounterMetadata>
BuildDeviceCounterRegistry(WMT::Device device) {
  std::vector<D3D11DeviceCounterMetadata> counters;
  uint32_t next_counter = D3D11_COUNTER_DEVICE_DEPENDENT_0;

  if (SupportsTimestampSampleBuffers(device)) {
    counters.push_back(D3D11DeviceCounterMetadata{
        D3D11_COUNTER(next_counter++),
        D3D11_COUNTER_TYPE_UINT64,
        1,
        "GPU Running Time",
        "ns",
        "GPU execution time accumulated between Begin and End.",
        DeviceCounterKind::GpuRunningTime,
    });
  }

  if (device.hasUnifiedMemory()) {
    counters.push_back(D3D11DeviceCounterMetadata{
        D3D11_COUNTER(next_counter++),
        D3D11_COUNTER_TYPE_UINT64,
        1,
        "GPU Shared Usage",
        "bytes",
        "Current GPU shared-memory usage in bytes.",
        DeviceCounterKind::GpuSharedUsage,
    });
  } else {
    counters.push_back(D3D11DeviceCounterMetadata{
        D3D11_COUNTER(next_counter++),
        D3D11_COUNTER_TYPE_UINT64,
        1,
        "GPU Dedicated Usage",
        "bytes",
        "Current GPU dedicated-memory usage in bytes.",
        DeviceCounterKind::GpuDedicatedUsage,
    });
  }

  return counters;
}

const D3D11DeviceCounterMetadata *
FindDeviceCounterMetadata(
    const std::vector<D3D11DeviceCounterMetadata> &metadata,
    D3D11_COUNTER counter) {
  const auto it = std::find_if(
      metadata.begin(), metadata.end(),
      [counter](const D3D11DeviceCounterMetadata &entry) {
        return entry.counter == counter;
      });
  return it == metadata.end() ? nullptr : &*it;
}

HRESULT
CopyDeviceCounterMetadata(
    const D3D11DeviceCounterMetadata &metadata,
    D3D11_COUNTER_TYPE *pType,
    UINT *pActiveCounters,
    LPSTR szName,
    UINT *pNameLength,
    LPSTR szUnits,
    UINT *pUnitsLength,
    LPSTR szDescription,
    UINT *pDescriptionLength) {
  if (pType)
    *pType = metadata.type;

  if (pActiveCounters)
    *pActiveCounters = metadata.active_counters;

  HRESULT hr = S_OK;
  hr = CopyCounterString(metadata.name, szName, pNameLength);
  if (FAILED(hr))
    return hr;
  if (hr == S_FALSE)
    return hr;

  auto units_hr = CopyCounterString(metadata.units, szUnits, pUnitsLength);
  if (FAILED(units_hr))
    return units_hr;
  if (units_hr == S_FALSE)
    hr = S_FALSE;

  auto description_hr = CopyCounterString(
      metadata.description, szDescription, pDescriptionLength);
  if (FAILED(description_hr))
    return description_hr;
  if (description_hr == S_FALSE)
    hr = S_FALSE;

  return hr;
}

HRESULT
CreateDeviceCounter(
    MTLD3D11Device *pDevice,
    const D3D11DeviceCounterMetadata &metadata,
    ID3D11Counter **ppCounter) {
  if (!ppCounter)
    return S_FALSE;

  switch (metadata.kind) {
  case DeviceCounterKind::GpuRunningTime:
    *ppCounter = ref(new MTLD3D11RunningTimeCounter(pDevice, metadata));
    return S_OK;
  case DeviceCounterKind::GpuSharedUsage:
  case DeviceCounterKind::GpuDedicatedUsage:
    *ppCounter = ref(new MTLD3D11MemoryUsageCounter(pDevice, metadata));
    return S_OK;
  }

  return DXGI_ERROR_UNSUPPORTED;
}

} // namespace dxmt
