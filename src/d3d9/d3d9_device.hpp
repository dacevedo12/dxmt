#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d9_clear_quad.hpp"
#include "d3d9_common_texture.hpp"
#include "d3d9_diag.hpp"
#include "d3d9_enc_state.hpp"
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

// task_trait specialisation for the scheduler. Defined in-class so
// every TU that includes this header gets the same trampoline; methods
// are implicitly inline. The full D3D9PsoCompileTask definition is not
// needed at this point: only the pointer-to-incomplete; the trait
// methods are instantiated from d3d9_device.cpp where the class is
// complete. Same pattern as task_trait<ThreadpoolWork *> in
// src/d3d11/d3d11_pipeline_cache.cpp.
template <> struct task_trait<D3D9PsoCompileTask *> {
  D3D9PsoCompileTask *run_task(D3D9PsoCompileTask *task);
  bool get_done(D3D9PsoCompileTask *task);
  void set_done(D3D9PsoCompileTask *task);
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
// in d3d9_caps.h. SWVP devices expose 8192 / 2048 instead: not
// supported here yet. wined3d's D3D9_MAX_VERTEX_SHADER_CONSTANTF in
// dlls/d3d9/d3d9_private.h matches the 256.
inline constexpr unsigned D3D9_MAX_VS_CONST_F = 256;
inline constexpr unsigned D3D9_MAX_VS_CONST_I = 16;
inline constexpr unsigned D3D9_MAX_VS_CONST_B = 16;

// Pixel-shader constants. F is the SM3 hardware limit (224); SM2 caps
// at 32 but the API bound is the higher SM3 number; the SM2 limit is
// enforced at link time, not at Set*. DXVK MaxSM3FloatConstantsPS.
inline constexpr unsigned D3D9_MAX_PS_CONST_F = 224;
inline constexpr unsigned D3D9_MAX_PS_CONST_I = 16;
inline constexpr unsigned D3D9_MAX_PS_CONST_B = 16;

class MTLD3D9Interface;

// Per-category + per-render-state mask of states an app touched between
// BeginStateBlock and EndStateBlock, OR every state for D3DSBT_ALL
// blocks. Each MTLD3D9StateBlock owns one; the device's recording arms
// mark bits on the in-progress block as Set* calls land so Apply
// restores only the touched states (wined3d's wined3d_saved_states
// shape, dlls/wined3d/stateblock.c). The render-state slot is
// per-element because layering bugs hinge on stomping
// ALPHABLENDENABLE / ZENABLE alongside the one render state the app
// meant to flip.
struct D3D9StateBlockChanges {
  bool render_states[256] = {};
  bool sampler_states = false;
  bool texture_stage_states = false;
  bool transforms = false;
  bool clip_planes = false;
  bool viewport = false;
  bool scissor = false;
  bool fvf = false;
  // Per-stream bit (covers the buffer binding + offset + stride +
  // freq for that stream together; the recording arms seed all four
  // from live state on a stream's first record so a freq-only or
  // source-only record doesn't restore zeros for the rest).
  uint16_t streams = 0;
  bool material = false;
  // I + B register files only; the F file is range-tracked below so a
  // recorded handful of registers doesn't restore the whole 256-slot
  // file on Apply.
  bool vs_constants = false;
  bool ps_constants = false;
  // Recorded F-register range, half-open [lo, hi); empty when
  // hi <= lo. Capture / Apply touch only this span.
  uint16_t vs_const_f_lo = 0;
  uint16_t vs_const_f_hi = 0;
  uint16_t ps_const_f_lo = 0;
  uint16_t ps_const_f_hi = 0;
  // Per-slot bit, PS samplers 0..15 + VS samplers 16..19. A SetTexture
  // wholly overwrites its snapshot slot, so per-slot bits also keep
  // recorded blocks from pinning every texture bound at Begin time.
  uint32_t textures = 0;
  bool index_buffer = false;
  bool vertex_declaration = false;
  bool vertex_shader = false;
  bool pixel_shader = false;
  bool lights = false;

  void
  reset() {
    *this = D3D9StateBlockChanges{};
  }

  void
  markAll() {
    for (auto &b : render_states)
      b = true;
    sampler_states = texture_stage_states = transforms = clip_planes = true;
    viewport = scissor = fvf = material = true;
    streams = 0xFFFF;
    vs_constants = ps_constants = true;
    vs_const_f_lo = 0;
    vs_const_f_hi = D3D9_MAX_VS_CONST_F;
    ps_const_f_lo = 0;
    ps_const_f_hi = D3D9_MAX_PS_CONST_F;
    textures = (1u << D3D9_MAX_TEXTURE_UNITS) - 1;
    index_buffer = vertex_declaration = vertex_shader = pixel_shader = lights = true;
  }

  // D3DSBT_PIXELSTATE subset: render states from wined3d's
  // pixel_states_render[] (dlls/wined3d/stateblock.c), plus the
  // pixel-pipeline categories (sampler / texture-stage / textures /
  // pixel shader / PS constants). pixel_states_texture[] and
  // pixel_states_sampler[] are reflected through the coarse
  // texture_stage_states / sampler_states bools; TODO: replace with
  // per-stage / per-state-element bitmaps once a game shows
  // per-element divergence.
  void
  markPixelStateSubset() {
    static constexpr D3DRENDERSTATETYPE pixel_states_render[] = {
        D3DRS_ALPHABLENDENABLE,
        D3DRS_ALPHAFUNC,
        D3DRS_ALPHAREF,
        D3DRS_ALPHATESTENABLE,
        D3DRS_ANTIALIASEDLINEENABLE,
        D3DRS_BLENDFACTOR,
        D3DRS_BLENDOP,
        D3DRS_BLENDOPALPHA,
        D3DRS_CCW_STENCILFAIL,
        D3DRS_CCW_STENCILPASS,
        D3DRS_CCW_STENCILZFAIL,
        D3DRS_COLORWRITEENABLE,
        D3DRS_COLORWRITEENABLE1,
        D3DRS_COLORWRITEENABLE2,
        D3DRS_COLORWRITEENABLE3,
        D3DRS_DEPTHBIAS,
        D3DRS_DESTBLEND,
        D3DRS_DESTBLENDALPHA,
        D3DRS_DITHERENABLE,
        D3DRS_FILLMODE,
        D3DRS_FOGDENSITY,
        D3DRS_FOGEND,
        D3DRS_FOGSTART,
        D3DRS_LASTPIXEL,
        D3DRS_SCISSORTESTENABLE,
        D3DRS_SEPARATEALPHABLENDENABLE,
        D3DRS_SHADEMODE,
        D3DRS_SLOPESCALEDEPTHBIAS,
        D3DRS_SRCBLEND,
        D3DRS_SRCBLENDALPHA,
        D3DRS_SRGBWRITEENABLE,
        D3DRS_STENCILENABLE,
        D3DRS_STENCILFAIL,
        D3DRS_STENCILFUNC,
        D3DRS_STENCILMASK,
        D3DRS_STENCILPASS,
        D3DRS_STENCILREF,
        D3DRS_STENCILWRITEMASK,
        D3DRS_STENCILZFAIL,
        D3DRS_TEXTUREFACTOR,
        D3DRS_TWOSIDEDSTENCILMODE,
        D3DRS_WRAP0,
        D3DRS_WRAP1,
        D3DRS_WRAP10,
        D3DRS_WRAP11,
        D3DRS_WRAP12,
        D3DRS_WRAP13,
        D3DRS_WRAP14,
        D3DRS_WRAP15,
        D3DRS_WRAP2,
        D3DRS_WRAP3,
        D3DRS_WRAP4,
        D3DRS_WRAP5,
        D3DRS_WRAP6,
        D3DRS_WRAP7,
        D3DRS_WRAP8,
        D3DRS_WRAP9,
        D3DRS_ZENABLE,
        D3DRS_ZFUNC,
        D3DRS_ZWRITEENABLE,
    };
    for (auto rs : pixel_states_render)
      render_states[rs] = true;
    // TODO: per-stage / per-state-element granularity (wined3d's
    // pixel_states_texture[] / pixel_states_sampler[]).
    texture_stage_states = true;
    sampler_states = true;
    pixel_shader = true;
    ps_constants = true;
    ps_const_f_lo = 0;
    ps_const_f_hi = D3D9_MAX_PS_CONST_F;
    textures = (1u << D3D9_MAX_TEXTURE_UNITS) - 1;
  }

  // D3DSBT_VERTEXSTATE subset: render states from wined3d's
  // vertex_states_render[] (dlls/wined3d/stateblock.c), plus the
  // vertex-pipeline categories. vertex_states_sampler[] (DMAP_OFFSET)
  // and vertex_states_texture[] (TEXCOORD_INDEX, TEXTURE_TRANSFORM_FLAGS)
  // are reflected through the coarse sampler_states /
  // texture_stage_states bools; same TODO as the pixel subset.
  void
  markVertexStateSubset() {
    static constexpr D3DRENDERSTATETYPE vertex_states_render[] = {
        D3DRS_ADAPTIVETESS_W,
        D3DRS_ADAPTIVETESS_X,
        D3DRS_ADAPTIVETESS_Y,
        D3DRS_ADAPTIVETESS_Z,
        D3DRS_AMBIENT,
        D3DRS_AMBIENTMATERIALSOURCE,
        D3DRS_CLIPPING,
        D3DRS_CLIPPLANEENABLE,
        D3DRS_COLORVERTEX,
        D3DRS_CULLMODE,
        D3DRS_DIFFUSEMATERIALSOURCE,
        D3DRS_EMISSIVEMATERIALSOURCE,
        D3DRS_ENABLEADAPTIVETESSELLATION,
        D3DRS_FOGCOLOR,
        D3DRS_FOGDENSITY,
        D3DRS_FOGENABLE,
        D3DRS_FOGEND,
        D3DRS_FOGSTART,
        D3DRS_FOGTABLEMODE,
        D3DRS_FOGVERTEXMODE,
        D3DRS_INDEXEDVERTEXBLENDENABLE,
        D3DRS_LIGHTING,
        D3DRS_LOCALVIEWER,
        D3DRS_MAXTESSELLATIONLEVEL,
        D3DRS_MINTESSELLATIONLEVEL,
        D3DRS_MULTISAMPLEANTIALIAS,
        D3DRS_MULTISAMPLEMASK,
        D3DRS_NORMALDEGREE,
        D3DRS_NORMALIZENORMALS,
        D3DRS_PATCHEDGESTYLE,
        D3DRS_POINTSCALE_A,
        D3DRS_POINTSCALE_B,
        D3DRS_POINTSCALE_C,
        D3DRS_POINTSCALEENABLE,
        D3DRS_POINTSIZE,
        D3DRS_POINTSIZE_MAX,
        D3DRS_POINTSIZE_MIN,
        D3DRS_POINTSPRITEENABLE,
        D3DRS_POSITIONDEGREE,
        D3DRS_RANGEFOGENABLE,
        D3DRS_SHADEMODE,
        D3DRS_SPECULARENABLE,
        D3DRS_SPECULARMATERIALSOURCE,
        D3DRS_TWEENFACTOR,
        D3DRS_VERTEXBLEND,
    };
    for (auto rs : vertex_states_render)
      render_states[rs] = true;
    // TODO: per-stage / per-state-element granularity (wined3d's
    // vertex_states_texture[] / vertex_states_sampler[]).
    texture_stage_states = true;
    sampler_states = true;
    vertex_shader = true;
    vs_constants = true;
    vs_const_f_lo = 0;
    vs_const_f_hi = D3D9_MAX_VS_CONST_F;
    vertex_declaration = true;
    streams = 0xFFFF;
    index_buffer = true;
    material = true;
    lights = true;
    transforms = true;
    clip_planes = true;
    viewport = true;
    scissor = true;
    fvf = true;
  }
};

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

  // Internal accessors: used by surfaces / textures / buffers when
  // they need the underlying Metal device for resource allocation.
  // Not part of the IDirect3DDevice9 contract.
  WMT::Device
  metalDevice() const {
    return m_metalDevice;
  }
  // Public accessor to the underlying dxmt::CommandQueue. The swapchain's
  // Present routes its present-blit through a chunk on this queue
  // (chunk->emitcc + ctx.present + PresentBoundary). Returned by reference
  // because the queue owns its own encode/finish threads and must outlive
  // every chunk the device enqueues; the device owns it via unique_ptr
  // below.
  dxmt::CommandQueue &dxmtQueue() const;
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
    // success-status code. Currently unreachable (Lost transition
    // unwired) but the gate is correct when it becomes reachable.
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
  HRESULT STDMETHODCALLTYPE StretchRect(
      IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface,
      const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter
  ) override;
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
  // Create*<DEFAULT-pool> and the matching leaf dtor. Resources that
  // are never handed to the app (implicit RT0, auto-DS) skip these.
  void
  onLosableResourceCreated() {
    m_losableResourceCount.fetch_add(1, std::memory_order_relaxed);
  }
  void
  onLosableResourceDestroyed() {
    m_losableResourceCount.fetch_sub(1, std::memory_order_relaxed);
  }
  uint32_t
  losableResourceCount() const {
    return m_losableResourceCount.load(std::memory_order_relaxed);
  }

  // IDxmtDiag9: private diag QI surface (see d3d9_diag.hpp). Tests
  // probe this to assert teardown invariants; not user-facing.
  UINT STDMETHODCALLTYPE
  GetLosableResourceCount() override {
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

  // StateBlock registry: Reset walks this set and calls invalidate()
  // on every outstanding block. Pre-Reset Captures replay ref-pinned slots
  // but Reset orphans snapshots (swapchain backbuffers replaced in place,
  // auto-DS recreated, app-held DEFAULT-pool resources destroyed). Raw
  // pointers safe: device strictly outlives blocks (pub-AddRef pins device;
  // self-pin keeps block alive until ReleasePrivate, before device dtor).
  void
  registerStateBlock(class MTLD3D9StateBlock *sb) {
    m_stateBlocks.insert(sb);
  }
  void
  unregisterStateBlock(class MTLD3D9StateBlock *sb) {
    m_stateBlocks.erase(sb);
  }

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
  };

  // BatchedDraw: one capture + per-call args. override_* fields
  // replace bound streams when non-zero. MTLBuffer lifetime pinned by
  // m_constRing signal_seq + setBuffer retain; no per-draw Reference
  // needed.
  struct BatchedDraw {
    D3D9DrawCapture cap;
    // Per-draw POD snapshot captured at queue time for Resolve to read
    // frozen state without racing setters. COW via m_encShadowDirty:
    // consecutive draws share same shared_ptr, O(state-change clusters).
    std::shared_ptr<dxmt::D9EncodingState> pod_snapshot;
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
    // Per-RT flag: did Resolve swap the view's pixel-format to its sRGB
    // sibling (D3DRS_SRGBWRITEENABLE)? If yes, the chunk's pending-clear
    // load-action must hand-encode the linear clear color into sRGB
    // storage because Metal's clearColor bypasses the attachment's
    // pixel-format encode. Same hand-encode lives in drainPendingClear's
    // chunk-emit body for the lone-Clear-then-Present path.
    bool resolved_rt_srgb_swapped[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
    bool resolved_ds_has_stencil = false;
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
    // VS/PS f/i/b register-file + clip-plane constant uploads. Resolve
    // pulls the data from D9EncodingState, allocates from m_constRing,
    // and stores the resulting (buffer, offset) pairs here. Fixed slot
    // order: 0=vs_cb (F), 1=vs_ic (I), 2=vs_bc (packed bool), 3=ps_cb,
    // 4=ps_ic, 5=ps_bc, 6=vs_cp (packed clip planes), 7=vs_cc (clip
    // count).
    struct ResolvedConstUpload {
      obj_handle_t buffer = 0;
      uint64_t offset = 0;
    };
    std::array<ResolvedConstUpload, 8> resolved_const_uploads = {};
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
    enum class Kind : uint8_t { Copy = 0, Stretch = 1, Resolve = 2 };
    Kind kind = Kind::Copy;
    WMTSize dst_size = {};
    D3DTEXTUREFILTERTYPE filter = D3DTEXF_NONE;
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

public:
  // Public for the swapchain's Present, which drains queued draws onto
  // a chunk before posting the present-blit chunk.
  HRESULT FlushDrawBatch();

private:
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
    std::array<BatchedDraw::ResolvedConstUpload, 8> uploads = {};
  };
  // Cluster cache for the cluster-stable resolved bundle (PSO, DSSO,
  // sampler+texture views, RT/DS resolve, viewport/scissor, IA layout
  // metadata). Predicate: pod_snapshot.get() + m_encodeSideRefsGen
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
    bool resolved_rt_srgb_swapped[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {};
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
  // Dynamic VB / IB rename-ring DISCARD path reads m_currentCmdSeq /
  // m_cachedSignaled and may call m_completionEvent.waitUntilSignaledValue
  // to stall the calling thread until a previously-used rename slot
  // has been retired by the GPU. See MTLD3D9VertexBuffer::Lock.
  friend class MTLD3D9VertexBuffer;
  friend class MTLD3D9IndexBuffer;

public:
  // Device-level pool of host-placed Metal buffer backings keyed by size.
  // Share allocations across distinct buffer objects, avoiding repeated
  // newBuffer + wsi::aligned_malloc. acquireBufferBacking pops first
  // exact-size match; releaseBufferBacking pushes with capped pool size.
  bool acquireBufferBacking(
      size_t size, WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned
  );
  void releaseBufferBacking(WMT::Reference<WMT::Buffer> &&buffer, void *owned, uint64_t gpu_address, size_t capacity);
  // Pool-first acquire with fallback to a fresh wsi::aligned_malloc +
  // Metal newBufferWithBytesNoCopy. Used by CreateVertexBuffer and
  // CreateIndexBuffer; pure shape dedup; the slow-memset warn applies
  // to either buffer kind (the first-touch cliff is per-page, not per-
  // buffer-type), so the helper emits it unconditionally.
  HRESULT acquireOrAllocateBufferBacking(
      UINT length, WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned
  );

private:
  const bool m_isEx;
  // Cursor visibility: ShowCursor returns the previous visibility per
  // the wined3d_device_show_cursor contract (wined3d device.c).
  // Default FALSE matches the post-SetCursorProperties initial state
  // before the app calls ShowCursor(TRUE); i.e. no cursor visible
  // until the app explicitly turns it on. macOS draws the cursor
  // itself; this state is purely for the spec return-value contract.
  BOOL m_cursorVisible = FALSE;
  // D3D9 device-state machine: Ok → S_OK, Lost → D3DERR_DEVICELOST
  // (unreachable today), NotReset → D3DERR_DEVICENOTRESET. Ex devices
  // return S_OK; use CheckDeviceState instead.
  enum class DeviceState : uint8_t { Ok, Lost, NotReset };
  std::atomic<DeviceState> m_deviceState{DeviceState::Ok};
  // Live count of app-created D3DPOOL_DEFAULT resources. Bumped by
  // every public Create* that returns a DEFAULT-pool resource; the
  // resource's leaf dtor decrements (each leaf carries an m_isLosable
  // flag set on the Create* path so implicit RT0 / auto-DS allocations;
  // which never reach the app; don't count). Reset reads this and
  // returns D3DERR_INVALIDCALL if non-zero. wined3d / DXVK enforce the
  // same contract via wined3d_resource_release counts and DXVK's
  // m_losableResourceCounter respectively.
  std::atomic<uint32_t> m_losableResourceCount{0};
  // StateBlock registry; see registerStateBlock / unregisterStateBlock
  // above. Reset walks the set and calls invalidate() on each entry.
  // Single-threaded (calling thread only); no lock.
  std::unordered_set<class MTLD3D9StateBlock *> m_stateBlocks;
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
  std::vector<BufferBackingPoolEntry> m_bufferBackingPool;
  // Cap so a pathological create/destroy churn doesn't accumulate
  // unbounded VRAM. 128 entries × typical D3D9 buffer/texture sizes
  // caps worst-case pool memory in the low-hundreds-of-MB range on
  // Apple Silicon. Sized for level-transition bursts that create 100+
  // textures in one frame; at 64 the pool evicted entries before the
  // next level's CreateTexture could reuse them, paying the cold-
  // allocate cliff on every level boundary.
  static constexpr size_t kMaxBufferBackingPoolSize = 128;
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
  // First record of stream N between Begin/End seeds the block's whole
  // per-stream record (buffer + offset + stride + freq) from live
  // state: one changed-mask bit covers all four components, so the
  // ones a SetStreamSource / SetStreamSourceFreq doesn't provide must
  // restore live values rather than zeros on Apply.
  void seedRecordedStream(UINT StreamNumber);
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
  // Device9Ex frame-latency bookkeeping. SetMaximumFrameLatency
  // rejects >30 with INVALIDCALL; 0 selects the default 3 (DXVK
  // d3d9_device.cpp). Read-only round-trip; Metal
  // doesn't expose the equivalent waitable knob, but apps that poll
  // GetMaximumFrameLatency expect their last Set value back.
  UINT m_frameLatency = 3;
  WMT::Reference<WMT::Device> m_metalDevice;
  // COW snapshot cache for BatchedDraw::pod_snapshot. m_encShadowDirty
  // bitmask; setters OR category on value-change. Fresh snapshot copies
  // only dirty axes (~10 KB → ~5 KB per heavy cluster). Consecutive draws
  // with no setters share one shared_ptr via atomic refcount.
  uint32_t m_encShadowDirty = dxmt::D9ES_DIRTY_ALL;
  std::shared_ptr<dxmt::D9EncodingState> m_encShadowLastSnap;
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
  DWORD m_samplerStates[D3D9_MAX_TEXTURE_UNITS][D3DSAMP_DMAPOFFSET + 1];
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

  // VS constant register file. Hardware-VP sizes; SWVP would expand
  // F to 8192 / I to 2048 but we don't expose SWVP yet. Default-zero
  // initialized; D3D9's read-back contract is that an unset constant
  // reads as 0 / FALSE, see DXVK ResetState.
  float m_vsConstantsF[D3D9_MAX_VS_CONST_F][4] = {};
  int m_vsConstantsI[D3D9_MAX_VS_CONST_I][4] = {};
  BOOL m_vsConstantsB[D3D9_MAX_VS_CONST_B] = {};

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
  // routes through wined3d_device_set_clip_status / get_clip_status
  // which are also pure storage at the wined3d layer. Apps that don't
  // hr-check trip on E_NOTIMPL, so storage + D3D_OK matches the spec.
  D3DCLIPSTATUS9 m_clipStatus = {};

  // Transform state: view + projection + 8 texture stages + 256 world
  // matrices (DXVK d3d9_caps::MaxTransforms = 266). Compaction matches
  // DXVK GetTransformIndex (d3d9_util.h). Default identity; FFP
  // pipelines and apps that GetTransform without prior Set rely on it.
  static constexpr uint32_t kMaxTransforms = 10 + 256;
  D3DMATRIX m_transforms[kMaxTransforms];

  // FFP material. Bookkeeping until the FFP shader generator lands;
  // initDefaults seeds wined3d's default-on-CreateDevice values so a
  // GetMaterial before any SetMaterial reads back the spec defaults
  // (Diffuse=(1,1,1,1), all other channels 0, Power=0).
  D3DMATERIAL9 m_material = {};

  // FFP lights: sparsely indexed by app-supplied DWORD. wined3d
  // (state.c) and DXVK (d3d9_state.h) both grow a flat vector;
  // indices are typically small (0..8) so contiguous storage wins.
  // m_lightEnables runs in parallel, holding the LightEnable flag for
  // each index. SetLight grows the vectors; LightEnable on an
  // unset index is INVALIDCALL per the D3D9 contract.
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
  // saturating Apple's XPC compiler. Async build via m_psoScheduler;
  // first draw blocks at Wait(), subsequent hits cached task.
  std::unordered_map<uint64_t, std::unique_ptr<D3D9PsoCompileTask>> m_psoCache;
  task_scheduler<D3D9PsoCompileTask *> m_psoScheduler;

  // Compiled-shader module dedup, keyed by bytecode hash. DXVK's
  // D3D9ShaderModuleSet (src/d3d9/d3d9_shader.h): apps that recreate the
  // same shader (streaming worlds tear down and re-mint identical
  // bytecode) would otherwise get a fresh wrapper + variant cache +
  // distinct MTLFunction every CreateVertexShader / CreatePixelShader,
  // and since the PSO key embeds the function handle, identical draws
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
  obj_handle_t dummyFragmentTexture2D();
  Rc<dxmt::Texture> m_dummyFragTexAlloc;
  obj_handle_t m_dummyFragTexHandle = 0;

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
  // Drain any pending Clear and deferred blit work onto the current
  // chunk. drainPendingClear posts a clear-only render-pass chunk;
  // flushDeferredBlitWork posts generateMipmaps chunks for
  // AUTOGENMIPMAP-mipsDirty bound textures. Both go via chunk->emitcc
  // on the queue's CurrentChunk(); the EncodingThread replays them in
  // emit order. Called from FlushDrawBatch (before the draw-batch
  // emit), EndScene, the destructor, and Present.
  void flushOpenWork();

  // Submit one-shot blit filling mip levels 1..N from level 0 via
  // Metal's generateMipmapsForTexture. Optionally drains m_pendingDraws
  // before posting so app draws land first. flushDeferredBlitWork must
  // pass drain_pending_draws=false to prevent recursion (FlushDrawBatch
  // → generateMipmaps → FlushDrawBatch) blowing i386 stack.
  void generateMipmaps(WMT::Texture texture, bool drain_pending_draws = true);

  // Drain deferred blit work. The only caller is the AUTOGENMIPMAP-
  // mipsDirty sweep; each dirty bound texture posts its own
  // generateMipmaps chunk (deduped within this call across PS+VTF
  // aliasing). Pending texture uploads emit chunks at stageTextureUpload
  // time and don't queue through here.
  void flushDeferredBlitWork();

  // Stage texture sub-region upload through m_uploadRing + chunk-emitted
  // blit. Allocates slice, memcpys src, posts wmtcmd_blit_copy_from_buffer
  // signaling m_completionEvent for ring recycling. Caller computes
  // src_pitch and is_compressed (Metal contract).
  // src_slice_pitch is the source stride between depth slices for a 3D
  // (volume) upload; 0 = contiguous (2D, or a full-box 3D upload).
  void stageTextureUpload(
      WMT::Texture dst, uint32_t mip_level, uint32_t slice, WMTOrigin origin, WMTSize size, const void *src,
      uint32_t src_pitch, bool is_compressed, uint32_t src_slice_pitch = 0
  );

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
