// DXGI compatibility surface and swap chain plumbing.
// Included inside class CommandQueueImpl; not a standalone translation unit.
public:
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) override {}

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) override {}

  void STDMETHODCALLTYPE EndEvent() override {}

  HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *fence, UINT64 value) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::QueueSignal);
    if (!fence)
      return WARN_E_INVALIDARG(__func__);

    auto *state = dynamic_cast<Fence *>(fence);
    if (!state || state->GetParentDevice() != device_.ptr())
      return WARN_E_INVALIDARG(__func__);

    if (device_->GetDXMTDevice().queue().HasDeviceError())
      return DXGI_ERROR_DEVICE_REMOVED;

    static std::atomic<uint32_t> log_count = 0;
    if (D3D12DiagShouldLog(log_count, D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE"))) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: Signal enqueue"
           " queue=", reinterpret_cast<uintptr_t>(this),
           " queueType=", desc_.Type,
           " fence=", reinterpret_cast<uintptr_t>(fence),
           " state=", reinterpret_cast<uintptr_t>(state),
           " value=", value,
           " submittedBatches=", submitted_batches_.load(std::memory_order_relaxed),
           " hasWaited=", has_waited_.load(std::memory_order_relaxed));
    }

    PendingOperation op(
        FenceSignalSubmission{FencePrivateReference(state), value});
    EnqueuePendingOperation(std::move(op), nullptr);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *fence, UINT64 value) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::QueueWait);
    if (!fence)
      return WARN_E_INVALIDARG(__func__);

    auto *state = dynamic_cast<Fence *>(fence);
    if (!state || state->GetParentDevice() != device_.ptr())
      return WARN_E_INVALIDARG(__func__);

    if (device_->GetDXMTDevice().queue().HasDeviceError())
      return DXGI_ERROR_DEVICE_REMOVED;

    static std::atomic<uint32_t> log_count = 0;
    if (D3D12DiagShouldLog(log_count, D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE"))) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: Wait enqueue"
           " queue=", reinterpret_cast<uintptr_t>(this),
           " queueType=", desc_.Type,
           " fence=", reinterpret_cast<uintptr_t>(fence),
           " state=", reinterpret_cast<uintptr_t>(state),
           " value=", value);
    }

    PendingOperation op(
        FenceWaitSubmission{FencePrivateReference(state), value});
    EnqueuePendingOperation(std::move(op), nullptr);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *frequency) override {
    if (!frequency)
      return WARN_E_INVALIDARG(__func__);

    const auto timestamp_frequency =
        device_->GetMTLDevice().queryTimestampFrequency();
    if (!timestamp_frequency)
      return E_FAIL;

    *frequency = timestamp_frequency;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) override {
    if (!gpu_timestamp || !cpu_timestamp)
      return WARN_E_INVALIDARG(__func__);

#ifdef _WIN32
    LARGE_INTEGER cpu_before = {};
    if (!QueryPerformanceCounter(&cpu_before))
      return E_FAIL;
#endif

    UINT64 metal_cpu_timestamp = 0;
    UINT64 metal_gpu_timestamp = 0;
    if (!device_->GetMTLDevice().sampleTimestamps(
            &metal_cpu_timestamp, &metal_gpu_timestamp))
      return E_FAIL;

    const auto timestamp_frequency =
        device_->GetMTLDevice().queryTimestampFrequency();
    if (!timestamp_frequency)
      return E_FAIL;

    constexpr UINT64 nanoseconds_per_second = 1000000000ull;
    const auto scaled_gpu_timestamp =
        static_cast<unsigned __int128>(metal_gpu_timestamp) *
        timestamp_frequency;
    *gpu_timestamp = static_cast<UINT64>(
        scaled_gpu_timestamp / nanoseconds_per_second);

#ifdef _WIN32
    LARGE_INTEGER cpu_after = {};
    if (!QueryPerformanceCounter(&cpu_after))
      return E_FAIL;
    const auto before = static_cast<UINT64>(cpu_before.QuadPart);
    const auto after = static_cast<UINT64>(cpu_after.QuadPart);
    *cpu_timestamp = before + (after - before) / 2;
#else
    // Native retrace has no Win32 QPC domain. Use Metal's synchronously
    // sampled CPU timestamp while keeping the GPU value in query tick units.
    *cpu_timestamp = metal_cpu_timestamp;
#endif
    return S_OK;
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) override {
    *__ret = desc_;
    return __ret;
  }
#else
  D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override {
    return desc_;
  }
#endif

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {
    return device_->GetParent(riid, ppParent);
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override {
    return device_->GetAdapter(pAdapter);
  }

  HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC *desc, UINT surface_count,
                                          DXGI_USAGE usage,
                                          const DXGI_SHARED_RESOURCE *shared_resource,
                                          IDXGISurface **surface) override {
    InitReturnPtr(surface);
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown *const *resources,
                                                   DXGI_RESIDENCY *residency,
                                                   UINT resource_count) override {
    return device_->QueryResourceResidency(resources, residency, resource_count);
  }

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override {
    return device_->SetGPUThreadPriority(priority);
  }

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *priority) override {
    return device_->GetGPUThreadPriority(priority);
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
    return device_->SetMaximumFrameLatency(max_latency);
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
    return device_->GetMaximumFrameLatency(max_latency);
  }

  HRESULT STDMETHODCALLTYPE OfferResources(UINT resource_count, IDXGIResource *const *resources,
                                           DXGI_OFFER_RESOURCE_PRIORITY priority) override {
    return device_->OfferResources(resource_count, resources, priority);
  }

  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT resource_count, IDXGIResource *const *resources,
                                             WINBOOL *discarded) override {
    return device_->ReclaimResources(resource_count, resources, discarded);
  }

  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override {
    return device_->EnqueueSetEvent(event);
  }

  void STDMETHODCALLTYPE Trim() override {
    device_->Trim();
  }

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() {
    return GetD3D12DeviceMetalDevice(device_.ptr());
  }

  DxgiBackendKind STDMETHODCALLTYPE GetBackendKind() override {
    return DxgiBackendKind::Metal4;
  }

  uint64_t STDMETHODCALLTYPE GetMetalDeviceHandle() override {
    return device_->GetMetalDeviceHandle();
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() override {
    return device_->GetLocalD3DKMT();
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory1 *factory, HWND hWnd,
                                            const DXGI_SWAP_CHAIN_DESC1 *desc,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
                                            IDXGISwapChain1 **swap_chain) override {
    InitReturnPtr(swap_chain);
    if (!swap_chain || !factory || !hWnd || !desc)
      return DXGI_ERROR_INVALID_CALL;
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
      return DXGI_ERROR_UNSUPPORTED;
    return CreateD3D12SwapChain(*this, factory, hWnd, *desc, fullscreen_desc,
                                swap_chain);
  }

