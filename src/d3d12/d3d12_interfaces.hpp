#pragma once

#include "dxgi_interfaces.h"
#include "com/com_guid.hpp"
#include <d3d12.h>

namespace dxmt {
class Device;
}

// Internal DXGI bridge implemented by the D3D12 device so existing DXGI
// swapchain/presenter code can recognize DXMT-backed D3D12 devices.
DEFINE_COM_INTERFACE("7f7f9293-9c67-4c3f-865b-30c92e1a7d12", IMTLD3D12Device)
    : public IMTLDXGIDevice {
  virtual dxmt::Device &GetDXMTDevice() = 0;
  virtual uint64_t NextTimestampQueryValue() = 0;
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

struct MTL_TEMPORAL_UPSCALE_D3D12_DESC {
  UINT InputContentWidth;
  UINT InputContentHeight;
  BOOL AutoExposure;
  BOOL InReset;
  BOOL DepthReversed;
  BOOL MotionVectorInDisplayRes;
  ID3D12Resource *Color;
  ID3D12Resource *Depth;
  ID3D12Resource *MotionVector;
  ID3D12Resource *Output;
  FLOAT MotionVectorScaleX;
  FLOAT MotionVectorScaleY;
  FLOAT PreExposure;
  ID3D12Resource *ExposureTexture;
  FLOAT JitterOffsetX;
  FLOAT JitterOffsetY;
};

typedef enum MTL_D3D12_FEATURE {
  MTL_D3D12_FEATURE_METALFX_TEMPORAL_SCALER = 0,
} MTL_D3D12_FEATURE;

DEFINE_COM_INTERFACE("b47d2fe2-e31a-43ea-a994-10e34834cc09",
                     IMTLD3D12GraphicsCommandListExt)
    : public IUnknown {
  virtual void STDMETHODCALLTYPE TemporalUpscale(
      const MTL_TEMPORAL_UPSCALE_D3D12_DESC *pDesc) = 0;
  virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
      MTL_D3D12_FEATURE Feature, void *pFeatureSupportData,
      UINT FeatureSupportDataSize) = 0;
};
