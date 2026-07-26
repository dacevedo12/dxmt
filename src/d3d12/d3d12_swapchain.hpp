#pragma once

#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_texture.hpp"

#include <dxgi1_6.h>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {

class PresentSemaphoreSignals final {
public:
  PresentSemaphoreSignals(HANDLE queue_depth, HANDLE waitable) noexcept;
  PresentSemaphoreSignals(const PresentSemaphoreSignals &) = delete;
  PresentSemaphoreSignals &
  operator=(const PresentSemaphoreSignals &) = delete;
  ~PresentSemaphoreSignals() noexcept;

  void Signal() noexcept;

private:
  void Close() noexcept;

  HANDLE queue_depth_ = nullptr;
  HANDLE waitable_ = nullptr;
};

struct D3D12PresentSubmission final {
  D3D12PresentSubmission(
      Rc<Texture> backbuffer, Com<ID3D12Resource> resource,
      TextureViewKey api_thread_view, Rc<Presenter> presenter,
      double present_after, uint64_t apitrace_frame_index, UINT sync_interval,
      UINT flags, std::shared_ptr<Presenter::PresentState> state,
      std::shared_ptr<PresentSemaphoreSignals> signals) noexcept
      : backbuffer(std::move(backbuffer)), resource(std::move(resource)),
        api_thread_view(api_thread_view), presenter(std::move(presenter)),
        present_after(present_after),
        apitrace_frame_index(apitrace_frame_index),
        sync_interval(sync_interval), flags(flags), state(std::move(state)),
        signals(std::move(signals)) {}

  D3D12PresentSubmission(const D3D12PresentSubmission &) = delete;
  D3D12PresentSubmission &
  operator=(const D3D12PresentSubmission &) = delete;
  D3D12PresentSubmission(D3D12PresentSubmission &&) noexcept = default;
  D3D12PresentSubmission &
  operator=(D3D12PresentSubmission &&) noexcept = default;
  ~D3D12PresentSubmission() noexcept = default;

  Rc<Texture> backbuffer;
  Com<ID3D12Resource> resource;
  TextureViewKey api_thread_view = {};
  Rc<Presenter> presenter;
  double present_after = 0.0;
  uint64_t apitrace_frame_index = 0;
  UINT sync_interval = 0;
  UINT flags = 0;
  std::shared_ptr<Presenter::PresentState> state;
  std::shared_ptr<PresentSemaphoreSignals> signals;
};

static_assert(std::is_nothrow_move_constructible_v<D3D12PresentSubmission>);
static_assert(!std::is_copy_constructible_v<D3D12PresentSubmission>);

class D3D12SwapChainHost {
public:
  D3D12SwapChainHost() = default;
  D3D12SwapChainHost(const D3D12SwapChainHost &) = delete;
  D3D12SwapChainHost &operator=(const D3D12SwapChainHost &) = delete;

  [[nodiscard]] virtual IMTLD3D12Device &
  SwapChainDevice() noexcept = 0;
  [[nodiscard]] virtual ID3D12CommandQueue *
  SwapChainQueue() noexcept = 0;
  [[nodiscard]] virtual bool PrepareSwapChainResize(
      const std::vector<Com<ID3D12Resource>> &backbuffers) = 0;
  virtual void SubmitPresent(D3D12PresentSubmission submission,
                             uint64_t frame_id) = 0;
  virtual void NotifyPresentBoundary() noexcept = 0;

protected:
  ~D3D12SwapChainHost() = default;
};

HRESULT CreateD3D12SwapChain(
    D3D12SwapChainHost &host, IDXGIFactory1 *factory, HWND window,
    const DXGI_SWAP_CHAIN_DESC1 &desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
    IDXGISwapChain1 **swap_chain);

} // namespace dxmt::d3d12
