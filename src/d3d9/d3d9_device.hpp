#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d9_clear_quad.hpp"
#include "d3d9_common_texture.hpp"
#include "d3d9_diag.hpp"
#include "d3d9_enc_state.hpp"
#include "d3d9_mem.hpp"
#include "d3d9_multithread.hpp"
#include "d3d9_state_block_changes.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_command.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "dxmt_sampler.hpp"
#include "dxmt_tasks.hpp"
#include "dxmt_texture.hpp"
// DxsoShaderMetadata, the by-value arg of the getOrCreate*ShaderModule
// accessors below. Re-exported from airconv so d3d9 and the compiler
// agree on the decoded-shader struct layout.
#include "dxso_decoder.hpp"
#include "rc/util_rc_ptr.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt {

// Forward declarations.
class CommandQueue;

// Forward declaration of the async PSO-compile task. Defined in
// d3d9_device.cpp where the shader types are complete (the task holds
// Com<> refs to MTLD3D9{Vertex,Pixel}Shader to keep the underlying
// MTLFunctions alive across the worker thread). Mirrors d3d11's
// MTLCompiledGraphicsPipelineImpl shape (src/d3d11/d3d11_pipeline.cpp):
// same async ThreadpoolWork pattern, single-shader-pair scope.
class D3D9PsoCompileTask;
// Async work base shared by the PSO-link task and the per-variant
// function-compile task, so both flow through the one scheduler and a PSO
// task can park on a function task; full definitions in d3d9_shader.hpp /
// d3d9_shader.cpp / d3d9_device.cpp.
class D3D9AsyncTask;
class D3D9CompiledFunction;

// task_trait specialisation for the scheduler. Defined in-class so
// every TU that includes this header gets the same trampoline; methods
// are implicitly inline. The full D3D9AsyncTask definition is not
// needed at this point: only the pointer-to-incomplete; the trait
// methods are instantiated from d3d9_device.cpp where the class is
// complete. Same pattern as task_trait<ThreadpoolWork *> in
// src/d3d11/d3d11_pipeline_cache.cpp.
template <> struct task_trait<D3D9AsyncTask *> {
  D3D9AsyncTask *run_task(D3D9AsyncTask *task);
  bool get_done(D3D9AsyncTask *task);
  void set_done(D3D9AsyncTask *task);
};

// 16 fragment samplers (PS 0..15) + 4 vertex samplers
// (D3DVERTEXTEXTURESAMPLER0..3) → 20 internal slots. Mirrors wined3d's
// D3D9_MAX_TEXTURE_UNITS in dlls/d3d9/d3d9_private.h.
inline constexpr unsigned D3D9_MAX_TEXTURE_UNITS = 20;

// Vertex stream slots: D3D9 pins this at 16 across both DXVK
// (caps::MaxStreams) and wined3d (WINED3D_MAX_STREAMS).
inline constexpr unsigned D3D9_MAX_VERTEX_STREAMS = 16;

// Vertex-shader constant register file sizes. D3D9 SM2/SM3 spec at
// the hardware-VP limit. DXVK's MaxFloatConstantsVS / MaxOtherConstants
// in d3d9_caps.h. A software (or mixed) vertex-processing device exposes
// 8192 float constants instead (DXVK MaxFloatConstantsSoftware): the hot
// register file below stays 256 (the draw path uploads only those), and
// the extended c256..c8191 range is backed by a device-level overflow.
// wined3d's D3D9_MAX_VERTEX_SHADER_CONSTANTF matches the 256.
inline constexpr unsigned D3D9_MAX_VS_CONST_F = 256;
inline constexpr unsigned D3D9_MAX_VS_CONST_F_SWVP = 8192;
inline constexpr unsigned D3D9_MAX_VS_CONST_I = 16;
inline constexpr unsigned D3D9_MAX_VS_CONST_B = 16;

// Pixel-shader constants. F is the SM3 hardware limit (224); SM2 caps
// at 32 but the API bound is the higher SM3 number; the SM2 limit is
// enforced at link time, not at Set*. DXVK MaxSM3FloatConstantsPS.
inline constexpr unsigned D3D9_MAX_PS_CONST_F = 224;
inline constexpr unsigned D3D9_MAX_PS_CONST_I = 16;
inline constexpr unsigned D3D9_MAX_PS_CONST_B = 16;

// The float-constant upload extent, in registers. A shader that reads a
// constant through relative addressing (c[a0.x + n]) can touch any register
// in the file, so the whole file uploads and the zero-initialised constant
// shadow stands in for registers the app never Set (native reads zeros
// there); a narrower copy would leak the packed ring's neighbouring bytes.
// This is DXVK's rule: a relative c# read forces the extent to the full
// float count (dxso_compiler.cpp maxConstIndexF = floatCount). A
// direct-addressing shader uploads only the app's sticky high-water mark.
inline constexpr uint32_t
resolve_const_f_extent(uint32_t sticky_regs, bool uses_relative, uint32_t file_size) {
  if (uses_relative)
    return file_size;
  return sticky_regs < file_size ? sticky_regs : file_size;
}

class MTLD3D9Interface;

// D3D9StateBlockChanges (the per-category + per-render-state change-tracking
// mask each MTLD3D9StateBlock owns) lives in d3d9_state_block_changes.hpp,
// included above, so the host unit tier can pin its PIXELSTATE / VERTEXSTATE /
// ALL membership tables without the device include surface. The device's
// recording arms mark bits on the in-progress block's instance directly.

// Multi-inherits IDxmtDiag9 alongside the public Ex device. IDxmtDiag9
// deliberately does NOT inherit IUnknown (see d3d9_diag.hpp); the
// extra base introduces no ambiguity in the QI vtable. The device's
// QueryInterface override below hands callers an aliasing
// IDxmtDiag9 * for the private diag UUID without bumping the COM
// refcount; diag use is bracketed by the caller's existing
// IDirect3DDevice9 ref.
class MTLD3D9Device final : public ComObject<IDirect3DDevice9Ex>, public IDxmtDiag9 {
public:
  MTLD3D9Device(
      MTLD3D9Interface *parent, bool isEx, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow, DWORD behaviorFlags,
      const D3DPRESENT_PARAMETERS &validatedParams, WMT::Reference<WMT::Device> &&metalDevice
  );
  ~MTLD3D9Device();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
  // D3D9 clamps Release-at-0. The device multiply-inherits (ComObject +
  // IDxmtDiag9) so ComObjectClamp cannot wrap it; the override hand-folds the
  // guard. AddRef inherits ComObject (no underflow to clamp).
  ULONG STDMETHODCALLTYPE Release() override;

  // Internal accessors: used by surfaces / textures / buffers when
  // they need the underlying Metal device for resource allocation.
  // Not part of the IDirect3DDevice9 contract.
  WMT::Device
  metalDevice() const {
    return m_metalDevice;
  }
  // Size of the vertex float constant register file this device exposes: 256
  // on a hardware-VP device, 8192 on a software / mixed-VP device. The shader
  // compile path threads it into the DXSO codegen so the constant ceiling and
  // reladdr clamp match the file the device binds.
  UINT
  vertexShaderFloatConstantCount() const {
    return m_vsConstFCount;
  }
  // Realized Metal sample count for a D3D9 multisample request (1 on an
  // unsupported type). Device-exposed accessor for the same file-local
  // mapping CreateRenderTarget and CreateDepthStencilSurface use, so the
  // swapchain backbuffer derives its count from one place and every
  // attachment a render pass binds agrees.
  uint32_t metalSampleCount(D3DMULTISAMPLE_TYPE type, DWORD quality) const;
  // Public accessor to the underlying dxmt::CommandQueue. The swapchain's
  // Present routes its present-blit through a chunk on this queue
  // (chunk->emitcc + ctx.present + PresentBoundary). Returned by reference
  // because the queue owns its own encode/finish threads and must outlive
  // every chunk the device enqueues; the device owns it via unique_ptr
  // below.
  dxmt::CommandQueue &dxmtQueue() const;
  // Zero-fill every subresource of a freshly allocated DEFAULT-pool texture
  // through the queue's ResourceInitializer, so a sample / blit / present that
  // reads it before the app's first write sees defined content instead of
  // whatever the Metal allocator recycled from freed process memory. Each
  // subresource routes to an RT/DS clear or a buffer zero via the texture's
  // usage bit. Call at creation after rename(); the queue already stamps and
  // waits on the initializer's event every chunk, so no extra sync is needed.
  // d3d11 does the same at its own create sites (d3d11_texture_device.cpp),
  // and DXVK's d3d9 initializer clears every DEFAULT resource the same way;
  // MANAGED / SYSTEMMEM resources are skipped because they get their content
  // from the app's upload path, not from this zero.
  void initTextureWithZero(dxmt::Texture *texture);
  // CommitCurrentChunk wrapper that RAII-times the calling-thread commit
  // into the stall instrument (chunk-ring backpressure: the calling thread
  // can block here on a free chunk while the encode thread is busy). Every
  // d3d9 calling-thread commit routes through here, including the swapchain
  // Present and query flush paths, whose queue is this device's queue.
  void commitCurrentChunkTimed();
  // Current device-wide frame latency, as Set/Get via the d3d9Ex API.
  // Read by MTLD3D9SwapChain to clamp the queue's max_latency_ to
  // min(m_frameLatency, BackBufferCount + 1): DXVK d3d9_swapchain.cpp
  // GetActualFrameLatency. Pre-Ex apps that never call
  // SetMaximumFrameLatency get the default 3; the BackBufferCount
  // clamp then drops it to 2 for typical single-back-buffer titles.
  UINT
  getFrameLatency() const {
    return m_frameLatency;
  }
  // Compiled-once Metal library holding dxmt's internal compute/blit
  // shaders. Shared by per-context helpers (EmulatedCommandContext et al
  // in dxmt_command.{cpp,hpp}) and by the Presenter, which loads the
  // present-blit/scale fragment shader from it. d3d11 plumbs this via
  // dxmt::CommandQueue::cmd_library; d3d9 doesn't use dxmt::CommandQueue
  // so the device owns it directly.
  InternalCommandLibrary &
  internalCommandLibrary() {
    return m_internalCmdLib;
  }
  // Exposes the device-state for the swapchain's Present-on-Lost
  // gate. Returns D3D_OK / D3DERR_DEVICELOST / D3DERR_DEVICENOTRESET:
  // same shape as TestCooperativeLevel but valid for both Ex and
  // non-Ex devices, since Ex Present still respects the lost
  // transition (CheckDeviceState is what becomes the no-op).
  HRESULT
  presentStateGate() const {
    // Ex devices report S_PRESENT_OCCLUDED for Lost / NotReset states
    // (per wined3d swapchain.c, device.c) instead of
    // the non-Ex D3DERR_DEVICELOST. Apps querying with Ex-aware code
    // distinguish "not currently presenting (foreground lost)" from
    // "device truly lost (needs Reset)" via the S_PRESENT_OCCLUDED
    // success-status code.
    switch (m_deviceState.load(std::memory_order_relaxed)) {
    case DeviceState::Ok:
      return D3D_OK;
    case DeviceState::Lost:
      return m_isEx ? S_PRESENT_OCCLUDED : D3DERR_DEVICELOST;
    case DeviceState::NotReset:
      return m_isEx ? S_PRESENT_OCCLUDED : D3DERR_DEVICELOST;
    }
    return D3D_OK;
  }

  // emitCmdbufTailSignal(): public hook swapchain calls once per
  // Present to keep m_completionEvent in lock-step with cmdbuf-retirement.
  // Per-FlushDrawBatch signalEvents dropped (coalescer collapses adjacent
  // Render encoders only on non-SignalEvent nodes); one signal per cmdbuf
  // at tail feeds recycling.
  void emitCmdbufTailSignal();
  // Force-commit the current chunk so its cmdbuf actually reaches the
  // GPU. Used by Lock(DISCARD) when the rename ring runs dry; we
  // need the most recently retired backing's signal_seq to be a
  // signal the GPU will eventually retire, not a chunk still buffered
  // on the encode thread. Drains queued draws, drains the legacy sync
  // path, emits the tail signal, and commits. Pulled out of the
  // vertex/index buffer Lock helpers so both share one shape.
  void forceFlushAndCommit();

  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() override;
  HRESULT STDMETHODCALLTYPE EvictManagedResources() override;
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **ppD3D9) override;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *pCaps) override;
  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) override;
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override;
  HRESULT STDMETHODCALLTYPE
  SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) override;
  void STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override;
  BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override;
  HRESULT STDMETHODCALLTYPE
  CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **pSwapChain) override;
  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain) override;
  UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override;
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) override;
  HRESULT STDMETHODCALLTYPE Present(
      const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion
  ) override;
  HRESULT STDMETHODCALLTYPE
  GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) override;
  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) override;
  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override;
  void STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp) override;
  void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp) override;
  HRESULT STDMETHODCALLTYPE CreateTexture(
      UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture,
      HANDLE *pSharedHandle
  ) override;
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(
      UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle
  ) override;
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(
      UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture,
      HANDLE *pSharedHandle
  ) override;
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(
      UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle
  ) override;
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(
      UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer,
      HANDLE *pSharedHandle
  ) override;
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
      BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
  ) override;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
      BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
  ) override;
  HRESULT STDMETHODCALLTYPE UpdateSurface(
      IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface,
      const POINT *pDestPoint
  ) override;
  HRESULT STDMETHODCALLTYPE
  UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) override;
  HRESULT STDMETHODCALLTYPE
  GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) override;
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) override;
  // Front-buffer readback core shared by the device entry point (the
  // implicit chain) and IDirect3DSwapChain9::GetFrontBufferData (each
  // additional chain reads its own backbuffer).
  HRESULT frontBufferReadback(class MTLD3D9SwapChain *chain, IDirect3DSurface9 *pDestSurface);
  HRESULT STDMETHODCALLTYPE StretchRect(
      IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface,
      const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter
  ) override;
  HRESULT stretchRectImpl(
      IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface,
      const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter, bool from_readback
  );
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR color) override;
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(
      UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
  ) override;
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) override;
  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) override;
  HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) override;
  HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) override;
  HRESULT STDMETHODCALLTYPE BeginScene() override;
  HRESULT STDMETHODCALLTYPE EndScene() override;
  HRESULT STDMETHODCALLTYPE
  Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) override;
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override;
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) override;
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *pViewport) override;
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport) override;
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *pMaterial) override;
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) override;
  HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, const D3DLIGHT9 *pLight) override;
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *pLight) override;
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override;
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL *pEnable) override;
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, const float *pPlane) override;
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) override;
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override;
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) override;
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) override;
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB) override;
  HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus) override;
  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) override;
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override;
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override;
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override;
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override;
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) override;
  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries) override;
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override;
  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override;
  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override;
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *pRect) override;
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) override;
  HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override;
  BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override;
  HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override;
  float STDMETHODCALLTYPE GetNPatchMode() override;
  HRESULT STDMETHODCALLTYPE
  DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
      D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount
  ) override;
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(
      D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData,
      UINT VertexStreamZeroStride
  ) override;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(
      D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount,
      const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride
  ) override;
  HRESULT STDMETHODCALLTYPE ProcessVertices(
      UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer,
      IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags
  ) override;
  HRESULT STDMETHODCALLTYPE
  CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) override;
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override;
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override;
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override;
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *pFVF) override;
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) override;
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *pShader) override;
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **ppShader) override;
  HRESULT STDMETHODCALLTYPE
  SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) override;
  HRESULT STDMETHODCALLTYPE
  GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override;
  HRESULT STDMETHODCALLTYPE
  SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) override;
  HRESULT STDMETHODCALLTYPE
  GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override;
  HRESULT STDMETHODCALLTYPE
  SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) override;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override;
  HRESULT STDMETHODCALLTYPE
  SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) override;
  HRESULT STDMETHODCALLTYPE GetStreamSource(
      UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride
  ) override;
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override;
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) override;
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *pIndexData) override;
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **ppIndexData) override;
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) override;
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *pShader) override;
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **ppShader) override;
  HRESULT STDMETHODCALLTYPE
  SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) override;
  HRESULT STDMETHODCALLTYPE
  GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override;
  HRESULT STDMETHODCALLTYPE
  SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) override;
  HRESULT STDMETHODCALLTYPE
  GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override;
  HRESULT STDMETHODCALLTYPE
  SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) override;
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override;
  HRESULT STDMETHODCALLTYPE
  DrawRectPatch(UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo) override;
  HRESULT STDMETHODCALLTYPE
  DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo) override;
  HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override;
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) override;

  HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT width, UINT height, float *rows, float *columns) override;
  HRESULT STDMETHODCALLTYPE ComposeRects(
      IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst, IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects,
      IDirect3DVertexBuffer9 *pDstRectDescs, D3DCOMPOSERECTSOP Operation, INT Xoffset, INT Yoffset
  ) override;
  HRESULT STDMETHODCALLTYPE PresentEx(
      const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion,
      DWORD dwFlags
  ) override;
  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *pPriority) override;
  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override;
  HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT iSwapChain) override;
  HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9 **pResourceArray, UINT32 NumResources) override;
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override;
  HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND hDestinationWindow) override;

  bool fullscreenOwnsDisplay();
  // Drives the non-Ex fullscreen focus-loss transitions from the poll
  // points; see the definition for the wined3d WM_ACTIVATEAPP model.
  void updateNonExLostState();
  // WM_ACTIVATEAPP transitions for a fullscreen device, run from the focus proc
  // BEFORE the message reaches the application: wined3d applies every side
  // effect first and chains last, so by the time the app sees WM_ACTIVATEAPP the
  // display mode is already restored and the device window already minimized.
  // Emitting them the other way round inverts the order the tests assert.
  void onFocusActivation(bool activated);
  bool focusMessagesFiltered() const { return m_focusMessagesFiltered; }
  HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
      BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage
  ) override;
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(
      UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle,
      DWORD Usage
  ) override;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
      BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage
  ) override;
  HRESULT STDMETHODCALLTYPE
  ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode) override;
  HRESULT STDMETHODCALLTYPE
  GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) override;

private:
  // Seed the D3D9-spec render-state defaults. Called once from the
  // ctor; mirrors DXVK's D3D9DeviceEx::ResetState in shape (the
  // wined3d defaults match value-for-value but are stored under
  // wined3d's own enum).
public:
  // Losable-resource counter hooks called from each app-facing
  // Create*<DEFAULT-pool> and the matching leaf dtor. The implicit RT0 and
  // auto-DS skip this create-time count, but still gate Reset via the separate
  // m_isImplicitLosable path while the app holds a public ref (markImplicitLosable).
  void
  onLosableResourceCreated(int64_t bytes = 0) {
    m_losableResourceCount.fetch_add(1, std::memory_order_relaxed);
    if (bytes)
      m_reportedTextureMemory.fetch_add(bytes, std::memory_order_relaxed);
  }
  void
  onLosableResourceDestroyed(int64_t bytes = 0) {
    m_losableResourceCount.fetch_sub(1, std::memory_order_relaxed);
    if (bytes)
      m_reportedTextureMemory.fetch_sub(bytes, std::memory_order_relaxed);
  }
  uint32_t
  losableResourceCount() const {
    return m_losableResourceCount.load(std::memory_order_relaxed);
  }
  // Device created with software vertex processing only. Buffers on such a
  // device ignore DISCARD and NOOVERWRITE: native processes vertices on the
  // CPU, so the lock pointer stays stable and the contents persist (DXVK
  // d3d9_device.h CanOnlySWVP).
  bool
  canOnlySWVP() const {
    return (m_creationParams.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0;
  }
  // A lost device ignores D3DLOCK_DISCARD on a Lock (native drivers keep the
  // buffer contents; the wine d3d9 tests assert it, DXVK strips it the same way).
  bool
  isDeviceLost() const {
    return m_deviceState.load(std::memory_order_relaxed) == DeviceState::Lost;
  }
  // Running total of device-local (DEFAULT-pool) allocation bytes, subtracted
  // from the GetAvailableTextureMem base so the reported figure falls as the
  // app creates resources (DXVK/wined3d keep the same counter).
  int64_t
  reportedTextureMemory() const {
    return m_reportedTextureMemory.load(std::memory_order_relaxed);
  }

  // IDxmtDiag9: private diag QI surface (see d3d9_diag.hpp). Tests
  // probe this to assert teardown invariants; not user-facing.
  UINT STDMETHODCALLTYPE
  GetLosableResourceCount() override {
    D9DeviceLock lock = LockDevice();
    return losableResourceCount();
  }

private:
  void initDefaultRenderStates(bool enableAutoDepthStencil);
  // Resets every category of D3D9 device state to its post-CreateDevice
  // default (sampler states, transforms, stream freq, material,
  // texture-stage states, clip planes, VS/PS constants, lights, FVF,
  // bound textures / vertex buffers / index buffer / declaration /
  // shaders). The render-state array is re-seeded via
  // initDefaultRenderStates. Bound RT/DS are left to the caller;
  // Reset replaces those by hand with the new backbuffer + auto-DS.
  // Called by Reset; the ctor runs the equivalent code inline.
  void resetStateToDefaults(bool enableAutoDepthStencil);
  // Allocates an implicit depth-stencil surface matching `params` and
  // assigns it to m_depthStencilSurface. Used by the ctor for the
  // first auto-DS creation and by Reset to rebuild after a resolution
  // change. No-op if EnableAutoDepthStencil is FALSE or the format
  // doesn't lower to Metal. Caller is responsible for dropping the
  // prior m_depthStencilSurface beforehand.
  void createAutoDepthStencil(const D3DPRESENT_PARAMETERS &params);

  // Shared body for Draw{,Indexed}Primitive{,UP}. override_* fields
  // non-zero: use override via setVertexBuffer at unused slot (Apple
  // guarantees this retains MTLBuffer; useResource is residency-only).
  // opt_cap non-null: read validation gates from capture instead of m_*.
public:
  struct D3D9DrawCapture;
  // D9EncodingRefs is the per-draw reference-counted state container:
  // shaders, vertex decl, render targets, depth stencil, textures,
  // vertex buffers, index buffer. Each non-null Com<,false> slot pins
  // one private ref on the resource it points at; BatchedDraw carries
  // a std::shared_ptr<D9EncodingRefs> so the resources survive until
  // every BatchedDraw that referenced them is destroyed.
  struct D9EncodingRefs {
    Com<class MTLD3D9VertexShader, false> vertex_shader;
    Com<class MTLD3D9PixelShader, false> pixel_shader;
    Com<class MTLD3D9VertexDeclaration, false> vertex_declaration;
    Com<class MTLD3D9Surface, false> render_targets[D3D_MAX_SIMULTANEOUS_RENDERTARGETS];
    Com<class MTLD3D9Surface, false> depth_stencil_surface;
    Com<class MTLD3D9CommonTexture, false> textures[D3D9_MAX_TEXTURE_UNITS];
    Com<class MTLD3D9VertexBuffer, false> vertex_buffers[D3D9_MAX_VERTEX_STREAMS];
    Com<class MTLD3D9IndexBuffer, false> index_buffer;
  };

private:
  // Capture + batch infrastructure. D3D9DrawCapture snapshots
  // per-draw state by VALUE for faithful replay on encode thread.
  // Retain shapes (WMT::Reference, Com<>, Rc<>) ensure outliving of
  // calling-thread mutations.
public:
  // D3D9DrawCapture / BatchedDraw / ChunkEmitState are reached from
  // file-scope static helpers in d3d9_device.cpp (EmitDrawBatch_d9_chunk
  // and friends), so the types must be accessible outside the class.
  // The block re-closes to private: after ChunkEmitState. m_pendingDraws
  // (a std::vector<BatchedDraw>) stays private because it's only ever
  // touched through QueueBatchedDraw / FlushDrawBatch.
  struct D3D9DrawCapture {
    // PSO + render-pass identity. POD state (render_states, samplers,
    // streams, constants) on D9EncodingState; ref-counted (textures, VBs)
    // on D9EncodingRefs. Per-draw rename-cursor freeze: gpu_address() /
    // currentOffset() snapshot at queue time (advance on Lock(DISCARD)).
    struct VBSlot {
      obj_handle_t buffer = 0;
      uint64_t gpu_address = 0;
      uint32_t offset = 0;
      uint32_t stride = 0;
      // The DynamicBuffer's current allocation when this draw was recorded,
      // frozen from the same immediateName() read that produced buffer /
      // gpu_address above. The emit registers the Vertex-stage read against
      // it, so binding and fence-tracking stay on one allocation even after
      // a later Lock(DISCARD) renames the buffer. Both map modes populate
      // it now (BUFFER orders the upload copy, DIRECT fences the in-place
      // backing); null only for unbound streams.
      Rc<dxmt::BufferAllocation> alloc;
    };
    std::array<VBSlot, 16> vb_slots = {};
    // Index buffer frozen rename-cursor data (indexed draws only).
    // Buffer handle is stable across rename moves so it could come
    // from m_encodeSideRefs.index_buffer->metalBuffer().handle, but
    // routing through the freeze keeps a single Build-time source
    // of truth.
    obj_handle_t ib_buffer = 0;
    uint64_t ib_offset = 0;
    D3DFORMAT ib_format = D3DFMT_UNKNOWN;
    // The frozen index-buffer allocation, mirroring VBSlot::alloc; both
    // map modes populate it, null only for an unbound index buffer.
    Rc<dxmt::BufferAllocation> ib_alloc;
  };

  // BatchedDraw: one capture + per-call args. override_* fields
  // replace bound streams when non-zero. MTLBuffer lifetime pinned by
  // m_constRing signal_seq + setBuffer retain; no per-draw Reference
  // needed.
  struct BatchedDraw {
    D3D9DrawCapture cap;
    // Per-draw POD snapshot captured at queue time for Resolve to read
    // frozen state without racing setters. COW via m_encShadowDirty:
    // consecutive draws share one snapshot, O(state-change clusters).
    // Points into the queue's command-data ring; valid until the owning
    // chunk retires, which outlives every Resolve read of it.
    const dxmt::D9EncodingState *pod_snapshot = nullptr;
    // Ref-counted state is no longer per-draw; the chunk walker
    // mutates the persistent device-side D9EncodingRefs mirror
    // (MTLD3D9Device::m_encodeSideRefs) by replaying the SetRef ops in
    // arrival order. Resolve reads from that mirror; arrival-order on
    // the op stream guarantees correctness without a per-draw snapshot.
    // The 40-Com<>-slot AddRefPrivate / heap-alloc cost the COW model
    // paid per cluster boundary is gone; wined3d CS / d3d11 EmitOP shape.
    enum Type : uint8_t { kNonIndexed, kIndexed } type;
    UINT vertex_or_index_count = 0;
    UINT start_vertex_or_index = 0;
    INT base_vertex = 0;
    D3DPRIMITIVETYPE primitive_type = D3DPT_TRIANGLELIST;
    // DrawPrimitiveUP / DrawIndexedPrimitiveUP transient-buffer
    // overrides. Zero/null when the draw uses bound streams.
    obj_handle_t override_vb_buffer = 0;
    uint64_t override_vb_addr = 0;
    uint32_t override_vb_length = 0;
    uint32_t override_vb_stride = 0;
    obj_handle_t override_ib_buffer = 0;
    uint64_t override_ib_offset = 0;
    D3DFORMAT override_ib_format = D3DFMT_UNKNOWN;
    // ---- Resolved fields filled by ResolveBatchedDrawForChunk ----
    // Encode-thread work: PSO build, IA layout, view derivation,
    // sampler/DSSO cache. Caches encode-thread-only. resolved_pso_task
    // non-owning (pinned by m_psoCache for device lifetime).
    obj_handle_t resolved_pso = 0;
    D3D9PsoCompileTask *resolved_pso_task = nullptr;
    bool resolved_pso_first_use = false;
    obj_handle_t resolved_dsso = 0;
    uint8_t resolved_stencil_ref = 0;
    uint32_t resolved_slot_mask = 0;
    uint32_t resolved_ib_fmt = 0; // DXSO_INDEX_BUFFER_FORMAT: 0=none, 1=u16, 2=u32
    obj_handle_t resolved_vbuf_table_buffer = 0;
    uint64_t resolved_vbuf_table_offset = 0;
    obj_handle_t resolved_vs_resident_handles[D3D9_MAX_VERTEX_STREAMS] = {};
    // Per-stage bound texture handle. For the device-owned dummy
    // placeholder (no app texture) this is the dummy handle bound
    // directly. For an app texture it's the resolved per-bind view
    // handle (sRGB / swizzle / LOD), kept here for the cluster cache +
    // the per-encoder bind shadow.
    obj_handle_t resolved_frag_textures[16] = {};
    obj_handle_t resolved_frag_samplers[16] = {};
    // Per-stage dxmt::Texture wrapper for fence-tracked access.
    // ctx.access<Pixel>(..., Read) registers read dependency on prior
    // encoder writes. Without it, same-cmdbuf parallel execution causes
    // missing/dim render-to-texture (headlights, reflections, post-process).
    Rc<dxmt::Texture> resolved_frag_texture_dxmt[16];
    // Per-stage TextureViewKey (as u64) for the bound app texture's
    // sample view: sRGB swap, format swizzle, SetLOD mip clamp folded
    // in via dxmt::Texture::checkViewUse{Format,Swizzle,MipRange}. The
    // chunk-emit setup passes this to ctx.access(viewId), which resolves
    // the Metal handle AND keeps the view object alive (it lives on the
    // TextureAllocation the ref_tracker retains). Replaces the old
    // per-wrapper D3D9ViewCache whose views died with the wrapper on
    // Reset. 0 for dummy stages.
    uint64_t resolved_frag_view[16] = {};
    // Vertex texture fetch (VTF, SM3.0): D3DVERTEXTEXTURESAMPLER0-3 map to
    // texture slots 16-19 (texture_stage_to_slot) and are sampled in the VS,
    // bound on the Metal vertex stage via setVertexTexture/Sampler. The
    // shader's vertex sampler s<N> binds at Metal vertex index N. Resolved
    // every draw (not cached) since VTF draws are rare; 0/null when unused.
    obj_handle_t resolved_vert_textures[4] = {};
    obj_handle_t resolved_vert_samplers[4] = {};
    Rc<dxmt::Texture> resolved_vert_texture_dxmt[4];
    uint64_t resolved_vert_view[4] = {};
    // Render-pass attachments: pre-resolved to Metal handles (with sRGB
    // RT swap if D3DRS_SRGBWRITEENABLE), levels/slices, dimensions.
    obj_handle_t resolved_rt_handles[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
    obj_handle_t resolved_ds_handle = 0;
    Rc<dxmt::Texture> resolved_rt_dxmt[D3D_MAX_SIMULTANEOUS_RENDERTARGETS];
    Rc<dxmt::Texture> resolved_ds_dxmt;
    // TextureViewKey-as-u64 picked at resolve time. fullView for the
    // common path; checkViewUseFormat(srgb) for D3DRS_SRGBWRITEENABLE.
    // The chunk lambda passes this to ctx.access for residency tracking.
    uint64_t resolved_rt_view[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
    uint64_t resolved_ds_view = 0;
    uint16_t resolved_rt_level[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
    uint16_t resolved_rt_slice[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
    uint16_t resolved_ds_level = 0;
    uint16_t resolved_ds_slice = 0;
    uint32_t resolved_rt_width = 0;
    uint32_t resolved_rt_height = 0;
    uint8_t resolved_rt_count = 0;
    // MSAA sample count of the bound attachments. PSO is built with this
    // count and the render-pass descriptor must match; Metal validates
    // PSO.raster_sample_count == pass.default_raster_sample_count at
    // setRenderPipelineState time. Resolved on the calling thread from
    // the RT0 (or DS if no RT) descriptor's MultiSampleType.
    uint8_t resolved_raster_sample_count = 1;
    bool resolved_ds_has_stencil = false;
    // The bound depth-stencil is also sampled at a fragment stage and neither
    // depth nor stencil is written this draw, so it is bound read-only: Metal
    // permits the in-pass sample only when there is no write to the attachment
    // (a depth-aware post-process samples the depth it tests against). See the
    // render-pass start. The depth-write case is a true feedback loop, left as-is.
    bool resolved_ds_readonly = false;
    // D3DRS_DEPTHBIAS r-value scale, resolved from the bound DS's D3D9
    // format. The app-side bias is in normalized depth-buffer space;
    // Metal applies `bias * r` in hardware where r is the format's
    // minimum resolvable difference. Multiplying by 1/r at emit time
    // restores D3D9 semantics. See DepthBiasScale() in d3d9_format.cpp
    // for the per-format table (ports DXVK's d3d9_util.h shape).
    // Default 1.0 = no-op for the no-DS-bound case.
    float resolved_depth_bias_scale = 1.0f;
    // Indexed-draw IB handle + base offset resolved on the calling
    // thread so the lambda doesn't dereference cap.ib_ref / its mutable
    // rename cursor. For UP-indexed draws this is the override buffer
    // and offset; for bound IBs this is metalBuffer().handle and
    // currentOffset() snapped at resolve time.
    obj_handle_t resolved_ib_handle = 0;
    uint64_t resolved_ib_base_offset = 0;
    // Lifetime pins on VB / IB wrappers. setVertexBuffer doesn't retain
    // MTLBuffer; wrapper destruction mid-chunk causes dead handle, GPU error
    // kIOGPUCommandBufferCallbackErrorInvalidResource. Textures have Rc<>
    // pins; VB/IB need explicit pins. Migration: 17 slots per-draw, 23 on device.
    Com<class MTLD3D9VertexBuffer, false> resolved_vb_pins[D3D9_MAX_VERTEX_STREAMS];
    Com<class MTLD3D9IndexBuffer, false> resolved_ib_pin;
    // BUFFER-mode streams only: the tracked Private allocation to register
    // a Vertex-stage read access against so the fence tracker orders the
    // staged upload copy ahead of this draw. Copied from cap.vb_slots[].alloc
    // (frozen on the calling thread at BuildDrawCapture), NOT re-read from
    // immediateName() here, so the read tracks the same allocation the
    // binding was frozen against even after a later Lock(DISCARD) renames
    // the buffer. Null for DIRECT-mode and override (UP) streams, which bind by
    // raw handle and need no read tracking. The wrapper pins above keep the
    // wrapper alive; the Rc keeps the allocation alive through GPU read.
    Rc<dxmt::BufferAllocation> resolved_vb_dxmt[D3D9_MAX_VERTEX_STREAMS];
    Rc<dxmt::BufferAllocation> resolved_ib_dxmt;
    // VS/PS f/i/b register-file + clip-plane constant uploads. Resolve
    // pulls the data from D9EncodingState, allocates from m_constRingResolve,
    // and stores the resulting (buffer, offset) pairs here. Fixed slot
    // order: 0=vs_cb (F), 1=vs_ic (I), 2=vs_bc (packed bool), 3=ps_cb,
    // 4=ps_ic, 5=ps_bc, 6=vs_cp (packed clip planes), 7=vs_cc (clip
    // count).
    struct ResolvedConstUpload {
      obj_handle_t buffer = 0;
      uint64_t offset = 0;
    };
    // Index 8 carries the pre-transform viewport remap (invExtent/invOffset)
    // bound to VS buffer 5; only populated when resolved_position_transformed.
    // Index 9 carries the point-size uniform (size/min/max) bound to VS
    // buffer 6; only bound when resolved_inject_point_size.
    std::array<ResolvedConstUpload, 10> resolved_const_uploads = {};
    // Pre-transformed (POSITIONT/XYZRHW) draw: the VS variant injects the
    // screen->clip remap and reads the viewport uniform at VS buffer 5.
    bool resolved_position_transformed = false;
    // Point-list draw whose injecting VS variant reads the point-size
    // uniform at VS buffer 6 (size/min/max clamp); the size rides the
    // uniform so one variant serves every size.
    bool resolved_inject_point_size = false;
    // Viewport / scissor pre-converted to Metal shape by Resolve (from
    // D9EncodingState's D3D9-shape fields + D3DRS_SCISSORTESTENABLE).
    // Cheap to store per-draw (one cache-line); avoids re-running the
    // wmt_*_from_d3d9 helpers in every per-draw emit pass.
    WMTViewport resolved_viewport = {};
    WMTScissorRect resolved_scissor = {};
    // Pending-clear no longer rides on the BatchedDraw; it's emitted as
    // a standalone Clear chunk by flushOpenWork's drainPendingClear and
    // folded into the first surviving Render encoder by the dxmt_context
    // coalescer (dxmt_context.cpp). The standalone chunk also keeps the
    // clear alive when every queued draw fails Resolve.
  };

  // Calling-thread pending-op record for non-draw operations.
  // Discriminated-union model matching d3d11/d3d12 dxmt and wined3d CS:
  // typed records in single arrival-order stream. blits/clears/uploads
  // ride same stream as draws without per-call FlushDrawBatch round-trip.
  struct PendingBlitOp {
    // Rc<> on src + dst for lifetime pins. ctx.access<Compute>
    // registers read/write dependencies with fence tracker. Without it,
    // same-RT Render merge folds across Blit, flipping order (caused black
    // 3D world after op-stream refactor).
    Rc<dxmt::Texture> src_tex;
    Rc<dxmt::Texture> dst_tex;
    uint32_t src_mip = 0;
    uint32_t dst_mip = 0;
    uint32_t src_slice = 0;
    uint32_t dst_slice = 0;
    WMTOrigin src_origin = {};
    WMTOrigin dst_origin = {};
    WMTSize size = {};
    // Stretch / format-convert path: render-pass sample/store vs copy.
    // Copy is same-extent bit copy ignoring filter (Metal semantics);
    // Stretch viewport spans dst sub-rect (differs from src under scaling).
    // BufferCopy stages a locked BUFFER-mode buffer's dirty range from an
    // upload-ring block into its Private buffer; its access<Compute> write
    // is what the draw's read access synchronises against. BufferToTexture is
    // the texture analogue: a mirror-backed level's dirty bytes staged from an
    // upload-ring block into the Private texture, riding the same arrival-order
    // stream so a draw queued before the Lock samples the pre-upload contents.
    // GenerateMipmaps rides the op stream (dst_tex = the texture) so the AUTOGEN
    // mip-gen is ordered AFTER the level-0 upload/StretchRect that feeds it
    // (those are op-stream writes too now); emitting it directly at flush would
    // run it before those writes. No per-op signal (a mid-chunk signal recycles
    // ring blocks whose ops have not executed); chunk completion covers recycling.
    enum class Kind : uint8_t {
      Copy = 0,
      Stretch = 1,
      Resolve = 2,
      BufferCopy = 3,
      BufferToTexture = 4,
      GenerateMipmaps = 5,
      DepthResolve = 6
    };
    Kind kind = Kind::Copy;
    WMTSize dst_size = {};
    D3DTEXTUREFILTERTYPE filter = D3DTEXF_NONE;
    // Format-converting Stretch only: the D3DFormatSamplerSwizzle of the SOURCE
    // format, applied to the sampled src view (EmitStretchBlitOp_d9) so a stretch
    // FROM a fixup-needing format (L8, A4R4G4B4, V8U8, ATI2, 2-channel...) reads
    // the D3D9 channel shape instead of raw storage. Identity for same-shape
    // sources and every non-Stretch kind. The write side into permuted storage
    // (A4R4G4B4 dst) stays a documented deferral (S3-F8b), so only the read is
    // corrected here.
    WMTTextureSwizzleChannels src_swizzle = {
        WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha
    };
    // BufferCopy fields. buf_dst_alloc is the tracked destination
    // allocation, frozen from the DynamicBuffer's immediateName() when
    // flushDirty recorded the copy (calling thread) rather than re-read at
    // emit: a later Lock(DISCARD) renames the buffer, and the draw that
    // reads this upload froze the same allocation, so the write must land
    // on it too. Its write is registered via access<Compute> so the fence
    // tracker orders the copy against draws reading the allocation;
    // buf_src_handle / buf_src_offset name the upload-ring source block.
    Rc<dxmt::BufferAllocation> buf_dst_alloc;
    obj_handle_t buf_src_handle = 0;
    uint64_t buf_src_offset = 0;
    uint64_t buf_dst_offset = 0;
    uint64_t buf_length = 0;
    // BufferToTexture fields. The destination texture level is dst_tex /
    // dst_mip / dst_slice / dst_origin / size (shared with the texture blit
    // kinds); tex_src_pitch is the source bytes-per-row and tex_bytes_per_image
    // the per-slice stride of the staged upload-ring block (buf_src_handle /
    // buf_src_offset).
    uint32_t tex_src_pitch = 0;
    uint32_t tex_bytes_per_image = 0;
  };

  // Calling-thread record of ref-counted state mutation. Setter AddRefPrivate
  // once; walker installs via move semantics. Single AddRef ownership until
  // walker consumes (no exception leak: chunk lambda always completes under
  // dxmt FIFO queue).
  struct PendingRefOp {
    // Flattened slot enum: same shape as D9EncodingRefs (1 VS + 1 PS +
    // 1 VertexDecl + 4 RTs + 1 DS + 16 Textures + 16 VertexBuffers + 1 IB
    // = 41 slots). Encoded as uint8_t to keep the op record at 16 bytes
    // (slot + padding + void* on 64-bit, slot + 3 bytes pad + void*
    // on 32-bit i386).
    enum Slot : uint8_t {
      VertexShader = 0,
      PixelShader = 1,
      VertexDeclaration = 2,
      RenderTarget0 = 3,
      RenderTarget1 = 4,
      RenderTarget2 = 5,
      RenderTarget3 = 6,
      DepthStencilSurface = 7,
      // Texture0..19: PS samplers 0..15 + VS samplers 16..19.
      // Apply/resetStateToDefaults index by Texture0+slot; must cover
      // D3D9_MAX_TEXTURE_UNITS=20. Pre-fix: collision with VertexBuffer0
      // caused texture installed in vertex_buffers via wrong static_cast (UB).
      Texture0 = 8,
      Texture19 = 27,
      VertexBuffer0 = 28,
      VertexBuffer15 = 43,
      IndexBuffer = 44,
    };
    Slot slot;
    void *com_ptr; // type-erased; walker static_casts by slot
  };

  // 8-byte tagged ref into per-kind storage. m_pendingOps holds these
  // in arrival order; the chunk lambda dispatches each by kind to the
  // matching m_pending<Kind>s[index] entry. Keeping per-kind storage
  // (instead of a fat std::variant) avoids 1.5 KB of slack per Blit/
  // Clear entry that BatchedDraw would force.
  struct PendingOpRef {
    enum Kind : uint8_t { Draw = 0, Blit = 1, SetRef = 2 };
    Kind kind;
    uint32_t index;
  };

  // Encoder-side binding shadow as a per-chunk-lambda struct. The lambda
  // runs on the encode thread; the shadow lives on its stack frame for
  // one render pass at a time. A fresh ChunkEmitState() is constructed
  // on every startRenderPass so the next encoder doesn't observe stale
  // shadow from the previous one.
  struct ChunkEmitState {
    obj_handle_t pso = 0;
    obj_handle_t dsso = 0;
    int stencil_ref = -1;
    int fill_mode = -1;
    int cull_mode = -1;
    int depth_clip_mode = -1;
    uint32_t depth_bias_bits = 0xFFFFFFFFu;
    uint32_t slope_scale_bits = 0xFFFFFFFFu;
    uint32_t blend_color_bits = 0; // packed BGRA from D3DRS_BLENDFACTOR (snapshot)
    bool blend_color_set = false;
    // Viewport / scissor shadow. Both rarely change between draws in a
    // batched encoder, and Metal's debug layer flags re-setting them to the
    // same value as a redundant setViewport / setScissorRect. Emit only on
    // change (like the rasterizer / DSSO / blend skips). *_set == false means
    // "not yet emitted this encoder", forcing the first draw to emit since a
    // fresh encoder has no viewport / scissor bound.
    WMTViewport viewport = {};
    bool viewport_set = false;
    WMTScissorRect scissor = {};
    bool scissor_set = false;
    obj_handle_t frag_tex[16] = {};
    obj_handle_t frag_smp[16] = {};
    obj_handle_t vs_resident[D3D9_MAX_VERTEX_STREAMS] = {};
    // Shadow of (dxmt::Texture wrapper, view key) pair per PS stage.
    // ctx.access<Pixel>() registers fence dependency and resolves view
    // handle; idempotent within encoder so skip re-access on same (wrapper,
    // view). SetLOD / sRGB-toggle produces new view key, must re-access.
    class dxmt::Texture *frag_tex_access[16] = {};
    uint64_t frag_view[16] = {};
    // Shadow of buffer handles at VS/PS slots. Const-buffer slots share
    // one handle; most draws emit offset-only update instead of full setBuffer.
    // Cluster-stable draws share same offset, skip command entirely.
    obj_handle_t vs_buf_handle[32] = {};
    obj_handle_t fs_buf_handle[32] = {};
    uint64_t vs_buf_offset[32] = {};
    uint64_t fs_buf_offset[32] = {};
  };

private:
  // BuildDrawCapture freezes per-draw rename cursors (gpu_address/
  // currentOffset advance on Lock(DISCARD), snapshot at queue time).
  // Ref-counted state travels via setters to D9EncodingState on encode thread;
  // override_* args are per-call, not device state.
  D3D9DrawCapture BuildDrawCapture();

  // Op-stream queue helpers. QueueBatchedDraw appends to m_pendingDraws
  // *and* records its position in m_pendingOps; QueueBlitOp does the
  // same for blits. FlushDrawBatch then hands all three vectors plus
  // the arrival-order ref vector to a single chunk lambda. The
  // discriminated stream replaces StretchRect's old per-call
  // FlushDrawBatch + chunk->emitcc pattern; see PendingOpRef comment
  // above for the design alignment with d3d11 EmitOP / wined3d CS.
  void QueueBatchedDraw(BatchedDraw &&draw);
  void QueueBlitOp(PendingBlitOp &&op);
  // Software-VP draw gate. Returns true when the currently bound vertex
  // shader references the extended constant file (c256..) while the device is
  // in hardware vertex processing, and this is the first such draw: the caller
  // then rejects it with D3DERR_INVALIDCALL and queues nothing. Subsequent
  // draws return false and fall back to fixed-function vertex processing (the
  // encode side forces the FFP path from the captured mode). A shader that
  // stays within c0..c255, or a device in software VP, always returns false,
  // so a pure hardware-VP device never enters this path.
  bool swvpDrawGateRejects();
  // Allocate a throwaway single-sample Private render target for the
  // resolve-then-stretch StretchRect compose (D3-O1): a scaled or fixup-source
  // MSAA resolve first resolves into this transient at the source extent, then
  // the Stretch reads it. RenderTarget|ShaderRead|PixelFormatView usage so it
  // serves as the resolve destination and the swizzled Stretch source. Returns
  // nullptr on allocation failure. The two queued ops hold the only refs, so it
  // frees once the chunk that runs them retires.
  Rc<dxmt::Texture> createTransientResolveTarget(WMTPixelFormat format, uint32_t width, uint32_t height);
  // Queue a ref-state mutation onto the op stream. The setter must
  // have already AddRefPrivate'd new_com (or passed nullptr for an
  // unbind). The walker takes ownership: installs into
  // m_encodeSideRefs.<slot> by move semantics (Release-old, no further
  // AddRef). See PendingRefOp's class-level comment for the wined3d
  // CS / d3d11 EmitOP shape this is porting.
  void QueueRefOp(PendingRefOp::Slot slot, void *new_com);
  // Encode-thread walker hook (called from the chunk lambda in
  // FlushDrawBatch). Installs one PendingRefOp into m_encodeSideRefs
  // by static_cast'ing op.com_ptr to the slot's resource type and
  // doing a take-ownership move into the Com<,false> slot. NOT thread-
  // safe; only the encode worker should call this.
  void ApplyRefOp_d9(const PendingRefOp &op);
  // Returns the persistent fan-list IB handle if PrimitiveCount fits
  // the pre-built capacity, otherwise 0 (caller falls back to the
  // per-call m_constRing alloc). Lazily allocates on first call.
  obj_handle_t fanListIBForPrimCount(uint32_t prim_count);
  // Resolve a fan→list u32 IB for the four Draw* fan emulation sites.
  // When `src == nullptr` (synthesise 0..N-1) tries the cached
  // fanListIBForPrimCount path first; otherwise allocates from
  // m_constRing and remaps from `src` (a u16/u32 source IB or inline
  // pIndexData). Returns (buffer_handle, offset_into_buffer).
  std::pair<obj_handle_t, uint32_t> BuildFanIndexBuffer(uint32_t prim_count, const void *src, uint32_t src_idx_size);

  // Widens a run of 16-bit indices to a fresh 32-bit index buffer on
  // m_constRing. Used to lift a strip draw whose index range contains the
  // 0xffff strip-restart sentinel out of Metal's always-on primitive restart
  // (as 32-bit, 0xffff is 0x0000ffff, not the 0xffffffff sentinel).
  std::pair<obj_handle_t, uint32_t> BuildWidenedIndexBuffer(const uint16_t *src, uint32_t index_count);

public:
  // Public for the swapchain's Present, which drains queued draws onto
  // a chunk before posting the present-blit chunk.
  HRESULT FlushDrawBatch();

private:
  // Serializes m_bufferBackingPool / m_bufferBackingPoolBytes and the
  // m_tearingDown latch. Donations run inside resource destructors, and
  // a chunk-pinned wrapper drops its last reference on the queue's
  // encode or finish thread when the chunk tears down, racing the
  // calling thread's acquire path. Declared before every member whose
  // destructor can still donate so it is destroyed after them. Same
  // discipline as the D3D9MemoryAllocator lock (d3d9_mem.cpp).
  dxmt::mutex m_bufferBackingPoolMutex;

  // Per-kind storage. m_pendingOps's index field selects the entry in
  // the matching vector; arrival-order is preserved by m_pendingOps's
  // own order. All three are moved into the chunk lambda on flush;
  // their capacities are restored via reserve() on the calling thread
  // to skip the geometric grow each frame would otherwise hit.
  std::vector<PendingOpRef> m_pendingOps;
  std::vector<BatchedDraw> m_pendingDraws;
  std::vector<PendingBlitOp> m_pendingBlits;
  std::vector<PendingRefOp> m_pendingRefOps;

  // Encode-thread-only mirror of ref-counted state. Mutated by walker
  // on SetRef ops in arrival order (wined3d CS / d3d11 shape). SoR for
  // what's bound at Draw-resolve; calling-thread m_* SoR for app queries.
  // m_encodeSideRefsGen bumps on SetRef; consecutive Draws short-circuit.
  D9EncodingRefs m_encodeSideRefs;
  uint64_t m_encodeSideRefsGen = 1;

  // Resolve queued draw on encode thread. Moved from calling thread
  // (was cap on throughput). Compiles variants, builds PSO, allocates
  // vbuf-table, resolves views/samplers. chunk_seq captured at push-time
  // so m_constRing.allocate owner matches (lambda runs after ++m_currentCmdSeq).
  struct ConstUploadCache {
    const dxmt::D9EncodingState *pod_ptr = nullptr;
    // Shader identity when its def'd constants were stamped into the
    // float CB upload (relative-addressing shaders only; see the def
    // re-apply in ResolveBatchedDrawForChunk). null when the bound
    // shader contributed nothing, which keeps pod-only reuse across
    // shader switches for the common case.
    const void *vs_defs_key = nullptr;
    const void *ps_defs_key = nullptr;
    // Two upload inputs that are NOT carried on pod_snapshot and so must key
    // this cache separately, or a same-pod hit reuses stale bytes:
    //   - ffp_texcoord_width (packed): the generated FFP texture-matrix fold
    //     reads the vertex declaration's texcoord component widths, and a
    //     SetVertexDeclaration does not rotate the pod snapshot.
    //   - ds_bound: the POSITIONT viewport-remap Z passthrough gates on whether
    //     a depth-stencil is bound, and a SetDepthStencilSurface does not rotate
    //     the pod snapshot.
    //   - pos_transformed: FFP clip planes are transformed to world space only
    //     for a non-POSITIONT draw; a POSITIONT draw packs the raw plane, and
    //     the vertex declaration that decides this is not on the pod snapshot.
    uint32_t ffp_texcoord_width_key = 0;
    bool ds_bound = false;
    bool pos_transformed = false;
    std::array<BatchedDraw::ResolvedConstUpload, 10> uploads = {};
  };
  // Cluster cache for the cluster-stable resolved bundle (PSO, DSSO,
  // sampler+texture views, RT/DS resolve, viewport/scissor, IA layout
  // metadata). Predicate: the pod_snapshot pointer + m_encodeSideRefsGen
  // plus per-draw shape bits (UP override flags, primitive_type, draw
  // type). On hit, copy cached fields into bd and skip the FNV
  // hashes + cache-lookups + atomic incRefs that fill them. Same
  // lifetime as ConstUploadCache; chunk-lambda stack, reset implicitly
  // per chunk.
  struct ResolveCache {
    const dxmt::D9EncodingState *pod_ptr = nullptr;
    // Encode-side ref-state generation at the time this cache entry was
    // recorded. The walker bumps m_encodeSideRefsGen on every SetRef op
    // application; consecutive Draw ops with no intervening SetRef
    // observe the same gen and hit the cluster cache.
    uint64_t ref_gen = 0;
    bool up_vb = false;
    bool up_ib = false;
    D3DFORMAT up_ib_format = D3DFMT_UNKNOWN;
    D3DPRIMITIVETYPE primitive_type = D3DPT_TRIANGLELIST;
    BatchedDraw::Type draw_type = BatchedDraw::kNonIndexed;
    // Cached resolved fields (cluster-stable subset of BatchedDraw).
    obj_handle_t resolved_pso = 0;
    D3D9PsoCompileTask *resolved_pso_task = nullptr;
    obj_handle_t resolved_dsso = 0;
    uint8_t resolved_stencil_ref = 0;
    uint32_t resolved_slot_mask = 0;
    uint32_t resolved_ib_fmt = 0;
    uint8_t resolved_raster_sample_count = 1;
    float resolved_depth_bias_scale = 1.0f;
    bool resolved_ds_has_stencil = false;
    // Pre-transformed (POSITIONT) draw: cluster-cached so back-to-back same-state
    // draws still bind the loc-5 viewport-remap uniform (the cached PSO declares it).
    bool resolved_position_transformed = false;
    // Point-size injection: cluster-cached so back-to-back same-state point
    // draws still bind the loc-6 point-size uniform (the cached PSO declares it).
    bool resolved_inject_point_size = false;
    // FFP texcoord component widths from the vertex declaration. The IA loop that
    // derives these runs only on a cluster MISS, so cache them: a cluster-hit
    // draw must still fold its texture matrices with the true widths (not the
    // {2..} default), and the const-upload cache keys on the true value.
    uint32_t ffp_texcoord_width[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    uint8_t resolved_rt_count = 0;
    uint32_t resolved_rt_width = 0;
    uint32_t resolved_rt_height = 0;
    obj_handle_t resolved_ds_handle = 0;
    uint64_t resolved_ds_view = 0;
    uint16_t resolved_ds_level = 0;
    uint16_t resolved_ds_slice = 0;
    WMTViewport resolved_viewport{};
    WMTScissorRect resolved_scissor{};
    obj_handle_t resolved_rt_handles[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
    uint64_t resolved_rt_view[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
    uint16_t resolved_rt_level[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
    uint16_t resolved_rt_slice[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
    Rc<dxmt::Texture> resolved_rt_dxmt[D3D_MAX_SIMULTANEOUS_RENDERTARGETS];
    Rc<dxmt::Texture> resolved_ds_dxmt;
    obj_handle_t resolved_frag_textures[16] = {};
    obj_handle_t resolved_frag_samplers[16] = {};
    Rc<dxmt::Texture> resolved_frag_texture_dxmt[16];
    uint64_t resolved_frag_view[16] = {};
    // Last computed PSO key + its task. When cluster_hit misses but the
    // PSO inputs (vs/ps function + RT/DS formats + blend state) are
    // unchanged, the recomputed pso_key matches this value and we skip
    // the m_psoCache.find unordered_map probe: a cheap memcmp of the
    // previous draw's pso_key before the map probe. Catches the common
    // "ref_ptr changed but PSO inputs didn't" case (e.g., texture
    // rebind on the same shader/RT).
    uint64_t last_pso_key = 0;
    D3D9PsoCompileTask *last_pso_task = nullptr;
    // Vbuf-table cache: ~80% cluster hit rate, byte-identical tables
    // across clusters without SetStreamSource. Cache inputs + (buffer, offset),
    // reuse when matched; skips allocate mutex, bump, and per-slot writes.
    uint32_t last_vbuf_slot_mask = 0xFFFFFFFFu;
    uint64_t last_vbuf_base_addr[D3D9_MAX_VERTEX_STREAMS] = {};
    uint32_t last_vbuf_stride[D3D9_MAX_VERTEX_STREAMS] = {};
    uint32_t last_vbuf_length[D3D9_MAX_VERTEX_STREAMS] = {};
    obj_handle_t last_vbuf_table_buffer = 0;
    uint64_t last_vbuf_table_offset = 0;
  };
  bool ResolveBatchedDrawForChunk(
      BatchedDraw &bd, uint64_t chunk_seq, uint64_t chunk_coherent_id, ConstUploadCache &const_cache,
      ResolveCache &resolve_cache
  );

  // Refresh m_cachedSignaled from m_completionEvent and trim retired
  // blocks out of m_constRing / m_uploadRing. Call after every
  // ++m_currentCmdSeq on the calling thread. All draw/blit paths route
  // through chunks so every chunk-emit site must refresh by hand, or
  // the rings will burn fresh placed-buffer blocks per allocate (one
  // wine_unix_call per newBuffer, plus monotonic memory growth).
  void refreshSignaledAndTrimRings();

  Com<MTLD3D9Interface> m_parent;
  // Friended so MTLD3D9StateBlock::Capture / Apply can read and
  // write the per-category state. DXVK takes the same shape; the
  // alternative; public accessors per category; would pollute the
  // device's external surface for an internal concern.
  friend class MTLD3D9StateBlock;
  // A DIRECT-mode dynamic VB / IB reads m_currentCmdSeq / m_cachedSignaled
  // for its DynamicBuffer DISCARD recycle, and a plain in-place Lock may
  // call m_completionEvent.waitUntilSignaledValue to stall the calling
  // thread until the current allocation's last GPU read has retired (the
  // write-after-read gate). See MTLD3D9VertexBuffer::Lock.
  friend class MTLD3D9VertexBuffer;
  friend class MTLD3D9IndexBuffer;

public:
  // Device-level pool of host-placed Metal buffer backings keyed by size.
  // Share allocations across distinct resources, avoiding repeated
  // newBuffer + wsi::aligned_malloc. acquireBufferBacking pops the first
  // exact-size match; releaseBufferBacking pushes with capped pool size.
  // Purely a MANAGED texture-mirror recycling cache: the buffer path
  // (vertex / index) now routes through dxmt::DynamicBuffer and never
  // touches this pool. The clients are the buffer-backed MANAGED texture
  // create path and the 2D / cube MANAGED-mirror eviction (d3d9_texture.cpp,
  // d3d9_cube_texture.cpp); the pool mutex guards their queue-thread
  // donations (56463fba).
  bool acquireBufferBacking(
      size_t size, WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned
  );
  void releaseBufferBacking(WMT::Reference<WMT::Buffer> &&buffer, void *owned, uint64_t gpu_address, size_t capacity);

private:
  const bool m_isEx;
  // Serializes the public API for D3DCREATE_MULTITHREADED devices; a
  // no-op otherwise. Declared before every member an entry point can
  // touch and initialized right after m_isEx so it guards from the
  // first call on. See d3d9_multithread.hpp for the contract.
  D9Multithread m_multithread;

public:
  // Taken at the top of every public entry point (device and resource
  // methods alike; resources route through their owning device).
  // Recursive: entry points nest through Reset and the state setters.
  D9DeviceLock
  LockDevice() {
    return m_multithread.AcquireLock();
  }

  // DXMT_D9_STALL_MS per-frame decomposition, reported from the
  // swapchain Present tail: the funnel both a device Present and a
  // direct GetSwapChain(0)->Present reach.
  void reportPresentStall(std::chrono::steady_clock::time_point present_enter);

private:
  // Cursor visibility: ShowCursor returns the previous visibility per
  // the wined3d_device_show_cursor contract (wined3d device.c).
  // Default FALSE matches the post-SetCursorProperties initial state
  // before the app calls ShowCursor(TRUE); i.e. no cursor visible
  // until the app explicitly turns it on. Visibility only latches once
  // a cursor image has been set, also per wined3d.
  BOOL m_cursorVisible = FALSE;
  bool m_cursorImageSet = false;
#ifdef _WIN32
  // Win32 cursor realised from a SetCursorProperties bitmap: 32x32
  // always (wined3d_device_set_cursor_properties), any size clamped to
  // 32x32 when windowed (DXVK). wined3d covers the remaining sizes
  // with a present-time software blit dxmt does not implement; those
  // keep whatever cursor is current.
  HCURSOR m_hwCursor = nullptr;
#endif
  // D3D9 device-state machine: Ok → S_OK, Lost → D3DERR_DEVICELOST
  // (unreachable today), NotReset → D3DERR_DEVICENOTRESET. Ex devices
  // return S_OK; use CheckDeviceState instead.
  enum class DeviceState : uint8_t { Ok, Lost, NotReset };
  std::atomic<DeviceState> m_deviceState{DeviceState::Ok};
  // Live count of app-created D3DPOOL_DEFAULT resources. Bumped by
  // every public Create* that returns a DEFAULT-pool resource; the
  // resource's leaf dtor decrements (each leaf carries an m_isLosable
  // flag set on the Create* path; the implicit RT0 / auto-DS skip this
  // create-time count but still gate Reset via m_isImplicitLosable while the
  // app holds a public ref). Reset reads this and
  // returns D3DERR_INVALIDCALL if non-zero. wined3d / DXVK enforce the
  // same contract via wined3d_resource_release counts and DXVK's
  // m_losableResourceCounter respectively.
  std::atomic<uint32_t> m_losableResourceCount{0};
  // Bytes of device-local allocation reported through GetAvailableTextureMem;
  // tracked alongside the losable count (same DEFAULT-pool lifecycle).
  std::atomic<int64_t> m_reportedTextureMemory{0};
  // Fullscreen activation latch behind fullscreenOwnsDisplay(); the
  // sample seeds at construction and Reset so a stale creation-time
  // foreground never reads as occlusion. WM_ACTIVATEAPP sets these from the
  // focus window's thread and takes over entirely once the focus proc is
  // installed; the foreground poll covers only the window before that. Atomic
  // because the two are not the same thread.
  std::atomic<HWND> m_lastForegroundSample{nullptr};
  std::atomic<bool> m_fullscreenOccluded{false};
  // Pooled buffer backings; see acquireBufferBacking comment above.
  // Vector is fine: typical workloads keep the pool small (a handful
  // of distinct sizes); a hash-map keyed by size would add allocator
  // overhead for negligible search gain.
  struct BufferBackingPoolEntry {
    WMT::Reference<WMT::Buffer> buffer;
    void *owned_backing;
    uint64_t gpu_address;
    size_t capacity;
  };
  // AMD FETCH4 latch, one bit per sampler slot 0..15: set by the
  // D3DSAMP_MIPMAPLODBIAS magic 'GET4', cleared by 'GET1'; other bias
  // writes leave it alone (DXVK d3d9_device.cpp keeps the same sticky
  // shape). A raw state-block Apply bypasses SetSamplerState, so the latch is
  // re-derived from the restored LOD-bias in d3d9_state_block.cpp Apply (DXVK
  // handles it there too, its Apply routes through SetStateSamplerState).
  uint16_t m_fetch4Latch = 0;
  // Generated fixed-function shader cache: vertex functions keyed by
  // the IA-layout fingerprint (plus the has-diffuse bit), one pixel
  // function until the stage-combiner milestones add axes. The value is
  // the async compile task; a task whose function() latches null caches a
  // compile failure the same way the variant caches do. One of the
  // task-owning containers m_psoScheduler must outlive at teardown (see its
  // declaration for the ordering invariant).
  std::unordered_map<uint64_t, std::unique_ptr<D3D9CompiledFunction>> m_ffpVSCache;
  std::unordered_map<uint64_t, std::unique_ptr<D3D9CompiledFunction>> m_ffpPSCache;
  // Function tasks that lost a 64-bit key collision in m_ffpVSCache /
  // m_ffpPSCache. The maps hold one entry per key, so on the astronomically
  // rare verified collision the fresh task is pinned here for device lifetime
  // (the PSO task holds a non-owning pointer to it) instead of being cached.
  // Like the caches above, one of the task-owning containers m_psoScheduler
  // must outlive at teardown.
  std::vector<std::unique_ptr<D3D9CompiledFunction>> m_functionKeyCollisions;

  // CPU-mirror storage for lockable resources: pagefile-backed file
  // mappings on a 32-bit guest so idle mirrors release their address
  // range, plain allocations on 64-bit (see d3d9_mem.hpp). Consumers
  // bracket access with the owners' map / unmap helpers.
  D3D9MemoryAllocator m_mirrorAllocator;

public:
  D3D9MemoryAllocator &
  mirrorAllocator() {
    return m_mirrorAllocator;
  }

  // Queue an async work item (a shader-module variant task) onto the
  // shared PSO scheduler. The variant caches live on the shader modules,
  // so they submit through the device that owns the scheduler.
  void submitAsyncTask(D3D9AsyncTask *task);

private:
  D3D9CompiledFunction *ffpVertexFunction(
      const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout, bool has_diffuse, bool has_texcoord0, bool has_specular,
      uint32_t fog_vertex_mode, bool range_fog, bool point_size, bool point_scale, uint32_t lighting_key,
      uint32_t texcoord_mask, uint32_t texcoord_transform_key, uint32_t vertex_blend, uint32_t texgen_key,
      uint32_t texcoord_index_key, bool point_size_per_vertex, bool decl_has_diffuse
  );
  D3D9CompiledFunction *ffpPixelFunction(
      const uint32_t (*stages)[3], bool specular_enable, bool point_sprite, int fog_mode, bool fog_coord_w,
      uint32_t alpha_func, uint32_t sampler_kind_key, bool flat_shading, bool emit_sample_mask,
      uint32_t unorm_snap_mask
  );
  // Set at destructor entry; flips releaseBufferBacking to direct-free so
  // late donors (members destructing after the pool) never touch it.
  bool m_tearingDown = false;
  std::vector<BufferBackingPoolEntry> m_bufferBackingPool;
  // Running sum of m_bufferBackingPool entry capacities. The count cap alone
  // does not bound address space: MANAGED mirror eviction routes full-texture
  // backings here, so 128 of them would re-hoard the very <4 GB space the
  // eviction freed. Past kMaxBufferBackingPoolBytes a release frees the
  // backing to the OS instead of pooling it (small VB/IB churn still pools
  // freely below the cap).
  size_t m_bufferBackingPoolBytes = 0;
  // High-water of m_bufferBackingPoolBytes, for the device-teardown
  // address-space diagnostic (d9DebugEnabled). Updated under the pool mutex
  // whenever the running sum grows.
  size_t m_bufferBackingPoolPeak = 0;
  // Cap so a pathological create/destroy churn doesn't accumulate
  // unbounded VRAM. 128 entries × typical D3D9 buffer/texture sizes
  // caps worst-case pool memory in the low-hundreds-of-MB range on
  // Apple Silicon. Sized for level-transition bursts that create 100+
  // textures in one frame; at 64 the pool evicted entries before the
  // next level's CreateTexture could reuse them, paying the cold-
  // allocate cliff on every level boundary.
  static constexpr size_t kMaxBufferBackingPoolSize = 128;
  // Byte budget for the pool (see m_bufferBackingPoolBytes). 64 MB absorbs a
  // level-transition VB/IB burst while keeping the pool from re-hoarding
  // evicted MANAGED mirrors in the scarce 32-bit address space.
  static constexpr size_t kMaxBufferBackingPoolBytes = 64ull << 20;
  // True between BeginStateBlock and EndStateBlock. While set, every
  // Set* path validates as usual, then writes its value into
  // m_recordingBlock's snapshot storage + changed mask and returns
  // WITHOUT touching live device state: wine d3d9 repoints
  // device->update_state at the recording stateblock so recorded sets
  // never reach the live state; DXVK returns early through
  // m_recorder->Set*. Get* keeps reading live state, which recording
  // leaves unchanged (both references agree).
  bool m_inStateBlockRecord = false;
  // The in-progress recording block created by BeginStateBlock and
  // handed out by EndStateBlock. Raw pointer: the block's ctor
  // self-pin keeps it alive; Reset (recording aborted, wine resets
  // device->recording in its reset path) and the device dtor drop
  // that pin for a block that never gained a public ref.
  class MTLD3D9StateBlock *m_recordingBlock = nullptr;
  // FVF dword from SetFVF. SetFVF synthesizes decl via
  // build_fvf_decl_elements and rebinds m_vertexDeclaration; cache keyed
  // by FVF dword. Captured by StateBlock::Capture for Apply round-trip.
  DWORD m_fvf = 0;
  std::unordered_map<DWORD, Com<class MTLD3D9VertexDeclaration, false>> m_fvfDeclCache;
  // Cache lookup-or-build for the FVF-synthesized declaration. Shared
  // by SetFVF's live and recording arms; never returns null for a
  // non-zero FVF.
  class MTLD3D9VertexDeclaration *getOrCreateFvfDecl(DWORD FVF);
  D3DDEVICE_CREATION_PARAMETERS m_creationParams;
  D3DPRESENT_PARAMETERS m_presentParams;
  // Fullscreen device-window management (wined3d swapchain.c
  // setup_fullscreen / restore_from_fullscreen). When the app requests
  // Windowed=FALSE, resize + restyle the device window to a borderless
  // fullscreen rect and restore it on the windowed transition; without
  // this a fullscreen game keeps its small windowed window. This is pure
  // window geometry: the display-mode switch is a winemac concern (and a
  // known hang), never touched here. m_fullscreenWindow is the window we
  // currently hold fullscreen (null = none); the saved style/exstyle/rect
  // are its pre-fullscreen state for restore.
  HWND m_fullscreenWindow = nullptr;
  LONG m_savedWindowStyle = 0;
  LONG m_savedWindowExStyle = 0;
  RECT m_savedWindowRect = {};
  // Output the device window went fullscreen on, kept because the focus-gain
  // reposition runs while that window is minimized. Written on the device
  // thread at fullscreen entry, read on the focus-window thread, so atomic for
  // the same reason as the activation latch above.
  std::atomic<HMONITOR> m_fullscreenMonitor{nullptr};
  // Drive the device window into / out of the borderless fullscreen state.
  // enterFullscreenWindow saves + restyles on first entry and repositions
  // on a later resolution change; both honor D3DCREATE_NOWINDOWCHANGES.
  void enterFullscreenWindow(HWND window, UINT width, UINT height);
  void leaveFullscreenWindow();
  // While fullscreen, the D3D9 runtime subclasses the FOCUS window's wndproc
  // (never the device window) so it can observe focus/display messages; the
  // wine d3d9 tests assert GetWindowLongPtr(focus, GWLP_WNDPROC) changes on the
  // fullscreen transition and is restored when windowed again. The hook is
  // installed unconditionally on the fullscreen transition (wined3d gates only
  // the window restyle on D3DCREATE_NOWINDOWCHANGES, not the focus-proc hook);
  // the focus window defaults to the device window when the app passed none.
  // Idempotent. m_focusWindow is the hooked window, m_focusProcUnicode the slot
  // we hooked.
  void hookFocusWindowProc(HWND fallbackWindow);
  void unhookFocusWindowProc();
  HWND m_focusWindow = nullptr;
  bool m_focusProcHooked = false;
  bool m_focusProcUnicode = false;
  // Suppresses delivery of dxmt's own window traffic to the application, the
  // way wined3d filters messages around every self-initiated window change.
  // Only observable when the device window IS the focus window, since that is
  // the only window subclassed. WM_DISPLAYCHANGE is deliberately exempt.
  // Same-thread reentrancy only: the messages it guards are generated by our
  // own calls from inside the focus proc.
  bool m_focusMessagesFiltered = false;
  // Device9Ex frame-latency bookkeeping. SetMaximumFrameLatency
  // rejects >30 with INVALIDCALL; 0 selects the default 3 (DXVK
  // d3d9_device.cpp). Read-only round-trip; Metal
  // doesn't expose the equivalent waitable knob, but apps that poll
  // GetMaximumFrameLatency expect their last Set value back.
  UINT m_frameLatency = 3;
  WMT::Reference<WMT::Device> m_metalDevice;
  // COW snapshot cache for BatchedDraw::pod_snapshot. m_encShadowDirty
  // bitmask; setters OR category on value-change. Fresh snapshot copies
  // only dirty axes; consecutive draws with no setters share one snapshot.
  // Storage lives in the queue's command-data ring, tagged with the
  // recording chunk and recycled wholesale at completion, so snapshots
  // never touch the process heap. m_encShadowLastSnapChunk records the
  // owning chunk: a snapshot is reusable only within it, because the
  // previous chunk's ring block may already be recycled by the time the
  // next chunk records (QueueBatchedDraw rebuilds from the device shadows
  // on a chunk change).
  uint32_t m_encShadowDirty = dxmt::D9ES_DIRTY_ALL;
  const dxmt::D9EncodingState *m_encShadowLastSnap = nullptr;
  uint64_t m_encShadowLastSnapChunk = ~0ull;
  // AUTOGENMIPMAP pre-draw sweep gate. markAutogenMipsDirty bumps the epoch on
  // a mark-dirty or a dirty-texture bind; QueueBatchedDraw sweeps the bound
  // textures whenever the epoch has moved past m_autogenSweptEpoch, keeping the
  // certified-lean draw path to one atomic load per draw when nothing changed.
  // Atomic only for defensiveness: both sides run under the device lock.
  std::atomic<uint32_t> m_autogenDirtyEpoch{0};
  uint32_t m_autogenSweptEpoch = 0;
  // Deferred MANAGED upload pre-draw sweep gate, the same shape as the autogen
  // epoch above. markManagedUploadPending bumps it when a MANAGED texture gains
  // pending-upload levels while it could be sampled (bind of a pending texture,
  // EvictManagedResources); QueueBatchedDraw sweeps the bound textures once the
  // epoch moves, keeping the lean draw path to one relaxed load when idle.
  std::atomic<uint32_t> m_managedUploadEpoch{0};
  uint32_t m_managedUploadSweptEpoch = 0;
  // dxmt::CommandQueue; spins up encode/commit/event-listener worker
  // threads. Owns WMT::CommandQueue, InternalCommandLibrary, staging/
  // argbuf allocators. Sequenced after m_metalDevice; unique_ptr for
  // forward-declare to minimize include surface.
  std::unique_ptr<dxmt::CommandQueue> m_dxmtQueue;
  // Compiled lazily in the ctor init list; its ctor newLibrary's the
  // embedded MSL. Declared after m_metalDevice so its initializer
  // observes a constructed device. Outlives every consumer the device
  // hands it to (sampler ctxs, the Presenter on the implicit chain).
  InternalCommandLibrary m_internalCmdLib;
  // Scissored clear quad for Clear's partial regions; see
  // d3d9_clear_quad.hpp for why it is d3d9-private. Declared after
  // m_internalCmdLib: its ctor pulls shader functions from it. PSO /
  // begin-end state is touched only from chunk lambdas on the encode
  // thread, which the device outlives (the dtor drains the queue).
  D3D9ClearQuad m_clearQuad;
  // Per-draw constant + vbuf-table upload ring: placed_buffer=true
  // (host malloc). CommandQueue staging_allocator is placed_buffer=false
  // (Metal-allocated, unsafe to memcpy from i386 calling thread).
  // GPU lifetime via m_completionEvent signal; rings written only by
  // calling-thread paths holding constRing mutex.
  WMT::Reference<WMT::SharedEvent> m_completionEvent;
  uint64_t m_currentCmdSeq = 1;
  // Cached last-signaled cmdbuf seq. Refreshed after commit; read by
  // upload paths to avoid per-call signaledValue() wine_unix_call.
  // Stale reads are conservative; max delay is one cmdbuf's worth.
  std::atomic<uint64_t> m_cachedSignaled{0};
  // Throttle for refreshSignaledAndTrimRings; last m_currentCmdSeq
  // value at which the ring-trim ran. Per-chunk refresh was previously
  // unconditional and the wine_unix_call cost dominated the blit-heavy
  // blit-heavy paths (thousands of chunks in one frame). Refresh at
  // most every kRingRefreshGap chunks; staleness delays ring-block recycle
  // by at most kRingRefreshGap-1 chunks, never breaks correctness.
  uint64_t m_lastRingRefreshSeq = 0;
  RingBumpState<StagingBufferBlockAllocator> m_constRing;
  RingBumpState<StagingBufferBlockAllocator> m_uploadRing;
  // The encode thread's ResolveBatchedDrawForChunk allocations get their
  // own const ring so every ring has exactly one writer thread, which is
  // what lets seal_latest() rotate a ring safely: m_constRing / m_uploadRing
  // are written only by the calling thread, this one only by the encode
  // thread. All three seal once per command buffer (at commit), so a block
  // is never written by a command buffer other than the one that pinned it.
  RingBumpState<StagingBufferBlockAllocator> m_constRingResolve;
  // Persistent fan-list IB: static-fan draws (DrawPrimitive +
  // DrawPrimitiveUP with PrimitiveType==TRIANGLEFAN) all need the same
  // [0,1,2, 0,2,3, 0,3,4, ...] pattern, which only depends on
  // PrimitiveCount. Build once at first use, reuse forever; the bound
  // count covers UI-heavy frames. Above-cap fan draws fall back to the
  // per-call m_constRing path.
  WMT::Reference<WMT::Buffer> m_fanListIB;
  void *m_fanListIBBacking = nullptr;
  // Raw pointer + private refcount; see d3d9_swapchain.hpp's lifetime
  // contract. The destructor (in the .cpp) tears the chain down by hand
  // BEFORE the implicit member-destruction order kicks in, so the chain
  // can release any Metal handles while m_commandQueue and m_metalDevice
  // are still alive. Reordering these declarations breaks that contract.
  class MTLD3D9SwapChain *m_implicitSwapChain = nullptr;
  // Bound render target slots. D3D_MAX_SIMULTANEOUS_RENDERTARGETS = 4.
  // Stored as private refs (Com<,false>); surfaces SHOULD NOT cycle
  // through the device's public refcount when bound. Slot 0 is bound
  // to the implicit backbuffer at ctor/Reset; SetRenderTarget(0, NULL)
  // is rejected per wined3d's contract.
  Com<class MTLD3D9Surface, false> m_renderTargets[D3D_MAX_SIMULTANEOUS_RENDERTARGETS];
  // Bound depth-stencil surface. NULL is allowed (depth-disabled
  // rendering). Same private-ref pinning shape as the RT array.
  Com<class MTLD3D9Surface, false> m_depthStencilSurface;
  // Implicit auto-DS surface kept alive separately from
  // m_depthStencilSurface so apps that called SetDepthStencilSurface
  // to a custom DS (or NULL) don't drop the auto-DS, and apps that
  // held GetDepthStencilSurface across Reset get the same surface
  // object back (its Metal texture swapped in place by
  // createAutoDepthStencil, mirroring the swapchain backbuffer's
  // identity-preserving Reset). NULL when EnableAutoDepthStencil
  // is FALSE on the active D3DPRESENT_PARAMETERS.
  Com<class MTLD3D9Surface, false> m_autoDepthStencilSurface;
  // Bound textures: 0..15 PS, 16..19 VS samplers. D3DDMAPSAMPLER (256)
  // silently ignored. Stored as private refs; SetTexture doesn't cycle
  // public refcount. Slot type is common base for 2D/Cube/Volume.
  Com<class MTLD3D9CommonTexture, false> m_textures[D3D9_MAX_TEXTURE_UNITS];
  // Sampler state: wined3d's shape (combined PS+VS samplers, indexed
  // by D3DSAMPLERSTATETYPE 1..D3DSAMP_DMAPOFFSET). Slot 0 is unused
  // (D3D9 has no D3DSAMP_0). Defaults are seeded in the ctor; the
  // values feed the Metal sampler descriptor via
  // sampler_info_from_d3d9_state.
  DWORD m_samplerStates[D3D9_MAX_TEXTURE_UNITS][D3DSAMP_DMAPOFFSET + 1] = {};
  // FFP texture-stage state: wined3d's shape, indexed by D3DTSS_* up
  // to D3DTSS_CONSTANT (32). 8 stages matches D3DCAPS9::MaxTextureBlendStages
  // and wined3d MAX_TEXTURES. Programmable-PS games still issue
  // SetTextureStageState; storing the value silently is the right
  // shape; the FFP shader generator reads these when it lands, draw
  // paths that bind a real PS ignore them.
  DWORD m_textureStageStates[8][D3DTSS_CONSTANT + 1] = {};
  // D3D9 render state. The D3DRS_* enum runs up to 209
  // (D3DRS_BLENDOPALPHA). Sized [256] (matches DXVK) so a Set in the
  // [0, 7..255] live-storage range can index directly. Zero-init;
  // initDefaultRenderStates only seeds the ~85 slots that have a
  // D3DRS_ name; the remaining live indices (gaps in the enum) need
  // a defined zero value rather than uninitialized memory in case
  // an app stores into one and reads it back later.
  DWORD m_renderStates[256] = {};
  // User clip planes (SetClipPlane). 4 floats per plane, sized to
  // MaxUserClipPlanes from GetDeviceCaps (8). VS path reads these
  // when D3DRS_CLIPPLANEENABLE bit i is set, computing the dot
  // product against the post-transform position. wined3d tracks
  // these on the device (clip_planes[]); DXVK as m_state.clipPlanes
  // in d3d9_state.h.
  float m_clipPlanes[8][4] = {};
  // Bound vertex streams (SetStreamSource). Priv-pinned via
  // Com<,false>; offset/stride sit beside the buffer ref. Sized
  // D3D9_MAX_VERTEX_STREAMS == 16. wined3d also tracks per-stream
  // frequency/divider for instancing; m_streamFreq below mirrors it.
  Com<class MTLD3D9VertexBuffer, false> m_vertexBuffers[D3D9_MAX_VERTEX_STREAMS];
  UINT m_streamOffsets[D3D9_MAX_VERTEX_STREAMS] = {};
  UINT m_streamStrides[D3D9_MAX_VERTEX_STREAMS] = {};
  // SetStreamSourceFreq / GetStreamSourceFreq packed setting per
  // stream. Default 1 = "advance once per vertex, draw 1 instance".
  // Encoding follows the D3D9 spec (matches DXVK m_state.streamFreq):
  //   Stream 0 ⇒  D3DSTREAMSOURCE_INDEXEDDATA | InstanceCount
  //   Stream N ⇒  D3DSTREAMSOURCE_INSTANCEDATA | DivisorOrZero
  // The draw path inspects bit 30 (INDEXEDDATA) on stream 0 to pull
  // instance_count out of the low 23 bits, and bit 31 (INSTANCEDATA)
  // on streams 1..15 to mark the IA element as per-instance.
  UINT m_streamFreq[D3D9_MAX_VERTEX_STREAMS] = {};
  // Bound index buffer (SetIndices). Single slot.
  Com<class MTLD3D9IndexBuffer, false> m_indexBuffer;
  // Bound vertex declaration. Same priv-pin shape as the texture /
  // RT slots; Get/Release cycles must not leave a dangling slot ref.
  Com<class MTLD3D9VertexDeclaration, false> m_vertexDeclaration;
  // Bound vertex / pixel shaders. NULL is allowed (apps unbind to
  // switch to FFP). Same priv-pin shape as the other slot bindings.
  Com<class MTLD3D9VertexShader, false> m_vertexShader;
  Com<class MTLD3D9PixelShader, false> m_pixelShader;

  // VS constant register file. The hot array stays the hardware-VP size;
  // Default-zero initialized; D3D9's read-back contract is that an unset
  // constant reads as 0 / FALSE, see DXVK ResetState.
  float m_vsConstantsF[D3D9_MAX_VS_CONST_F][4] = {};
  int m_vsConstantsI[D3D9_MAX_VS_CONST_I][4] = {};
  BOOL m_vsConstantsB[D3D9_MAX_VS_CONST_B] = {};
  // Extended float file c256..c8191 for a software/mixed VP device, holding
  // (m_vsConstFCount - 256) registers of 4 floats; null on a hardware-VP device,
  // which caps at 256. Backs Set/GetVertexShaderConstantF storage for the extended
  // range; the draw shadow still uploads only the hot 256 file (extended-range
  // reads on draw are a separate, deferred change). m_vsConstFCount is the device's
  // advertised register count (256 or 8192), the SetVertexShaderConstantF bound.
  std::unique_ptr<float[]> m_vsConstantsFOverflow;
  UINT m_vsConstFCount = D3D9_MAX_VS_CONST_F;

  // PS constant register file. SM2 apps only touch [0..31] but the
  // storage is sized to SM3's 224; see D3D9_MAX_PS_CONST_F above.
  float m_psConstantsF[D3D9_MAX_PS_CONST_F][4] = {};
  int m_psConstantsI[D3D9_MAX_PS_CONST_I][4] = {};
  BOOL m_psConstantsB[D3D9_MAX_PS_CONST_B] = {};

  // High-water mark of SetVertexShaderConstantF/SetPixelShaderConstantF
  // coverage. Encode-side memcpy clamps to (max * 16) bytes instead of
  // full register file (~16× reduction). Typical shaders set ~30 VS / ~20 PS.
  uint16_t m_vsConstFMax = 0;
  uint16_t m_psConstFMax = 0;

  // Viewport / scissor. Seeded in the ctor and reseeded on
  // SetRenderTarget(0, …) per D3D9 spec.
  D3DVIEWPORT9 m_viewport = {};
  RECT m_scissorRect = {};

  // Begin/EndScene pair-bracket flag. Tracked here for misnested-call
  // rejection only; the eventual flush hint at EndScene will hang off
  // the same flag.
  bool m_inScene = false;

  // D3D9 ClipStatus: vestigial occlusion-test bookkeeping from FFP.
  // Set/Get round-trip the struct; nothing else consumes it. wined3d
  // stubs wined3d_device_set_clip_status / get_clip_status (FIXME: store
  // nothing, return OK); dxmt stores it like DXVK so a read-back returns
  // what was set, and apps that don't hr-check never trip E_NOTIMPL.
  // {ClipUnion, ClipIntersection}: native and DXVK default the
  // intersection to all-ones (no plane has yet rejected every vertex).
  D3DCLIPSTATUS9 m_clipStatus = {0, 0xffffffff};

  // Software-vertex-processing mode (Set/GetSoftwareVertexProcessing). Pure
  // state echo; Metal is always hardware-VP. Seeded in the ctor from the
  // behaviour flags (TRUE on a pure-SWVP device, DXVK m_isSWVP), toggled by the
  // two legal transitions Set allows.
  bool m_isSWVP = false;

  // One-shot latch for the software-VP draw gate. A vertex shader that
  // references the extended constant file (c256..) cannot run in hardware
  // vertex processing: native rejects the FIRST such draw with
  // D3DERR_INVALIDCALL and renders nothing, then falls back to fixed-function
  // vertex processing (the vertex data's own colour) for every draw after.
  // Latched true on that first rejection; reset when the bound shader or the
  // software/hardware mode changes.
  bool m_swvpDrawRejected = false;

  // N-patch tessellation segment count (Set/GetNPatchMode). Stored for
  // round-trip only; N-patch tessellation is not advertised (DXVK stores
  // m_state.nPatchSegments the same way, wined3d set/get_npatch_mode).
  float m_nPatchMode = 0.0f;

  // Transform state: view + projection + 8 texture stages + 256 world
  // matrices (DXVK d3d9_caps::MaxTransforms = 266). Compaction matches
  // DXVK GetTransformIndex (d3d9_util.h). Default identity; FFP
  // pipelines and apps that GetTransform without prior Set rely on it.
  static constexpr uint32_t kMaxTransforms = 10 + 256;
  D3DMATRIX m_transforms[kMaxTransforms];
  // Cached world*view*projection product for the generated
  // fixed-function vertex shader; recomputed at snapshot capture when
  // a SetTransform (or a state-block Apply of transforms) staled it.
  float m_ffpWVP[16] = {};
  // Inverse of view*projection, recomputed alongside m_ffpWVP. The generated
  // fixed-function vertex shader dots each clip plane against the clip-space
  // position, so the host pre-transforms world-space FFP clip planes by this
  // inverse (row-vector convention: plane' = (VP)^-1 * plane) to make the
  // clip-space dot equal the world-space dot native/DXVK compute.
  float m_ffpVPInv[16] = {};
  // World matrices 1..3 folded with view*projection for
  // D3DRS_VERTEXBLEND; index i holds WORLDMATRIX(i + 1).
  float m_ffpWVPBlend[3][16] = {};
  float m_ffpWVZ[4] = {};
  float m_ffpWVX[4] = {};
  float m_ffpWVY[4] = {};
  // Per-matrix world*view columns (index b = WORLDMATRIX(b + 1)) for the
  // D3DRS_VERTEXBLEND eye-space blend, and the inverse-transpose of the
  // matrix-0 world*view (x/y/z rows of inverse(WV)) for the eye normal.
  // Recomputed alongside m_ffpWVP.
  float m_ffpWVBlend[3][12] = {};
  float m_ffpNormal[3][4] = {};
  bool m_ffpFogCoordW = false;
  bool m_ffpWVPStale = true;

  // FFP material, default-constructed all-zero: wined3d
  // (stateblock_state_init_default leaves it zero), DXVK and d9vk all default
  // D3DMATERIAL9 to {}. So a GetMaterial before any SetMaterial reads back all
  // zero (Diffuse included), and an app that enables lighting without a
  // SetMaterial renders black, the documented D3D9 behavior.
  D3DMATERIAL9 m_material = {};

  // FFP lights: sparsely indexed by app-supplied DWORD. wined3d
  // (state.c) and DXVK (d3d9_state.h) both grow a flat vector;
  // indices are typically small (0..8) so contiguous storage wins.
  // m_lightEnables runs in parallel, holding the LightEnable flag for
  // each index. SetLight grows the vectors; LightEnable on an unset index
  // implicitly creates a default directional light there (wined3d and DXVK do
  // the same). GetLight / GetLightEnable are the calls that return INVALIDCALL
  // on a never-set index.
  std::vector<D3DLIGHT9> m_lights;
  std::vector<BOOL> m_lightEnables;

  // Texture palettes: sparsely indexed by app-supplied PaletteNumber.
  // Each palette is 256 PALETTEENTRY values (D3D9 spec: paletted
  // textures use D3DFMT_P8 / D3DFMT_A8P8 indices into a 256-entry
  // A8R8G8B8 palette). Storage-only today: no FFP P8 sampler exists,
  // so SetCurrentTexturePalette doesn't yet influence sampling. Spec
  // requires Set/Get to be a faithful round-trip; STUB_HR was
  // breaking apps that hr-check init. DXVK d3d9_device.cpp
  // is the literal model.
  std::unordered_map<UINT, std::array<PALETTEENTRY, 256>> m_texturePalettes;
  UINT m_currentTexturePalette = 0;

  // PSO cache for unique (vs, ps, RT format, blend, depth/stencil)
  // tuples. Without cache, every draw invokes newRenderPipelineState,
  // saturating Apple's XPC compiler. Async build via m_psoScheduler
  // (declared below, after every task-owning container); first draw
  // blocks at Wait(), subsequent hits cached task.
  std::unordered_map<uint64_t, std::unique_ptr<D3D9PsoCompileTask>> m_psoCache;
  // Tasks that lost a 64-bit key collision in m_psoCache. The map holds one
  // entry per hash and the loser can't overwrite the winner (an in-flight
  // chunk may hold a non-owning pointer to it), so on the astronomically rare
  // verified collision the fresh task is pinned here for device lifetime
  // instead. Declared alongside m_psoCache so the scheduler (declared after
  // both) still tears down first, joining workers before any task frees.
  std::vector<std::unique_ptr<D3D9PsoCompileTask>> m_psoCacheCollisions;

  // Compiled-shader module dedup, keyed by bytecode hash. DXVK's
  // D3D9ShaderModuleSet (src/d3d9/d3d9_shader.h): apps that recreate the
  // same shader (streaming worlds tear down and re-mint identical
  // bytecode) would otherwise get a fresh wrapper + variant cache +
  // distinct MTLFunction every CreateVertexShader / CreatePixelShader,
  // and since the PSO key embeds the variant-task pointer, identical draws
  // spawn distinct PSOs that pin for device lifetime. The map owns each
  // module's Rc so it survives wrapper release and Reset (modules are
  // bytecode-derived, valid across Reset like the PSO cache they feed),
  // cleared only at device destruction by member teardown. Mutex-guarded
  // because shader creation is cross-thread for some apps, mirroring
  // D3D9ShaderModuleSet's lock. The hash is 64-bit so the accessors
  // memcmp the bytecode on a hit before aliasing.
  dxmt::mutex m_shaderModuleMutex;
  std::unordered_map<uint64_t, Rc<class MTLD3D9VertexShaderModule>> m_vsShaderModules;
  std::unordered_map<uint64_t, Rc<class MTLD3D9PixelShaderModule>> m_psShaderModules;

  // Owns the worker threads that run both the PSO-link tasks and the
  // function-compile tasks. Declared after EVERY container that owns a task a
  // worker may touch: the FFP function caches, m_psoCache, and the shader
  // module maps above (each module's per-variant cache owns function tasks).
  // Reverse-order member teardown therefore destroys the scheduler first, so
  // its destructor joins the workers before any task they might still be
  // running is freed. Keep this the last such member.
  task_scheduler<D3D9AsyncTask *> m_psoScheduler;
  // Lookup-or-compile. On miss, build the module and insert with the
  // DXVK double-check; on hit, reuse after confirming the bytecode.
  Rc<class MTLD3D9VertexShaderModule>
  getOrCreateVertexShaderModule(const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata);
  Rc<class MTLD3D9PixelShaderModule>
  getOrCreatePixelShaderModule(const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata);

  // Sampler-state cache avoids millions of redundant MTLDevice
  // newSamplerState calls across long runs of similar state (matches
  // wined3d shape). Key is 24-byte WMTSamplerInfo prefix; insertions
  // stable for device lifetime.
  struct SamplerKey {
    WMTSamplerMinMagFilter min_filter;
    WMTSamplerMinMagFilter mag_filter;
    WMTSamplerMipFilter mip_filter;
    WMTSamplerAddressMode r_address_mode;
    WMTSamplerAddressMode s_address_mode;
    WMTSamplerAddressMode t_address_mode;
    WMTSamplerBorderColor border_color;
    WMTCompareFunction compare_function;
    float lod_min_clamp;
    float lod_max_clamp;
    uint32_t max_anisotroy;
    bool normalized_coords;
    bool lod_average;
    bool support_argument_buffers;
    bool operator==(const SamplerKey &) const = default;
  };
  struct SamplerKeyHash {
    size_t
    operator()(const SamplerKey &k) const noexcept {
      // FNV-1a over the byte image; trivially-copyable so memcpy is
      // safe; no padding bytes leak because every field aligns at its
      // natural boundary and the largest is 4 bytes.
      static_assert(std::is_trivially_copyable_v<SamplerKey>);
      const uint8_t *p = reinterpret_cast<const uint8_t *>(&k);
      uint64_t h = 1469598103934665603ull;
      for (size_t i = 0; i < sizeof(SamplerKey); ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
      }
      return static_cast<size_t>(h);
    }
  };
  std::unordered_map<SamplerKey, Rc<Sampler>, SamplerKeyHash> m_samplerCache;

  // Build a SamplerKey from the Metal-side info struct so the lookup
  // path can populate it identically to the build-time path.
  static SamplerKey
  samplerKeyFromInfo(const WMTSamplerInfo &info) {
    return SamplerKey{
        info.min_filter,     info.mag_filter,
        info.mip_filter,     info.r_address_mode,
        info.s_address_mode, info.t_address_mode,
        info.border_color,   info.compare_function,
        info.lod_min_clamp,  info.lod_max_clamp,
        info.max_anisotroy,  info.normalized_coords,
        info.lod_average,    info.support_argument_buffers,
    };
  }
  // Lookup-or-build: returns a cached Sampler reusing the existing
  // dxmt::Sampler::createSampler factory. Null only if Metal rejects
  // the descriptor, which the draw path treats as "skip the bind".
  Rc<Sampler> getOrCreateSampler(const WMTSamplerInfo &info);

  // 1×1 placeholder bound to unbound fragment sampler slot. Metal
  // requires texture+sampler at every index the shader declares;
  // unbound slot faults encoder. Classic trigger: Reset clearing all
  // textures before app re-binds. Lazily created on encode thread.
  // Per-type, indexed 0 = 2D, 1 = 3D, 2 = Cube. The bound dummy's type
  // must match the sampler kind the PS was compiled with, mirroring
  // wined3d's per-type dummy textures; one 2D dummy would mis-bind a 2D
  // texture to a 3D/cube sampler (Metal type mismatch, undefined sample).
  obj_handle_t dummyFragmentTexture(WMTTextureType type);
  Rc<dxmt::Texture> m_dummyFragTexAlloc[3];
  obj_handle_t m_dummyFragTexHandle[3] = {0, 0, 0};

  // DSSO cache. Same shape as the sampler cache above and as d3d11's
  // StateObjectCache<D3D11_DEPTH_STENCIL_DESC, ...>
  // (src/d3d11/d3d11_state_object.cpp). Without it, every draw's
  // depth-stencil bind would call m_metalDevice.newDepthStencilState;
  // millions of redundant cross-WoW64 round-trips per session,
  // which dominates the per-draw cost. WMTDepthStencilInfo is fixed
  // 32 bytes of POD so we key directly on the struct image (memcmp via
  // operator==, FNV-1a hash over the byte image; same as SamplerKeyHash).
  struct DepthStencilKey {
    WMTDepthStencilInfo info;
    bool
    operator==(const DepthStencilKey &o) const noexcept {
      // WMTDepthStencilInfo is a C POD aggregate with no operator==,
      // so default-defaulted compare won't synthesise. Byte-compare
      // is safe; every field aligns at its natural boundary, padding
      // is zero-initialised by the producer (zero-init brace-init in
      // depth_stencil_info_from_d3d9_state).
      static_assert(std::is_trivially_copyable_v<WMTDepthStencilInfo>);
      return std::memcmp(&info, &o.info, sizeof(WMTDepthStencilInfo)) == 0;
    }
  };
  struct DepthStencilKeyHash {
    size_t
    operator()(const DepthStencilKey &k) const noexcept {
      static_assert(std::is_trivially_copyable_v<DepthStencilKey>);
      const uint8_t *p = reinterpret_cast<const uint8_t *>(&k);
      uint64_t h = 1469598103934665603ull;
      for (size_t i = 0; i < sizeof(DepthStencilKey); ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
      }
      return static_cast<size_t>(h);
    }
  };
  std::unordered_map<DepthStencilKey, WMT::Reference<WMT::DepthStencilState>, DepthStencilKeyHash> m_dssoCache;
  // Lookup-or-build. Null only if Metal rejects the descriptor.
  WMT::DepthStencilState getOrCreateDSSO(const WMTDepthStencilInfo &info);

  // Lazy-clear state. D3D9 Clear is eager; Metal shape folds into
  // render pass loadAction. Clear records colour/Z/stencil; drainPendingClear
  // or first draw consumes them. Edge case: if RT changes before draw,
  // SetRenderTarget invokes drainPendingClear on OLD attachment.
  struct PendingClear {
    bool color_valid = false;
    double color[4] = {}; // linear RGBA in [0, 1]
    bool depth_valid = false;
    float depth = 1.0f;
    bool stencil_valid = false;
    uint8_t stencil = 0;
  };
  PendingClear m_pendingClear;

public:
  // Drain any pending Clear onto the current chunk (a clear-only render-pass
  // chunk via chunk->emitcc on the queue's CurrentChunk(); the EncodingThread
  // replays it in emit order). Called from FlushDrawBatch (before the
  // draw-batch emit), EndScene, the destructor, and Present.
  void flushOpenWork();

  // Bump the AUTOGENMIPMAP pre-draw sweep epoch. Called when a texture is
  // marked mips-dirty (flagContainerAutoGenDirty / UpdateTexture) and when a
  // dirty texture is bound, so the next QueueBatchedDraw sees the epoch move
  // and regenerates before the draws that sample the chain.
  void markAutogenMipsDirty() {
    m_autogenDirtyEpoch.fetch_add(1, std::memory_order_relaxed);
  }

  // Bump the deferred-MANAGED-upload sweep epoch. Called when a MANAGED texture
  // with pending upload levels is bound (SetTexture / StateBlock Apply) and from
  // EvictManagedResources, so the next QueueBatchedDraw re-pushes those levels
  // from the sysmem mirror before the draws that sample them.
  void markManagedUploadPending() {
    m_managedUploadEpoch.fetch_add(1, std::memory_order_relaxed);
  }

  // Queue a one-shot op filling mip levels 1..N from level 0 via Metal's
  // generateMipmapsForTexture. drain_pending_draws=false queues the op into
  // the current arrival-order stream without draining (the sweep does this
  // just before pushing a draw, so the mip-gen op precedes it); the true
  // default drains first for a standalone GenerateMipSubLevels call.
  void generateMipmaps(WMT::Texture texture, const Rc<dxmt::Texture> &alloc, bool drain_pending_draws = true);

  // Regenerate the mip chain of every bound AUTOGENMIPMAP texture whose level
  // 0 went dirty, deduped across PS+VTF aliasing. Called from QueueBatchedDraw
  // (gated on the sweep epoch) so the mip-gen op lands in the op stream ahead
  // of the draws that sample the chain, matching DXVK's PrepareDraw.
  void sweepBoundAutogenMips();

  // Push every bound MANAGED texture's pending-upload levels from its sysmem
  // mirror to the Metal texture. Called from QueueBatchedDraw (gated on the
  // managed-upload sweep epoch) so the upload ops land in the op stream ahead of
  // the draws that sample them, the DXVK UploadManagedTextures shape. The
  // per-texture work + mask bookkeeping lives in MTLD3D9Texture::sweepManagedUpload.
  void sweepBoundManagedUploads();

  // Stage texture sub-region upload through m_uploadRing + chunk-emitted
  // blit. Allocates slice, memcpys src, posts wmtcmd_blit_copy_from_buffer
  // signaling m_completionEvent for ring recycling. Caller computes
  // src_pitch and is_compressed (Metal contract). dst_alloc is the
  // destination's dxmt::Texture so the upload blit registers its write for
  // cross-encoder ordering (a same-chunk sampling draw must observe it).
  // src_slice_pitch is the source stride between depth slices for a 3D
  // (volume) upload; 0 = contiguous (2D, or a full-box 3D upload).
  void stageTextureUpload(
      WMT::Texture dst, const Rc<dxmt::Texture> &dst_alloc, uint32_t mip_level, uint32_t slice, WMTOrigin origin,
      WMTSize size, const void *src, uint32_t src_pitch, bool is_compressed, uint32_t src_slice_pitch = 0
  );

  // Stage a BUFFER-mode buffer's dirty range into its Private allocation.
  // Copies src into an m_uploadRing block on the calling thread, then
  // queues a BufferCopy blit op (arrival-order, so it interleaves with
  // draws). The op's access<Compute> write against dst is what orders the
  // copy against draws that register a read on the same allocation; see
  // MTLD3D9VertexBuffer::flushDirty and the op-stream walker.
  void stageBufferUpload(
      const Rc<dxmt::BufferAllocation> &dst_alloc, uint64_t dst_offset, const void *src, uint64_t length
  );

  // Synchronous GPU -> host-mirror readback for a lockable render
  // target's LockRect: drain queued draws, blit the surface's texture
  // into an m_uploadRing block, wait for retirement, memcpy into the
  // surface's mirror. Same drain + wait tail as GetRenderTargetData.
  void readbackSurfaceMirror(class MTLD3D9Surface *surface);

  // Force a staged Clear (m_pendingClear) onto the CURRENT bindings by
  // posting a chunk lambda that opens a clear-only render pass against
  // the bound RT0 + DS, then ends. Called from
  // SetRenderTarget / SetDepthStencilSurface when an attachment is
  // about to change while a clear is staged; without this, the next
  // batched draw's render-pass open would land the clear on the *new*
  // attachments and the old RT/DS would never get the clear the app
  // asked for.
  void drainPendingClear();

  // Emit a Clear whose clipped region set (viewport intersected with scissor and pRects,
  // already clamped against degenerate rects) does NOT cover every
  // bound attachment whole. Regions are clamped per attachment; a
  // region covering one attachment entirely folds into its next
  // render pass loadAction via ctx.clearColor / clearDepthStencil,
  // everything else encodes through ClearRenderTargetContext's
  // scissored quad like d3d11 ClearView. Mirrors DXVK
  // d3d9_device.cpp's ClearImageView full-vs-partial split.
  void emitClippedClear(
      const std::vector<WMTScissorRect> &regions, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil
  );
};

} // namespace dxmt
