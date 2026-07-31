#include "d3d9_device.hpp"

#include "airconv_public.h"
#include "d3d9_buffer.hpp"
#include "d3d9_create_validation.hpp"
#include "d3d9_cube_texture.hpp"
#include "d3d9_cursor_validation.hpp"
#include "d3d9_debug.hpp"
#include "d3d9_volume_texture.hpp"
#include "d3d9_format.hpp"
#include "d3d9_fvf.hpp"
#include "d3d9_ia_element.hpp"
#include "d3d9_interface.hpp"
#include "d3d9_matrix.hpp"
#include "d3d9_point_size.hpp"
#include "d3d9_process_vertices.hpp"
#include "d3d9_query.hpp"
#include "d3d9_query_contract.hpp"
#include "d3d9_render_state_translate.hpp"
#include "d3d9_sampler_translate.hpp"
#include "d3d9_shader.hpp"
#include "d3d9_state_defaults.hpp"
#include "d3d9_texture_upload.hpp"
#include "d3d9_update_mip.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_state_block.hpp"
#include "d3d9_stretchrect.hpp"
#include "d3d9_swapchain.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_texture_mem.hpp"
#include "d3d9_viewport.hpp"
#include "d3d9_validation.hpp"
#include "d3d9_vertex_declaration.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "dxso_header.hpp"
#include "log/log.hpp"
#include "wsi_platform.hpp"
#include "wsi_window.hpp"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>
#include "com/com_pointer.hpp"

namespace dxmt {

// Size parity between MTLD3D9Device's calling-thread shadow and the
// encode-thread D9EncodingState. The state struct lives in its own
// header to keep the include surface small; these asserts catch any
// drift if either side's array bounds change.
static_assert(D9ES_MAX_TEXTURE_UNITS == D3D9_MAX_TEXTURE_UNITS, "");
static_assert(D9ES_MAX_VERTEX_STREAMS == D3D9_MAX_VERTEX_STREAMS, "");
static_assert(kIaMaxVertexStreams == D3D9_MAX_VERTEX_STREAMS, "");
static_assert(D9ES_MAX_VS_CONST_F == D3D9_MAX_VS_CONST_F, "");
static_assert(D9ES_MAX_VS_CONST_I == D3D9_MAX_VS_CONST_I, "");
static_assert(D9ES_MAX_VS_CONST_B == D3D9_MAX_VS_CONST_B, "");
static_assert(D9ES_MAX_PS_CONST_F == D3D9_MAX_PS_CONST_F, "");
static_assert(D9ES_MAX_PS_CONST_I == D3D9_MAX_PS_CONST_I, "");
static_assert(D9ES_MAX_PS_CONST_B == D3D9_MAX_PS_CONST_B, "");
// d3d9_state_block_changes.hpp mirrors these three so it stays host-includable
// without the device surface (the d3d9_ia_element.hpp pattern); catch drift.
static_assert(kSbcMaxVsConstF == D3D9_MAX_VS_CONST_F, "");
static_assert(kSbcMaxPsConstF == D3D9_MAX_PS_CONST_F, "");
static_assert(kSbcMaxTextureUnits == D3D9_MAX_TEXTURE_UNITS, "");

namespace {

// SetTexture and UpdateTexture receive an app-owned IDirect3DBaseTexture9 and must
// not call through its vtable to identify it: a wrapped or hostile vtable (some
// overlays install one, and the conformance path for this deliberately corrupts it)
// would divert the call to garbage. Capture each texture leaf's real vtable pointer
// when it is created and match an incoming pointer against them, the way wined3d's
// unsafe_impl_from_IDirect3DBaseTexture9 reads the vtable pointer instead of calling
// it. A matched leaf's MTLD3D9CommonTexture sub-object carries its own, untouched
// vtable, so reading its type afterwards is safe.
enum { kLeaf2D = 0, kLeafCube = 1, kLeafVolume = 2 };
std::atomic<const void *> g_textureLeafVtable[3] = {};

void
captureTextureLeafVtable(IDirect3DBaseTexture9 *leaf, unsigned index) {
  g_textureLeafVtable[index].store(*reinterpret_cast<const void *const *>(leaf), std::memory_order_relaxed);
}

// Returns the common texture for a recognised dxmt texture, or null for a null,
// foreign, or hostile-vtable pointer (the caller treats that as "no texture",
// matching wined3d). Never dereferences the app vtable.
MTLD3D9CommonTexture *
commonTextureFromBound(IDirect3DBaseTexture9 *iface) {
  if (!iface)
    return nullptr;
  const void *vtable = *reinterpret_cast<const void *const *>(iface);
  // A null vtable is never one of ours, and this guards the degenerate match
  // against a capture slot that no texture of that type has populated yet.
  if (!vtable)
    return nullptr;
  if (vtable == g_textureLeafVtable[kLeaf2D].load(std::memory_order_relaxed))
    return static_cast<MTLD3D9Texture *>(static_cast<IDirect3DTexture9 *>(iface));
  if (vtable == g_textureLeafVtable[kLeafCube].load(std::memory_order_relaxed))
    return static_cast<MTLD3D9CubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(iface));
  if (vtable == g_textureLeafVtable[kLeafVolume].load(std::memory_order_relaxed))
    return static_cast<MTLD3D9VolumeTexture *>(static_cast<IDirect3DVolumeTexture9 *>(iface));
  return nullptr;
}

// D3D9 rejects a block-compressed (DXTn) resource whose requested top-level
// (mip 0) width or height is not a whole number of 4x4 blocks, returning
// INVALIDCALL on every pool. Lower mip levels may be sub-block (a legal 4x4
// two-level texture has a 2x2 mip 1), so only the requested dimensions are
// gated. Volume depth is unconstrained (block depth is always 1). wined3d
// resource.c enforces this once at resource init; shared here by all four
// texture/surface create entry points.
bool
isBlockAlignedCreate(D3DFORMAT format, UINT width, UINT height) {
  if (!IsCompressedFormat(format))
    return true;
  return (width & 3u) == 0u && (height & 3u) == 0u;
}

// D3DDECLTYPE -> MTLAttributeFormat (dxbc_signature.cpp mirrors the table).
// D3DCOLOR legacy 0xAARRGGBB layout matches Metal's UChar4Normalized_BGRA.
// UDEC3 / DEC3N packed formats need custom unpack; not covered.
inline uint32_t
to_mtl_attr_format(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return 28; // Float
  case D3DDECLTYPE_FLOAT2:
    return 29; // Float2
  case D3DDECLTYPE_FLOAT3:
    return 30; // Float3
  case D3DDECLTYPE_FLOAT4:
    return 31; // Float4
  case D3DDECLTYPE_D3DCOLOR:
    return 42; // UChar4Normalized_BGRA
  case D3DDECLTYPE_UBYTE4:
    return 3; // UChar4
  case D3DDECLTYPE_SHORT2:
    return 16; // Short2
  case D3DDECLTYPE_SHORT4:
    return 18; // Short4
  case D3DDECLTYPE_UBYTE4N:
    return 9; // UChar4Normalized
  case D3DDECLTYPE_SHORT2N:
    return 22; // Short2Normalized
  case D3DDECLTYPE_SHORT4N:
    return 24; // Short4Normalized
  case D3DDECLTYPE_USHORT2N:
    return 19; // UShort2Normalized
  case D3DDECLTYPE_USHORT4N:
    return 21; // UShort4Normalized
  case D3DDECLTYPE_FLOAT16_2:
    return 25; // Half2
  case D3DDECLTYPE_FLOAT16_4:
    return 27; // Half4
  case D3DDECLTYPE_DEC3N:
    // 10-10-10-2 signed normalized: Metal's Int1010102Normalized is
    // exact: 10-bit signed integer x/y/z normalized to [-1, 1] + 2-bit
    // signed w. Game engines pack tangent-space vectors here.
    return 40; // Int1010102Normalized
  case D3DDECLTYPE_UDEC3:
    // 10-10-10-2 unsigned UNnormalized per D3D9 spec. Metal has no
    // unnormalized 10-bit attribute format, so apps that wrote
    // x in [0,1023] read x in [0,1]. DXVK keeps the raw values via
    // Vulkan's USCALED format; Metal has no equivalent. Accepted gap.
    return 41; // UInt1010102Normalized
  default:
    return 0; // Invalid
  }
}

// D3DPRIMITIVETYPE -> Metal primitive class. The Metal type enum is at
// winemetal.h; the D3D9 enum is contiguous from 1
// (D3DPT_POINTLIST). D3D9 fans have no Metal equivalent; the entry
// points emulate them with an index-buffer rewrite that reaches the
// Resolve/EmitDrawBatch path as TRIANGLELIST, so the encoder never
// sees a fan.
inline WMTPrimitiveType
to_mtl_prim_type(D3DPRIMITIVETYPE pt) {
  switch (pt) {
  case D3DPT_POINTLIST:
    return WMTPrimitiveTypePoint;
  case D3DPT_LINELIST:
    return WMTPrimitiveTypeLine;
  case D3DPT_LINESTRIP:
    return WMTPrimitiveTypeLineStrip;
  case D3DPT_TRIANGLELIST:
    return WMTPrimitiveTypeTriangle;
  case D3DPT_TRIANGLESTRIP:
    return WMTPrimitiveTypeTriangleStrip;
  default:
    return WMTPrimitiveTypeTriangle; // unreachable
  }
}

// PrimitiveCount -> vertex count (MGL/wined3d use the same arithmetic).
// MaxPrimitiveCount is advertised via D3DCAPS9 only; neither wined3d
// (device.c d3d9_device_DrawPrimitive) nor DXVK (d3d9_device.cpp
// D3D9DeviceEx::DrawPrimitive) rejects a draw whose PrimitiveCount
// exceeds the cap, so no max-count gate lives here.
//
// TRIANGLEFAN matches TRIANGLESTRIP's vertex-count formula (count + 2);
// the entry points fold the fan into a list before the encoder so the
// per-prim arithmetic still uses the original D3D9 vertex count.
inline UINT
prim_to_vertex_count(D3DPRIMITIVETYPE pt, UINT count) {
  switch (pt) {
  case D3DPT_POINTLIST:
    return count;
  case D3DPT_LINELIST:
    return count * 2;
  case D3DPT_LINESTRIP:
    return count + 1;
  case D3DPT_TRIANGLELIST:
    return count * 3;
  case D3DPT_TRIANGLESTRIP:
    return count + 2;
  case D3DPT_TRIANGLEFAN:
    return count + 2;
  default:
    return 0;
  }
}

// D3D9 fan with N verts yields N-2 triangles: (0, k+1, k+2).
// Metal has no fan; synthesise index list as TRIANGLELIST.
// src may be null (generate 0..N-1) or u16/u32 array; src_idx_size ∈ {0,2,4}.
inline void
fill_fan_to_list_indices(uint32_t *dst, UINT prim_count, const void *src, uint32_t src_idx_size) {
  if (src == nullptr) {
    for (UINT k = 0; k < prim_count; ++k) {
      dst[k * 3 + 0] = 0;
      dst[k * 3 + 1] = k + 1;
      dst[k * 3 + 2] = k + 2;
    }
  } else if (src_idx_size == 2) {
    auto *s = static_cast<const uint16_t *>(src);
    for (UINT k = 0; k < prim_count; ++k) {
      dst[k * 3 + 0] = s[0];
      dst[k * 3 + 1] = s[k + 1];
      dst[k * 3 + 2] = s[k + 2];
    }
  } else {
    auto *s = static_cast<const uint32_t *>(src);
    for (UINT k = 0; k < prim_count; ++k) {
      dst[k * 3 + 0] = s[0];
      dst[k * 3 + 1] = s[k + 1];
      dst[k * 3 + 2] = s[k + 2];
    }
  }
}

// D3D9 has no view objects, so each surface op synthesizes its Metal view
// inline: keep the multisample bit derived from the texture in one place
// (d3d11/d3d12 do the equivalent in their own view helpers).
inline WMTTextureType
surface_view_type(const dxmt::Texture *texture) {
  return texture->sampleCount() > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
}

// Per-RT D3DRS_COLORWRITEENABLE row. Order matches m_renderTargets[]
// (RT 0..3); index 0 reuses the original D3DRS_COLORWRITEENABLE slot
// per D3D9 spec.
constexpr D3DRENDERSTATETYPE kColorWriteEnableRS[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {
    D3DRS_COLORWRITEENABLE,
    D3DRS_COLORWRITEENABLE1,
    D3DRS_COLORWRITEENABLE2,
    D3DRS_COLORWRITEENABLE3,
};

// D3DMULTISAMPLE_TYPE (+ quality) -> Metal sampleCount + create-path
// accept/reject HRESULT, for CreateRenderTarget / CreateDepthStencilSurface.
// The create path validates the way wined3d texture.c does: every invalid or
// device-unsupported request is D3DERR_INVALIDCALL. NOTAVAILABLE is a
// CheckDeviceMultiSampleType-only result and never comes back from a create.
// The per-count supportsTextureSampleCount probe is the same one
// CheckDeviceMultiSampleType reports from, so a count the app saw accepted at
// probe time is accepted here too. D3DMULTISAMPLE_NONMASKABLE reads the
// quality level as a sample-count selector (1 << quality), the DXVK
// GetSampleCount mapping. Returns sample_count 1 on rejection so the callers
// that ignore the HRESULT (the implicit-DS and metalSampleCount paths) still
// fall back to single-sample.
inline std::pair<uint32_t, HRESULT>
multisample_type_to_metal_sample_count(D3DMULTISAMPLE_TYPE ms, DWORD quality, WMT::Device device) {
  switch (ms) {
  case D3DMULTISAMPLE_NONE:
    return {1u, D3D_OK};
  // A masked type names an exact sample count: it must be device-supported,
  // and the quality level must be 0 (only NONMASKABLE carries a quality).
  case D3DMULTISAMPLE_2_SAMPLES:
  case D3DMULTISAMPLE_4_SAMPLES:
  case D3DMULTISAMPLE_8_SAMPLES:
  case D3DMULTISAMPLE_16_SAMPLES:
    if (quality != 0 || !device.supportsTextureSampleCount(static_cast<uint8_t>(ms)))
      return {1u, static_cast<HRESULT>(D3DERR_INVALIDCALL)};
    return {static_cast<uint32_t>(ms), D3D_OK};
  // NONMASKABLE quality level N selects 1 << N samples: level 0 is the
  // always-present single-sample level, levels above it must name a
  // device-supported power-of-two count. Sample-count support is monotonic on
  // Apple GPUs, so a level the device backs here is exactly a level inside the
  // count CheckDeviceMultiSampleType reports.
  case D3DMULTISAMPLE_NONMASKABLE: {
    if (quality == 0)
      return {1u, D3D_OK};
    if (quality > 4)
      return {1u, static_cast<HRESULT>(D3DERR_INVALIDCALL)};
    uint32_t samples = 1u << quality;
    if (!device.supportsTextureSampleCount(static_cast<uint8_t>(samples)))
      return {1u, static_cast<HRESULT>(D3DERR_INVALIDCALL)};
    return {samples, D3D_OK};
  }
  // Non-power-of-two counts (3, 5, 6, 7, 9..15) and any out-of-enum value are
  // invalid input.
  default:
    return {1u, static_cast<HRESULT>(D3DERR_INVALIDCALL)};
  }
}

} // namespace

uint32_t
MTLD3D9Device::metalSampleCount(D3DMULTISAMPLE_TYPE type, DWORD quality) const {
  auto [count, hr] = multisample_type_to_metal_sample_count(type, quality, m_metalDevice);
  (void)hr;
  return count;
}

// Async PSO compile: mirrors d3d11 MTLCompiledGraphicsPipelineImpl.
// Waits on the two function-compile tasks (dependencies), then calls
// newRenderPipelineState off-thread; signals ready bit when done. So the
// encode thread never runs the function-compile LLVM emit nor the link.
// Device-lifetime cache holds completed PSOs; Com<> retains in-flight ones.
class D3D9PsoCompileTask final : public D3D9AsyncTask {
public:
  D3D9PsoCompileTask(
      WMT::Device device, Com<MTLD3D9VertexShader, false> vs, Com<MTLD3D9PixelShader, false> ps,
      const WMTRenderPipelineInfo &info, D3D9CompiledFunction *vs_fn, D3D9CompiledFunction *ps_fn
  ) :
      m_device(device),
      m_vs(std::move(vs)),
      m_ps(std::move(ps)),
      m_info(info),
      m_vs_fn(vs_fn),
      m_ps_fn(ps_fn) {}

  // Worker entry. Runs on a pool thread; the scheduler re-enters this whole
  // body after a park, so it is idempotent and checks its dependencies
  // first. A not-done function task is returned as the continuation: the
  // scheduler parks this task on it and requeues us once it completes
  // (get-or-create submitted both function tasks, so neither can hang here;
  // mirrors d3d11 MTLCompiledGraphicsPipelineImpl::RunThreadpoolWork).
  D3D9AsyncTask *
  RunTask() override {
    // A d3d9 draw always resolves to both a vertex and a pixel function task:
    // the fixed-function path synthesises the stage the app leaves unbound, so
    // unlike d3d11 (which guards a genuinely absent pixel shader) neither
    // pointer is ever null here. A failed compile is a live task whose
    // function() latches null, handled below, not a null task pointer.
    if (!m_vs_fn->GetDone())
      return m_vs_fn;
    if (!m_ps_fn->GetDone())
      return m_ps_fn;
    // Both function compiles have latched. A null function is a cached
    // compile failure (the emit is deterministic on its input); leave
    // m_state null so the resolve-time null-state skip drops the draw, and
    // never feed a null function to the link.
    WMT::Function vs_function = m_vs_fn->function();
    WMT::Function ps_function = m_ps_fn->function();
    if (vs_function.handle == 0 || ps_function.handle == 0)
      return this;
    m_info.vertex_function = vs_function.handle;
    m_info.fragment_function = ps_function.handle;
    WMT::Reference<WMT::Error> err;
    m_state = m_device.newRenderPipelineState(m_info, err);
    if (!m_state)
      // Link runs once per key and a null state latches (the resolve site then
      // skips every draw with this key), so surface the failure the way d3d11
      // does rather than dropping the draw silently. Every link failure seen in
      // practice has been a deterministic descriptor bug, not transient pressure.
      Logger::err(
          str::format(
              "d3d9: failed to create pipeline state: ", err ? err.description().getUTF8String() : "(no NSError)"
          )
      );
    return this; // signals "task complete" to the scheduler
  }

  // task_trait hooks. atomic_bool.notify_all wakes Wait() callers.
  bool
  GetDone() const noexcept override {
    return m_ready.load(std::memory_order_acquire);
  }
  void
  SetDone(bool s) noexcept override {
    m_ready.store(s, std::memory_order_release);
    m_ready.notify_all();
  }

  // Block the calling thread until the worker has finished. First-draw
  // hits this; subsequent draws against the same key see ready=true and
  // return immediately.
  void
  Wait() const noexcept {
    while (!m_ready.load(std::memory_order_acquire))
      m_ready.wait(false, std::memory_order_acquire);
  }

  WMT::RenderPipelineState
  state() const noexcept {
    return m_state ? WMT::RenderPipelineState{m_state.handle} : WMT::RenderPipelineState{};
  }
  const WMTRenderPipelineInfo &
  info() const noexcept {
    return m_info;
  }

  // Full-key verify for the PSO cache. m_psoCache keys on a 64-bit FNV fold
  // of exactly these inputs, so a hash hit is confirmed against them before
  // the task is reused, the same discipline the bytecode module cache applies
  // on a hash hit (see bytecode_equal in d3d9_shader.cpp): a 64-bit collision
  // would otherwise render draws against the wrong pipeline. The comparison
  // is field-wise, not a memcmp of m_info: vertex_function / fragment_function
  // are filled in later by RunTask (so they differ from the freshly-built
  // probe, and are excluded here because the fold keys on the function-TASK
  // pointers, not the compiled handles), and WMTRenderPipelineInfo carries
  // padding a memcmp would read.
  bool
  matchesKeyInputs(
      D3D9CompiledFunction *vs_fn, D3D9CompiledFunction *ps_fn, const WMTRenderPipelineInfo &info
  ) const noexcept {
    if (m_vs_fn != vs_fn || m_ps_fn != ps_fn)
      return false;
    if (m_info.depth_pixel_format != info.depth_pixel_format ||
        m_info.stencil_pixel_format != info.stencil_pixel_format ||
        m_info.input_primitive_topology != info.input_primitive_topology ||
        m_info.raster_sample_count != info.raster_sample_count)
      return false;
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      const auto &a = m_info.colors[i];
      const auto &b = info.colors[i];
      if (a.pixel_format != b.pixel_format || a.blending_enabled != b.blending_enabled ||
          a.write_mask != b.write_mask || a.rgb_blend_operation != b.rgb_blend_operation ||
          a.alpha_blend_operation != b.alpha_blend_operation || a.src_rgb_blend_factor != b.src_rgb_blend_factor ||
          a.dst_rgb_blend_factor != b.dst_rgb_blend_factor || a.src_alpha_blend_factor != b.src_alpha_blend_factor ||
          a.dst_alpha_blend_factor != b.dst_alpha_blend_factor)
        return false;
    }
    return true;
  }

private:
  WMT::Device m_device;
  // Pin the shader wrappers (hence their modules, hence the bytecode the
  // function tasks read on a pool thread) for this task's whole life.
  Com<MTLD3D9VertexShader, false> m_vs;
  Com<MTLD3D9PixelShader, false> m_ps;
  WMTRenderPipelineInfo m_info;
  WMT::Reference<WMT::RenderPipelineState> m_state;
  // The function-compile dependencies. Non-owning: the module variant caches
  // (or the device FFP caches) own them for device lifetime, and this task's
  // m_vs / m_ps keep the owning module alive, so the pointers never dangle.
  D3D9CompiledFunction *m_vs_fn;
  D3D9CompiledFunction *m_ps_fn;
  mutable std::atomic<bool> m_ready{false};
};

// task_trait specialisation methods. Out-of-class so the bodies live in
// a single TU; visible to the device ctor below where m_psoScheduler is
// constructed (which instantiates task_scheduler<D3D9AsyncTask*>::
// worker_func, which calls these). The trait dispatches virtually, so the
// one scheduler drives both the PSO-link task and the function-compile
// tasks. Same shape as d3d11_pipeline_cache.cpp.
D3D9AsyncTask *
task_trait<D3D9AsyncTask *>::run_task(D3D9AsyncTask *task) {
  return task->RunTask();
}
bool
task_trait<D3D9AsyncTask *>::get_done(D3D9AsyncTask *task) {
  return task->GetDone();
}
void
task_trait<D3D9AsyncTask *>::set_done(D3D9AsyncTask *task) {
  task->SetDone(true);
}

// Shader-module variant caches submit their compile tasks through here (the
// device owns the scheduler). Thread-agnostic: submit is internally locked.
void
MTLD3D9Device::submitAsyncTask(D3D9AsyncTask *task) {
  m_psoScheduler.submit(task);
}

// D3D9 reprograms the x87 FPU at device creation (unless D3DCREATE_FPU_PRESERVE):
// single precision (24-bit mantissa), all exceptions masked, round-to-nearest.
// 32-bit games carry their float math on x87, so without this their CPU-side
// work (culling, LOD, exposure / tonemap divisors, matrix prep) runs at the
// default extended precision and diverges from Windows. Ported from wined3d
// device.c setup_fpu and DXVK SetupFPU; the mask is identical, (cw & 0xf0c0) |
// 0x003f. x86 only: SSE-compiled x86_64 code ignores the x87 word, but native
// reprograms it there too (the conformance suite expects that on 64-bit).
static void
setupFpu() {
#if defined(__i386__) || (defined(__x86_64__) && !defined(__arm64ec__))
  uint16_t control;
  __asm__ __volatile__("fnstcw %0" : "=m"(*&control));
  control &= 0xf0c0;
  control |= 0x003f;
  __asm__ __volatile__("fldcw %0" : : "m"(*&control));
#endif
}

MTLD3D9Device::MTLD3D9Device(
    MTLD3D9Interface *parent, bool isEx, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow, DWORD behaviorFlags,
    const D3DPRESENT_PARAMETERS &validatedParams, WMT::Reference<WMT::Device> &&metalDevice
) :
    m_parent(parent),
    m_isEx(isEx),
    m_multithread(behaviorFlags & D3DCREATE_MULTITHREADED),
    m_metalDevice(std::move(metalDevice)),
    m_residency(std::make_shared<dxmt::DeviceResidency>(m_metalDevice)),
    m_dxmtQueue(std::make_unique<dxmt::CommandQueue>(m_metalDevice, m_residency)),
    m_internalCmdLib(m_metalDevice),
    m_clearQuad(m_metalDevice, m_internalCmdLib),
    m_constRing(
        {m_metalDevice, static_cast<WMTResourceOptions>(
                            WMTResourceCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                            WMTResourceStorageModeShared
                        )}
    ),
    m_uploadRing(
        {m_metalDevice, static_cast<WMTResourceOptions>(
                            WMTResourceCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                            WMTResourceStorageModeShared
                        )}
    ),
    m_constRingResolve(
        {m_metalDevice, static_cast<WMTResourceOptions>(
                            WMTResourceCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                            WMTResourceStorageModeShared
                        )},
        // Written only by the encode thread (Resolve allocates, commit seals);
        // opt into the DXMT_DEBUG single-writer assertion.
        /*single_writer=*/true
    ) {
  // Match Windows D3D9 float behaviour on the app's creating thread before any
  // device work; D3DCREATE_FPU_PRESERVE opts out (wined3d / DXVK gate the same).
  if (!(behaviorFlags & D3DCREATE_FPU_PRESERVE))
    setupFpu();
  m_completionEvent = m_metalDevice.newSharedEvent();
  // Pre-allocate 2 blocks per ring: first-touch of a fresh
  // Metal-registered block page-faults expensively under Rosetta
  // x86_32, so pay it at device construction instead of mid-frame.
  // GPU completion recycles the blocks indefinitely, so 2 is enough
  // headroom for typical CPU/GPU overlap.
  m_constRing.preallocate(2);
  m_uploadRing.preallocate(2);
  m_constRingResolve.preallocate(2);
  m_creationParams.AdapterOrdinal = adapter;
  m_creationParams.DeviceType = deviceType;
  m_creationParams.hFocusWindow = focusWindow;
  // Seed the fullscreen activation latch: whatever is foreground at
  // create time is the baseline, so the first ownership poll only reacts
  // to a subsequent activation change (wine's device state starts OK).
  m_lastForegroundSample.store(wsi::foregroundWindow(), std::memory_order_relaxed);
  m_creationParams.BehaviorFlags = behaviorFlags;
  // A pure-SWVP device starts in software vertex processing (DXVK m_isSWVP,
  // seeded from D3DCREATE_SOFTWARE_VERTEXPROCESSING); MIXED and HWVP devices
  // start in hardware. Get/SetSoftwareVertexProcessing echo this bit.
  m_isSWVP = (behaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0;
  // A software or mixed VP device exposes the extended 8192 float register file
  // (DXVK sizes its constant set the same way), independent of the runtime
  // SetSoftwareVertexProcessing toggle. Allocate the c256..c8191 overflow once;
  // a hardware-VP device caps at 256 and never allocates it.
  if (behaviorFlags & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)) {
    m_vsConstFCount = D3D9_MAX_VS_CONST_F_SWVP;
    m_vsConstantsFOverflow =
        std::make_unique<float[]>(static_cast<size_t>(D3D9_MAX_VS_CONST_F_SWVP - D3D9_MAX_VS_CONST_F) * 4);
  }
  m_presentParams = validatedParams;
  // hDeviceWindow falls back to hFocusWindow per D3D9 spec; see
  // wined3d device.c. Smokes pass NULL for both, which the chain
  // treats as headless.
  HWND effectiveWindow = validatedParams.hDeviceWindow ? validatedParams.hDeviceWindow : focusWindow;
  m_implicitSwapChain = new MTLD3D9SwapChain(this, isEx, validatedParams, effectiveWindow, /*isImplicit=*/true);
  // Pin the chain alive against the device's lifetime; its public
  // refcount is driven entirely by GetSwapChain handouts, this one is
  // private. Released in our destructor.
  m_implicitSwapChain->AddRefPrivate();

  // A device created fullscreen resizes its device window to the borderless
  // backbuffer rect up front, the same as a windowed->fullscreen Reset; wined3d
  // does this in swapchain_init when the implicit chain is not windowed.
  if (!validatedParams.Windowed) {
    enterFullscreenWindow(effectiveWindow, validatedParams.BackBufferWidth, validatedParams.BackBufferHeight);
    hookFocusWindowProc(effectiveWindow);
  }

  // Seed the device-wide queue latency from the primary (implicit) chain.
  // The clamp is min(frameLatency, BackBufferCount + 1) per DXVK
  // d3d9_swapchain.cpp GetActualFrameLatency; without it the queue runs at
  // the hardcoded max_latency_=3 default, ~2 vsync intervals of Present
  // Delay at 60Hz for a BackBufferCount=1 chain. Present re-clamps the same
  // value every frame; this lives on the device, not the swapchain ctor, so
  // creating an additional swapchain never perturbs device-wide pacing.
  dxmtQueue().SetMaxLatency(std::min(getFrameLatency(), validatedParams.BackBufferCount + 1u));

  // Sampler / texture-stage / render-state power-on defaults via the
  // same free functions resetStateToDefaults calls, so the ctor and
  // Reset can't drift. The texture-stage seed matters as much as the
  // sampler one: without it a pre-Reset GetTextureStageState(0,
  // D3DTSS_COLOROP) reads 0 instead of D3DTOP_MODULATE (wined3d
  // stateblock.c seeds both at device init).
  init_default_sampler_states(m_samplerStates, D3D9_MAX_TEXTURE_UNITS);
  init_default_texture_stage_states(m_textureStageStates, 8);
  initDefaultRenderStates(validatedParams.EnableAutoDepthStencil != 0);

  // Transform state defaults: identity matrices everywhere.
  // wined3d stateblock.c / DXVK ResetState seeds the same way.
  for (uint32_t i = 0; i < kMaxTransforms; ++i) {
    D3DMATRIX m = {};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    m_transforms[i] = m;
  }

  // SetStreamSourceFreq defaults to 1 per stream: "draw 1 instance,
  // step per-vertex". DXVK seeds the same way (d3d9_device.cpp).
  for (uint32_t i = 0; i < D3D9_MAX_VERTEX_STREAMS; ++i)
    m_streamFreq[i] = 1;

  // FFP material default is all-zero: wined3d (stateblock_state_init_default
  // leaves it zero), DXVK and d9vk all default D3DMATERIAL9 to {}. An app that
  // enables lighting without a SetMaterial renders black, the documented D3D9
  // behavior.
  m_material = {};

  // Auto-bind implicit backbuffer to RT0 (also reseeds viewport+scissor
  // to the RT extent). wined3d device.c device_init.
  SetRenderTarget(0, m_implicitSwapChain->backBuffer());

  // Auto-DS: implicit surface per D3D9 spec. Private storage (Memoryless cannot
  // survive encoder boundary; batched draws split frames). Load/Store across encoders.
  createAutoDepthStencil(validatedParams);

  // Each BatchedDraw carries a per-draw pod_snapshot. Mark every axis
  // dirty so the next QueueBatchedDraw captures a fresh COW snapshot
  // off the just-initialised shadows.
  m_encShadowDirty = dxmt::D9ES_DIRTY_ALL;
  // Ref-counted state needs no reseed here: the SetRef ops queued by
  // the ctor binds above install m_encodeSideRefs in arrival order on
  // the encode thread.
}

void
MTLD3D9Device::resetStateToDefaults(bool enableAutoDepthStencil) {
  // Sampler-state, texture-stage-state and render-state power-on
  // defaults. The tables live in d3d9_state_defaults.cpp as free
  // functions so they can be checked host-native against the reference
  // (wined3d / the Wine d3d9 conformance suite) without a device.
  init_default_sampler_states(m_samplerStates, D3D9_MAX_TEXTURE_UNITS);
  init_default_texture_stage_states(m_textureStageStates, 8);
  initDefaultRenderStates(enableAutoDepthStencil);
  // Transform state defaults: identity matrices everywhere. The cached
  // world*view*projection product must go stale with them or a
  // fixed-function draw right after Reset transforms by the old scene.
  for (uint32_t i = 0; i < kMaxTransforms; ++i) {
    D3DMATRIX m = {};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    m_transforms[i] = m;
  }
  m_ffpWVPStale = true;
  // SetStreamSourceFreq defaults to 1 per stream. Push SetRef(null)
  // ops alongside the calling-thread shadow clears so m_encodeSideRefs
  // stays in lockstep with the post-Reset zero-state; without these
  // the encode-side mirror would carry stale refs to surfaces /
  // textures / buffers that the app is about to release through Reset.
  for (uint32_t i = 0; i < D3D9_MAX_VERTEX_STREAMS; ++i) {
    m_streamFreq[i] = 1;
    m_streamOffsets[i] = 0;
    m_streamStrides[i] = 0;
    if (m_vertexBuffers[i].ptr())
      QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::VertexBuffer0 + i), nullptr);
    m_vertexBuffers[i] = nullptr;
  }
  if (m_indexBuffer.ptr())
    QueueRefOp(PendingRefOp::IndexBuffer, nullptr);
  m_indexBuffer = nullptr;
  if (m_vertexDeclaration.ptr())
    QueueRefOp(PendingRefOp::VertexDeclaration, nullptr);
  m_vertexDeclaration = nullptr;
  if (m_vertexShader.ptr())
    QueueRefOp(PendingRefOp::VertexShader, nullptr);
  m_vertexShader = nullptr;
  if (m_pixelShader.ptr())
    QueueRefOp(PendingRefOp::PixelShader, nullptr);
  m_pixelShader = nullptr;
  m_fvf = 0;
  // Bound textures: drop all D3D9_MAX_TEXTURE_UNITS slots (PS + VS).
  for (uint32_t i = 0; i < D3D9_MAX_TEXTURE_UNITS; ++i) {
    if (m_textures[i].ptr())
      QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::Texture0 + i), nullptr);
    m_textures[i] = nullptr;
  }
  // FFP material default is all-zero (wined3d / DXVK / d9vk); see the ctor.
  m_material = {};
  // Lights + clip planes: empty.
  m_lights.clear();
  m_lightEnables.clear();
  std::memset(m_clipPlanes, 0, sizeof(m_clipPlanes));
  // VS/PS constants: zero per spec. The extended SWVP file (c256..c8191) is
  // zeroed too when present so a post-Reset Get reads 0 like the hot file.
  std::memset(m_vsConstantsF, 0, sizeof(m_vsConstantsF));
  if (m_vsConstantsFOverflow)
    std::memset(
        m_vsConstantsFOverflow.get(), 0,
        static_cast<size_t>(D3D9_MAX_VS_CONST_F_SWVP - D3D9_MAX_VS_CONST_F) * 4 * sizeof(float)
    );
  std::memset(m_vsConstantsI, 0, sizeof(m_vsConstantsI));
  std::memset(m_vsConstantsB, 0, sizeof(m_vsConstantsB));
  std::memset(m_psConstantsF, 0, sizeof(m_psConstantsF));
  std::memset(m_psConstantsI, 0, sizeof(m_psConstantsI));
  std::memset(m_psConstantsB, 0, sizeof(m_psConstantsB));
  // Reset the const-F coverage trackers so the post-Reset upload
  // clamp starts at minimum again. Sticky-monotonic across the device
  // *between* Resets only.
  m_vsConstFMax = 0;
  m_psConstFMax = 0;
  // POD state is now per-draw via BatchedDraw::pod_snapshot; mark
  // every axis dirty so the next QueueBatchedDraw takes a fresh
  // snapshot off the just-reset shadows.
  m_encShadowDirty = dxmt::D9ES_DIRTY_ALL;
  // REF state lives on the encode-side m_encodeSideRefs mirror. The
  // SetRef(null) ops queued above bump m_encodeSideRefsGen as the
  // encode-thread walker applies them, invalidating stale cluster
  // caches; the walker is that gen's sole writer, so there is no
  // inline bump here.
}

void
MTLD3D9Device::initDefaultRenderStates(bool enableAutoDepthStencil) {
  // D3D9 spec defaults from DXVK reference (same D3DRS_* indexing).
  // Float states stored as IEEE-754 bit pattern; apps Set/Get as DWORD.
  init_default_render_states(m_renderStates, enableAutoDepthStencil);
}

bool
MTLD3D9Device::acquireBufferBacking(
    size_t size, WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned
) {
  std::lock_guard<dxmt::mutex> lock(m_bufferBackingPoolMutex);
  for (auto it = m_bufferBackingPool.begin(); it != m_bufferBackingPool.end(); ++it) {
    if (it->capacity == size) {
      out_buffer = std::move(it->buffer);
      out_owned = it->owned_backing;
      out_host = it->owned_backing;
      out_gpu = it->gpu_address;
      m_bufferBackingPoolBytes -= it->capacity;
      m_bufferBackingPool.erase(it);
      // Restore the blank-backing invariant the cold aligned_malloc path (the
      // texture mirror and buffer-backed create both memset a fresh backing)
      // guarantees at this one shared handout point. A recycled backing still
      // holds the donor resource's last image, so a consumer that samples a
      // level before its first Lock upload would alias that stale content.
      // Donations are GPU-idle by precondition (see releaseBufferBacking) and
      // the pages are already faulted, so this is a bandwidth-only zero that
      // keeps the pool's newBuffer XPC and first-touch-cliff win.
      std::memset(out_owned, 0, size);
      return true;
    }
  }
  return false;
}

void
MTLD3D9Device::releaseBufferBacking(
    WMT::Reference<WMT::Buffer> &&buffer, void *owned, uint64_t gpu_address, size_t capacity
) {
  // Precondition: the donated backing is GPU-idle. The only callers are
  // resource dtors, and a bound resource's wrapper is pinned by the chunk's
  // resolved pins until the GPU retires it, so the dtor (and this donation)
  // runs only once no in-flight cmdbuf still reads the backing. GPU-idle
  // does not mean calling-thread: a chunk-pinned wrapper destructs on the
  // queue's encode or finish thread, so the pool lock serializes this
  // donation against the calling thread's acquire.
  // Device teardown: member destruction order can run this after the pool
  // vector is gone; free directly (the GPU was quiesced at dtor entry).
  std::lock_guard<dxmt::mutex> lock(m_bufferBackingPoolMutex);
  if (m_tearingDown || m_bufferBackingPool.size() >= kMaxBufferBackingPoolSize ||
      m_bufferBackingPoolBytes + capacity > kMaxBufferBackingPoolBytes) {
    // Pool full (by count or by bytes): drop on the floor. The moved-in
    // WMT::Reference releases the Metal buffer when it goes out of scope
    // below; we free the wsi backing here.
    buffer = WMT::Reference<WMT::Buffer>{};
    if (owned)
      wsi::aligned_free(owned);
    return;
  }
  BufferBackingPoolEntry entry;
  entry.buffer = std::move(buffer);
  entry.owned_backing = owned;
  entry.gpu_address = gpu_address;
  entry.capacity = capacity;
  m_bufferBackingPoolBytes += capacity;
  m_bufferBackingPoolPeak = std::max(m_bufferBackingPoolPeak, m_bufferBackingPoolBytes);
  m_bufferBackingPool.push_back(std::move(entry));
}

MTLD3D9Device::~MTLD3D9Device() {
  // Buffer wrappers can drop their last reference from ANY later member's
  // destructor (the encode-side refs, a bound stream, an unfinished
  // recording block), and member teardown runs after the backing pool's
  // drain below; from here on releaseBufferBacking frees donations
  // directly instead of pushing into a pool that may already be gone.
  // Latched under the pool lock so an encode / finish thread donation
  // in flight observes either pre-teardown pooling or the direct free,
  // never a torn state.
  {
    std::lock_guard<dxmt::mutex> lock(m_bufferBackingPoolMutex);
    m_tearingDown = true;
  }
  // Address-space diagnostic. DEFAULT+DYNAMIC buffers map Direct, so a
  // DISCARD-heavy frame stacks several CpuPlaced renames in flight, each
  // holding low-4GB space until its command buffer retires. The figure sums
  // every live CpuPlaced allocation, so it also folds in the i386 staging
  // ring's placed baseline: it is the resident placed footprint, not the
  // dynamic renames alone. Report that high-water and the staged mirror
  // pool's high-water so a run approaching the 32-bit address-space wall is
  // visible without an external profiler. Peaks are stable, so teardown is a
  // fine sampling point.
  if (d9DebugEnabled())
    INFO(
        "D3D9: peak live CpuPlaced ", dxmt::cpu_placed_peak_bytes() >> 10, " KiB, peak buffer backing pool ",
        m_bufferBackingPoolPeak >> 10, " KiB"
    );
  // Restore the device window to its windowed style/rect so a fullscreen
  // game leaving does not strand a borderless, topmost window. No-op unless
  // a fullscreen window is currently held. unhook restores the focus window's
  // wndproc so our forwarding proc never outlives the device.
  leaveFullscreenWindow();
  unhookFocusWindowProc();
  // Drain the encoder + cmdbuf before anything else; they hold Metal
  // handles whose dispose path needs the queue and device alive. The
  // local autorelease pool is required because flushOpenWork's
  // endEncoding/commit go through autoreleased Metal selectors and
  // wine's main thread has no outer NSAutoreleasePool.
  {
    auto pool = WMT::MakeAutoreleasePool();
    // Drain queued draws (including any pre-draw mip-gen ops already in the
    // op stream) plus any pending clear onto chunks first (FlushDrawBatch then
    // flushOpenWork's drainPendingClear). Then commit and wait so resources
    // captured in chunk lambdas (Rc<>'s, Com<>'s, retained Metal buffers) drop
    // before the device's other fields tear down underneath them. A texture
    // left mips-dirty and never drawn needs no teardown regen (no sampler).
    // Gate on the master op queue, not m_pendingDraws: a SetRef op (a
    // SetRenderTarget / SetTexture / Set* with no following draw) queues
    // into m_pendingOps with one outstanding AddRefPrivate that only the
    // chunk walker's ApplyRefOp_d9 balances into m_encodeSideRefs. Without
    // a draw m_pendingDraws is empty but m_pendingOps is not, so guarding
    // on draws would orphan that ref and leak the bound resource (e.g. the
    // implicit render target) past teardown, including its private data.
    if (!m_pendingOps.empty())
      FlushDrawBatch();
    flushOpenWork();
    if (m_dxmtQueue) {
      // Per-FlushDrawBatch signalEvents were dropped (encoder-coalesce
      // unblock), so a tail-signal must be emitted explicitly before
      // CommitCurrentChunk to advance m_completionEvent. Otherwise the
      // m_completionEvent.waitUntilSignaledValue below would block
      // forever waiting for a value never set.
      emitCmdbufTailSignal();
      uint64_t seq = m_dxmtQueue->CurrentSeqId();
      commitCurrentChunkTimed();
      m_dxmtQueue->WaitCPUFence(seq);
    }
    // Wait for the GPU to retire every cmdbuf we ever submitted before
    // the staging allocator's destructor frees its placed-buffer host
    // backings. free(mapped_address) inside StagingBufferBlockAllocator::
    // Block's dtor would otherwise yank memory out from under live GPU
    // reads. m_currentCmdSeq was incremented past the last committed
    // cmdbuf; waiting for (m_currentCmdSeq - 1) covers the most recent
    // commit and is a no-op if the GPU has already caught up.
    if (m_currentCmdSeq > 1)
      waitForGpuOrDeviceError(m_currentCmdSeq - 1);
    // Drain the FIFO under a known-quiescent GPU so all blocks land in
    // the "adhoc or expired" recycle branch and dispose cleanly.
    m_constRing.free_blocks(static_cast<uint64_t>(-1));
    m_uploadRing.free_blocks(static_cast<uint64_t>(-1));
    m_constRingResolve.free_blocks(static_cast<uint64_t>(-1));
    // Drain the buffer-backing pool. Entries' WMT::References release
    // their Metal buffers via the vector's element destructors; the
    // wsi backings need explicit aligned_free since BufferBackingPoolEntry
    // has no destructor of its own. The WaitCPUFence above quiesced the
    // queue threads, but take the pool lock anyway so every pool access
    // is uniformly serialized.
    {
      std::lock_guard<dxmt::mutex> lock(m_bufferBackingPoolMutex);
      for (auto &entry : m_bufferBackingPool) {
        if (entry.owned_backing)
          wsi::aligned_free(entry.owned_backing);
      }
      m_bufferBackingPool.clear();
      m_bufferBackingPoolBytes = 0;
    }
  }
  // A BeginStateBlock with no matching EndStateBlock leaves the
  // recording block holding only its ctor self-pin (no public ref was
  // ever handed out), so nothing else will ever destruct it. Drop the
  // pin before teardown.
  if (m_recordingBlock) {
    auto *sb = m_recordingBlock;
    m_recordingBlock = nullptr;
    sb->ReleasePrivate();
  }
  // Tear the implicit chain down explicitly so it has the queue + device
  // available while it releases any Metal handles. After this returns,
  // member destruction in reverse declaration order finishes off the
  // queue and Metal device.
  if (m_implicitSwapChain) {
    m_implicitSwapChain->ReleasePrivate();
    m_implicitSwapChain = nullptr;
  }
  // Fan-list IB cleanup. Drop the MTLBuffer reference first (releases
  // the Metal NSObject's retain on the placement) then free the host
  // backing. Same ordering rule as the texture mirror in
  // MTLD3D9Texture's dtor.
  m_fanListIB = WMT::Reference<WMT::Buffer>{};
  if (m_fanListIBBacking) {
    wsi::aligned_free(m_fanListIBBacking);
    m_fanListIBBacking = nullptr;
  }
#ifdef _WIN32
  if (m_hwCursor)
    ::DestroyCursor(m_hwCursor);
#endif
}

dxmt::CommandQueue &
MTLD3D9Device::dxmtQueue() const {
  return *m_dxmtQueue;
}

void
MTLD3D9Device::initTextureWithZero(dxmt::Texture *texture) {
  auto &initializer = dxmtQueue().initializer;
  // arrayLength() already folds a cube's 6 faces in, and miplevelCount() is the
  // realized Metal mip count; a 3D texture keeps arrayLength()==1 and its depth
  // is covered inside one per-level zero. This is the d3d11 per-subresource
  // walk (EnumerateSubresources) expressed over dxmt::Texture's own metadata.
  const uint32_t slices = texture->arrayLength();
  const uint32_t levels = texture->miplevelCount();
  for (uint32_t slice = 0; slice < slices; ++slice)
    for (uint32_t level = 0; level < levels; ++level)
      initializer.initWithZero(texture, texture->current(), slice, level);
}

void
MTLD3D9Device::recordPresentThreadCounters() {
  auto *perf_stats = frameStats();
  if (!perf_stats)
    return;
  WMTThreadCounters now = {};
  WMTQueryThreadCounters(&now);
  // Skip the first present: with no prior sample the delta would be everything
  // since process start, which would read as one frame consuming all of it.
  if (m_havePresentCounters) {
    const auto &was = m_lastPresentCounters;
    const auto delta = [](uint64_t current, uint64_t previous) -> uint64_t {
      return current >= previous ? current - previous : 0;
    };
    if (now.valid & 1) {
      perf_stats->frame_thread_user_interval += std::chrono::duration_cast<dxmt::clock::duration>(
          std::chrono::microseconds(delta(now.thread_user_us, was.thread_user_us))
      );
      perf_stats->frame_thread_kernel_interval += std::chrono::duration_cast<dxmt::clock::duration>(
          std::chrono::microseconds(delta(now.thread_system_us, was.thread_system_us))
      );
    }
    if (now.valid & 2) {
      perf_stats->frame_task_fault_count += delta(now.task_faults, was.task_faults);
      perf_stats->frame_task_pagein_count += delta(now.task_pageins, was.task_pageins);
      perf_stats->frame_task_cow_fault_count += delta(now.task_cow_faults, was.task_cow_faults);
    }
    if (now.valid & 4)
      perf_stats->frame_host_zero_fill_count += delta(now.vm_zero_fill_count, was.vm_zero_fill_count);
  }
  m_lastPresentCounters = now;
  m_havePresentCounters = true;
}

void
MTLD3D9Device::commitCurrentChunkTimed() {
  dxmt::perf::ScopedFrameDurationCounted _commit_timer(
      frameStats(), &dxmt::FrameStatistics::frame_chunk_commit_interval,
      &dxmt::FrameStatistics::frame_chunk_commit_count
  );
  // Per command-buffer ring seal. A CPU store into a placed Metal buffer an
  // in-flight command buffer references faults on every store under x86
  // translation on macOS 27 Beta 3, so no ring block may receive writes for
  // a later command buffer while this one is still in flight. Sealing at the
  // commit boundary rotates each ring onto a fresh block for the next command
  // buffer; the many flushes that packed into this chunk keep sharing its
  // block, so the block count tracks in-flight command buffers, not flushes.
  //
  // Each ring has exactly one writer, which is what keeps allocate() and
  // seal_latest() from racing. m_constRingResolve is written only by the
  // encode thread (Resolve allocates from it, and this seal runs as the
  // chunk's final encode command, so it must be emitted onto the chunk, not
  // called here); it carries the DXMT_DEBUG single-writer assertion.
  // m_constRing and m_uploadRing are written only by the calling thread
  // (draw / upload recording allocates, and this commit seals), which is why
  // they are sealed here directly rather than emitted; their OS thread can
  // change across calls under D3DCREATE_MULTITHREADED, so they are serialised
  // by the device lock rather than pinned to one thread id.
  m_dxmtQueue->CurrentChunk()->emitcc([this](ArgumentEncodingContext &) { m_constRingResolve.seal_latest(); });
  m_constRing.seal_latest();
  m_uploadRing.seal_latest();
  m_dxmtQueue->CommitCurrentChunk();
  // The allocations renamed into this command buffer become recyclable once it
  // retires, so the pressure this counter tracks is now the next one's to carry.
  m_renamedBytesSinceCommit = 0;
}

// Sampler cache lookup. Builds the prefix key from the input info,
// reuses on hit; on miss invokes the dxmt::Sampler factory shared
// with d3d11 (src/dxmt/dxmt_sampler.cpp) and inserts. Insertion
// is unconditional even on factory failure so a repeatedly bad
// descriptor doesn't burn a Metal round-trip every draw.
Rc<Sampler>
MTLD3D9Device::getOrCreateSampler(const WMTSamplerInfo &info) {
  SamplerKey key = samplerKeyFromInfo(info);
  if (auto it = m_samplerCache.find(key); it != m_samplerCache.end())
    return it->second;
  auto sampler = Sampler::createSampler(
      m_metalDevice, info,
      /*lod_bias=*/0.0f
  );
  auto [ins, _] = m_samplerCache.emplace(key, std::move(sampler));
  return ins->second;
}

obj_handle_t
MTLD3D9Device::dummyFragmentTexture(WMTTextureType type) {
  // Opaque-black 1x1 placeholder for a sampled-but-unbound stage, pixel or
  // vertex: the texture is ShaderRead, so the bind stage is the only
  // difference and the same dummies serve both. Typed per sampler kind because
  // a 2D bind on a 3D/cube sampler is a Metal type mismatch (wined3d keeps
  // per-type dummies too). The fill is (0, 0, 0, 1): D3D9 unbound samples read
  // opaque black (wined3d's WINED3D_LEGACY_UNBOUND_RESOURCE_COLOR), and shaders
  // that sample an intentionally-unbound stage rely on the alpha being 1.
  uint32_t idx;
  switch (type) {
  case WMTTextureType3D:
    idx = 1;
    break;
  case WMTTextureTypeCube:
    idx = 2;
    break;
  default:
    idx = 0;
    type = WMTTextureType2D;
    break;
  }
  if (m_dummyFragTexHandle[idx] != 0)
    return m_dummyFragTexHandle[idx];
  WMTTextureInfo info{};
  info.pixel_format = WMTPixelFormatBGRA8Unorm;
  info.width = 1;
  info.height = 1;
  info.depth = 1;
  // Metal TextureCube is one texture with 6 implicit slices (array_length
  // selects Cube vs CubeArray), so array_length stays 1 for all three.
  info.array_length = 1;
  info.type = type;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsageShaderRead;
  // Shared (not Private) so the fill below is CPU-writable.
  info.options = WMTResourceStorageModeShared;
  Rc<dxmt::Texture> tex = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  auto allocation = tex->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return 0; // creation failed; resolve binds null (no worse than before)
  // BGRA8 bytes (B, G, R, A) = opaque black; see the header comment.
  const uint32_t black = 0xFF000000u;
  const uint32_t slices = (type == WMTTextureTypeCube) ? 6u : 1u;
  for (uint32_t slice = 0; slice < slices; ++slice)
    allocation->texture().replaceRegion(
        WMTOrigin{0, 0, 0}, WMTSize{1, 1, 1}, 0, slice, &black, sizeof(black), sizeof(black)
    );
  m_dummyFragTexHandle[idx] = allocation->texture().handle;
  tex->rename(std::move(allocation));
  m_dummyFragTexAlloc[idx] = std::move(tex);
  return m_dummyFragTexHandle[idx];
}

// DSSO cache lookup. Mirrors the sampler-cache shape above and
// d3d11's StateObjectCache<D3D11_DEPTH_STENCIL_DESC, ...>. WMTDepthStencilInfo
// is the natural key: fully-specified 32-byte POD with no Metal-side
// out-fields to mask. On miss, defer the m_metalDevice.newDepthStencilState
// round-trip; on hit, reuse the WMT::Reference held in the cache.
WMT::DepthStencilState
MTLD3D9Device::getOrCreateDSSO(const WMTDepthStencilInfo &info) {
  DepthStencilKey key{info};
  if (auto it = m_dssoCache.find(key); it != m_dssoCache.end())
    return WMT::DepthStencilState{it->second.handle};
  auto dsso = m_metalDevice.newDepthStencilState(info);
  auto [ins, _] = m_dssoCache.emplace(key, std::move(dsso));
  return WMT::DepthStencilState{ins->second.handle};
}

void
MTLD3D9Device::flushOpenWork() {
  // Drain any pending Clear (D3D9 Clear is eager; apps that issue
  // Clear and then immediately Present / GetRenderTargetData / blit
  // expect the targeted attachments to be wiped) onto the current chunk
  // via chunk->emitcc on CurrentChunk(); the chunk's EncodingThread
  // replays it in emit order. No sync cmdbuf is built here: all sync
  // paths now route through chunks. The AUTOGENMIPMAP mip-gen sweep does
  // NOT run here: it runs pre-draw (sweepBoundAutogenMips) so a mip-gen
  // op precedes the draws that sample the chain, not the whole batch.
  drainPendingClear();
}

void
MTLD3D9Device::drainPendingClear() {
  // No-op if there's nothing to drain.
  if (!m_pendingClear.color_valid && !m_pendingClear.depth_valid && !m_pendingClear.stencil_valid)
    return;
  // Route through ArgumentEncodingContext clear methods (not empty render pass).
  // dxmt_context coalescer folds Clear->Render into next encoder's loadAction.
  // Fires only when no queued draws AND clear must reach GPU before next sync.
  MTLD3D9Surface *ds = m_depthStencilSurface.ptr();

  Rc<dxmt::Texture> ds_tex = ds ? ds->dxmtTexture() : nullptr;
  TextureViewKey ds_view = 0;
  bool ds_has_stencil = false;
  if (ds_tex) {
    ds_has_stencil = HasStencilAspect(ds->desc().Format);
    ds_view = ds_tex->createView({
        .format = ds->metalPixelFormat(),
        .type = surface_view_type(ds_tex.ptr()),
        .firstMiplevel = static_cast<uint16_t>(ds->mipLevel()),
        .miplevelCount = 1,
        .firstArraySlice = static_cast<uint16_t>(ds->arraySlice()),
        .arraySize = 1,
    });
  }

  PendingClear pc = m_pendingClear;
  m_pendingClear = {};

  bool have_any_color = false;
  if (pc.color_valid)
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      MTLD3D9Surface *rt = m_renderTargets[i].ptr();
      if (rt && !IsNullFormat(rt->desc().Format) && rt->dxmtTexture()) {
        have_any_color = true;
        break;
      }
    }
  const bool have_ds_clear = ds_tex && (pc.depth_valid || (pc.stencil_valid && ds_has_stencil));
  if (!have_any_color && !have_ds_clear)
    return; // (D3D9's "no-RT Clear" no-op)

  auto *chunk = m_dxmtQueue->CurrentChunk();

  // D3D9 Clear(D3DCLEAR_TARGET) clears every bound render target, not just
  // slot 0 (DXVK loops m_state.renderTargets). A deferred renderer binding an
  // MRT G-buffer otherwise keeps stale content in the unflagged slots. Emit
  // one Clear encoder per bound colour target; each folds into the next
  // matching Render's loadAction.
  if (pc.color_valid) {
    const bool srgb_write_pass = m_renderStates[D3DRS_SRGBWRITEENABLE] != 0;
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      MTLD3D9Surface *rt = m_renderTargets[i].ptr();
      if (!rt || IsNullFormat(rt->desc().Format))
        continue;
      Rc<dxmt::Texture> rt_tex = rt->dxmtTexture();
      if (!rt_tex)
        continue;
      // D3DRS_SRGBWRITEENABLE clears through the sRGB-format view of the
      // target; the attachment hardware-encodes the clear colour on store
      // exactly as it encodes a fragment output, so the colour is passed
      // linear with no software pre-encode (DXVK clears through
      // GetRenderTargetView(srgb) the same way). Recall_sRGB_ForRenderTarget
      // returns the format unchanged when it has no sRGB pair, and strips the
      // X-format AlphaIsOne swizzle that would make the view sample-only.
      auto effective_fmt = rt->metalPixelFormat();
      if (srgb_write_pass)
        effective_fmt = Recall_sRGB_ForRenderTarget(effective_fmt);
      TextureViewKey rt_view = rt_tex->createView({
          .format = effective_fmt,
          .type = surface_view_type(rt_tex.ptr()),
          .firstMiplevel = static_cast<uint16_t>(rt->mipLevel()),
          .miplevelCount = 1,
          .firstArraySlice = static_cast<uint16_t>(rt->arraySlice()),
          .arraySize = 1,
      });
      WMTClearColor clear_color{pc.color[0], pc.color[1], pc.color[2], pc.color[3]};
      chunk->emitcc([rt_tex_cap = rt_tex, rt_view, clear_color](ArgumentEncodingContext &ctx) mutable {
        ctx.clearColor(std::move(rt_tex_cap), rt_view, 1, clear_color);
      });
    }
  }
  if (have_ds_clear) {
    unsigned flag = (pc.depth_valid ? 1u : 0u) | ((pc.stencil_valid && ds_has_stencil) ? 2u : 0u);
    float clear_depth = pc.depth_valid ? pc.depth : 0.0f;
    uint8_t clear_stencil = pc.stencil_valid ? pc.stencil : 0u;
    chunk->emitcc([ds_tex_cap = ds_tex, ds_view, flag, clear_depth,
                   clear_stencil](ArgumentEncodingContext &ctx) mutable {
      ctx.clearDepthStencil(std::move(ds_tex_cap), ds_view, 1, flag, clear_depth, clear_stencil);
    });
  }
}

void
MTLD3D9Device::emitClippedClear(
    const std::vector<WMTScissorRect> &regions, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil
) {
  auto *chunk = m_dxmtQueue->CurrentChunk();

  // The region list is in render-target space; an attachment can be
  // smaller than the viewport, so narrow per attachment and drop
  // empties (DXVK d3d9_device.cpp ClearImageView clamps against the
  // image extent the same way).
  auto clamp_regions = [&regions](uint32_t width, uint32_t height) {
    std::vector<WMTScissorRect> clamped;
    clamped.reserve(regions.size());
    for (const auto &r : regions) {
      if (r.x >= width || r.y >= height)
        continue;
      clamped.push_back({
          r.x,
          r.y,
          std::min<uint64_t>(r.width, width - r.x),
          std::min<uint64_t>(r.height, height - r.y),
      });
    }
    return clamped;
  };
  // A clamped region can still cover one attachment whole (smaller RT
  // in an MRT set, viewport sized to the DS but not the RT); that
  // attachment keeps the loadAction-folding clear (DXVK's fullClear
  // split).
  auto covers_whole = [](const std::vector<WMTScissorRect> &rects, uint32_t width, uint32_t height) {
    return rects.size() == 1 && rects[0].x == 0 && rects[0].y == 0 && rects[0].width == width &&
           rects[0].height == height;
  };

  if (Flags & D3DCLEAR_TARGET) {
    // Decode D3DCOLOR (0xAARRGGBB). DXVK DecodeD3DCOLOR same shape.
    const float color[4] = {
        ((Color >> 16) & 0xFF) / 255.0f,
        ((Color >> 8) & 0xFF) / 255.0f,
        (Color & 0xFF) / 255.0f,
        ((Color >> 24) & 0xFF) / 255.0f,
    };
    // D3D9 Clear(D3DCLEAR_TARGET) hits every bound colour target, not
    // just slot 0; see drainPendingClear's loop for the references.
    const bool srgb_write_pass = m_renderStates[D3DRS_SRGBWRITEENABLE] != 0;
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      MTLD3D9Surface *rt = m_renderTargets[i].ptr();
      if (!rt || IsNullFormat(rt->desc().Format))
        continue;
      Rc<dxmt::Texture> rt_tex = rt->dxmtTexture();
      if (!rt_tex)
        continue;
      auto rt_regions = clamp_regions(rt->desc().Width, rt->desc().Height);
      if (rt_regions.empty())
        continue;
      // See drainPendingClear: the sRGB-format view hardware-encodes the
      // clear on store, so the colour is passed linear.
      auto effective_fmt = rt->metalPixelFormat();
      if (srgb_write_pass)
        effective_fmt = Recall_sRGB_ForRenderTarget(effective_fmt);
      TextureViewKey rt_view = rt_tex->createView({
          .format = effective_fmt,
          .type = surface_view_type(rt_tex.ptr()),
          .firstMiplevel = static_cast<uint16_t>(rt->mipLevel()),
          .miplevelCount = 1,
          .firstArraySlice = static_cast<uint16_t>(rt->arraySlice()),
          .arraySize = 1,
      });
      if (covers_whole(rt_regions, rt->desc().Width, rt->desc().Height)) {
        WMTClearColor clear_color{color[0], color[1], color[2], color[3]};
        chunk->emitcc([rt_tex_cap = rt_tex, rt_view, clear_color](ArgumentEncodingContext &ctx) mutable {
          ctx.clearColor(std::move(rt_tex_cap), rt_view, 1, clear_color);
        });
        continue;
      }
      // The quad's fragment output stores through the sRGB view's
      // encode, so it takes the linear colour as-is.
      std::array<float, 4> quad_color = {color[0], color[1], color[2], color[3]};
      chunk->emitcc([quad = &m_clearQuad, rt_tex_cap = rt_tex, rt_view, rects = std::move(rt_regions),
                     quad_color](ArgumentEncodingContext &ctx) mutable {
        // First use of a format compiles the clear PSO on this thread;
        // wine's encode worker has no outer NSAutoreleasePool.
        auto pool = WMT::MakeAutoreleasePool();
        quad->begin(ctx, std::move(rt_tex_cap), rt_view);
        for (const auto &r : rects)
          quad->clear(ctx, r.x, r.y, r.width, r.height, quad_color);
        quad->end(ctx);
      });
    }
  }

  MTLD3D9Surface *ds = m_depthStencilSurface.ptr();
  Rc<dxmt::Texture> ds_tex = ds ? ds->dxmtTexture() : nullptr;
  const bool ds_has_stencil = ds && HasStencilAspect(ds->desc().Format);
  const unsigned ds_flag =
      ((Flags & D3DCLEAR_ZBUFFER) ? 1u : 0u) | (((Flags & D3DCLEAR_STENCIL) && ds_has_stencil) ? 2u : 0u);
  if (ds_tex && ds_flag) {
    auto ds_regions = clamp_regions(ds->desc().Width, ds->desc().Height);
    if (ds_regions.empty())
      return;
    TextureViewKey ds_view = ds_tex->createView({
        .format = ds->metalPixelFormat(),
        .type = surface_view_type(ds_tex.ptr()),
        .firstMiplevel = static_cast<uint16_t>(ds->mipLevel()),
        .miplevelCount = 1,
        .firstArraySlice = static_cast<uint16_t>(ds->arraySlice()),
        .arraySize = 1,
    });
    const float clear_depth = (Flags & D3DCLEAR_ZBUFFER) ? Z : 0.0f;
    const uint8_t clear_stencil = static_cast<uint8_t>(Stencil);
    if (covers_whole(ds_regions, ds->desc().Width, ds->desc().Height)) {
      chunk->emitcc([ds_tex_cap = ds_tex, ds_view, ds_flag, clear_depth,
                     clear_stencil](ArgumentEncodingContext &ctx) mutable {
        ctx.clearDepthStencil(std::move(ds_tex_cap), ds_view, 1, ds_flag, clear_depth, clear_stencil);
      });
      return;
    }
    chunk->emitcc([quad = &m_clearQuad, ds_tex_cap = ds_tex, ds_view, ds_flag, clear_depth, clear_stencil,
                   rects = std::move(ds_regions)](ArgumentEncodingContext &ctx) mutable {
      // Same first-use PSO compile concern as the colour quad above.
      auto pool = WMT::MakeAutoreleasePool();
      quad->beginDepthStencil(ctx, std::move(ds_tex_cap), ds_view, ds_flag, clear_stencil);
      for (const auto &r : rects)
        quad->clear(ctx, r.x, r.y, r.width, r.height, {clear_depth, 0.0f, 0.0f, 0.0f});
      quad->end(ctx);
    });
  }
}

void
MTLD3D9Device::generateMipmaps(WMT::Texture texture, const Rc<dxmt::Texture> &alloc, bool drain_pending_draws) {
  if (texture.handle == 0)
    return;
  // Metal's generateMipmaps requires mipmapLevelCount > 1; on a
  // 1-level texture the blit is invalid and wedges the command buffer.
  // Reset can legally re-run the AUTOGENMIPMAP path against a 1-level
  // texture, so no-op.
  if (texture.mipmapLevelCount() <= 1)
    return;
  // The walker derefs op.dst_tex (this alloc) for its level/slice counts and
  // access registration; a null wrapper would fault there. Every caller passes
  // a live dxmtTexture() so this is defensive, matching stageTextureUpload.
  if (!alloc.ptr())
    return;
  // drain_pending_draws=false from the pre-draw sweep: it queues this op into
  // the stream the draw is about to join, so draining would split the batch
  // and reorder the mip-gen after the earlier draws. The true default is for a
  // standalone GenerateMipSubLevels call, which flushes any open batch first so
  // its regeneration lands after the draws already queued; Metal serialises the
  // chunks in submission order.
  if (drain_pending_draws && !m_pendingOps.empty())
    FlushDrawBatch();

  // Ride the arrival-order op stream rather than emitting the blit directly.
  // The mip-gen reads level 0, which a same-flush Unlock upload or StretchRect
  // writes through the op stream too (the upload-hazard fix); queuing it lets
  // the walker emit it after those writes and, from the pre-draw sweep, before
  // the draw. No per-op completion signal: a mid-chunk signal folds a pre-bump
  // seq into m_cachedSignaled and recycles upload/const-ring blocks whose
  // consuming ops have not run yet. The op's dst_tex Rc pins the texture until
  // the walker emits; the access<> registration there orders it.
  PendingBlitOp op;
  op.kind = PendingBlitOp::Kind::GenerateMipmaps;
  op.dst_tex = alloc;
  QueueBlitOp(std::move(op));
}

void
MTLD3D9Device::sweepBoundAutogenMips() {
  // Runs just before a draw is pushed (QueueBatchedDraw): each bound
  // AUTOGENMIPMAP texture whose level 0 went dirty queues a mip-gen op into
  // the same arrival-order stream the draw is about to join, so the walker
  // emits the regeneration ahead of the draws that sample the chain (deduped
  // within this call across PS+VTF aliasing).
  bool any_dirty_mips = false;
  for (uint32_t slot = 0; slot < D3D9_MAX_TEXTURE_UNITS; ++slot) {
    auto *t = m_textures[slot].ptr();
    if (t && t->mipsDirty()) {
      any_dirty_mips = true;
      break;
    }
  }
  if (!any_dirty_mips)
    return;

  // Same texture may be bound at multiple stages (VTF + PS, or two PS
  // stages aliasing one source). Track handles we've already emitted
  // to avoid double generateMipmaps on the same Metal texture.
  obj_handle_t emitted[D3D9_MAX_TEXTURE_UNITS] = {};
  uint32_t emitted_count = 0;
  for (uint32_t slot = 0; slot < D3D9_MAX_TEXTURE_UNITS; ++slot) {
    auto *t = m_textures[slot].ptr();
    if (!t || !t->mipsDirty())
      continue;
    auto mt = t->metalTexture();
    bool seen = false;
    for (uint32_t i = 0; i < emitted_count; ++i)
      if (emitted[i] == mt.handle) {
        seen = true;
        break;
      }
    if (!seen) {
      // drain_pending_draws=false: queue the mip-gen op into the current
      // stream without flushing, so it sits right before the draw the caller
      // is about to push (a drain would split the batch and reorder it after
      // the earlier draws).
      generateMipmaps(mt, t->dxmtTexture(), /*drain_pending_draws=*/false);
      emitted[emitted_count++] = mt.handle;
    }
    t->clearMipsDirty();
  }
}

void
MTLD3D9Device::sweepBoundManagedUploads() {
  // Each bound texture pushes its own pending MANAGED levels from the mirror;
  // cube/volume leaves inherit the no-op default. sweepManagedUpload is
  // idempotent (it clears the pending mask), so a texture aliased across two
  // stages is uploaded once and the second visit no-ops.
  for (uint32_t slot = 0; slot < D3D9_MAX_TEXTURE_UNITS; ++slot) {
    auto *t = m_textures[slot].ptr();
    if (t)
      t->sweepManagedUpload();
  }
}

void
MTLD3D9Device::stageTextureUpload(
    WMT::Texture dst, const Rc<dxmt::Texture> &dst_alloc, uint32_t mip_level, uint32_t slice, WMTOrigin origin,
    WMTSize size, const void *src, uint32_t src_pitch, bool is_compressed, uint32_t src_slice_pitch
) {
  if (dst_alloc == nullptr || dst.handle == 0 || src == nullptr || src_pitch == 0 || size.width == 0 ||
      size.height == 0)
    return;
  // 3Dc fiction to BC reality: callers speak the app-facing linear layout
  // for ATI1/ATI2 (one byte per pixel, d3d9_format.cpp), while the Metal
  // texture is real BC4/BC5. The mirror's contiguous byte stream IS the
  // block stream, so the conversion is pitch and layout only; callers
  // clamp 3Dc uploads to the full level, so src is the level start and
  // size the level extent.
  if (!is_compressed && dst_alloc != nullptr &&
      (dst_alloc->pixelFormat() == WMTPixelFormatBC4_RUnorm || dst_alloc->pixelFormat() == WMTPixelFormatBC5_RGUnorm)) {
    const uint32_t block_bytes = dst_alloc->pixelFormat() == WMTPixelFormatBC4_RUnorm ? 8u : 16u;
    src_pitch = ((static_cast<uint32_t>(size.width) + 3u) / 4u) * block_bytes;
    is_compressed = true;
  }
  // Calling-thread staging cost: the ring alloc + memcpy + queue.
  auto *perf_stats = frameStats();
  dxmt::perf::ScopedFrameDuration _upload_timer(perf_stats, &dxmt::FrameStatistics::frame_staging_upload_interval);

  // Per-destination-slice + total staging bytes (texture_upload_layout
  // handles the compressed block-row rounding and the depth scaling).
  // Volume (3D) textures stage every depth slice, not just one; source
  // slices are spaced by src_slice_pitch (the full mip's slice stride,
  // which a sub-box upload makes larger than bytes_per_image), 0 meaning
  // contiguous (2D, or a full-box 3D upload). Ignoring depth left 3D
  // textures with only their first slice written, the rest reading
  // unallocated ring memory (zero); wined3d carries the slice pitch
  // through its upload for the same reason (texture.c
  // wined3d_texture_get_pitch).
  const uint32_t depth = size.depth ? static_cast<uint32_t>(size.depth) : 1u;
  const auto layout = texture_upload_layout(src_pitch, static_cast<uint32_t>(size.height), depth, is_compressed);
  const uint32_t bytes_per_image = layout.bytes_per_image;
  const uint32_t src_slice = src_slice_pitch ? src_slice_pitch : bytes_per_image;
  const size_t total_bytes = layout.total_bytes;
  dxmt::perf::addFrameCounter(perf_stats, &dxmt::FrameStatistics::frame_staging_upload_bytes, total_bytes);

  // Coherent_id reads the GPU's last signalled cmdbuf seq so the ring
  // can recycle blocks whose tag has retired. Same shape as the
  // per-draw uploads on the Resolve/EmitDrawBatch path. Cached value
  // refreshed at flushOpenWork (post-commit); saves a wine_unix_call
  // per stageTextureUpload invocation, which streaming workloads hit
  // hundreds of times per frame.
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  // 16-byte alignment matches the per-draw upload's VB/IB shape and
  // is sufficient for any format Metal accepts on this path. The
  // bump allocator pads to alignment but doesn't enforce a row
  // alignment beyond what src_pitch already encodes.
  auto span = tryRingAllocate(m_uploadRing, m_currentCmdSeq, coherent_id, total_bytes, 16);
  if (!span)
    return;
  char *staged = static_cast<char *>(span.host);
  if (src_slice == bytes_per_image) {
    std::memcpy(staged, src, total_bytes);
  } else {
    // Sub-box 3D upload: copy slice by slice, the staged slices packed
    // tightly while the source skips the rest of each full mip slice.
    for (uint32_t z = 0; z < depth; ++z)
      std::memcpy(
          staged + static_cast<size_t>(z) * bytes_per_image,
          static_cast<const char *>(src) + static_cast<size_t>(z) * src_slice, bytes_per_image
      );
  }

  // Ride the arrival-order op stream instead of emitting the blit directly:
  // a draw queued before this Lock (still un-flushed in m_pendingOps) must
  // sample the pre-upload contents, which only holds if the upload is emitted
  // at its arrival position by FlushDrawBatch. Same shape as the buffer upload
  // below; no seq bump, the block recycles when the chunk that reads it retires.
  PendingBlitOp op;
  op.kind = PendingBlitOp::Kind::BufferToTexture;
  op.dst_tex = dst_alloc;
  op.dst_mip = mip_level;
  op.dst_slice = slice;
  op.dst_origin = origin;
  op.size = size;
  op.buf_src_handle = span.handle;
  op.buf_src_offset = span.offset;
  op.tex_src_pitch = src_pitch;
  op.tex_bytes_per_image = bytes_per_image;
  QueueBlitOp(std::move(op));
}

bool
MTLD3D9Device::waitForGpuOrDeviceError(uint64_t value) {
  // Long enough that a healthy wait costs one wakeup, short enough that a dead
  // command buffer is noticed rather than waited on.
  constexpr uint64_t kSliceMilliseconds = 250;
  while (!m_completionEvent.waitUntilSignaledValue(value, kSliceMilliseconds)) {
    if (m_dxmtQueue && m_dxmtQueue->HasDeviceError())
      return false;
  }
  return true;
}

void
MTLD3D9Device::readbackSurfaceMirror(MTLD3D9Surface *surface) {
  // Local pool for the same reason as GetRenderTargetData: the commit
  // and wait below go through autoreleased Metal selectors and wine's
  // main thread has no outer NSAutoreleasePool.
  auto pool = WMT::MakeAutoreleasePool();
  const D3DSURFACE_DESC &desc = surface->desc();
  // 3Dc reads back through the real BC geometry into the fiction mirror's
  // head; every other format uses the mirror's own layout pitch.
  const uint32_t pitch =
      Is3DcFormat(desc.Format) ? D3DFormatMetalTransferPitch(desc.Format, desc.Width) : surface->pitch();
  const uint32_t width = desc.Width;
  const uint32_t height = desc.Height;
  // Byte counts run over block-rows, not pixel-rows: a compressed format packs
  // 4 texel rows per row of pitch, so pitch * height would over-read by 4x. The
  // blit's source size below stays in pixels (Metal blocks internally).
  const uint32_t row_count = D3DFormatMetalTransferRows(desc.Format, height);
  const size_t total_bytes = static_cast<size_t>(pitch) * row_count;
  if (surface->metalTexture().handle == 0 || surface->cpuPtr() == nullptr || total_bytes == 0)
    return;

  // Drain queued draws and any staged clear onto chunks first so the
  // readback sees them; the unconditional pair is the same sync-point
  // shape EndScene and Present use.
  FlushDrawBatch();
  flushOpenWork();

  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto span = tryRingAllocate(m_uploadRing, m_currentCmdSeq, coherent_id, total_bytes, 16);
  if (!span)
    return;
  const uint64_t offset = span.offset;

  WMT::Reference<WMT::Texture> src_tex_retain(surface->metalTexture());
  obj_handle_t src_texture_handle = surface->metalTexture().handle;
  obj_handle_t dst_buffer_handle = span.handle;
  uint32_t src_mip = surface->mipLevel();
  uint32_t src_slice = surface->arraySlice();

  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;

  auto *chunk = m_dxmtQueue->CurrentChunk();
  chunk->emitcc([src_tex_retain = std::move(src_tex_retain), src_texture_handle, dst_buffer_handle, offset, pitch,
                 width, height, row_count, src_mip, src_slice, event_handle,
                 signal_seq](ArgumentEncodingContext &ctx) mutable {
    ctx.startBlitPass();
    auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
    cmd.src = src_texture_handle;
    cmd.slice = src_slice;
    cmd.level = src_mip;
    cmd.origin = WMTOrigin{0, 0, 0};
    cmd.size = WMTSize{width, height, 1};
    cmd.dst = dst_buffer_handle;
    cmd.offset = offset;
    cmd.bytes_per_row = pitch;
    cmd.bytes_per_image = pitch * row_count;
    ctx.endPass();
    ctx.signalEventByHandle(event_handle, signal_seq);
  });
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();

  // Synchronous: the caller's LockRect hands out the mirror pointer
  // right after this returns. Wait for the chunk's encode AND the
  // GPU-side retirement, then copy the block into the mirror.
  uint64_t seq = m_dxmtQueue->CurrentSeqId();
  commitCurrentChunkTimed();
  m_dxmtQueue->WaitCPUFence(seq);
  if (!waitForGpuOrDeviceError(signal_seq))
    return;
  std::memcpy(surface->cpuPtr(), static_cast<const char *>(span.host), total_bytes);
}

ULONG STDMETHODCALLTYPE
MTLD3D9Device::Release() {
  // D3D9 clamps Release-at-0 (a quirk apps rely on; com/com_object.hpp
  // ComObjectClamp). The device multiply-inherits, so ComObjectClamp cannot
  // wrap it; fold the same guard by hand. The implicit resources that pin the
  // device unpin it on their own public 1->0, so this only ever guards a
  // genuine over-release at 0.
  if (m_refCount.load() == 0)
    return 0;
  return ComObject<IDirect3DDevice9Ex>::Release();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9)) {
    *ppvObject = static_cast<IDirect3DDevice9 *>(this);
    AddRef();
    return S_OK;
  }
  if (m_isEx && riid == __uuidof(IDirect3DDevice9Ex)) {
    *ppvObject = static_cast<IDirect3DDevice9Ex *>(this);
    AddRef();
    return S_OK;
  }
  // Private dxmt diag surface. Borrowed pointer; lifetime
  // tied to the IDirect3DDevice9 ref the caller already holds, so we
  // do NOT AddRef here. See d3d9_diag.hpp for the lifetime contract.
  // Apps never QI for this UUID; only tests do.
  if (riid == dxmt::IID_IDxmtDiag9) {
    *ppvObject = static_cast<dxmt::IDxmtDiag9 *>(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::TestCooperativeLevel() {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::addFrameCounter(frameStats(), &dxmt::FrameStatistics::frame_device_status_poll_count);
  // D3D9Ex spec: always returns S_OK; apps probe device loss via
  // CheckDeviceState on the Ex interface. wined3d device.c d3d9_device_
  // TestCooperativeLevel and DXVK both match this.
  if (m_isEx)
    return D3D_OK;
  updateNonExLostState();
  switch (m_deviceState.load(std::memory_order_relaxed)) {
  case DeviceState::Ok:
    return D3D_OK;
  case DeviceState::Lost:
    return D3DERR_DEVICELOST;
  case DeviceState::NotReset:
    return D3DERR_DEVICENOTRESET;
  }
  return D3D_OK;
}
UINT STDMETHODCALLTYPE
MTLD3D9Device::GetAvailableTextureMem() {
  D9DeviceLock lock = LockDevice();
  // Returning the strictly-truthful UMA answer (0) drives era-typical engines
  // into recreate-every-frame fallbacks. Mirror dxgi/d3d11: half of
  // recommendedMaxWorkingSetSize (available_texture_mem_bytes halves on unified
  // memory), clamped below 4 GB so the result fits UINT and the running
  // allocation counter stays visible instead of saturating a multi-GB budget.
  //
  // The configurable ceiling caps the advertised budget: era engines size their
  // texture and streaming pools off this figure, so on a 32-bit guest an
  // unrestricted UMA report lets them commit past the limited process address
  // space until a Metal command buffer faults out of memory. DXVK exposes
  // d3d9.maxAvailableMemory for the same reason; honor DXMT_MAX_VRAM_MB and keep
  // a conservative default on the 32-bit build so streaming titles fit without
  // per-game configuration.
  static const uint64_t cap_bytes = []() -> uint64_t {
    if (const char *e = std::getenv("DXMT_MAX_VRAM_MB"); e && e[0]) {
      char *end = nullptr;
      unsigned long mb = std::strtoul(e, &end, 10);
      if (end != e && mb > 0)
        return static_cast<uint64_t>(mb) << 20;
    }
#ifdef __i386__
    return 1024ull << 20;
#else
    return 0;
#endif
  }();
  // Subtract the device-local (DEFAULT-pool) allocations the app has made so the
  // figure falls as resources are created, the behavior era engines poll for.
  // wined3d and DXVK keep the same running counter. An Ex device reports the
  // WDDM-virtualised figure that does NOT fall with allocations (wine's d3d9ex
  // tests assert a near-constant report across twenty render-target creates;
  // DXVK's ChangeReportedMemory early-outs on IsExtended the same way).
  const int64_t used = m_isEx ? 0 : reportedTextureMemory();
  return available_texture_mem_bytes(
      m_metalDevice.recommendedMaxWorkingSetSize(), m_metalDevice.hasUnifiedMemory(), cap_bytes, used
  );
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EvictManagedResources() {
  D9DeviceLock lock = LockDevice();
  // Native D3D9 drops MANAGED resources from VRAM and reloads each from its
  // sysmem master on next use; UMA has no separate VRAM to free, but games rely
  // on the reload observable: a NO_DIRTY_UPDATE write made before the evict
  // becomes visible after it. Re-arm the deferred-upload sweep for every bound
  // MANAGED texture with a live mirror so the next draw re-pushes its bytes. (An
  // unbound managed texture needs no reload until it is bound; DXVK and wined3d
  // also make this a lazy, use-time reload, so bound coverage matches the
  // observable.) cf. DXVK d3d9_device.cpp EvictManagedResources (a bare no-op
  // there because its managed upload is always use-time; dxmt uploads eagerly, so
  // it must reinstate the pending state the eager path skipped.)
  for (uint32_t slot = 0; slot < D3D9_MAX_TEXTURE_UNITS; ++slot) {
    auto *t = m_textures[slot].ptr();
    if (t)
      t->evictManagedMirror();
  }
  markManagedUploadPending();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDirect3D(IDirect3D9 **ppD3D9) {
  D9DeviceLock lock = LockDevice();
  if (!ppD3D9)
    return D3DERR_INVALIDCALL;
  *ppD3D9 = m_parent.ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDeviceCaps(D3DCAPS9 *pCaps) {
  D9DeviceLock lock = LockDevice();
  HRESULT hr = m_parent->GetDeviceCaps(m_creationParams.AdapterOrdinal, m_creationParams.DeviceType, pCaps);
  // A pure-SWVP device advertises the extended float register count; a MIXED
  // device keeps the 256 hardware value in caps (native reports the hardware
  // count there, only the SetVertexShaderConstantF bound widens to 8192). Gate on
  // the immutable creation flag, not the runtime m_isSWVP (which
  // SetSoftwareVertexProcessing toggles on a MIXED device): D3DCAPS9 is fixed per
  // device.
  if (SUCCEEDED(hr) && pCaps && (m_creationParams.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING))
    pCaps->MaxVertexShaderConst = D3D9_MAX_VS_CONST_F_SWVP;
  return hr;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) {
  D9DeviceLock lock = LockDevice();
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  // The adapter's mode, not the presentation parameters, even while fullscreen.
  // A driver that takes a device fullscreen switches the adapter to the
  // requested mode, so on Windows this call, GetAdapterDisplayMode and
  // GetMonitorInfo all report that one mode. This port never performs the
  // switch (under winemac a real one cannot complete while the display is
  // captured, and the application waits forever for a mode that never
  // arrives), so answering from the presentation parameters here would make
  // the three disagree with each other, which is worse than all three
  // agreeing on the desktop.
  return m_parent->GetAdapterDisplayMode(m_creationParams.AdapterOrdinal, pMode);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) {
  D9DeviceLock lock = LockDevice();
  if (!pParameters)
    return D3DERR_INVALIDCALL;
  *pParameters = m_creationParams;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) {
  D9DeviceLock lock = LockDevice();
  // Validation gates per DXVK d3d9_device.cpp, then the wined3d
  // realisation: a 32x32 bitmap becomes a Win32 hardware cursor via
  // CreateIconIndirect (wined3d_device_set_cursor_properties). A bitmap of
  // any other size is cropped into the 32x32 hardware cursor, windowed or
  // fullscreen alike (the DXVK crop model, consistent both ways), so an
  // oversized cursor shows cropped rather than not at all. wined3d instead
  // blits larger cursors at present time via a software cursor; that full
  // >32x32 realisation is a deferred parity gap, not implemented here.
  if (!pCursorBitmap)
    return D3DERR_INVALIDCALL;
  auto *bitmap = static_cast<MTLD3D9Surface *>(pCursorBitmap);
  const D3DSURFACE_DESC &desc = bitmap->desc();
  const UINT w = desc.Width;
  const UINT h = desc.Height;
  // The format / power-of-two / hotspot / display-mode gates are a pure
  // predicate (validate_cursor_properties); the runtime validates the bitmap
  // against the display-mode dimensions even on windowed swapchains, so fetch
  // the mode first and feed it in.
  D3DDISPLAYMODE mode;
  HRESULT hr = m_parent->GetAdapterDisplayMode(m_creationParams.AdapterOrdinal, &mode);
  if (FAILED(hr))
    return hr;
  hr = validate_cursor_properties(desc.Format, w, h, XHotSpot, YHotSpot, mode.Width, mode.Height);
  if (FAILED(hr))
    return hr;
#ifdef _WIN32
  {
    D3DLOCKED_RECT locked;
    if (FAILED(bitmap->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return D3DERR_INVALIDCALL;
    // Crop/clamp any bitmap into the 32x32 hardware cursor; a 32x32 bitmap
    // copies whole, a larger one is truncated to the top-left 32x32 (std::min).
    uint32_t pixels[32 * 32] = {};
    const UINT copy_w = std::min(w, 32u) * 4;
    const UINT copy_h = std::min(h, 32u);
    for (UINT row = 0; row < copy_h; row++)
      std::memcpy(&pixels[row * 32], static_cast<const uint8_t *>(locked.pBits) + row * locked.Pitch, copy_w);
    bitmap->UnlockRect();
    // 32-bit user32 cursors fall back to the mono mask when the alpha
    // channel is all zeroes; an all-ones mask keeps such bitmaps fully
    // transparent instead (wined3d).
    uint8_t mask[32 * 32 / 8];
    std::memset(mask, 0xff, sizeof(mask));
    ICONINFO info;
    info.fIcon = FALSE;
    info.xHotspot = XHotSpot;
    info.yHotspot = YHotSpot;
    info.hbmMask = ::CreateBitmap(32, 32, 1, 1, mask);
    info.hbmColor = ::CreateBitmap(32, 32, 1, 32, pixels);
    HCURSOR cursor = ::CreateIconIndirect(&info);
    if (info.hbmMask)
      ::DeleteObject(info.hbmMask);
    if (info.hbmColor)
      ::DeleteObject(info.hbmColor);
    if (m_hwCursor)
      ::DestroyCursor(m_hwCursor);
    m_hwCursor = cursor;
    if (m_cursorVisible)
      ::SetCursor(m_hwCursor);
  }
#endif
  m_cursorImageSet = true;
  return D3D_OK;
}
void STDMETHODCALLTYPE
MTLD3D9Device::SetCursorPosition(int X, int Y, DWORD Flags) {
  D9DeviceLock lock = LockDevice();
  // wined3d device.c warps the OS pointer only when a hardware cursor
  // is realised, and skips the call when the position is unchanged
  // (apps echo back the position they just read every frame). Its
  // software-cursor divergence fallback is dropped: dxmt has no
  // software cursor to fall back to. Flags is documented as a hint
  // set (D3DCURSOR_IMMEDIATE_UPDATE) the runtime can ignore.
  (void)Flags;
#ifdef _WIN32
  if (m_hwCursor) {
    POINT pt;
    if (::GetCursorPos(&pt) && pt.x == X && pt.y == Y)
      return;
    ::SetCursorPos(X, Y);
  }
#else
  (void)X;
  (void)Y;
#endif
}
BOOL STDMETHODCALLTYPE
MTLD3D9Device::ShowCursor(BOOL bShow) {
  D9DeviceLock lock = LockDevice();
  // Returns the previous visibility per the wined3d_device_show_cursor
  // contract (wined3d device.c); UI toggle code reads the return to
  // drive its own state. Visibility latches only once a cursor image
  // has been set, also per wined3d.
  BOOL prev = m_cursorVisible;
  if (m_cursorImageSet)
    m_cursorVisible = bShow;
#ifdef _WIN32
  if (m_hwCursor)
    ::SetCursor(bShow ? m_hwCursor : nullptr);
#endif
  return prev;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateAdditionalSwapChain(
    D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **ppSwapChain
) {
  D9DeviceLock lock = LockDevice();
  if (ppSwapChain)
    *ppSwapChain = nullptr;
  // A lost non-Ex device cannot hand out a new chain: return DEVICELOST like
  // DXVK d3d9_device.cpp CreateAdditionalSwapChainEx. The out-pointer is nulled
  // above first, so a caller that Releases it sees NULL. Ex never enters Lost.
  if (!m_isEx && m_deviceState.load(std::memory_order_relaxed) == DeviceState::Lost)
    return D3DERR_DEVICELOST;
  if (!pPresentationParameters || !ppSwapChain)
    return D3DERR_INVALIDCALL;
  // Additional swapchains are windowed-only, and the implicit chain owns
  // any exclusive-fullscreen display: native rejects a fullscreen request
  // outright and rejects every additional chain while the device itself is
  // fullscreen (wined3d device.c, DXVK CreateAdditionalSwapChain).
  if (!pPresentationParameters->Windowed)
    return D3DERR_INVALIDCALL;
  if (m_implicitSwapChain && !m_presentParams.Windowed)
    return D3DERR_INVALIDCALL;

  // Validate + canonicalize through the same gates the implicit chain and
  // Reset use; the concrete BackBufferWidth/Height/Count are written back
  // into the caller's struct (native fills a zero extent from the device
  // window's client rect and a zero count to 1).
  if (const char *reason = PresentParamsRejectReason(*pPresentationParameters, m_isEx)) {
    Logger::warn(str::format("CreateAdditionalSwapChain: rejected D3DPRESENT_PARAMETERS::", reason));
    return D3DERR_INVALIDCALL;
  }
  if (!CanonicalisePresentParams(
          *pPresentationParameters, m_creationParams.hFocusWindow, m_creationParams.AdapterOrdinal
      ))
    return D3DERR_INVALIDCALL;

  // hDeviceWindow falls back to the device focus window (wined3d
  // swapchain.c). The chain stores the resolved window so its
  // GetPresentParameters reports it, but the caller's struct keeps the
  // window it passed: native leaves a NULL request NULL.
  HWND effectiveWindow =
      pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : m_creationParams.hFocusWindow;
  D3DPRESENT_PARAMETERS chainParams = *pPresentationParameters;
  chainParams.hDeviceWindow = effectiveWindow;

  auto *chain = new MTLD3D9SwapChain(this, m_isEx, chainParams, effectiveWindow, /*isImplicit=*/false);
  // App-owned: hand it out with a public ref, which pins the device. The
  // device takes no AddRefPrivate (unlike the implicit chain), so the
  // app's final Release destroys it.
  *ppSwapChain = ::dxmt::ref(static_cast<IDirect3DSwapChain9 *>(chain));
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain) {
  D9DeviceLock lock = LockDevice();
  // GetSwapChain NULLs the out-pointer on the failure path, matching the
  // wined3d d3d9 layer (InitReturnPtr): a caller that Releases whatever the
  // out-pointer holds must see NULL after a failed call, not a stale value.
  if (!pSwapChain)
    return D3DERR_INVALIDCALL;
  *pSwapChain = nullptr;
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  *pSwapChain = ::dxmt::ref(static_cast<IDirect3DSwapChain9 *>(m_implicitSwapChain));
  return D3D_OK;
}
UINT STDMETHODCALLTYPE
MTLD3D9Device::GetNumberOfSwapChains() {
  D9DeviceLock lock = LockDevice();
  return 1;
}
// Port of wined3d_swapchain_state_setup_fullscreen (dlls/wined3d/swapchain.c):
// resize + restyle the device window to a borderless fullscreen rect. On first
// entry the pre-fullscreen style/exstyle/rect are saved for restore; a later
// call on the same window (a resolution-change Reset) only repositions. Pure
// window geometry: the monitor query is read-only and the display mode is never
// touched.
void
MTLD3D9Device::enterFullscreenWindow(HWND window, UINT width, UINT height) {
  if (!window)
    return;
  // The app asked dxmt not to touch its window; leave it exactly as is.
  if (m_creationParams.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES)
    return;

  // Fullscreen rect: the window's monitor origin plus the backbuffer extent.
  // Single-monitor desktops sit at (0, 0); a read-only MonitorFromWindow keeps
  // multi-monitor correct without a display-mode switch.
  LONG x = 0, y = 0;
  HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (monitor && GetMonitorInfoW(monitor, &mi)) {
    x = mi.rcMonitor.left;
    y = mi.rcMonitor.top;
  }
  // Remembered for the focus-gain reposition, which runs while the window is
  // still minimized and so cannot ask the window itself which output it is on.
  m_fullscreenMonitor.store(monitor, std::memory_order_relaxed);

  if (m_fullscreenWindow != window) {
    // First entry (or a newly designated device window). Save the windowed
    // style/exstyle/rect, then restyle to a borderless popup: wined3d
    // fullscreen_style / fullscreen_exstyle.
    m_fullscreenWindow = window;
    m_savedWindowStyle = GetWindowLongW(window, GWL_STYLE);
    m_savedWindowExStyle = GetWindowLongW(window, GWL_EXSTYLE);
    GetWindowRect(window, &m_savedWindowRect);
    LONG style = (m_savedWindowStyle | WS_POPUP | WS_SYSMENU) & ~(WS_CAPTION | WS_THICKFRAME);
    LONG exStyle = m_savedWindowExStyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    SetWindowLongW(window, GWL_STYLE, style);
    SetWindowLongW(window, GWL_EXSTYLE, exStyle);
  }
  // wined3d uses HWND_TOPMOST + SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW.
  SetWindowPos(
      window, HWND_TOPMOST, x, y, static_cast<int>(width), static_cast<int>(height),
      SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW
  );
}

// Port of wined3d_swapchain_state_restore_from_fullscreen. Restores the saved
// style (preserving the live WS_VISIBLE / WS_EX_TOPMOST bits, as wined3d does so
// it never hides or un-tops a window the app is driving). Non-Ex d3d9 is
// style-only (it does not set RESTORE_WINDOW_RECT); Ex also restores the saved
// window rect.
void
MTLD3D9Device::leaveFullscreenWindow() {
  HWND window = m_fullscreenWindow;
  if (!window)
    return;
  m_fullscreenWindow = nullptr;

  LONG liveStyle = GetWindowLongW(window, GWL_STYLE);
  LONG liveExStyle = GetWindowLongW(window, GWL_EXSTYLE);
  LONG style = (m_savedWindowStyle & ~WS_VISIBLE) | (liveStyle & WS_VISIBLE);
  LONG exStyle = (m_savedWindowExStyle & ~WS_EX_TOPMOST) | (liveExStyle & WS_EX_TOPMOST);
  SetWindowLongW(window, GWL_STYLE, style);
  SetWindowLongW(window, GWL_EXSTYLE, exStyle);
  if (m_isEx)
    SetWindowPos(
        window, HWND_NOTOPMOST, m_savedWindowRect.left, m_savedWindowRect.top,
        m_savedWindowRect.right - m_savedWindowRect.left, m_savedWindowRect.bottom - m_savedWindowRect.top,
        SWP_FRAMECHANGED | SWP_NOACTIVATE
    );
  else
    SetWindowPos(
        window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE
    );

  m_savedWindowStyle = 0;
  m_savedWindowExStyle = 0;
  m_savedWindowRect = RECT{};
  m_fullscreenMonitor.store(nullptr, std::memory_order_relaxed);
}

namespace {
// Window properties held on the focus window while dxmt has it subclassed: the
// application's original wndproc, and the device the transitions belong to. The
// forwarding proc reads both per message; hook/unhook own them.
constexpr const wchar_t *kFocusProcProp = L"DXMTD3D9FocusOrigProc";
constexpr const wchar_t *kFocusDeviceProp = L"DXMTD3D9FocusDevice";

LRESULT CALLBACK
focusWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto orig = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kFocusProcProp));
  auto *device = static_cast<MTLD3D9Device *>(GetPropW(hwnd, kFocusDeviceProp));
  BOOL unicode = IsWindowUnicode(hwnd);
  // The focus window is being torn down while still subclassed: drop our proc
  // so neither the property nor our function pointer outlives it.
  if (message == WM_NCDESTROY && orig) {
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
    RemovePropW(hwnd, kFocusProcProp);
    RemovePropW(hwnd, kFocusDeviceProp);
    // The window is going away, so nothing below applies and the application
    // must still receive this: falling through would hand it to DefWindowProc
    // whenever the filter happens to be armed.
    return unicode ? CallWindowProcW(orig, hwnd, message, wparam, lparam)
                   : CallWindowProcA(orig, hwnd, message, wparam, lparam);
  }

  if (device) {
    // The filter is checked first so that the window traffic dxmt generates
    // below cannot re-enter this handler and drive a second transition. It only
    // has an observable effect when the device window IS the focus window,
    // since that is the only window subclassed. WM_DISPLAYCHANGE is exempt: the
    // application is entitled to see a mode change it did not ask for.
    if (device->focusMessagesFiltered() && message != WM_DISPLAYCHANGE)
      return unicode ? DefWindowProcW(hwnd, message, wparam, lparam) : DefWindowProcA(hwnd, message, wparam, lparam);
    // Every side effect runs before the application's proc sees the message,
    // so the app observes the display mode already restored and the device
    // window already minimized. wined3d device.c device_process_message.
    if (message == WM_ACTIVATEAPP)
      device->onFocusActivation(wparam != FALSE);
    // wined3d answers SC_RESTORE itself and then still forwards, so the
    // application sees WM_SYSCOMMAND after the restore rather than before.
    if (message == WM_SYSCOMMAND && wparam == SC_RESTORE) {
      if (unicode)
        DefWindowProcW(hwnd, message, wparam, lparam);
      else
        DefWindowProcA(hwnd, message, wparam, lparam);
    }
  }

  if (!orig)
    return unicode ? DefWindowProcW(hwnd, message, wparam, lparam) : DefWindowProcA(hwnd, message, wparam, lparam);
  return unicode ? CallWindowProcW(orig, hwnd, message, wparam, lparam)
                 : CallWindowProcA(orig, hwnd, message, wparam, lparam);
}
} // namespace

// Port of wined3d_swapchain_activate (dlls/wined3d/swapchain.c). The two
// directions are deliberately asymmetric, matching the reference: losing focus
// restores the display mode and minimizes, regaining it only repositions. There
// is no un-minimize anywhere in wined3d; bringing the window back is the window
// manager's job, which is why the conformance test has to do it by hand.
void
MTLD3D9Device::onFocusActivation(bool activated) {
  // Windowed devices never subclass the focus window, so this cannot fire for
  // one; guard anyway since Reset can flip a device windowed under the hook.
  if (!m_implicitSwapChain || m_implicitSwapChain->windowed())
    return;

  HWND device_window = m_implicitSwapChain->hWindow();
  // D3DCREATE_NOWINDOWCHANGES suppresses only the window moves, never the mode
  // restore or the device-state transition. wined3d consults it at exactly the
  // two window-touching sites.
  const bool may_touch_window = !(m_creationParams.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES);
  const UINT backbuffer_width = m_presentParams.BackBufferWidth;
  const UINT backbuffer_height = m_presentParams.BackBufferHeight;

  // ShowWindow and SetWindowPos deliver messages to the application
  // synchronously, and applications have been observed releasing the device
  // from inside that handling. Hold a private reference so the members touched
  // afterwards still exist; the public counter is left alone so a game that
  // reads its own refcount in that handler sees the value it expects, and so an
  // over-release there still clamps at zero instead of freeing under us.
  AddRefPrivate();

  // The window traffic below is ours, not the application's.
  m_focusMessagesFiltered = true;

  if (!activated) {
    // Put the display back on the mode the registry records, which is what
    // releases a display the application captured by changing the mode itself.
    // Inert when nothing moved it, and dxmt never sets one: it renders
    // fullscreen into a borderless window at the desktop mode. Restoring only
    // the device window's output rather than every output, as wined3d does,
    // keeps a mode change the application made elsewhere alone.
    if (device_window)
      wsi::restoreDisplayMode(wsi::getWindowMonitor(device_window));

    // Marked lost BEFORE the window work, following wined3d. Native orders it
    // the other way round and the tests record that, but doing so means writing
    // device state after handing control to the application, which is the same
    // window in which it may have released the device.
    m_fullscreenOccluded.store(true, std::memory_order_relaxed);
    m_lastForegroundSample.store(wsi::foregroundWindow(), std::memory_order_relaxed);
    if (!m_isEx) {
      auto state = DeviceState::Ok;
      m_deviceState.compare_exchange_strong(state, DeviceState::Lost, std::memory_order_relaxed);
    }

    if (may_touch_window && device_window && IsWindowVisible(device_window)) {
      // Native minimizes with SW_SHOWMINIMIZED, and the conformance suite
      // records that as the reason it also sees WM_ACTIVATE on the device
      // window. SW_MINIMIZE is used anyway, following wined3d: under a window
      // manager SW_SHOWMINIMIZED leaves the device window active and breaks
      // reactivation, and that is the environment this runs in.
      ShowWindow(device_window, SW_MINIMIZE);
    }
  } else {
    // Plain D3D9 hands the app a device it must Reset; Ex recovers on its own.
    // dxmt never sets a display mode, so the mode re-apply wined3d does here
    // for Ex has nothing to undo.
    m_fullscreenOccluded.store(false, std::memory_order_relaxed);
    m_lastForegroundSample.store(wsi::foregroundWindow(), std::memory_order_relaxed);
    auto state = DeviceState::Lost;
    m_deviceState.compare_exchange_strong(
        state, m_isEx ? DeviceState::Ok : DeviceState::NotReset, std::memory_order_relaxed
    );

    if (may_touch_window && device_window) {
      // Size from the backbuffer, origin from the monitor the device went
      // fullscreen on, and explicitly no activate and no Z-order change. The
      // monitor is the saved one because the window is normally still minimized
      // here, and a minimized window's rect answers for the primary monitor
      // rather than its own. Some titles resume drawing only once they see a
      // WM_WINDOWPOSCHANGED on the device window, which is the whole reason
      // this runs even when the geometry is unchanged.
      LONG x = 0, y = 0;
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      HMONITOR monitor = m_fullscreenMonitor.load(std::memory_order_relaxed);
      if (!monitor)
        monitor = MonitorFromWindow(device_window, MONITOR_DEFAULTTOPRIMARY);
      if (monitor && GetMonitorInfoW(monitor, &mi)) {
        x = mi.rcMonitor.left;
        y = mi.rcMonitor.top;
      }
      SetWindowPos(
          device_window, nullptr, x, y, static_cast<int>(backbuffer_width), static_cast<int>(backbuffer_height),
          SWP_NOACTIVATE | SWP_NOZORDER
      );
    }
  }

  m_focusMessagesFiltered = false;
  ReleasePrivate();
}

void
MTLD3D9Device::hookFocusWindowProc(HWND fallbackWindow) {
  if (m_focusProcHooked)
    return;
  // wined3d installs the subclass unconditionally on the fullscreen transition;
  // D3DCREATE_NOWINDOWCHANGES suppresses only the window restyle, not the proc
  // hook. The focus window defaults to the device window when none was given.
  HWND focus = m_creationParams.hFocusWindow;
  if (!focus)
    focus = fallbackWindow;
  if (!focus || !IsWindow(focus))
    return;
  // A second device on the same focus window would save our own proc as the
  // one to chain to and recurse without end, and would take over the first
  // device's property besides. wined3d dedups the same way, by refusing to
  // register a window it already holds.
  if (GetPropW(focus, kFocusProcProp))
    return;
  // Match the window's existing ANSI/Unicode flavour so the forwarding path
  // stays consistent with the app's own proc. Cache it so the unhook restores
  // through the same slot even if the window's flavour is queried later.
  m_focusProcUnicode = IsWindowUnicode(focus);
  LONG_PTR orig = m_focusProcUnicode
                      ? SetWindowLongPtrW(focus, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(focusWindowProc))
                      : SetWindowLongPtrA(focus, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(focusWindowProc));
  SetPropW(focus, kFocusProcProp, reinterpret_cast<HANDLE>(orig));
  SetPropW(focus, kFocusDeviceProp, static_cast<HANDLE>(this));
  m_focusWindow = focus;
  m_focusProcHooked = true;
}

void
MTLD3D9Device::unhookFocusWindowProc() {
  if (!m_focusProcHooked)
    return;
  HWND focus = m_focusWindow;
  m_focusProcHooked = false;
  m_focusWindow = nullptr;
  if (!focus || !IsWindow(focus))
    return;
  // Drop the device property unconditionally, before anything can fail out
  // below. The original-proc property may have to stay behind for the
  // WM_NCDESTROY self-heal, but a device pointer that outlives its device would
  // be dereferenced by the next activation message; the proc treats its absence
  // as "forward only", which is exactly the right behavior once the device is
  // gone.
  RemovePropW(focus, kFocusDeviceProp);
  auto orig = reinterpret_cast<WNDPROC>(GetPropW(focus, kFocusProcProp));
  if (!orig)
    return;
  // Restore only while our proc is still the installed one; an app that
  // re-subclassed on top of us keeps its proc (wined3d guards the same way).
  // Use the slot we hooked through, not a fresh IsWindowUnicode query. Leave
  // the property in place if someone else owns the proc: its WM_NCDESTROY
  // self-heal still needs it to find the original.
  auto current = reinterpret_cast<WNDPROC>(
      m_focusProcUnicode ? GetWindowLongPtrW(focus, GWLP_WNDPROC) : GetWindowLongPtrA(focus, GWLP_WNDPROC)
  );
  if (current == focusWindowProc) {
    if (m_focusProcUnicode)
      SetWindowLongPtrW(focus, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
    else
      SetWindowLongPtrA(focus, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
    RemovePropW(focus, kFocusProcProp);
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) {
  D9DeviceLock lock = LockDevice();
  // Wine's main thread has no outer NSAutoreleasePool. Reset tears
  // down and recreates the backbuffer + auto-DS, each of which routes
  // through Metal APIs that return autoreleased handles (newBuffer,
  // newTexture, render command encoder). Without a pool here the
  // autoreleased temporaries leak across every Reset (typical
  // resolution-change path on WM_SIZE).
  auto pool = WMT::MakeAutoreleasePool();
  // Reset: run the non-Ex DEFAULT-pool losable gate, drain the GPU, rebuild the
  // swapchain, and (non-Ex only) reset render state to defaults and abort any
  // recording block. Ex devices keep their state, matching wine/DXVK.
  if (!pPresentationParameters)
    return D3DERR_INVALIDCALL;
  if (!m_implicitSwapChain)
    return D3DERR_INVALIDCALL;
  // A fullscreen device that lost the foreground cannot Reset until it
  // regains it: run the focus-loss transitions here too (apps may
  // Reset without polling first) and reject while Lost, the wined3d
  // shape where Reset on a still-deactivated device fails DEVICELOST.
  updateNonExLostState();
  if (!m_isEx && m_deviceState.load(std::memory_order_relaxed) == DeviceState::Lost)
    return D3DERR_DEVICELOST;

  // Validate + canonicalize (wined3d/DXVK pattern): resolve unknown formats,
  // validate SwapEffect/BackBufferCount/SampleQuality, write back to caller.
  if (const char *reason = PresentParamsRejectReason(*pPresentationParameters, m_isEx)) {
    Logger::warn(str::format("Reset: rejected D3DPRESENT_PARAMETERS::", reason));
    return D3DERR_INVALIDCALL;
  }
  if (!CanonicalisePresentParams(
          *pPresentationParameters, m_creationParams.hFocusWindow, m_creationParams.AdapterOrdinal
      )) {
    // A failed fullscreen-mode (or extent) validation leaves a non-Ex device
    // needing a reset, so TestCooperativeLevel reports DEVICENOTRESET (native
    // shape). An Ex device never enters NotReset (wine/DXVK gate the store on
    // !extended); its ResetEx returns the failure and stays presentable.
    if (!m_isEx)
      m_deviceState.store(DeviceState::NotReset, std::memory_order_relaxed);
    return D3DERR_INVALIDCALL;
  }

  // Spec gate: non-Ex devices reject Reset when any app-held
  // D3DPOOL_DEFAULT resource or state block is still alive. wined3d
  // device.c runs reset_enum_callback only on !extended; DXVK
  // d3d9_device.cpp gates with !IsExtended() and counts state blocks in
  // the same losable counter to match native D3D9 (wined3d instead keeps
  // blocks usable across Reset; native fails the call). Ex devices are
  // expected to succeed Reset with live DEFAULT resources; the runtime
  // drops those resources via internal release rather than failing.
  if (!m_isEx && m_losableResourceCount.load(std::memory_order_relaxed) != 0) {
    m_deviceState.store(DeviceState::NotReset, std::memory_order_relaxed);
    return D3DERR_INVALIDCALL;
  }

  // 1. Drain GPU so the old backbuffer / auto-DS textures are safe to
  //    release. Same wait shape as the dtor; emit a tail-signal,
  //    commit the chunk, wait for cmdbuf retirement. Per-FlushDrawBatch
  //    signals are gone (encoder-coalesce unblock), so the chunk-commit
  //    boundary is the only consistent place to advance the event.
  FlushDrawBatch();
  flushOpenWork();
  if (m_dxmtQueue) {
    emitCmdbufTailSignal();
    uint64_t seq = m_dxmtQueue->CurrentSeqId();
    commitCurrentChunkTimed();
    m_dxmtQueue->WaitCPUFence(seq);
  }
  if (m_currentCmdSeq > 1)
    waitForGpuOrDeviceError(m_currentCmdSeq - 1);

  // Invalidate the COW snapshot: its ring block recycles with the old
  // chunks, and the next QueueBatchedDraw must rebuild from the freshly
  // reset device shadows rather than copy stale state forward.
  m_encShadowLastSnap = nullptr;
  m_encShadowLastSnapChunk = ~0ull;

  // Unbind ALL RT slots + DS (encoder mirror must be clear).
  // MRT apps: slots 1+ must be cleared or losable-resource gate blocks Reset.
  for (uint32_t i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
    if (m_renderTargets[i].ptr())
      QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::RenderTarget0 + i), nullptr);
    m_renderTargets[i] = nullptr;
  }
  if (m_depthStencilSurface.ptr())
    QueueRefOp(PendingRefOp::DepthStencilSurface, nullptr);
  m_depthStencilSurface = nullptr;

  // 3. Tell the swapchain to drop + rebuild its backbuffer at the new
  //    dimensions/format. If that fails (OOM / bad format), the chain
  //    is left without a backbuffer; drive the state-machine to
  //    NotReset so the app's next TestCooperativeLevel observes it.
  // hDeviceWindow falls back to the device focus window per D3D9 spec; the
  // swapchain reports this resolved window from GetPresentParameters.
  HWND effectiveWindow =
      pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : m_creationParams.hFocusWindow;
  HRESULT hr = m_implicitSwapChain->ResetForDeviceReset(*pPresentationParameters, effectiveWindow);
  if (FAILED(hr)) {
    // Non-Ex only: an Ex device never enters NotReset (wine/DXVK gate the store
    // on !extended), so a failed ResetEx returns the error and leaves the
    // device presentable rather than silently occluding Present.
    if (!m_isEx)
      m_deviceState.store(DeviceState::NotReset, std::memory_order_relaxed);
    return hr;
  }

  // 4. Update our own cached params. Used by GetPresentParameters and
  //    the swapchain ctor path's defaults; the swapchain already has
  //    its own copy via ResetForDeviceReset.
  std::memcpy(&m_presentParams, pPresentationParameters, sizeof(D3DPRESENT_PARAMETERS));

  // Drive the device window to match the new mode, the windowed<->fullscreen
  // handling wined3d's reset path runs via the swapchain state. enter/leave are
  // idempotent on m_fullscreenWindow: a fullscreen->fullscreen Reset just
  // repositions, a windowed->windowed Reset is a no-op.
  {
    if (!m_presentParams.Windowed) {
      enterFullscreenWindow(effectiveWindow, m_presentParams.BackBufferWidth, m_presentParams.BackBufferHeight);
      hookFocusWindowProc(effectiveWindow);
    } else {
      leaveFullscreenWindow();
      unhookFocusWindowProc();
    }
  }

  // 5. Reset every category of device state to D3D9 defaults; non-Ex
  //    only. DXVK d3d9_device.cpp skips full state-reset on Ex
  //    ("D3D9Ex doesn't end scene in Reset"); wined3d device.c
  //    also skips wined3d_stateblock_reset on extended. Apps that rely
  //    on Ex-Reset state persistence (e.g. compatibility-mode resolution
  //    switches) see state stomped if we don't gate.
  if (!m_isEx) {
    resetStateToDefaults(m_presentParams.EnableAutoDepthStencil != 0);

    // Abort an in-progress BeginStateBlock recording (wine resets
    // device->recording in its reset path). The recording block never
    // gained a public ref, so dropping its ctor self-pin destructs it;
    // exactly-once shape mirrors the device dtor's cleanup.
    if (m_inStateBlockRecord) {
      m_inStateBlockRecord = false;
      auto *recording = m_recordingBlock;
      m_recordingBlock = nullptr;
      recording->ReleasePrivate();
    }
    // Reset closes the implicit scene per MSDN ("Reset...returns all
    // resources to a state similar to the state immediately after
    // the device is created"). Apps that called BeginScene before a
    // Reset would otherwise still see m_inScene=true after; and a
    // subsequent BeginScene would fail with D3DERR_INVALIDCALL even
    // though the spec says the post-Reset device is ready to accept
    // a fresh Begin. Ex Reset doesn't close the scene per the DXVK
    // comment cited above.
    m_inScene = false;
    // wined3d_device_reset drops the cursor texture but keeps the
    // hardware cursor and its visibility, so ShowCursor latches again
    // only behind a realised cursor.
#ifdef _WIN32
    m_cursorImageSet = m_hwCursor != nullptr;
#endif
  }

  // 6. Recreate the implicit auto-DS surface at new dimensions if the
  //    new params still ask for one. createAutoDepthStencil is a no-op
  //    when EnableAutoDepthStencil is FALSE.
  createAutoDepthStencil(m_presentParams);

  // 7. Re-bind the new backbuffer to RT0 (matches the ctor's auto-bind
  //    shape: SetRenderTarget(0, ...) also resets viewport/scissor to
  //    the new RT extents, which is what apps expect post-Reset).
  // A D3D9Ex device preserves the app's viewport Z range (MinZ/MaxZ)
  // across Reset, unlike a non-Ex device which resets it to 0..1;
  // SetRenderTarget(0) zeroes it either way, so snapshot and restore it on
  // the Ex path (DXVK d3d9_device.cpp restores MinZ/MaxZ only on extended).
  const float savedMinZ = m_viewport.MinZ;
  const float savedMaxZ = m_viewport.MaxZ;
  if (auto *bb = m_implicitSwapChain->backBuffer()) {
    SetRenderTarget(0, static_cast<IDirect3DSurface9 *>(bb));
  }
  // The retained depth-stencil binding survives Reset as-is: the
  // encode-thread ref op from the original bind is still standing, and
  // a self-assign would no-op through SetDepthStencilSurface's
  // unchanged-value gate anyway.
  if (m_isEx) {
    m_viewport.MinZ = savedMinZ;
    m_viewport.MaxZ = savedMaxZ;
    m_encShadowDirty |= dxmt::D9ES_DIRTY_VIEWPORT;
  }
  m_deviceState.store(DeviceState::Ok, std::memory_order_relaxed);
  // Re-latch the fullscreen activation state the way wine re-initialises
  // its device state on Reset; the current foreground becomes the new
  // edge-detection baseline.
  m_fullscreenOccluded.store(false, std::memory_order_relaxed);
  m_lastForegroundSample.store(wsi::foregroundWindow(), std::memory_order_relaxed);
  return D3D_OK;
}

void
MTLD3D9Device::createAutoDepthStencil(const D3DPRESENT_PARAMETERS &params) {
  if (!params.EnableAutoDepthStencil) {
    // EnableAutoDepthStencil=FALSE on a Reset that previously had an
    // auto-DS: drop the cache. Any app pub-ref to the auto-DS keeps
    // the surface object alive with its old Metal texture; new draws
    // see no DS bound (post-Reset state).
    m_autoDepthStencilSurface = nullptr;
    return;
  }
  WMTPixelFormat fmt = D3DFormatToMetal(params.AutoDepthStencilFormat, D3D9FormatUsage::DepthStencil);
  if (fmt == WMTPixelFormatInvalid)
    return;
  // The implicit DS must carry the same sample count the app's MSAA render
  // targets do, or a render pass that binds both hits a Metal color/depth
  // sample-count mismatch (an AGX command-buffer hang). Mirror
  // CreateDepthStencilSurface; an unsupported request falls back to 1.
  auto [sampleCount, msHr] =
      multisample_type_to_metal_sample_count(params.MultiSampleType, params.MultiSampleQuality, m_metalDevice);
  (void)msHr;
  WMTTextureInfo info{};
  info.pixel_format = fmt;
  info.width = params.BackBufferWidth;
  info.height = params.BackBufferHeight;
  info.depth = 1;
  info.array_length = 1;
  info.type = sampleCount > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = sampleCount;
  // A multisampled auto depth-stencil can be the source of a StretchRect depth
  // resolve, which samples it in a fragment shader; a single-sample one is never
  // sampled (see CreateDepthStencilSurface).
  info.usage =
      static_cast<WMTTextureUsage>(WMTTextureUsageRenderTarget | (sampleCount > 1 ? WMTTextureUsageShaderRead : 0));
  info.options = WMTResourceStorageModePrivate;
  Rc<dxmt::Texture> dxmt_ds_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> ds_flags;
  ds_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto ds_allocation = dxmt_ds_texture->allocate(ds_flags);
  if (!ds_allocation || !ds_allocation->texture())
    return;
  WMT::Texture dsRawTex = ds_allocation->texture();
  dxmt_ds_texture->rename(std::move(ds_allocation));
  // Fresh implicit depth-stencil: clear it so a first render pass that opens
  // load=Load, or a Present before the app's first Clear, reads defined depth
  // (0.0) / stencil (0) rather than recycled memory. The depth-stencil pixel
  // format is what routes this to the initializer's depth-stencil clear arm
  // (checked ahead of the RenderTarget usage bit these textures also carry).
  initTextureWithZero(dxmt_ds_texture.ptr());
  D3DSURFACE_DESC dsDesc{};
  dsDesc.Format = params.AutoDepthStencilFormat;
  dsDesc.Type = D3DRTYPE_SURFACE;
  dsDesc.Usage = D3DUSAGE_DEPTHSTENCIL;
  dsDesc.Pool = D3DPOOL_DEFAULT;
  // Reflect the realized sample count so GetDesc reads back the actual MSAA
  // mode; a fallback to 1 on an unsupported request reads back as NONE.
  dsDesc.MultiSampleType = sampleCount > 1 ? params.MultiSampleType : D3DMULTISAMPLE_NONE;
  dsDesc.MultiSampleQuality = sampleCount > 1 ? params.MultiSampleQuality : 0;
  dsDesc.Width = params.BackBufferWidth;
  dsDesc.Height = params.BackBufferHeight;
  // Identity-preserving Reset path; if the auto-DS already exists
  // (every call from Reset; the device ctor sees m_autoDepthStencilSurface
  // null and falls through to fresh-create), reuse the same
  // MTLD3D9Surface and swap its Metal backing in place. Apps that
  // held GetDepthStencilSurface() across Reset get the same surface
  // object back, now pointing at the new texture. Mirrors the
  // swapchain backbuffer's resetBacking shape.
  if (m_autoDepthStencilSurface.ptr()) {
    m_autoDepthStencilSurface->resetBacking(dsDesc, WMT::Reference<WMT::Texture>(dsRawTex), std::move(dxmt_ds_texture));
  } else {
    auto *dsSurface = new MTLD3D9Surface(
        this, dsDesc,
        /*container=*/static_cast<IDirect3DDevice9 *>(this), WMT::Reference<WMT::Texture>(dsRawTex),
        /*mipLevel=*/0,
        /*selfPin=*/false,
        /*parentTextureType=*/WMTTextureType2D,
        /*buffer=*/{},
        /*cpuPtr=*/nullptr,
        /*pitch=*/0,
        /*arraySlice=*/0,
        /*ownedBacking=*/nullptr,
        /*dxmtTexture=*/std::move(dxmt_ds_texture)
    );
    // Counts in the device loss gate while the app holds a public ref: native
    // fails a non-Ex Reset with an app-held auto depth-stencil alive.
    dsSurface->markImplicitLosable();
    m_autoDepthStencilSurface = dsSurface;
  }
  m_depthStencilSurface = m_autoDepthStencilSurface.ptr();
  // Op-stream mirror: the inline assignment above bypasses
  // SetDepthStencilSurface, so push the SetRef explicitly. The op
  // takes one outstanding AddRefPrivate that the walker consumes when
  // it installs into m_encodeSideRefs.depth_stencil_surface.
  if (auto *ds = m_autoDepthStencilSurface.ptr()) {
    ds->AddRefPrivate();
    QueueRefOp(PendingRefOp::DepthStencilSurface, ds);
  }
}
// Present: device-level forwards to the implicit swapchain. wined3d
// device.c forwards iSwapChain=0 by hand. Additional swapchains
// (CreateAdditionalSwapChain) present through their own
// IDirect3DSwapChain9::Present; a device-level Present targets only the
// implicit chain, which is the native shape.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::Present(
    const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion
) {
  D9DeviceLock lock = LockDevice();
  // An Ex device presenting an unfocused fullscreen chain reports
  // occlusion without presenting (wine d3d9 device.c); the non-Ex
  // device transitions toward the lost state instead, and the chain's
  // Present-on-Lost gate turns the state into DEVICELOST below.
  if (m_isEx && m_implicitSwapChain && !m_implicitSwapChain->windowed() && !fullscreenOwnsDisplay())
    return S_PRESENT_OCCLUDED;
  updateNonExLostState();
  HRESULT hr = m_implicitSwapChain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, 0);
  return hr;
}

// Device-level GetBackBuffer is a thin forwarder to the chain identified
// by iSwapChain (we only have one). wined3d device.c same shape.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetBackBuffer(
    UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer
) {
  D9DeviceLock lock = LockDevice();
  // Unlike the swapchain method, the device wrapper clears the out-pointer
  // up front (wined3d device.c InitReturnPtr): an invalid iSwapChain or a
  // forwarded out-of-range index then both leave the caller with NULL
  // rather than a stale pointer it would Release into a crash.
  if (ppBackBuffer)
    *ppBackBuffer = nullptr;
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return m_implicitSwapChain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) {
  D9DeviceLock lock = LockDevice();
  // Thin forwarder to the swapchain that owns the raster. wined3d
  // device.c::d3d9_device_GetRasterStatus and DXVK
  // D3D9DeviceEx::GetRasterStatus share this shape; same pattern dxmt
  // uses for GetBackBuffer / GetDisplayMode.
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return m_implicitSwapChain->GetRasterStatus(pRasterStatus);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetDialogBoxMode(BOOL bEnableDialogs) {
  D9DeviceLock lock = LockDevice();
  // MSDN documents many error conditions; DXVK's note
  // (d3d9_swapchain.cpp) is "doesn't appear to error at all in any of
  // my tests of these cases." Silently accept; apps' init paths
  // hr-check this and fail device-bring-up if it returns E_NOTIMPL.
  // There's no Metal-side mode to toggle (GDI dialog interop is
  // Win32-only).
  (void)bEnableDialogs;
  return D3D_OK;
}
void STDMETHODCALLTYPE
MTLD3D9Device::SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp) {
  D9DeviceLock lock = LockDevice();
  // Spec is void-return but the swapchain index is real: dxmt only owns
  // the implicit chain today (additional swapchains aren't implemented
  // yet), so anything past 0 is silently dropped; same as
  // wined3d::d3d9_device_SetGammaRamp under multi-swapchain absence.
  if (iSwapChain != 0 || !m_implicitSwapChain)
    return;
  m_implicitSwapChain->SetGammaRampForChain(Flags, pRamp);
}
void STDMETHODCALLTYPE
MTLD3D9Device::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp) {
  D9DeviceLock lock = LockDevice();
  if (!pRamp)
    return;
  if (iSwapChain != 0 || !m_implicitSwapChain) {
    // Synthesize identity for callers that hr-check the ramp bytes.
    // the prior void no-op left the struct uninitialised which a few
    // apps (calibration utilities) mis-read.
    for (uint32_t i = 0; i < 256; ++i) {
      WORD v = static_cast<WORD>(i * 257);
      pRamp->red[i] = v;
      pRamp->green[i] = v;
      pRamp->blue[i] = v;
    }
    return;
  }
  m_implicitSwapChain->GetGammaRampForChain(pRamp);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateTexture(
    UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture,
    HANDLE *pSharedHandle
) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::ScopedFrameDurationCounted _create_timer(
      frameStats(), &dxmt::FrameStatistics::frame_create_resource_interval,
      &dxmt::FrameStatistics::frame_create_resource_count
  );
  if (!ppTexture)
    return D3DERR_INVALIDCALL;
  *ppTexture = nullptr;
  // Pure pool/usage/dims/levels/depth-placement rejection matrix, shared with
  // CreateCubeTexture / CreateVolumeTexture. Carries the
  // depth-format-outside-DEFAULT reject. The format-lowering rejects (unsupported format, RT capability) and
  // the shared-handle policy stay below.
  if (HRESULT hr = validate_texture_create(
          {D3D9TextureCreateKind::Texture2D, Format, Pool, Usage, Width, Height, 1, Levels, m_isEx}
      );
      hr != D3D_OK)
    return hr;

  // pSharedHandle doubles as a user-memory pointer for single-level
  // SYSTEMMEM textures (the Vista-era user-memory path; the rows are
  // tightly packed). wined3d aliases the app pointer in place: LockRect
  // hands it back and the GPU upload sources from it, with no copy. The
  // app owns the storage for the texture's lifetime. Cross-process
  // sharing (DEFAULT pool) stays unimplemented.
  void *user_memory = nullptr;
  if (pSharedHandle) {
    // wined3d device.c gates the whole shared-handle block on the device:
    // a non-extended device rejects any handle with E_NOTIMPL before the
    // user-memory branch is reached. The overload is D3D9Ex-only.
    if (!m_isEx)
      return E_NOTIMPL;
    if (Pool == D3DPOOL_SYSTEMMEM && Levels == 1) {
      user_memory = *reinterpret_cast<void **>(pSharedHandle);
      pSharedHandle = nullptr;
    } else if (Pool != D3DPOOL_DEFAULT) {
      return D3DERR_INVALIDCALL;
    } else {
      // Ex + DEFAULT + handle: a cross-process share request. Real sharing is
      // unimplemented; wined3d (PRIMARY) FIXMEs and proceeds without sharing,
      // never writing the out-handle, and the RT/DS creates already take that
      // stance, so match it here rather than fail. An in-process-only
      // use of the "shared" texture still works; a genuine cross-process
      // dependency degrades the same way it does on wine.
      Logger::warn("d3d9: CreateTexture: cross-process shared handle ignored (created unshared)");
      pSharedHandle = nullptr;
    }
  }

  // Format gating depends on the requested role. Attachment usage on
  // a format Metal cannot attach (RENDERTARGET on compressed, say) is
  // rejected here even though wined3d accepts the create: wined3d's GL
  // backend can convert or render such formats, Metal never can, and
  // apps ship a create-time fallback for the rejection because native
  // runtimes and DXVK (CheckImageSupport) reject it too. Accepting
  // would strand them past their fallback at an unbindable texture.
  D3D9FormatUsage formatUsage = D3D9FormatUsage::SampleableTexture;
  if (Usage & D3DUSAGE_RENDERTARGET)
    formatUsage = D3D9FormatUsage::RenderTarget;
  else if (Usage & D3DUSAGE_DEPTHSTENCIL)
    formatUsage = D3D9FormatUsage::DepthStencil;
  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, formatUsage);
  // 'NULL' FOURCC render-target texture: the write-skipped placeholder
  // CreateRenderTarget already builds for standalone surfaces. There is no
  // real Metal format, so back it with BGRA8; the desc keeps D3DFMT_NULL and
  // the render path drops the slot. NULL is render-target only, so a NULL
  // without RT usage stays unsupported and falls through to the reject below.
  // Unlike the standalone surface (1x1 dummy) and DXVK (no image at all), this
  // allocates the full-size BGRA8 chain: the texture level/mirror machinery
  // ties the level descs to the Metal texture dimensions, so a 1x1 split would
  // be invasive. The backing is inert (never written, sampled, or read back),
  // so it is correctness-neutral; shrinking it is a deferred memory tidy.
  if (IsNullFormat(Format) && (Usage & D3DUSAGE_RENDERTARGET))
    pixelFormat = WMTPixelFormatBGRA8Unorm;
  // Packed-YUV formats have no Metal pixel format but are creatable as a
  // CPU-only SCRATCH mirror; the texture-less branch below builds them. Let
  // them past the unsupported-format reject (any other pool stays
  // INVALIDCALL via the SCRATCH guard there).
  if (pixelFormat == WMTPixelFormatInvalid && !IsScratchableUnsupportedFormat(Format)) {
    Logger::warn(str::format("d3d9: CreateTexture: unsupported format ", (unsigned)Format, " usage ", (unsigned)Usage));
    return D3DERR_INVALIDCALL;
  }

  // A RENDERTARGET texture must use a color-render-target-capable format:
  // CheckDeviceFormat denies the usage on other formats (isColorRTFormat), and
  // native + DXVK reject the create to match. D3DFormatToMetal maps some
  // swizzle-permuted formats (A4R4G4B4) to an attachable Metal texture, so the
  // Invalid gate above lets them through; rendering into one writes channels
  // permuted vs the sampler swizzle. NULL is render-target-only and mapped above.
  if ((Usage & D3DUSAGE_RENDERTARGET) && !IsNullFormat(Format) && !isColorRTFormat(Format)) {
    Logger::warn(str::format("d3d9: CreateTexture: format ", (unsigned)Format, " not render-target-capable"));
    return D3DERR_INVALIDCALL;
  }

  // Levels=0 means full chain to 1x1 (wined3d's wined3d_log2i + 1). A count
  // past the chain was rejected in validate_texture_create above.
  uint32_t real_levels;
  if (Levels == 0) {
    real_levels = 1;
    UINT m = std::max(Width, Height);
    while (m > 1) {
      m >>= 1;
      ++real_levels;
    }
  } else {
    real_levels = Levels;
  }

  // AUTOGENMIPMAP: app sees one level (D3D9 spec; GetSurfaceLevel /
  // LockRect on level > 0 returns INVALIDCALL). The Metal allocation
  // gets the full chain so generateMipmapsForTexture has somewhere to
  // write; a level-0 edit (UnlockRect, UpdateSurface, or an RT render)
  // flags the chain dirty and the pre-draw sweep regenerates it before
  // the next sample, keeping it in sync with level-0 edits.
  uint32_t metal_levels = real_levels;
  uint32_t app_levels = real_levels;
  if (Usage & D3DUSAGE_AUTOGENMIPMAP) {
    app_levels = 1;
  }

  // Packed-YUV SCRATCH texture: no Metal format, so there is no backing
  // dxmt::Texture. Build it mirror-only (null allocation); the level
  // surfaces are texture-less and Lock straight into the host mirror, the
  // same shape as the offscreen-plain and volume YUV paths. Any other pool
  // already failed the format reject above with INVALIDCALL.
  if (IsScratchableUnsupportedFormat(Format)) {
    if (Pool != D3DPOOL_SCRATCH)
      return D3DERR_INVALIDCALL;
    auto *tex = new MTLD3D9Texture(
        this, Width, Height, app_levels, Usage, Format, Pool, Rc<dxmt::Texture>(nullptr), 0, nullptr
    );
    captureTextureLeafVtable(tex, kLeaf2D);
    tex->AddRef();
    *ppTexture = tex;
    return D3D_OK;
  }

  // Pool -> storage (like CreateOffscreenPlainSurface + usage flags).
  // RT-promotion: every DEFAULT color texture gets RenderTarget unconditionally.
  WMTResourceOptions storage;
  WMTTextureUsage usage_bits = WMTTextureUsageShaderRead;
  // RT bit: D3DUSAGE_DEPTHSTENCIL (DS is render-target + sampler)
  // or DEFAULT-pool color (promotion, DXVK pattern).
  // Skip compressed: BC textures reject RenderTarget on Apple Silicon.
  if (Usage & D3DUSAGE_DEPTHSTENCIL)
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsageRenderTarget);
  else if (Pool == D3DPOOL_DEFAULT && !IsCompressedFormat(Format))
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsageRenderTarget);
  // PixelFormatView enables the sRGB-aliased sample view. Include BC formats: an
  // sRGB view of a BC texture decodes correctly on sample (matches DXVK), at the
  // cost of opting that texture out of AGX lossless. Without it, BC albedo samples
  // linear (no sRGB decode) and reads too bright, blowing out HDR scenes.
  // A8L8 is excluded (D3D9FormatSuppressSRGBRead): its RG8Unorm storage has an
  // sRGB sibling, but that sibling would decode the alpha lane, so A8L8 never
  // takes an sRGB view and needs no PixelFormatView (stays AGX-lossless-eligible).
  if (!(Usage & D3DUSAGE_DEPTHSTENCIL) && !D3D9FormatSuppressSRGBRead(Format) &&
      Recall_sRGB(D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture)) !=
          D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture))
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsagePixelFormatView);
  switch (Pool) {
  case D3DPOOL_DEFAULT:
    storage = WMTResourceStorageModePrivate;
    break;
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_SCRATCH:
    storage = WMTResourceStorageModeShared;
    break;
  case D3DPOOL_MANAGED:
    // Non-Ex MANAGED. Real D3D9 would keep both a sysmem master and a
    // GPU mirror with eviction; on Apple Silicon's unified memory the
    // distinction collapses. Track in project memory if real games
    // start hitting eviction-sensitive paths.
    storage = WMTResourceStorageModeShared;
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = metal_levels;
  info.sample_count = 1;
  info.usage = usage_bits;
  info.options = storage;

  // UMA-correct MANAGED single-buffer: Level=1 uncompressed 2D sampler-only. newTexture aliases level-0; LockRect →
  // Metal-mapped pBits; on UMA GPU samples host pages directly (no Unlock memcpy).
  // Depth/stencil FORMATS are excluded even without the DS usage flag: Metal
  // has no linear depth/stencil texture, so minimumLinearTextureAlignmentFor
  // PixelFormat asserts on one (a sampleable depth alias like INTZ reaches
  // here with Usage 0). The mirror path handles those instead.
  bool buffer_backed_eligible = Pool == D3DPOOL_MANAGED && app_levels == 1 && metal_levels == 1 &&
                                !(Usage & D3DUSAGE_AUTOGENMIPMAP) && !(Usage & D3DUSAGE_RENDERTARGET) &&
                                !(Usage & D3DUSAGE_DEPTHSTENCIL) && !(Usage & D3DUSAGE_DYNAMIC) &&
                                !IsCompressedFormat(Format) && !Is3DcFormat(Format) && !IsDepthStencilFormat(Format);
  // Zero-copy aliasing exposes Metal's linear-texture row alignment
  // as the LockRect pitch. wined3d and DXVK hand back (near-)tight
  // pitches, and shipped titles write rows at width*bpp regardless of
  // the reported value, so a padded pitch shears every such upload.
  // Widths whose tight pitch misses the alignment take the mirror
  // path, where the pitch is unconstrained and tight.
  uint64_t linear_alignment = 1;
  if (buffer_backed_eligible) {
    linear_alignment = m_metalDevice.minimumLinearTextureAlignmentForPixelFormat(pixelFormat);
    if (linear_alignment == 0)
      linear_alignment = 1;
    if ((D3DFormatRowPitch(Format, Width) % linear_alignment) != 0)
      buffer_backed_eligible = false;
  }

  // All textures rooted in Rc<dxmt::Texture> (survives thread boundaries). Buffer-backed: caller-managed page (pool
  // reuse, pre-fault optimizations); regular: allocate(). Same MTLD3D9Texture ctor; bufferPitch signals mode.
  Rc<dxmt::Texture> dxmt_texture;
  uint32_t backingPitch = 0;
  if (buffer_backed_eligible) {
    // The eligibility gate above guarantees the tight pitch satisfies
    // linear_alignment, so this round-up is a provable no-op kept for
    // shape parity with the offscreen-plain branch.
    const uint64_t row_bytes = static_cast<uint64_t>(D3DFormatRowPitch(Format, Width));
    backingPitch = static_cast<uint32_t>((row_bytes + linear_alignment - 1) & ~(linear_alignment - 1));
    const uint64_t backing_bytes = static_cast<uint64_t>(backingPitch) * Height;
    // Pool hit skips newBuffer XPC + page-fault cliff (pre-fault memset cost). Pool is texture-mirror only since
    // the buffer unification (VB/IB no longer donate); texture donation deferred for ref_tracker safety
    // (in-flight chunks retain allocation).
    WMT::Reference<WMT::Buffer> backingBuffer{};
    uint64_t backing_gpu_addr = 0;
    void *backingPtr = nullptr;
    void *backingHostPtr = nullptr;
    if (!acquireBufferBacking(
            static_cast<size_t>(backing_bytes), backingBuffer, backing_gpu_addr, backingHostPtr, backingPtr
        )) {
      backingPtr = wsi::aligned_malloc(backing_bytes, DXMT_PAGE_SIZE);
      if (!backingPtr)
        return D3DERR_OUTOFVIDEOMEMORY;
      // Pre-fault every page now so the app's first Lock+memcpy doesn't
      // pay the 100ms+/page Rosetta x86_32 first-touch cliff streamed
      // mid-frame. Same pattern as the texture-mirror path.
      std::memset(backingPtr, 0, backing_bytes);

      WMTBufferInfo binfo{};
      binfo.length = backing_bytes;
      // Shared (UMA aliasing), Default cache. Hazard tracking left at
      // Metal's default (Tracked): Untracked here suppressed barriers
      // between LockRect-time CPU writes (via the aliased pointer) and
      // GPU samples within the same cmdbuf, and between blit-encoder
      // generateMipmaps / replaceRegion fall-throughs and the render
      // encoder that samples them next.
      binfo.options = (WMTResourceOptions)(WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared);
      binfo.memory.set(backingPtr);
      backingBuffer = m_metalDevice.newBuffer(binfo);
      if (backingBuffer == nullptr) {
        wsi::aligned_free(backingPtr);
        return D3DERR_OUTOFVIDEOMEMORY;
      }
    }
    info.options = (WMTResourceOptions)(WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared);
    dxmt_texture = new dxmt::Texture(
        static_cast<unsigned>(backing_bytes), static_cast<unsigned>(backingPitch), info, m_metalDevice
    );
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    auto allocation = dxmt_texture->wrapBuffer(std::move(backingBuffer), backingPtr, alloc_flags);
    if (!allocation || !allocation->texture()) {
      // dxmt::Texture destructor will run as `dxmt_texture` falls out
      // of scope; the half-built allocation owns the (buffer, mapped)
      // pair and tears them down via its dtor's i386 aligned_free.
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    dxmt_texture->rename(std::move(allocation));
  } else {
    dxmt_texture = new dxmt::Texture(info, m_metalDevice);
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    if (Pool == D3DPOOL_DEFAULT)
      alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
    auto allocation = dxmt_texture->allocate(alloc_flags);
    if (!allocation || !allocation->texture())
      return D3DERR_OUTOFVIDEOMEMORY;
    dxmt_texture->rename(std::move(allocation));
  }

  // A freshly-allocated Metal texture holds undefined (recycled) contents, so a
  // sample before the app's first fill would read stale memory. UnlockRect
  // uploads only the locked rect, so a MANAGED / SYSTEMMEM texture sampled
  // before it is ever Locked would show that stale content; zero every mip up
  // front for every pool, mirroring the d3d11 texture create (no pool
  // carve-out). The buffer-backed arm is skipped: its backing was already
  // zeroed on the cold aligned_malloc path and on a pool hit
  // (acquireBufferBacking), and its single level aliases that backing, so a
  // blit-zero here would only redundantly clear the same bytes.
  if (!buffer_backed_eligible)
    initTextureWithZero(dxmt_texture.ptr());

  // user_memory: level 0 aliases the app pointer (see the ctor). The
  // texture starts fully dirty, so the app's create-time contents reach
  // a DEFAULT mirror through the first UpdateTexture with no copy here.
  auto *tex = new MTLD3D9Texture(
      this, Width, Height, app_levels, Usage, Format, Pool, std::move(dxmt_texture), backingPitch, user_memory
  );
  captureTextureLeafVtable(tex, kLeaf2D);
  if (Pool == D3DPOOL_DEFAULT)
    tex->markLosable();
  tex->AddRef();
  *ppTexture = tex;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVolumeTexture(
    UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle
) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::ScopedFrameDurationCounted _create_timer(
      frameStats(), &dxmt::FrameStatistics::frame_create_resource_interval,
      &dxmt::FrameStatistics::frame_create_resource_count
  );
  if (!ppVolumeTexture)
    return D3DERR_INVALIDCALL;
  *ppVolumeTexture = nullptr;
  if (pSharedHandle) {
    // Non-extended devices reject any handle with E_NOTIMPL (wined3d
    // device.c). On an extended device the user-memory overload is only
    // defined for single-subresource 2D resources, so a volume texture with a
    // non-DEFAULT-pool handle is INVALIDCALL. A DEFAULT-pool handle is a
    // cross-process share request; real sharing is unimplemented, so proceed
    // unshared like the 2D path and wined3d do.
    if (!m_isEx)
      return E_NOTIMPL;
    if (Pool != D3DPOOL_DEFAULT)
      return D3DERR_INVALIDCALL;
    Logger::warn("d3d9: CreateVolumeTexture: cross-process shared handle ignored (created unshared)");
    pSharedHandle = nullptr;
  }
  // Pure pool/usage/dims/levels/format-class rejection matrix, shared with
  // CreateTexture / CreateCubeTexture. Depth, block-compressed and packed-YUV volume
  // formats are gated here (IsVolumeTextureFormat + the SCRATCH-blob carve-out).
  if (HRESULT hr = validate_texture_create(
          {D3D9TextureCreateKind::Volume, Format, Pool, Usage, Width, Height, Depth, Levels, m_isEx}
      );
      hr != D3D_OK)
    return hr;
  // Levels=0 means the full chain to 1x1; a count past the chain was rejected
  // in validate_texture_create above.
  UINT real_levels = Levels;
  if (real_levels == 0) {
    real_levels = 1;
    UINT m = std::max({Width, Height, Depth});
    while ((m >>= 1) != 0)
      ++real_levels;
  }

  // Formats Metal cannot realize as a 3D texture but D3D9 still copies into a
  // SCRATCH blob: block-compressed (DXTn) volumes (no 3D BC pixel format) and
  // packed YUV (no 422 format). validate_texture_create already confirmed the
  // pool is SCRATCH for these (other pools are INVALIDCALL, matching the
  // CheckDeviceFormat probe). The volume is created mirror-only (no backing
  // dxmt::Texture): LockBox serves the host mirror and the GPU push no-ops on
  // the absent texture.
  if (IsCompressedFormat(Format) || Is3DcFormat(Format) || IsScratchableUnsupportedFormat(Format)) {
    auto *tex =
        new MTLD3D9VolumeTexture(this, Width, Height, Depth, real_levels, Usage, Format, Pool, /*texture=*/nullptr);
    captureTextureLeafVtable(tex, kLeafVolume);
    tex->AddRef();
    *ppVolumeTexture = tex;
    return D3D_OK;
  }

  // Lower the format. Volume textures are sampled-only on Apple Silicon (no 3D
  // RT), so use the SampleableTexture path. validate_texture_create passed only
  // formats IsVolumeTextureFormat accepts, which all lower to a valid Metal
  // pixel format, so no reject is needed here.
  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture);

  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = Width;
  info.height = Height;
  info.depth = Depth;
  info.array_length = 1;
  info.type = WMTTextureType3D;
  info.mipmap_level_count = real_levels;
  info.sample_count = 1;
  info.usage = WMTTextureUsageShaderRead;
  // PixelFormatView for the sRGB-aliased sample view (D3DSAMP_SRGBTEXTURE), same
  // as the 2D/cube paths. Gated on an sRGB pair existing so non-sRGB LUT volumes
  // keep AGX lossless; A8L8 is excluded (D3D9FormatSuppressSRGBRead) since its
  // sRGB sibling would decode the alpha lane and it never takes an sRGB view.
  if (!D3D9FormatSuppressSRGBRead(Format) && Recall_sRGB(pixelFormat) != pixelFormat)
    info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsagePixelFormatView);
  info.options = WMTResourceStorageModePrivate;

  Rc<dxmt::Texture> dxmt_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  if (Pool == D3DPOOL_DEFAULT)
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = dxmt_texture->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return D3DERR_OUTOFVIDEOMEMORY;
  dxmt_texture->rename(std::move(allocation));

  // See CreateTexture. This volume is Private for every pool, so even a MANAGED
  // or SYSTEMMEM volume sampled before its first LockBox upload would read stale
  // memory (a volume LUT sampled as garbage). Zero every mip for every pool; the
  // per-level zero covers the full depth of each mip.
  initTextureWithZero(dxmt_texture.ptr());

  auto *tex =
      new MTLD3D9VolumeTexture(this, Width, Height, Depth, real_levels, Usage, Format, Pool, std::move(dxmt_texture));
  captureTextureLeafVtable(tex, kLeafVolume);
  if (Pool == D3DPOOL_DEFAULT)
    tex->markLosable();
  tex->AddRef();
  *ppVolumeTexture = tex;
  return D3D_OK;
}

// CreateCubeTexture: same validation as CreateTexture, one dimension (EdgeLength).
// Allocates 6 faces x N levels sharing one TextureCube handle.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture,
    HANDLE *pSharedHandle
) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::ScopedFrameDurationCounted _create_timer(
      frameStats(), &dxmt::FrameStatistics::frame_create_resource_interval,
      &dxmt::FrameStatistics::frame_create_resource_count
  );
  if (!ppCubeTexture)
    return D3DERR_INVALIDCALL;
  *ppCubeTexture = nullptr;
  // Pure pool/usage/dims/levels/format-class rejection matrix, shared with
  // CreateTexture / CreateVolumeTexture. Carries the depth-format-cube reject
  // (any pool/usage). The format-lowering rejects (unsupported format, RT
  // capability) and the shared-handle policy stay below.
  if (HRESULT hr = validate_texture_create(
          {D3D9TextureCreateKind::Cube, Format, Pool, Usage, EdgeLength, EdgeLength, 1, Levels, m_isEx}
      );
      hr != D3D_OK)
    return hr;

  if (pSharedHandle) {
    // Non-extended devices reject any handle with E_NOTIMPL (wined3d
    // device.c). On an extended device the user-memory overload is only
    // defined for single-subresource 2D resources, so a cube texture (six
    // faces) with a non-DEFAULT-pool handle is INVALIDCALL. A DEFAULT-pool
    // handle is a cross-process share request; real sharing is unimplemented,
    // so proceed unshared like the 2D path and wined3d do.
    if (!m_isEx)
      return E_NOTIMPL;
    if (Pool != D3DPOOL_DEFAULT)
      return D3DERR_INVALIDCALL;
    Logger::warn("d3d9: CreateCubeTexture: cross-process shared handle ignored (created unshared)");
    pSharedHandle = nullptr;
  }

  // Attachment usage on a format Metal cannot attach is rejected at create;
  // see CreateTexture. Depth-stencil formats were already rejected on a cube by
  // validate_texture_create: no native driver backs a depth cube,
  // allowing it trips a broken path in Gothic 3 (DXVK), and CheckDeviceFormat
  // denies CUBETEXTURE + DEPTHSTENCIL, so create and probe agree.
  D3D9FormatUsage formatUsage = D3D9FormatUsage::SampleableTexture;
  if (Usage & D3DUSAGE_RENDERTARGET)
    formatUsage = D3D9FormatUsage::RenderTarget;
  else if (Usage & D3DUSAGE_DEPTHSTENCIL)
    formatUsage = D3D9FormatUsage::DepthStencil;
  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, formatUsage);
  // 'NULL' FOURCC render-target cube: CheckDeviceFormat advertises NULL for
  // cube RTs (the TEXTURE/CUBETEXTURE branch), so back it with BGRA8 like the
  // 2D path to keep caps and create consistent. The desc keeps D3DFMT_NULL.
  if (IsNullFormat(Format) && (Usage & D3DUSAGE_RENDERTARGET))
    pixelFormat = WMTPixelFormatBGRA8Unorm;
  // Packed-YUV cube: creatable only as a CPU-only SCRATCH mirror; see the
  // texture-less branch below. Let it past the unsupported-format reject.
  if (pixelFormat == WMTPixelFormatInvalid && !IsScratchableUnsupportedFormat(Format)) {
    Logger::warn(
        str::format("d3d9: CreateCubeTexture: unsupported format ", (unsigned)Format, " usage ", (unsigned)Usage)
    );
    return D3DERR_INVALIDCALL;
  }

  // A RENDERTARGET cube must use a color-render-target-capable format, the same
  // caps/create consistency CreateTexture and CreateRenderTarget enforce: a
  // swizzle-permuted format (A4R4G4B4) maps to an attachable Metal texture but
  // CheckDeviceFormat denies the usage, so rendering into it writes permuted
  // channels. NULL is render-target-only and mapped above.
  if ((Usage & D3DUSAGE_RENDERTARGET) && !IsNullFormat(Format) && !isColorRTFormat(Format)) {
    Logger::warn(str::format("d3d9: CreateCubeTexture: format ", (unsigned)Format, " not render-target-capable"));
    return D3DERR_INVALIDCALL;
  }

  // Levels=0 means the full chain to 1x1; a count past the chain was rejected
  // in validate_texture_create above.
  uint32_t real_levels;
  if (Levels == 0) {
    real_levels = 1;
    UINT m = EdgeLength;
    while (m > 1) {
      m >>= 1;
      ++real_levels;
    }
  } else {
    real_levels = Levels;
  }

  // AUTOGENMIPMAP: see CreateTexture for the full-chain rationale.
  uint32_t metal_levels = real_levels;
  uint32_t app_levels = real_levels;
  if (Usage & D3DUSAGE_AUTOGENMIPMAP) {
    app_levels = 1;
  }

  // Packed-YUV SCRATCH cube: no Metal format, so mirror-only (null
  // allocation); see CreateTexture. Other pools already failed the reject.
  if (IsScratchableUnsupportedFormat(Format)) {
    if (Pool != D3DPOOL_SCRATCH)
      return D3DERR_INVALIDCALL;
    auto *tex = new MTLD3D9CubeTexture(this, EdgeLength, app_levels, Usage, Format, Pool, Rc<dxmt::Texture>(nullptr));
    captureTextureLeafVtable(tex, kLeafCube);
    tex->AddRef();
    *ppCubeTexture = tex;
    return D3D_OK;
  }

  WMTResourceOptions storage;
  WMTTextureUsage usage_bits = WMTTextureUsageShaderRead;
  // RT bit: D3DUSAGE_DEPTHSTENCIL (DS is render-target + sampler)
  // or DEFAULT-pool color (promotion, DXVK pattern).
  // Skip compressed: BC textures reject RenderTarget on Apple Silicon.
  if (Usage & D3DUSAGE_DEPTHSTENCIL)
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsageRenderTarget);
  else if (Pool == D3DPOOL_DEFAULT && !IsCompressedFormat(Format))
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsageRenderTarget);
  // PixelFormatView capability: see CreateTexture above for rationale (BC sRGB
  // sample views decode correctly; without it BC env-maps read too bright).
  // A8L8 is excluded (D3D9FormatSuppressSRGBRead): its RG8Unorm storage has an
  // sRGB sibling, but that sibling would decode the alpha lane, so A8L8 never
  // takes an sRGB view and needs no PixelFormatView (stays AGX-lossless-eligible).
  if (!(Usage & D3DUSAGE_DEPTHSTENCIL) && !D3D9FormatSuppressSRGBRead(Format) &&
      Recall_sRGB(D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture)) !=
          D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture))
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsagePixelFormatView);
  switch (Pool) {
  case D3DPOOL_DEFAULT:
    storage = WMTResourceStorageModePrivate;
    break;
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_SCRATCH:
  case D3DPOOL_MANAGED:
    storage = WMTResourceStorageModeShared;
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = EdgeLength;
  info.height = EdgeLength;
  info.depth = 1;
  // Metal TextureCube is a single texture with 6 implicit slices.
  // array_length=1 selects TextureCube (vs TextureCubeArray which
  // wants array_length=#cubes).
  info.array_length = 1;
  info.type = WMTTextureTypeCube;
  info.mipmap_level_count = metal_levels;
  info.sample_count = 1;
  info.usage = usage_bits;
  info.options = storage;

  // Wrap in dxmt::Texture so chunk lambdas can capture a Rc<>. Cube
  // textures take only the regular ctor; MTLBuffer.newTexture rejects
  // non-Type2D so the buffer-backed shape doesn't apply here.
  // Pool -> flags mirrors CreateTexture above.
  Rc<dxmt::Texture> dxmt_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  if (Pool == D3DPOOL_DEFAULT)
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = dxmt_texture->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return D3DERR_OUTOFVIDEOMEMORY;
  dxmt_texture->rename(std::move(allocation));

  // See CreateTexture. A MANAGED / SYSTEMMEM cube sampled before its faces are
  // ever Locked would read stale memory, so zero every mip for every pool; the
  // walk covers all 6 faces * mips (arrayLength() folds the faces in).
  initTextureWithZero(dxmt_texture.ptr());

  auto *tex = new MTLD3D9CubeTexture(this, EdgeLength, app_levels, Usage, Format, Pool, std::move(dxmt_texture));
  captureTextureLeafVtable(tex, kLeafCube);
  if (Pool == D3DPOOL_DEFAULT)
    tex->markLosable();
  tex->AddRef();
  *ppCubeTexture = tex;
  return D3D_OK;
}
// Storage for a d3d9 vertex / index buffer; the buffer object wraps the
// returned dxmt::Buffer in a dxmt::DynamicBuffer. A CPU-writable allocation the
// GPU reads, plus a process-owned host mirror the app writes and the consuming
// draw stores into that allocation. The class must match the one DynamicBuffer
// hands to later generations, since a refresh stores into whichever name is
// current. The mirror is never registered with Metal, so the lockable pointer
// stays in the 32-bit guest address space and no unix-side host-mapping path is
// involved; the allocation is Metal-owned and the application never sees it.
// dxmt::Buffer::allocate builds its own WMTBufferInfo per newBuffer, so no info
// is reused across calls.
static bool
allocateD3D9BufferStorage(WMT::Device device, UINT length, Rc<dxmt::Buffer> &out_buffer, void *&out_mirror) {
  Rc<dxmt::Buffer> buffer = new dxmt::Buffer(length, device);
  auto allocation = buffer->allocate(BufferAllocationFlag::CpuWriteCombined);
  if (allocation == nullptr || allocation->buffer().handle == 0)
    return false;
  buffer->rename(std::move(allocation));
  void *mirror = wsi::aligned_malloc(length, DXMT_PAGE_SIZE);
  if (!mirror)
    return false;
  std::memset(mirror, 0, length);
  out_buffer = std::move(buffer);
  out_mirror = mirror;
  return true;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle
) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::ScopedFrameDurationCounted _create_timer(
      frameStats(), &dxmt::FrameStatistics::frame_create_resource_interval,
      &dxmt::FrameStatistics::frame_create_resource_count
  );
  if (!ppVertexBuffer)
    return D3DERR_INVALIDCALL;
  *ppVertexBuffer = nullptr;
  // wined3d device.c validates the shared handle before any size/pool/usage
  // gate: a shared-buffer request returns E_NOTIMPL on a non-Ex device, and
  // NOTAVAILABLE for a non-DEFAULT pool on an Ex device (the buffer-specific
  // contract, unlike the texture paths), ahead of everything below. An Ex +
  // DEFAULT handle is a cross-process share request; real sharing is
  // unimplemented, so proceed unshared like the texture creates and wined3d do
  // rather than fail the create.
  if (pSharedHandle) {
    if (!m_isEx)
      return E_NOTIMPL;
    if (Pool != D3DPOOL_DEFAULT)
      return D3DERR_NOTAVAILABLE;
    Logger::warn("d3d9: CreateVertexBuffer: cross-process shared handle ignored (created unshared)");
    pSharedHandle = nullptr;
  }
  if (Length == 0)
    return D3DERR_INVALIDCALL;

  // wined3d buffer.c; SCRATCH not allowed for buffers (unlike
  // surfaces, scratch buffers have no defined CPU-only role).
  if (Pool == D3DPOOL_SCRATCH)
    return D3DERR_INVALIDCALL;
  // wined3d buffer.c; MANAGED on Ex device is invalid.
  if (Pool == D3DPOOL_MANAGED && m_isEx)
    return D3DERR_INVALIDCALL;
  // wined3d buffer.c; buffers can't be RT or DS. AUTOGENMIPMAP
  // is texture-only (DXVK d3d9_common_buffer.cpp rejects).
  if (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP))
    return D3DERR_INVALIDCALL;
  // MANAGED + DYNAMIC is spec-forbidden; wined3d permits it at the
  // d3d9 layer but DXVK d3d9_common_buffer.cpp rejects. Reject
  // for spec-correctness; apps shipping the combo hit a defined
  // INVALIDCALL instead of silent acceptance.
  if (Pool == D3DPOOL_MANAGED && (Usage & D3DUSAGE_DYNAMIC))
    return D3DERR_INVALIDCALL;
  // WRITEONLY is buffer-only and is the *expected* flag for vertex
  // buffers; allow it.

  // Pool -> Metal storage. Every D3D9 vertex buffer is Lockable per the
  // API contract (the WRITEONLY flag is a hint, not a gate), so the
  // backing has to be CPU-mappable. Shared collapses to the same
  // physical pages as Private on Apple-Silicon UMA; Private would
  // save nothing and would force a staging-upload path on every Lock.
  // Pool only gates validity here; it doesn't change the storage
  // choice.
  switch (Pool) {
  case D3DPOOL_DEFAULT:
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_MANAGED: // non-Ex MANAGED collapses to CPU-resident
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  // Storage (d3d9_buffer_map.hpp): a CPU-writable allocation wrapped in a
  // DynamicBuffer inside the buffer object, plus a host mirror the app writes
  // and the consuming draw stores into that allocation.
  void *host_ptr = nullptr;
  Rc<dxmt::Buffer> dxmt_buffer{};
  if (!allocateD3D9BufferStorage(m_metalDevice, Length, dxmt_buffer, host_ptr))
    return D3DERR_OUTOFVIDEOMEMORY;

  auto *vb = new MTLD3D9VertexBuffer(this, Length, Usage, FVF, Pool, host_ptr, std::move(dxmt_buffer));
  if (Pool == D3DPOOL_DEFAULT)
    vb->markLosable();
  vb->AddRef();
  *ppVertexBuffer = vb;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer,
    HANDLE *pSharedHandle
) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::ScopedFrameDurationCounted _create_timer(
      frameStats(), &dxmt::FrameStatistics::frame_create_resource_interval,
      &dxmt::FrameStatistics::frame_create_resource_count
  );
  if (!ppIndexBuffer)
    return D3DERR_INVALIDCALL;
  *ppIndexBuffer = nullptr;
  // wined3d device.c validates the shared handle before the format/pool gate:
  // a shared-buffer request returns E_NOTIMPL on a non-Ex device, and
  // NOTAVAILABLE for a non-DEFAULT pool on an Ex device, ahead of the index
  // format check below. An Ex + DEFAULT handle is a cross-process share
  // request; real sharing is unimplemented, so proceed unshared like the
  // texture creates and wined3d do rather than fail the create.
  if (pSharedHandle) {
    if (!m_isEx)
      return E_NOTIMPL;
    if (Pool != D3DPOOL_DEFAULT)
      return D3DERR_NOTAVAILABLE;
    Logger::warn("d3d9: CreateIndexBuffer: cross-process shared handle ignored (created unshared)");
    pSharedHandle = nullptr;
  }
  if (Length == 0)
    return D3DERR_INVALIDCALL;

  // Index format must be one of the two D3D9-defined index formats.
  // wined3d defers this to wined3d_format lookup; dxmt rejects up
  // front so unsupported formats fail closed without reaching the
  // Metal allocator.
  if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32)
    return D3DERR_INVALIDCALL;

  // Same pool / usage gating as CreateVertexBuffer (buffer.c).
  if (Pool == D3DPOOL_SCRATCH)
    return D3DERR_INVALIDCALL;
  if (Pool == D3DPOOL_MANAGED && m_isEx)
    return D3DERR_INVALIDCALL;
  if (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP))
    return D3DERR_INVALIDCALL;
  if (Pool == D3DPOOL_MANAGED && (Usage & D3DUSAGE_DYNAMIC))
    return D3DERR_INVALIDCALL;

  // Pool -> Metal storage (mirrors CreateVertexBuffer; see that body
  // for the rationale on always going Shared on UMA).
  switch (Pool) {
  case D3DPOOL_DEFAULT:
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_MANAGED:
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  // Storage: see CreateVertexBuffer.
  void *host_ptr = nullptr;
  Rc<dxmt::Buffer> dxmt_buffer{};
  if (!allocateD3D9BufferStorage(m_metalDevice, Length, dxmt_buffer, host_ptr))
    return D3DERR_OUTOFVIDEOMEMORY;

  auto *ib = new MTLD3D9IndexBuffer(this, Length, Usage, Format, Pool, host_ptr, std::move(dxmt_buffer));
  if (Pool == D3DPOOL_DEFAULT)
    ib->markLosable();
  ib->AddRef();
  *ppIndexBuffer = ib;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) {
  D9DeviceLock lock = LockDevice();
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;
  if (Width == 0 || Height == 0)
    return D3DERR_INVALIDCALL;
  // Mirrors GetDeviceCaps's MaxTextureWidth/Height. Silicon GPUs go
  // higher in practice, but we report 16384 in caps so we should
  // honour the same bound here.
  if (Width > 16384 || Height > 16384)
    return D3DERR_INVALIDCALL;
  // pSharedHandle: the cross-process handle. Non-Ex devices reject
  // any non-null pSharedHandle with E_NOTIMPL (matches wined3d's
  // d3d9_device_CreateRenderTarget early-out). Ex devices accept
  // the pointer and silently proceed without sharing; wined3d
  // FIXMEs and proceeds; we match the outcome. Resource sharing
  // is a future feature; the no-op stance is the right placeholder.
  if (pSharedHandle && !m_isEx)
    return E_NOTIMPL;

  // 'NULL' FOURCC sentinel: app wants a colour RT slot bound but
  // never written. There's no real Metal pixel format; the surface
  // still needs a placeholder Metal texture so the rest of the dxmt
  // surface plumbing (refcount, GetRenderTarget round-trips, swizzle
  // math) doesn't have to special-case a null storage. Allocate a
  // 1x1 BGRA8 dummy and rely on the batched-draw render-pass open +
  // bindPSOAndDraw to skip the slot whenever the surface's D3DFORMAT is NULL.
  // Reference: DXVK src/d3d9/d3d9_common_texture.cpp.
  const bool isNullRT = IsNullFormat(Format);
  WMTPixelFormat pixelFormat =
      isNullRT ? WMTPixelFormatBGRA8Unorm : D3DFormatToMetal(Format, D3D9FormatUsage::RenderTarget);
  if (pixelFormat == WMTPixelFormatInvalid)
    return D3DERR_INVALIDCALL;
  // Reject formats D3D9 does not advertise as render-target capable, even when
  // D3DFormatToMetal produced an attachable Metal texture (swizzle-permuted
  // formats like A4R4G4B4 map to one but render with permuted channels). Keeps
  // creates consistent with CheckDeviceFormat; native + DXVK reject them too.
  if (!isNullRT && !isColorRTFormat(Format))
    return D3DERR_INVALIDCALL;

  // Multisample mapping shared with CreateDepthStencilSurface and
  // probed by CheckDeviceMultiSampleType; see the helper at the top
  // of this file for the rationale.
  auto [sampleCount, msHr] = multisample_type_to_metal_sample_count(MultiSample, MultisampleQuality, m_metalDevice);
  if (FAILED(msHr))
    return msHr;

  // D3D9 disallows lockable multisampled render targets; reject up
  // front with the spec HRESULT.
  if (sampleCount > 1 && Lockable)
    return D3DERR_INVALIDCALL;

  // Render targets are private storage; a Lockable RT gets a host
  // mirror (the DEFAULT-pool offscreen-plain shape) because linear
  // textures can't carry the RenderTarget bit on Apple Silicon and a
  // private texture can't hand out a CPU pointer. LockRect refreshes
  // the mirror through readbackSurfaceMirror since the GPU is the
  // writer. The renderTarget + shaderRead usage pair is the canonical
  // RT shape so SetTexture binds the same handle.
  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  // NULL-RT placeholder: 1x1, single sample, plain RT usage. The
  // Width/Height the app asked for stay on the D3DSURFACE_DESC so
  // queries round-trip, but no Metal storage proportional to the
  // real RT size gets allocated.
  info.width = isNullRT ? 1u : Width;
  info.height = isNullRT ? 1u : Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = (!isNullRT && sampleCount > 1) ? WMTTextureType2DMultisample : WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = isNullRT ? 1u : sampleCount;
  info.usage = static_cast<WMTTextureUsage>(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  // PixelFormatView for D3DRS_SRGBWRITEENABLE: the render-pass
  // attachment swaps to an sRGB-format view of the same texture.
  // NULL-RT placeholder skips the flag: it never participates in a
  // colour write that would care about gamma encoding, and BGRA8Unorm
  // already has an sRGB pair so the alias would succeed but be unused.
  if (!isNullRT && Recall_sRGB(pixelFormat) != pixelFormat)
    info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsagePixelFormatView);
  info.options = WMTResourceStorageModePrivate;

  Rc<dxmt::Texture> dxmt_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = dxmt_texture->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return D3DERR_OUTOFVIDEOMEMORY;
  WMT::Texture rawTex = allocation->texture();
  dxmt_texture->rename(std::move(allocation));
  // A render target is always DEFAULT-pool GPU-private; clear it so a pass that
  // opens load=Load, or a Present of a target the frame does not fully cover,
  // reads a defined (0,0,0,0) rather than recycled memory. Cheap on the 1x1
  // NULL-RT placeholder and harmless there (it is never actually sampled).
  initTextureWithZero(dxmt_texture.ptr());

  // Lockable RT mirror: tight-pitch host allocation, zero-filled so a
  // pre-render lock reads defined bytes. Compressed formats can't get
  // here (the RenderTarget format gate rejects them).
  void *ownedBacking = nullptr;
  void *cpuPtr = nullptr;
  uint32_t pitch = 0;
  if (Lockable) {
    // 'NULL' has no real pixel layout; treat it as the BGRA8 dummy (4 bpp) so
    // a lockable NULL RT hands back a defined, desc-sized region. The bytes
    // are never read by the GPU (the slot is write-skipped). Real formats use
    // the 4-byte-aligned lock pitch; moot for the 4-bpp majority, but R16F is
    // RT-capable and 2 bpp, where an odd width needs the alignment.
    pitch = isNullRT ? (Width * 4u) : D3DFormatLockPitch(Format, Width);
    if (pitch == 0)
      return D3DERR_INVALIDCALL;
    const uint64_t mirror_bytes = static_cast<uint64_t>(pitch) * Height;
    ownedBacking = wsi::aligned_malloc(mirror_bytes, DXMT_PAGE_SIZE);
    if (!ownedBacking)
      return D3DERR_OUTOFVIDEOMEMORY;
    std::memset(ownedBacking, 0, mirror_bytes);
    cpuPtr = ownedBacking;
  }

  D3DSURFACE_DESC desc{};
  desc.Format = Format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Width = Width;
  desc.Height = Height;

  auto *surface = new MTLD3D9Surface(
      this, desc,
      /*container=*/static_cast<IDirect3DDevice9 *>(this), WMT::Reference<WMT::Texture>(rawTex),
      /*mipLevel=*/0,
      /*selfPin=*/true,
      /*parentTextureType=*/WMTTextureType2D,
      /*buffer=*/{},
      /*cpuPtr=*/cpuPtr,
      /*pitch=*/pitch,
      /*arraySlice=*/0,
      /*ownedBacking=*/ownedBacking,
      /*dxmtTexture=*/std::move(dxmt_texture)
  );
  // CreateRenderTarget surfaces are always D3DPOOL_DEFAULT.
  surface->markLosable();
  surface->AddRef();
  *ppSurface = surface;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) {
  D9DeviceLock lock = LockDevice();
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;
  if (Width == 0 || Height == 0)
    return D3DERR_INVALIDCALL;
  if (Width > 16384 || Height > 16384)
    return D3DERR_INVALIDCALL;
  // pSharedHandle: see CreateRenderTarget for the policy rationale.
  if (pSharedHandle && !m_isEx)
    return E_NOTIMPL;

  // Create stays permissive for the unadvertised depth formats D32,
  // D16_LOCKABLE and D32_LOCKABLE: CheckDeviceFormat (isDSFormat) reports them
  // NOTAVAILABLE - matching native and DXVK, which no driver exposes - so a
  // cap-checking app never reaches here, but Metal genuinely backs all three, so
  // a blind create is honoured rather than failing content that ignores the
  // probe. A deliberate probe-conservative / create-permissive split; the only
  // gate here is that the format lowers to a real Metal depth format.
  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, D3D9FormatUsage::DepthStencil);
  if (pixelFormat == WMTPixelFormatInvalid)
    return D3DERR_INVALIDCALL;

  // Shared MSAA-resolve with CreateRenderTarget / CheckDeviceMultiSampleType.
  auto [sampleCount, msHr] = multisample_type_to_metal_sample_count(MultiSample, MultisampleQuality, m_metalDevice);
  if (FAILED(msHr))
    return msHr;

  // Always Private. dxmt's encoder layer splits a frame across multiple
  // command encoders (on a Clear, an RT/DS change, or a blit), and a
  // Memoryless depth attachment does not survive an encoder boundary, so
  // a later encoder's load=Load would read back undefined data mid-frame.
  // D3D9's Discard hint only means the contents need not survive a Present
  // or a SetDepthStencilSurface swap; it does not let the app manage Metal
  // encoder boundaries, so it cannot select Memoryless here.
  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = sampleCount > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = sampleCount;
  // Depth-stencil attachments need .renderTarget on Apple GPUs. A single-sample
  // surface is never sampled (the shadow-map case comes from CreateTexture with
  // D3DUSAGE_DEPTHSTENCIL, and a single-sample depth StretchRect copies via the
  // blit path), so it stays render-target only. A multisampled surface is the
  // source of a StretchRect depth resolve, which samples it in a fragment shader,
  // so it additionally needs ShaderRead.
  info.usage =
      static_cast<WMTTextureUsage>(WMTTextureUsageRenderTarget | (sampleCount > 1 ? WMTTextureUsageShaderRead : 0));
  info.options = WMTResourceStorageModePrivate;
  // Discard is ignored for the storage choice (always Private, above). DXVK additionally REJECTS Discard=TRUE on a
  // lockable-depth format (D32/D32F/D16/S8_LOCKABLE) with INVALIDCALL, but wined3d (PRIMARY) does no such validation
  // and the adjacent MSAA+lockable-depth cell is left wine-permissive too, so adding a DXVK-only reject here without a
  // native oracle would risk failing content wine accepts. Held pending a wine-test probe (see the C1 pin's oracle-debt
  // list).
  (void)Discard;
  Rc<dxmt::Texture> dxmt_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = dxmt_texture->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return D3DERR_OUTOFVIDEOMEMORY;
  WMT::Texture rawTex = allocation->texture();
  dxmt_texture->rename(std::move(allocation));
  // Standalone depth-stencil, always DEFAULT-pool GPU-private; clear it (0.0 /
  // 0) so a first pass that opens load=Load reads defined depth rather than
  // recycled memory. The depth-stencil pixel format is what routes this to the
  // clear arm (checked ahead of the RenderTarget usage bit it also carries).
  initTextureWithZero(dxmt_texture.ptr());

  // Lockable depth (D32F_LOCKABLE; D16_LOCKABLE takes the same path if it
  // is ever advertised): give the surface a host mirror like a lockable
  // render target's, so LockRect's existing DEFAULT-pool download and
  // UnlockRect's upload work unchanged; both sides are plain blit copies,
  // which Metal permits for single-plane depth formats. Multisampled
  // surfaces are not lockable, mirroring the render-target rule.
  void *dsCpuPtr = nullptr;
  void *dsOwnedBacking = nullptr;
  uint32_t dsPitch = 0;
  if ((Format == D3DFMT_D32F_LOCKABLE || Format == D3DFMT_D16_LOCKABLE) && sampleCount == 1) {
    dsPitch = D3DFormatLockPitch(Format, Width);
    if (dsPitch != 0) {
      const uint64_t mirror_bytes = static_cast<uint64_t>(dsPitch) * Height;
      dsOwnedBacking = wsi::aligned_malloc(mirror_bytes, DXMT_PAGE_SIZE);
      if (!dsOwnedBacking)
        return D3DERR_OUTOFVIDEOMEMORY;
      std::memset(dsOwnedBacking, 0, mirror_bytes);
      dsCpuPtr = dsOwnedBacking;
    }
  }

  D3DSURFACE_DESC desc{};
  desc.Format = Format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_DEPTHSTENCIL;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Width = Width;
  desc.Height = Height;

  auto *surface = new MTLD3D9Surface(
      this, desc,
      /*container=*/static_cast<IDirect3DDevice9 *>(this), WMT::Reference<WMT::Texture>(rawTex),
      /*mipLevel=*/0,
      /*selfPin=*/true,
      /*parentTextureType=*/WMTTextureType2D,
      /*buffer=*/{},
      /*cpuPtr=*/dsCpuPtr,
      /*pitch=*/dsPitch,
      /*arraySlice=*/0,
      /*ownedBacking=*/dsOwnedBacking,
      /*dxmtTexture=*/std::move(dxmt_texture)
  );
  // CreateDepthStencilSurface surfaces are always D3DPOOL_DEFAULT.
  surface->markLosable();
  surface->AddRef();
  *ppSurface = surface;
  return D3D_OK;
}
// UpdateSurface: SYSTEMMEM -> DEFAULT blit. Symmetric inverse of
// GetRenderTargetData. Validation per DXVK d3d9_device.cpp.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::UpdateSurface(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface,
    const POINT *pDestPoint
) {
  D9DeviceLock lock = LockDevice();
  if (!pSourceSurface || !pDestinationSurface)
    return D3DERR_INVALIDCALL;
  auto *src = static_cast<MTLD3D9Surface *>(pSourceSurface);
  auto *dst = static_cast<MTLD3D9Surface *>(pDestinationSurface);
  if (src->deviceRaw() != this || dst->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  const D3DSURFACE_DESC &sd = src->desc();
  const D3DSURFACE_DESC &dd = dst->desc();
  if (sd.Pool != D3DPOOL_SYSTEMMEM || dd.Pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  if (sd.Format != dd.Format)
    return D3DERR_INVALIDCALL;
  if (sd.MultiSampleType != D3DMULTISAMPLE_NONE || dd.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;
  // UpdateSurface has no defined depth/stencil semantics; wined3d's
  // UpdateSurface path (dlls/wined3d/device.c copy_sub_resource_region)
  // rejects on either aspect, DXVK d3d9_device.cpp does the same via the
  // dst-Usage RT/DS gate (a DS pool=DEFAULT dst always carries
  // D3DUSAGE_DEPTHSTENCIL). Format-equal on both sides means one check covers
  // src and dst.
  if (IsDepthStencilFormat(sd.Format))
    return D3DERR_INVALIDCALL;
  // A currently mapped surface is not coherent for a copy: wined3d's copy
  // path rejects either a mapped source or a mapped destination sub-resource
  // with INVALIDCALL (wined3d device.c copy_sub_resource_region).
  if (src->locked() || dst->locked())
    return D3DERR_INVALIDCALL;

  uint32_t src_x0 = 0, src_y0 = 0;
  uint32_t extent_w = sd.Width, extent_h = sd.Height;
  if (pSourceRect) {
    if (pSourceRect->left < 0 || pSourceRect->top < 0 || pSourceRect->right <= pSourceRect->left ||
        pSourceRect->bottom <= pSourceRect->top || (uint32_t)pSourceRect->right > sd.Width ||
        (uint32_t)pSourceRect->bottom > sd.Height)
      return D3DERR_INVALIDCALL;
    src_x0 = pSourceRect->left;
    src_y0 = pSourceRect->top;
    extent_w = pSourceRect->right - pSourceRect->left;
    extent_h = pSourceRect->bottom - pSourceRect->top;
  }
  uint32_t dst_x0 = 0, dst_y0 = 0;
  if (pDestPoint) {
    if (pDestPoint->x < 0 || pDestPoint->y < 0)
      return D3DERR_INVALIDCALL;
    dst_x0 = pDestPoint->x;
    dst_y0 = pDestPoint->y;
  }
  if (dst_x0 + extent_w > dd.Width || dst_y0 + extent_h > dd.Height)
    return D3DERR_INVALIDCALL;
  // BC compressed formats require 4x4 block alignment on RECT edges +
  // dst point. DXVK d3d9_device.cpp enforces. Without this,
  // an unaligned rect (e.g. (1, 1)..(33, 33) into a BC1 dst) smashes
  // the dst blit at the Metal level. Exception: full-extent locks
  // that round up to the texture extent are allowed even if the
  // texture's nominal width/height isn't a multiple of 4 (DXVK same
  // shape: apps creating sub-block-sized BC textures via mip chains
  // are a real pattern). Detect on the (DXT*/BC*) Format set plus the 3Dc
  // FOURCCs (ATI1/ATI2 are BC4/BC5, 4x4 blocks; both wined3d resource.c
  // check_box_dimensions and DXVK validate them). A block-aligned partial
  // 3Dc rect still relies on stageTextureUpload's whole-level contract, so
  // only the whole-surface case (NULL rect / full rect) uploads correctly;
  // validation here at least rejects the non-block-aligned rects both refs do.
  switch (sd.Format) {
  case D3DFMT_ATI1:
  case D3DFMT_ATI2:
  case D3DFMT_DXT1:
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5: {
    auto block_aligned = [](uint32_t v, uint32_t extent) { return (v % 4u == 0u) || (v == extent); };
    if (!block_aligned(src_x0, sd.Width) || !block_aligned(src_y0, sd.Height) ||
        !block_aligned(src_x0 + extent_w, sd.Width) || !block_aligned(src_y0 + extent_h, sd.Height) ||
        !block_aligned(dst_x0, dd.Width) || !block_aligned(dst_y0, dd.Height) ||
        !block_aligned(dst_x0 + extent_w, dd.Width) || !block_aligned(dst_y0 + extent_h, dd.Height))
      return D3DERR_INVALIDCALL;
    break;
  }
  default:
    break;
  }

  // Drain any queued batched draws onto a chunk first so their writes
  // serialise before this blit on the EncodingThread's queue (Metal
  // queue ordering is caller-issue FIFO). Then post the blit as its own
  // chunk lambda so the blit runs on EncodingThread, not the calling
  // thread.
  if (!m_pendingOps.empty())
    FlushDrawBatch();

  // Stage the SYSMEM source through the upload ring on the calling thread:
  // the app may Release the SYSMEM source right after UpdateSurface, so the
  // copy must read its bytes now, not from a deferred encode-thread blit that
  // would race the free. A texture-level SYSTEMMEM surface defers its sysmem
  // mirror until first Lock, and UpdateSurface can be that first touch, so
  // materialise it first. Same lifetime shape as UpdateTexture (DXVK
  // AllocStagingBuffer).
  src->ensureHostMirror();
  if (void *src_host = src->cpuPtr()) {
    const bool compressed = IsCompressedFormat(sd.Format);
    uint64_t row_off, col_off;
    if (compressed) {
      uint32_t block_bytes = (sd.Format == D3DFMT_DXT1) ? 8u : 16u;
      row_off = static_cast<uint64_t>(src_y0 >> 2) * src->pitch();
      col_off = static_cast<uint64_t>(src_x0 >> 2) * block_bytes;
    } else {
      row_off = static_cast<uint64_t>(src_y0) * src->pitch();
      col_off = static_cast<uint64_t>(src_x0) * D3DFormatBytesPerPixel(sd.Format);
    }
    const uint8_t *src_ptr = static_cast<const uint8_t *>(src_host) + row_off + col_off;
    stageTextureUpload(
        dst->metalTexture(), dst->dxmtTexture(), dst->mipLevel(), dst->arraySlice(), WMTOrigin{dst_x0, dst_y0, 0},
        WMTSize{extent_w, extent_h, 1}, src_ptr, src->pitch(), compressed
    );
    // A DYNAMIC DEFAULT destination keeps its host mirror authoritative on Lock
    // (the readback path skips DYNAMIC surfaces, d3d9_surface.cpp), so the GPU
    // upload alone would leave a later read-Lock serving the stale mirror. Copy
    // the same staged bytes into the dst mirror too, the lockstep UpdateTexture
    // performs for its DYNAMIC dst; a non-DYNAMIC DEFAULT dst re-downloads on
    // Lock and needs nothing here. The dst rows stride the dst pitch and the
    // src rows the src pitch (a format-equal pair with matching per-column
    // bytes but a possibly different aligned/tight row stride).
    if (dd.Usage & D3DUSAGE_DYNAMIC) {
      dst->ensureHostMirror();
      if (uint8_t *dst_host = static_cast<uint8_t *>(dst->cpuPtr())) {
        const uint32_t dst_pitch = dst->pitch();
        uint64_t dst_row_off, dst_col_off;
        if (compressed) {
          uint32_t block_bytes = (dd.Format == D3DFMT_DXT1) ? 8u : 16u;
          dst_row_off = static_cast<uint64_t>(dst_y0 >> 2) * dst_pitch;
          dst_col_off = static_cast<uint64_t>(dst_x0 >> 2) * block_bytes;
        } else {
          dst_row_off = static_cast<uint64_t>(dst_y0) * dst_pitch;
          dst_col_off = static_cast<uint64_t>(dst_x0) * D3DFormatBytesPerPixel(dd.Format);
        }
        uint8_t *dst_ptr = dst_host + dst_row_off + dst_col_off;
        const uint32_t copy_row_bytes = D3DFormatRowPitch(dd.Format, extent_w);
        const uint32_t copy_rows = D3DFormatRowCount(dd.Format, extent_h);
        for (uint32_t row = 0; row < copy_rows; ++row)
          std::memcpy(
              dst_ptr + static_cast<uint64_t>(row) * dst_pitch, src_ptr + static_cast<uint64_t>(row) * src->pitch(),
              copy_row_bytes
          );
      }
    }
    dst->flagContainerAutoGenDirty();
    return D3D_OK;
  }

  // A SYSTEMMEM source always resolves to a host mirror above (standalone
  // lockable backing, or the texture-level mirror ensureHostMirror just
  // materialised), so the staging path handles every legal case; there is
  // nothing to copy if it somehow has none.
  return D3D_OK;
}
// UpdateTexture: SYSTEMMEM master -> DEFAULT mirror (push to GPU).
// Validation: same type/format/level-0 dims, src_levels >= dst_levels.
// MANAGED resources push their own contents per-Unlock and are not a valid
// source or destination here (wined3d rejects them).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) {
  D9DeviceLock lock = LockDevice();
  // Wine main thread has no outer NSAutoreleasePool; the upload path
  // touches Metal APIs (texture view, fence access) that return
  // autoreleased handles, so create one here.
  auto pool = WMT::MakeAutoreleasePool();
  if (!pSourceTexture || !pDestinationTexture)
    return D3DERR_INVALIDCALL;
  if (pSourceTexture == pDestinationTexture)
    return D3DERR_INVALIDCALL;
  // Identify both textures without calling through their app-visible vtables (see
  // SetTexture); read each type from its own internal common-texture vtable.
  MTLD3D9CommonTexture *src_common = commonTextureFromBound(pSourceTexture);
  MTLD3D9CommonTexture *dst_common = commonTextureFromBound(pDestinationTexture);
  if (!src_common || !dst_common)
    return D3DERR_INVALIDCALL;
  D3DRESOURCETYPE src_type = src_common->commonTextureType();
  D3DRESOURCETYPE dst_type = dst_common->commonTextureType();
  if (src_type != dst_type)
    return D3DERR_INVALIDCALL;

  // UpdateTexture requires a SYSTEMMEM source and a DEFAULT destination. wined3d
  // rejects a MANAGED source or destination outright (d3d9 device.c gates on the
  // managed draw_texture), and native D3D9 returns INVALIDCALL for every other
  // pool pair, so a game cannot depend on the wider set succeeding.
  auto check_pools = [](D3DPOOL src_pool, D3DPOOL dst_pool) {
    if (src_pool != D3DPOOL_SYSTEMMEM || dst_pool != D3DPOOL_DEFAULT)
      return D3DERR_INVALIDCALL;
    return D3D_OK;
  };

  if (src_type == D3DRTYPE_TEXTURE) {
    auto *src = static_cast<MTLD3D9Texture *>(pSourceTexture);
    auto *dst = static_cast<MTLD3D9Texture *>(pDestinationTexture);
    if (src->deviceRaw() != this || dst->deviceRaw() != this)
      return D3DERR_INVALIDCALL;
    if (HRESULT hr = check_pools(src->pool(), dst->pool()); FAILED(hr))
      return hr;
    // A mismatched pair is a best-effort byte copy on native, not an error. An
    // equal-bpp uncompressed pair (formats equal, or a swap like A8R8G8B8 <->
    // X8R8G8B8) has a direct byte correspondence and takes the strict path below.
    // A block-compressed mismatch has none and rejects. An unequal-bpp uncompressed
    // reinterpret has a partial one and routes to the loose copy.
    const bool fmt_equal = src->d3dFormat() == dst->d3dFormat();
    if (!fmt_equal && (IsCompressedFormat(src->d3dFormat()) || IsCompressedFormat(dst->d3dFormat())))
      return D3DERR_INVALIDCALL;
    const bool bpp_equal =
        fmt_equal || D3DFormatBytesPerPixel(src->d3dFormat()) == D3DFormatBytesPerPixel(dst->d3dFormat());
    // A destination with more levels than the source keeps its extra tail
    // untouched: levels_to_copy caps at the source's remaining count, so the
    // wider-dst case native accepts needs no gate. Native and PRIMARY wined3d both
    // accept it; only DXVK rejects.
    const UINT src_levels = src->levelCount();
    const UINT dst_levels = dst->levelCount();
    // Mip-tail correspondence: when src has a larger base than dst, dst maps
    // onto src's mip tail. src_level_offset is the offset added to dst's level
    // index to get the src level, derived from the level-0 base-size ratio
    // (update_mip_offset, wined3d device.c wined3d_device_update_texture), not
    // the level-count difference: that top-aligns an AUTOGENMIPMAP dst (app
    // level count 1) and a shorter GPU chain sharing the src base (mip-stream),
    // which a count difference wrongly rejected. Equal-base full chains yield
    // offset 0, byte-identical to the prior shape.
    D3DSURFACE_DESC src0{}, dst0{};
    if (FAILED(src->GetLevelDesc(0, &src0)) || FAILED(dst->GetLevelDesc(0, &dst0)))
      return D3DERR_INVALIDCALL;
    const UINT src_level_offset = update_mip_offset(src0.Width, src0.Height, 1, dst0.Width, dst0.Height, 1);
    // Strict when the chains correspond: equal bpp, and the source mip tail at the
    // offset matches the destination base extent. A differing base size or an
    // unequal-bpp reinterpret has no correspondence and takes the loose copy, which
    // native performs and returns D3D_OK for (the resulting pixels are a raw byte
    // reinterpret the tests tolerate; only the hr and staying in bounds matter).
    D3DSURFACE_DESC src_tail{};
    const bool correspondent = bpp_equal && SUCCEEDED(src->GetLevelDesc(src_level_offset, &src_tail)) &&
                               src_tail.Width == dst0.Width && src_tail.Height == dst0.Height;
    if (!correspondent) {
      // Loose best-effort copy for a mismatched pair (differing base size or an
      // unequal-bpp reinterpret): top-aligned level pairing, each level copying
      // its overlapping corner at origin 0. Always uncompressed (a compressed
      // mismatch was rejected above). Pixels are native's partial / reinterpreted
      // result, tolerated by the tests; only the D3D_OK and staying in bounds
      // matter. A DYNAMIC destination's host mirror is left to its readback path,
      // as a mismatched loose copy into one is not a real workload.
      if (!src->isDirty())
        return D3D_OK;
      src->restoreMirrorForSource();
      const uint8_t *loose_src = static_cast<const uint8_t *>(src->mirrorBase());
      if (!loose_src)
        return D3D_OK;
      WMT::Texture loose_dst_tex = dst->metalTexture();
      if (loose_dst_tex == nullptr)
        return D3DERR_INVALIDCALL;
      const uint32_t sbpp = D3DFormatBytesPerPixel(src->d3dFormat());
      const uint32_t dbpp = D3DFormatBytesPerPixel(dst->d3dFormat());
      if (sbpp == 0 || dbpp == 0)
        return D3D_OK;
      const UINT loose_levels = std::min(src_levels, dst_levels);
      for (UINT level = 0; level < loose_levels; ++level) {
        D3DSURFACE_DESC sd{}, dd{};
        if (FAILED(src->GetLevelDesc(level, &sd)) || FAILED(dst->GetLevelDesc(level, &dd)))
          continue;
        uint32_t w = std::min(sd.Width, dd.Width);
        const uint32_t h = std::min(sd.Height, dd.Height);
        // Unequal bpp reinterprets bytes: a destination row is w*dbpp bytes, so
        // clamp w to keep that read inside the source row (sd.Width*sbpp bytes).
        if (dbpp != sbpp)
          w = std::min<uint32_t>(w, sd.Width * sbpp / dbpp);
        if (w == 0 || h == 0)
          continue;
        // A user-memory source aliases the app's tightly-packed buffer (RowPitch),
        // not the 4-byte-aligned mirror LockPitch; sourcing at LockPitch would read
        // past that buffer for a sub-DWORD-width level (same guard as the strict
        // loop above).
        const uint32_t sp = src->isUserMemory() ? D3DFormatRowPitch(src->d3dFormat(), sd.Width)
                                                : D3DFormatLockPitch(src->d3dFormat(), sd.Width);
        if (sp == 0)
          continue;
        WMTOrigin o{0, 0, 0};
        WMTSize sz{w, h, 1};
        stageTextureUpload(
            loose_dst_tex, dst->dxmtTexture(), level, /*slice=*/0, o, sz, loose_src + src->mirrorOffset(level), sp,
            /*compressed=*/false, /*slice_pitch=*/0
        );
      }
      src->clearDirty();
      if (dst->flagAutoGenDirty())
        markAutogenMipsDirty();
      return D3D_OK;
    }
    // A MANAGED source whose mirror was evicted after upload is
    // re-materialized from the Metal texture first.
    src->restoreMirrorForSource();
    // Source from the CPU mirror base, not the Metal mirror buffer: the
    // copy below stages through the upload ring from mirrorBase(), and a
    // user-memory source aliases the app pointer with no mirror buffer. A
    // SYSTEMMEM source populates its mirror lazily on first Lock, so a
    // never-modified source has no base yet; that is the no-op success native
    // returns (nothing to upload), not an error.
    if (src->mirrorBase() == nullptr)
      return D3D_OK;
    WMT::Texture dst_tex = dst->metalTexture();
    if (dst_tex == nullptr)
      return D3DERR_INVALIDCALL;
    // Sub-E: walk only the dirty region. wined3d texture.c::texture_resource_sub_resource_unmap
    // records dirty at level-0 coords; consumer scales down per level
    // by >> level. If src isn't dirty, UpdateTexture is a no-op (the
    // GPU side already reflects the source's current contents).
    if (!src->isDirty())
      return D3D_OK;
    const RECT dr0 = src->dirtyRectLevel0();
    const bool compressed = IsCompressedFormat(src->d3dFormat());
    const UINT levels_to_copy = std::min(src_levels - src_level_offset, dst_levels);
    // A DYNAMIC DEFAULT destination keeps its host mirror authoritative on Lock
    // (the readback path skips DYNAMIC surfaces), so the GPU upload alone would
    // not be visible to a later Lock. Mirror the copied bytes into the dst's
    // sysmem copy as well. ensureMirror is a no-op for pools without a mirror
    // (a plain DEFAULT dst has none and is served by readback instead).
    dst->ensureMirror();
    uint8_t *dst_mirror = static_cast<uint8_t *>(dst->mirrorBase());
    for (UINT dst_level = 0; dst_level < levels_to_copy; ++dst_level) {
      const UINT src_level = src_level_offset + dst_level;
      D3DSURFACE_DESC d{};
      if (FAILED(src->GetLevelDesc(src_level, &d)))
        continue;
      // Scale level-0 dirty rect down to src_level coords. Round-out
      // for safety (a partially-touched pixel at level N may come from
      // multiple level-0 pixels). For compressed formats, clamp to the
      // 4x4 block grid in src_level coords.
      LONG l = dr0.left >> src_level, t = dr0.top >> src_level;
      LONG r = (dr0.right + ((1 << src_level) - 1)) >> src_level;
      LONG b = (dr0.bottom + ((1 << src_level) - 1)) >> src_level;
      if (compressed) {
        l &= ~3;
        t &= ~3;
        r = (r + 3) & ~3;
        b = (b + 3) & ~3;
      }
      LONG lw = static_cast<LONG>(d.Width), lh = static_cast<LONG>(d.Height);
      if (l < 0)
        l = 0;
      if (t < 0)
        t = 0;
      if (r > lw)
        r = lw;
      if (b > lh)
        b = lh;
      if (r <= l || b <= t)
        continue;
      WMTOrigin origin{};
      origin.x = static_cast<uint32_t>(l);
      origin.y = static_cast<uint32_t>(t);
      origin.z = 0;
      WMTSize size{};
      size.width = static_cast<uint32_t>(r - l);
      size.height = static_cast<uint32_t>(b - t);
      size.depth = 1;
      // Aligned mirror row stride (D3DFormatLockPitch): the source mirror is
      // laid out with the 4-byte-aligned pitch, so the row walk and the staging
      // src pitch must use it. The per-column step below stays tight (block
      // bytes / bpp). A user-memory source aliases the app's pointer, whose
      // rows are tightly packed (LockRect reports the tight pitch for it),
      // so the aligned recompute would shear every row past the first.
      uint32_t src_pitch = src->isUserMemory() ? D3DFormatRowPitch(src->d3dFormat(), d.Width)
                                               : D3DFormatLockPitch(src->d3dFormat(), d.Width);
      if (src_pitch == 0)
        continue;
      // Byte offset into the mirror for the dirty sub-rect: the level
      // starts at mirrorOffset(src_level); within the level, the rect
      // origin shifts by t rows x pitch + l columns x bpp (compressed:
      // block-row pitch x block-row + block-column bytes).
      uint64_t row_off, col_off;
      if (compressed) {
        // DXT1: 8 bytes/block. DXT2-5: 16 bytes/block. Same switch as
        // D3DFormatRowPitch: kept inline since this is the only caller.
        uint32_t bytes_per_block = (src->d3dFormat() == D3DFMT_DXT1) ? 8u : 16u;
        row_off = static_cast<uint64_t>(t >> 2) * src_pitch;
        col_off = static_cast<uint64_t>(l >> 2) * bytes_per_block;
      } else {
        row_off = static_cast<uint64_t>(t) * src_pitch;
        col_off = static_cast<uint64_t>(l) * D3DFormatBytesPerPixel(src->d3dFormat());
      }
      // Stage the copy through the upload ring, not a direct blit from the
      // source's mirror buffer: the app frees the SYSTEMMEM source right after
      // UpdateTexture, but the blit chunk runs later on the encode thread, so a
      // buffer-direct copy would read freed host memory. The ring copy runs now
      // while the source is alive, into lifetime-safe storage the chunk owns.
      // Matches DXVK (AllocStagingBuffer + packImageData) and wined3d upload_bo.
      if (src->mirrorBase() == nullptr)
        continue;
      // 3Dc: whole-level copy from the mirror head; the fiction's rects
      // have no block mapping (stageTextureUpload converts the layout). Reset
      // the rect origin too (t, l) so the dst-mirror lockstep below, which
      // recomputes its own offset from t, also copies from the level head;
      // leaving t set would start the dst copy mid-level and run past the
      // last level's mirror region.
      if (Is3DcFormat(src->d3dFormat())) {
        origin = WMTOrigin{0, 0, 0};
        size = WMTSize{d.Width, d.Height, 1};
        row_off = 0;
        col_off = 0;
        t = 0;
        l = 0;
      }
      const uint8_t *src_ptr =
          static_cast<const uint8_t *>(src->mirrorBase()) + src->mirrorOffset(src_level) + row_off + col_off;
      stageTextureUpload(
          dst_tex, dst->dxmtTexture(), dst_level, /*slice=*/0, origin, size, src_ptr, src_pitch, compressed
      );
      // Keep the destination mirror in lockstep (DYNAMIC dst, see above). The
      // dst mirror always strides by its own aligned LockPitch (what its
      // LockRect reports), which a user-memory source's tight RowPitch can
      // differ from, so its row offset and per-row stride are dst_pitch, not
      // src_pitch: sharing src_pitch would shear an odd-width 16bpp dst. The
      // column offset is in bytes and matches (src and dst share the format).
      if (dst_mirror) {
        const uint32_t dst_pitch = D3DFormatLockPitch(dst->d3dFormat(), d.Width);
        const uint64_t dst_row_off =
            compressed ? static_cast<uint64_t>(t >> 2) * dst_pitch : static_cast<uint64_t>(t) * dst_pitch;
        uint8_t *dst_ptr = dst_mirror + dst->mirrorOffset(dst_level) + dst_row_off + col_off;
        const uint32_t copy_row_bytes = D3DFormatRowPitch(dst->d3dFormat(), size.width);
        const uint32_t copy_rows = D3DFormatRowCount(dst->d3dFormat(), size.height);
        for (uint32_t row = 0; row < copy_rows; ++row)
          std::memcpy(
              dst_ptr + static_cast<uint64_t>(row) * dst_pitch, src_ptr + static_cast<uint64_t>(row) * src_pitch,
              copy_row_bytes
          );
      }
    }
    src->clearDirty();
    // wined3d device.c flags the dst's auto-gen mipmap state after
    // UpdateTexture success: mirrors the d3d9_texture_flag_auto_gen_mipmap
    // call in the GL path. flagAutoGenDirty is a no-op unless dst was
    // created with D3DUSAGE_AUTOGENMIPMAP; bump the pre-draw sweep epoch when
    // it actually marked so the next draw regenerates before sampling.
    if (dst->flagAutoGenDirty())
      markAutogenMipsDirty();
    return D3D_OK;
  }

  if (src_type == D3DRTYPE_VOLUMETEXTURE) {
    auto *src = static_cast<MTLD3D9VolumeTexture *>(pSourceTexture);
    auto *dst = static_cast<MTLD3D9VolumeTexture *>(pDestinationTexture);
    if (src->deviceRaw() != this || dst->deviceRaw() != this)
      return D3DERR_INVALIDCALL;
    if (HRESULT hr = check_pools(src->pool(), dst->pool()); FAILED(hr))
      return hr;
    // UpdateTexture on a mismatched pair is a best-effort copy on native, not an
    // error: the runtime memcpys the overlapping bytes and returns D3D_OK across a
    // level-count, base-size, or format difference (dlls/d3d9/tests visual.c
    // test_updatetexture and device.c test_update_volumetexture require success for
    // every pool-valid pair on a hardware driver). A block-compressed mismatch has
    // no byte correspondence and rejects. An equal-bpp pair (formats equal or a
    // same-width swap) takes the strict min-overlap loop below; an unequal-bpp
    // reinterpret takes the loose copy, whose byte-count clamp the strict loop
    // (which strides both sides by the source bpp) cannot express.
    const bool fmt_equal = src->d3dFormat() == dst->d3dFormat();
    if (!fmt_equal && (IsCompressedFormat(src->d3dFormat()) || IsCompressedFormat(dst->d3dFormat())))
      return D3DERR_INVALIDCALL;
    const bool bpp_equal =
        fmt_equal || D3DFormatBytesPerPixel(src->d3dFormat()) == D3DFormatBytesPerPixel(dst->d3dFormat());
    // When the source base is larger the destination maps onto the source's mip
    // tail; update_mip_offset (shared with the 2D and cube paths) skips the source
    // levels above the matching extent, folding Depth into the base-size max. A
    // smaller or equal source base yields offset 0, and the per-level copy below
    // takes the min-overlap, so a differing base size copies its overlapping
    // corner instead of rejecting. A destination with more levels keeps its extra
    // tail untouched (levels_to_copy caps at the source's remaining count).
    const UINT src_levels = src->levelCount();
    const UINT dst_levels = dst->levelCount();
    D3DVOLUME_DESC src0{}, dst0{};
    if (FAILED(src->GetLevelDesc(0, &src0)) || FAILED(dst->GetLevelDesc(0, &dst0)))
      return D3DERR_INVALIDCALL;
    const UINT src_level_offset =
        update_mip_offset(src0.Width, src0.Height, src0.Depth, dst0.Width, dst0.Height, dst0.Depth);
    if (!src->hasMirror())
      return D3DERR_INVALIDCALL;
    WMT::Texture dst_tex = dst->metalTexture();
    if (dst_tex == nullptr)
      return D3DERR_INVALIDCALL;
    if (!src->isDirty())
      return D3D_OK;
    if (!bpp_equal) {
      // Loose best-effort copy for an unequal-bpp reinterpret (same shape as the
      // 2D and cube branches, carrying the volume depth axis): top-aligned levels,
      // min-overlap box at origin 0, uncompressed. Pixels are native's reinterpret,
      // tolerated by the tests; only the hr and in-bounds staging matter.
      const uint8_t *loose_src = src->mapMirror();
      if (loose_src) {
        const uint32_t sbpp = D3DFormatBytesPerPixel(src->d3dFormat());
        const uint32_t dbpp = D3DFormatBytesPerPixel(dst->d3dFormat());
        const UINT loose_levels = std::min(src_levels, dst_levels);
        for (UINT level = 0; sbpp != 0 && dbpp != 0 && level < loose_levels; ++level) {
          D3DVOLUME_DESC sd{}, dd{};
          if (FAILED(src->GetLevelDesc(level, &sd)) || FAILED(dst->GetLevelDesc(level, &dd)))
            continue;
          uint32_t w = std::min(sd.Width, dd.Width);
          const uint32_t h = std::min(sd.Height, dd.Height);
          const uint32_t dp = std::min(sd.Depth, dd.Depth);
          // Unequal bpp reinterprets bytes: a destination row is w*dbpp bytes, so
          // clamp w to keep that read inside the source row (sd.Width*sbpp bytes).
          if (dbpp != sbpp)
            w = std::min<uint32_t>(w, sd.Width * sbpp / dbpp);
          if (w == 0 || h == 0 || dp == 0)
            continue;
          const uint32_t sp = D3DFormatLockPitch(src->d3dFormat(), sd.Width);
          if (sp == 0)
            continue;
          WMTOrigin o{0, 0, 0};
          WMTSize sz{w, h, dp};
          stageTextureUpload(
              dst_tex, dst->dxmtTexture(), level, /*slice=*/0, o, sz, loose_src + src->mirrorOffset(level), sp,
              /*compressed=*/false, /*slice_pitch=*/sp * sd.Height
          );
        }
        src->unmapMirror();
      }
      src->clearDirty();
      return D3D_OK;
    }
    // A destination base larger than the whole source chain leaves no source
    // level to map onto, so there is nothing to copy (guards the unsigned
    // subtraction below from wrapping when the offset exceeds the level count).
    if (src_level_offset >= src_levels)
      return D3D_OK;
    const D3DBOX db0 = src->dirtyBoxLevel0();
    const UINT levels_to_copy = std::min(src_levels - src_level_offset, dst_levels);
    const uint32_t bpp = D3DFormatBytesPerPixel(src->d3dFormat());
    // Mirror views held for the copy's duration (the brackets are
    // no-ops on 64-bit); the dst side mirrors into a DYNAMIC dst's
    // sysmem copy too (see the 2D branch), null for pools without a
    // mirror, which are served by readback.
    const uint8_t *src_base = src->mapMirror();
    if (!src_base)
      return D3DERR_INVALIDCALL;
    uint8_t *dst_mirror = dst->hasMirror() ? dst->mapMirror() : nullptr;
    for (UINT dst_level = 0; dst_level < levels_to_copy; ++dst_level) {
      const UINT src_level = src_level_offset + dst_level;
      D3DVOLUME_DESC d{}, dd{};
      if (FAILED(src->GetLevelDesc(src_level, &d)) || FAILED(dst->GetLevelDesc(dst_level, &dd)))
        continue;
      // Scale level-0 dirty box to src_level coords (round-out).
      uint32_t l = db0.Left >> src_level, t = db0.Top >> src_level, f = db0.Front >> src_level;
      uint32_t r = (db0.Right + ((1u << src_level) - 1u)) >> src_level;
      uint32_t b = (db0.Bottom + ((1u << src_level) - 1u)) >> src_level;
      uint32_t bk = (db0.Back + ((1u << src_level) - 1u)) >> src_level;
      // Clamp to the per-axis min of the two levels: a matched pair copies the
      // full level, a differing base size copies only the overlapping corner so
      // the staged region stays inside the destination extent. Equal extents make
      // this the source clamp, byte-identical to a correspondent upload.
      const uint32_t ov_w = d.Width < dd.Width ? d.Width : dd.Width;
      const uint32_t ov_h = d.Height < dd.Height ? d.Height : dd.Height;
      const uint32_t ov_d = d.Depth < dd.Depth ? d.Depth : dd.Depth;
      if (r > ov_w)
        r = ov_w;
      if (b > ov_h)
        b = ov_h;
      if (bk > ov_d)
        bk = ov_d;
      if (r <= l || b <= t || bk <= f)
        continue;
      WMTOrigin origin{};
      origin.x = l;
      origin.y = t;
      origin.z = f;
      WMTSize size{};
      size.width = r - l;
      size.height = b - t;
      size.depth = bk - f;
      uint32_t src_pitch = D3DFormatLockPitch(src->d3dFormat(), d.Width);
      if (src_pitch == 0 || bpp == 0)
        continue;
      // 3D mirror layout: level base + slice_pitch * Front + row_pitch * Top + bpp * Left.
      uint32_t slice_pitch = src_pitch * d.Height;
      const uint8_t *src_ptr = src_base + src->mirrorOffset(src_level) + static_cast<size_t>(f) * slice_pitch +
                               static_cast<size_t>(t) * src_pitch + static_cast<size_t>(l) * bpp;
      // slice=0 for 3D textures; depth lives in the Origin/Size triplet,
      // not in the array dimension. slice_pitch threads the source's
      // per-depth-slice stride so every slice is staged, not just the first.
      stageTextureUpload(
          dst_tex, dst->dxmtTexture(), dst_level, 0, origin, size, src_ptr, src_pitch, /*compressed=*/false, slice_pitch
      );
      if (dst_mirror) {
        // The destination mirror is laid out at the destination's own pitch,
        // which differs from the source's when the base sizes differ, so index
        // it with the destination strides and copy one overlapping row at a time.
        const uint32_t dst_pitch = D3DFormatLockPitch(dst->d3dFormat(), dd.Width);
        const uint32_t dst_slice_pitch = dst_pitch * dd.Height;
        uint8_t *dst_ptr = dst_mirror + dst->mirrorOffset(dst_level) + static_cast<size_t>(f) * dst_slice_pitch +
                           static_cast<size_t>(t) * dst_pitch + static_cast<size_t>(l) * bpp;
        const uint32_t copy_row_bytes = size.width * bpp;
        for (uint32_t z = 0; z < size.depth; ++z)
          for (uint32_t y = 0; y < size.height; ++y) {
            size_t src_off = static_cast<size_t>(z) * slice_pitch + static_cast<size_t>(y) * src_pitch;
            size_t dst_off = static_cast<size_t>(z) * dst_slice_pitch + static_cast<size_t>(y) * dst_pitch;
            std::memcpy(dst_ptr + dst_off, src_ptr + src_off, copy_row_bytes);
          }
      }
    }
    if (dst_mirror)
      dst->unmapMirror();
    src->unmapMirror();
    src->clearDirty();
    return D3D_OK;
  }

  // Cube branch: same shape, per face x level. Cube mirror is a
  // plain std::vector<uint8_t> (no Metal buffer), so the upload routes
  // through stageTextureUpload (CPU pointer + staging-ring memcpy)
  // rather than the buffer-direct path.
  {
    auto *src = static_cast<MTLD3D9CubeTexture *>(pSourceTexture);
    auto *dst = static_cast<MTLD3D9CubeTexture *>(pDestinationTexture);
    if (src->deviceRaw() != this || dst->deviceRaw() != this)
      return D3DERR_INVALIDCALL;
    if (HRESULT hr = check_pools(src->pool(), dst->pool()); FAILED(hr))
      return hr;
    // Format + correspondence policy identical to the 2D branch: an uncompressed
    // pair is a best-effort copy, a compressed mismatch rejects, an unequal-bpp or
    // differing-size pair takes the loose copy. A cube is square, so only the edge
    // length participates in the mip-tail check.
    const bool fmt_equal = src->d3dFormat() == dst->d3dFormat();
    if (!fmt_equal && (IsCompressedFormat(src->d3dFormat()) || IsCompressedFormat(dst->d3dFormat())))
      return D3DERR_INVALIDCALL;
    const bool bpp_equal =
        fmt_equal || D3DFormatBytesPerPixel(src->d3dFormat()) == D3DFormatBytesPerPixel(dst->d3dFormat());
    const UINT src_levels = src->levelCount();
    const UINT dst_levels = dst->levelCount();
    D3DSURFACE_DESC src0{}, dst0{};
    if (FAILED(src->GetLevelDesc(0, &src0)) || FAILED(dst->GetLevelDesc(0, &dst0)))
      return D3DERR_INVALIDCALL;
    const UINT src_level_offset = update_mip_offset(src0.Width, src0.Width, 1, dst0.Width, dst0.Width, 1);
    D3DSURFACE_DESC src_tail{};
    const bool correspondent =
        bpp_equal && SUCCEEDED(src->GetLevelDesc(src_level_offset, &src_tail)) && src_tail.Width == dst0.Width;
    if (!correspondent) {
      // Loose best-effort copy (per face), the same top-aligned min-overlap shape
      // as the 2D branch, always uncompressed.
      bool any_dirty = false;
      for (uint32_t f = 0; f < 6; ++f)
        any_dirty |= src->isDirty(f);
      if (!any_dirty)
        return D3D_OK;
      src->restoreMirrorForSource();
      const uint8_t *loose_src = src->mirrorBase();
      if (!loose_src)
        return D3D_OK;
      WMT::Texture loose_dst_tex = dst->metalTexture();
      if (loose_dst_tex == nullptr)
        return D3DERR_INVALIDCALL;
      const uint32_t sbpp = D3DFormatBytesPerPixel(src->d3dFormat());
      const uint32_t dbpp = D3DFormatBytesPerPixel(dst->d3dFormat());
      if (sbpp == 0 || dbpp == 0)
        return D3D_OK;
      const UINT loose_levels = std::min(src_levels, dst_levels);
      for (uint32_t face = 0; face < 6; ++face) {
        if (!src->isDirty(face))
          continue;
        for (UINT level = 0; level < loose_levels; ++level) {
          D3DSURFACE_DESC sd{}, dd{};
          if (FAILED(src->GetLevelDesc(level, &sd)) || FAILED(dst->GetLevelDesc(level, &dd)))
            continue;
          uint32_t w = std::min(sd.Width, dd.Width);
          const uint32_t h = std::min(sd.Height, dd.Height);
          if (dbpp != sbpp)
            w = std::min<uint32_t>(w, sd.Width * sbpp / dbpp);
          if (w == 0 || h == 0)
            continue;
          const uint32_t sp = D3DFormatLockPitch(src->d3dFormat(), sd.Width);
          if (sp == 0)
            continue;
          WMTOrigin o{0, 0, 0};
          WMTSize sz{w, h, 1};
          stageTextureUpload(
              loose_dst_tex, dst->dxmtTexture(), level, /*slice=*/face, o, sz,
              loose_src + src->mirrorOffset(face, level), sp, /*compressed=*/false, /*slice_pitch=*/0
          );
        }
        src->clearDirty(face);
      }
      if (dst->flagAutoGenDirty())
        markAutogenMipsDirty();
      return D3D_OK;
    }
    // Re-materialize an evicted MANAGED mirror before sourcing it (the
    // 2D branch does the same through restoreMirrorForSource).
    src->restoreMirrorForSource();
    const uint8_t *src_base = src->mirrorBase();
    // A never-modified SYSTEMMEM source has no mirror base yet (populated lazily
    // on first Lock); that is native's no-op success, not an error.
    if (!src_base)
      return D3D_OK;
    WMT::Texture dst_tex = dst->metalTexture();
    if (dst_tex == nullptr)
      return D3DERR_INVALIDCALL;
    // Keep a DYNAMIC DEFAULT destination's host mirror authoritative: its Lock
    // skips the GPU readback, so the upload must also land in the mirror (same
    // as the 2D branch). ensureMirror is a no-op for pools without a mirror; the
    // const_cast writes the backing the const accessor reads back.
    dst->ensureMirror();
    uint8_t *dst_mirror = const_cast<uint8_t *>(dst->mirrorBase());
    const bool compressed = IsCompressedFormat(src->d3dFormat());
    const UINT levels_to_copy = std::min(src_levels - src_level_offset, dst_levels);
    const uint32_t bpp = compressed ? 0u : D3DFormatBytesPerPixel(src->d3dFormat());
    const uint32_t bytes_per_block = compressed ? (src->d3dFormat() == D3DFMT_DXT1 ? 8u : 16u) : 0u;
    for (uint32_t face = 0; face < 6; ++face) {
      if (!src->isDirty(face))
        continue;
      const RECT dr0 = src->dirtyRectLevel0(face);
      for (UINT dst_level = 0; dst_level < levels_to_copy; ++dst_level) {
        const UINT src_level = src_level_offset + dst_level;
        D3DSURFACE_DESC d{};
        if (FAILED(src->GetLevelDesc(src_level, &d)))
          continue;
        LONG l = dr0.left >> src_level, t = dr0.top >> src_level;
        LONG r = (dr0.right + ((1 << src_level) - 1)) >> src_level;
        LONG b = (dr0.bottom + ((1 << src_level) - 1)) >> src_level;
        if (compressed) {
          l &= ~3;
          t &= ~3;
          r = (r + 3) & ~3;
          b = (b + 3) & ~3;
        }
        LONG lw = static_cast<LONG>(d.Width), lh = static_cast<LONG>(d.Height);
        if (l < 0)
          l = 0;
        if (t < 0)
          t = 0;
        if (r > lw)
          r = lw;
        if (b > lh)
          b = lh;
        if (r <= l || b <= t)
          continue;
        WMTOrigin origin{};
        origin.x = static_cast<uint32_t>(l);
        origin.y = static_cast<uint32_t>(t);
        origin.z = 0;
        WMTSize size{};
        size.width = static_cast<uint32_t>(r - l);
        size.height = static_cast<uint32_t>(b - t);
        size.depth = 1;
        uint32_t src_pitch = D3DFormatLockPitch(src->d3dFormat(), d.Width);
        if (src_pitch == 0)
          continue;
        size_t row_off, col_off;
        if (compressed) {
          row_off = static_cast<size_t>(t >> 2) * src_pitch;
          col_off = static_cast<size_t>(l >> 2) * bytes_per_block;
        } else {
          row_off = static_cast<size_t>(t) * src_pitch;
          col_off = static_cast<size_t>(l) * bpp;
        }
        // 3Dc: whole-face-level copy from the mirror head, the 2D branch's arm
        // (stageTextureUpload converts the linear fiction to real BC; the
        // fiction's partial rects have no block mapping). Reset origin/size to
        // the level extent and row_off/col_off to 0 so both the upload and the
        // dst-mirror lockstep below (which strides from row_off/col_off) copy
        // from the level head; the cube lockstep reads row_off/col_off directly,
        // so unlike the 2D arm it needs no separate t/l reset. Without this a
        // partial-dirty ATI1/ATI2 face would stage fiction-offset garbage and
        // hand Metal a non-block-aligned BC blit origin.
        if (Is3DcFormat(src->d3dFormat())) {
          origin = WMTOrigin{0, 0, 0};
          size = WMTSize{d.Width, d.Height, 1};
          row_off = 0;
          col_off = 0;
        }
        const void *src_ptr = src_base + src->mirrorOffset(face, src_level) + row_off + col_off;
        // slice=face: cube faces are array slices on a MTLTextureCube;
        // stageTextureUpload's slice parameter routes the blit to the
        // correct face plane.
        stageTextureUpload(dst_tex, dst->dxmtTexture(), dst_level, face, origin, size, src_ptr, src_pitch, compressed);
        // Keep the DYNAMIC destination mirror in lockstep (same sub-rect: src and
        // dst share format and per-level dimensions here).
        if (dst_mirror) {
          const uint8_t *src_rows = static_cast<const uint8_t *>(src_ptr);
          uint8_t *dst_ptr = dst_mirror + dst->mirrorOffset(face, dst_level) + row_off + col_off;
          const uint32_t copy_row_bytes = D3DFormatRowPitch(dst->d3dFormat(), size.width);
          const uint32_t copy_rows = D3DFormatRowCount(dst->d3dFormat(), size.height);
          for (uint32_t row = 0; row < copy_rows; ++row)
            std::memcpy(
                dst_ptr + static_cast<uint64_t>(row) * src_pitch, src_rows + static_cast<uint64_t>(row) * src_pitch,
                copy_row_bytes
            );
        }
      }
      src->clearDirty(face);
    }
    // Cube AUTOGENMIPMAP regen: same shape as the 2D branch (wined3d
    // device.c). Flag the dst's lazy mipmap-dirty bit; no-op unless the cube
    // was created with D3DUSAGE_AUTOGENMIPMAP; bump the sweep epoch on a mark.
    if (dst->flagAutoGenDirty())
      markAutogenMipsDirty();
    return D3D_OK;
  }
}
// GetRenderTargetData: RT -> SYSTEMMEM blit. Validation per DXVK
// d3d9_device.cpp, which forwards a DEFAULT-pool destination to StretchRect.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) {
  D9DeviceLock lock = LockDevice();
  // Wine main thread has no outer NSAutoreleasePool. GetRenderTargetData
  // commits a sync chunk and waits; autoreleased Metal handles (blit
  // encoder, fence) leak across every screenshot capture without it.
  auto pool = WMT::MakeAutoreleasePool();
  // A lost non-Ex device fails the readback with DEVICELOST (DXVK
  // d3d9_device.cpp GetRenderTargetData); Ex devices never enter Lost.
  if (!m_isEx && m_deviceState.load(std::memory_order_relaxed) == DeviceState::Lost)
    return D3DERR_DEVICELOST;
  if (!pRenderTarget || !pDestSurface)
    return D3DERR_INVALIDCALL;
  auto *src = static_cast<MTLD3D9Surface *>(pRenderTarget);
  auto *dst = static_cast<MTLD3D9Surface *>(pDestSurface);
  if (src->deviceRaw() != this || dst->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (src == dst)
    return D3D_OK;
  const D3DSURFACE_DESC &sd = src->desc();
  const D3DSURFACE_DESC &dd = dst->desc();
  if (sd.Format != dd.Format)
    return D3DERR_INVALIDCALL;
  // The surface desc stores mip-shifted per-level extents (d3d9_texture.cpp),
  // so this compares the (texture, mipLevel) extent, not the level-0 size.
  if (sd.Width != dd.Width || sd.Height != dd.Height)
    return D3DERR_INVALIDCALL;
  // A DEFAULT-pool destination is a GPU-to-GPU copy, not a host readback;
  // forward it to the blit path as DXVK does. It goes through the impl with
  // from_readback set because the public StretchRect rejects a render-target
  // source into a plain surface, while the shared planner still permits a
  // DEFAULT offscreen-plain destination and rejects a plain-texture level.
  if (dd.Pool == D3DPOOL_DEFAULT)
    return stretchRectImpl(pRenderTarget, nullptr, pDestSurface, nullptr, D3DTEXF_NONE, /* from_readback */ true);
  if (sd.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;

  // Drain queued draws and any staged clear onto chunks first so the
  // read sees them. Then post the blit as its own chunk lambda + wait
  // on the chunk's completion before returning so the caller's next
  // LockRect sees fresh bytes.
  FlushDrawBatch();
  flushOpenWork();

  WMT::Reference<WMT::Texture> src_tex_retain(src->metalTexture());
  obj_handle_t src_texture_handle = src->metalTexture().handle;
  obj_handle_t dst_buffer_handle = dst->metalBuffer().handle;
  uint32_t src_mip = src->mipLevel();
  uint32_t dst_pitch = dst->pitch();
  uint32_t width = sd.Width;
  uint32_t height = sd.Height;
  // Block-row count for the destination layout: equals the pixel height
  // for uncompressed formats, height/4 for the block-compressed ones.
  const uint32_t row_count = D3DFormatRowCount(dd.Format, dd.Height);

  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;

  if (dst_buffer_handle != 0) {
    // Host-visible buffer backing (a buffer-backed SYSTEMMEM texture
    // surface): copy straight into it with explicit bytesPerRow; the
    // caller's LockRect maps that same buffer. The linear-texture-of-
    // buffer view path drops trailing rows on virtualized Apple Silicon,
    // so address the buffer directly.
    WMT::Reference<WMT::Buffer> dst_buf_retain(dst->metalBuffer());
    auto *chunk = m_dxmtQueue->CurrentChunk();
    chunk->emitcc([src_tex_retain = std::move(src_tex_retain), dst_buf_retain = std::move(dst_buf_retain),
                   src_texture_handle, dst_buffer_handle, src_mip, dst_pitch, width, height, row_count, event_handle,
                   signal_seq](ArgumentEncodingContext &ctx) mutable {
      ctx.startBlitPass();
      auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
      cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
      cmd.src = src_texture_handle;
      cmd.slice = 0;
      cmd.level = src_mip;
      cmd.origin = WMTOrigin{0, 0, 0};
      cmd.size = WMTSize{width, height, 1};
      cmd.dst = dst_buffer_handle;
      cmd.offset = 0;
      cmd.bytes_per_row = dst_pitch;
      cmd.bytes_per_image = dst_pitch * row_count;
      ctx.endPass();
      ctx.signalEventByHandle(event_handle, signal_seq);
    });
    ++m_currentCmdSeq;
    refreshSignaledAndTrimRings();
    // Synchronous from the app's perspective; LockRect maps the buffer
    // immediately after this call, so wait for the chunk's encode thread
    // and the per-cmdbuf completion event (the GPU-side signal) before
    // returning. m_currentCmdSeq was bumped after posting, so the chunk's
    // signal target is the pre-bump value.
    uint64_t seq = m_dxmtQueue->CurrentSeqId();
    commitCurrentChunkTimed();
    m_dxmtQueue->WaitCPUFence(seq);
    if (!waitForGpuOrDeviceError(signal_seq))
      return D3DERR_DEVICELOST;
    return D3D_OK;
  }

  // Mirror-backed destination (a CreateOffscreenPlainSurface SYSTEMMEM /
  // SCRATCH surface): the lock storage is a host mirror, and the surface's
  // Metal texture is a CpuInvisible placeholder that is never a CPU-readable
  // endpoint. A texture-to-texture blit into that placeholder would never
  // reach the mirror the caller locks. Stage the render target into an
  // upload-ring block, then memcpy the rows into the mirror: the same
  // readback shape GetFrontBufferData uses.
  //
  // A MANAGED destination is nonconformant: the runtime pins the dest to a
  // SYSTEMMEM (or SCRATCH) offscreen-plain or SYSTEMMEM texture level, and
  // native rejects a MANAGED dest outright. The mirror-backed path also cannot
  // honor it even permissively: a MANAGED level re-downloads its managed
  // content on the next LockRect (materializeLevelForLock), clobbering the
  // readback written here. Reject with a one-shot warn. (A buffer-backed dst
  // took the host-visible branch above, which has no reload to clobber.)
  if (dd.Pool == D3DPOOL_MANAGED) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      Logger::warn(
          "d3d9: GetRenderTargetData into a mirror-backed MANAGED surface is unsupported; returning INVALIDCALL"
      );
    return D3DERR_INVALIDCALL;
  }
  // A SYSTEMMEM / SCRATCH texture sub-resource (CreateTexture + GetSurfaceLevel)
  // defers its host mirror, cpu_ptr and pitch alike, until the first LockRect;
  // materialise it here the way GetFrontBufferData does before its pointer
  // check. The standalone offscreen-plain target above has an eager mirror, a
  // no-op here.
  dst->ensureHostMirror();
  uint8_t *dst_base = static_cast<uint8_t *>(dst->cpuPtr());
  if (!dst_base)
    return D3DERR_INVALIDCALL;
  // dst_pitch was captured before the mirror existed (0 for a lazy
  // sub-resource); refresh it now that materialisation set the level pitch.
  dst_pitch = dst->pitch();
  const size_t total_bytes = static_cast<size_t>(dst_pitch) * row_count;
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto span = tryRingAllocate(m_uploadRing, m_currentCmdSeq, coherent_id, total_bytes, 16);
  if (!span)
    return D3DERR_OUTOFVIDEOMEMORY;
  const uint64_t block_offset = span.offset;
  obj_handle_t ring_buffer_handle = span.handle;

  auto *chunk = m_dxmtQueue->CurrentChunk();
  chunk->emitcc([src_tex_retain = std::move(src_tex_retain), src_texture_handle, ring_buffer_handle, block_offset,
                 dst_pitch, width, height, row_count, src_mip, event_handle,
                 signal_seq](ArgumentEncodingContext &ctx) mutable {
    ctx.startBlitPass();
    auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
    cmd.src = src_texture_handle;
    cmd.slice = 0;
    cmd.level = src_mip;
    cmd.origin = WMTOrigin{0, 0, 0};
    cmd.size = WMTSize{width, height, 1};
    cmd.dst = ring_buffer_handle;
    cmd.offset = block_offset;
    cmd.bytes_per_row = dst_pitch;
    cmd.bytes_per_image = dst_pitch * row_count;
    ctx.endPass();
    ctx.signalEventByHandle(event_handle, signal_seq);
  });
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();

  uint64_t seq = m_dxmtQueue->CurrentSeqId();
  commitCurrentChunkTimed();
  m_dxmtQueue->WaitCPUFence(seq);
  if (!waitForGpuOrDeviceError(signal_seq))
    return D3DERR_DEVICELOST;

  // The ring block is laid out at the mirror's row pitch, so the copy out
  // is one contiguous span.
  const uint8_t *src_base = static_cast<const uint8_t *>(span.host);
  std::memcpy(dst_base, src_base, total_bytes);
  // Record the level-0 dirty region so a later UpdateTexture that sources this
  // SYSTEMMEM texture level picks the readback up (wined3d marks the blt dst
  // region dirty, texture.c wined3d_texture_blt). A no-op on a standalone
  // offscreen surface, whose container is not a texture.
  dst->flagContainerDirtyRegion(nullptr);
  return D3D_OK;
}
// GetFrontBufferData: synchronous backbuffer -> SYSTEMMEM readback.
// DXVK d3d9_swapchain.cpp GetFrontBufferData is the model: mismatched
// extents copy the intersection, a non-A8R8G8B8 display-format source is
// converted to the spec's A8R8G8B8 destination per pixel,
// and in windowed mode the front buffer lands at the window's
// client-area position inside a desktop-sized destination (the API
// takes a whole-screen screenshot; the rest of the screen is not
// captured). dxmt has no distinct front buffer; the persistent
// backbuffer doubles as the most recent frame, matching DXVK's
// SWAPEFFECT_COPY / single-backbuffer shortcut. The placement copy is
// CPU-side off an upload-ring readback: screenshot path, not hot.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) {
  D9DeviceLock lock = LockDevice();
  // The device entry point names the implicit chain; additional chains
  // route through their own IDirect3DSwapChain9::GetFrontBufferData.
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return frontBufferReadback(m_implicitSwapChain, pDestSurface);
}

HRESULT
MTLD3D9Device::frontBufferReadback(MTLD3D9SwapChain *chain, IDirect3DSurface9 *pDestSurface) {
  // Wine main thread has no outer NSAutoreleasePool; the readback
  // commits a sync chunk like GetRenderTargetData.
  auto pool = WMT::MakeAutoreleasePool();
  if (!chain || !pDestSurface)
    return D3DERR_INVALIDCALL;
  auto *dst = static_cast<MTLD3D9Surface *>(pDestSurface);
  if (dst->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  const D3DSURFACE_DESC &dd = dst->desc();
  if (dd.Pool != D3DPOOL_SYSTEMMEM && dd.Pool != D3DPOOL_SCRATCH)
    return D3DERR_INVALIDCALL;

  MTLD3D9Surface *front = chain->backBuffer();
  // A failed Reset can leave the swapchain without a backbuffer; bail rather
  // than dereference a null front buffer.
  if (!front)
    return D3DERR_INVALIDCALL;
  const D3DSURFACE_DESC &sd = front->desc();
  // An MSAA backbuffer cannot feed the texture-to-buffer copy; resolve
  // it into the swapchain's single-sample twin first (the same average
  // resolve Present encodes) and read that back. A fresh resolve, not
  // the last Present's, so rendering since then is captured the way the
  // single-sample arm's persistent backbuffer is.
  Rc<dxmt::Texture> msaa_resolve;
  if (sd.MultiSampleType != D3DMULTISAMPLE_NONE) {
    msaa_resolve = chain->resolveTarget();
    if (msaa_resolve == nullptr)
      return D3DERR_INVALIDCALL;
  }
  // The API converts the front buffer to the caller's destination, which the
  // spec pins to A8R8G8B8 (its 32-bit BGRA sibling X8R8G8B8 shares the byte
  // layout). A same-format destination is copied verbatim below; a display-
  // format source into a 32-bit BGRA destination is decoded per pixel
  // (DecodeFrontBufferPixelToBGRA8), the same conversion wined3d's blt and
  // DXVK's blit-convert temp image perform. A non-display source or a
  // destination that is neither the source format nor 32-bit BGRA has no
  // defined decode and stays rejected.
  const bool dst_is_bgra8 = dd.Format == D3DFMT_A8R8G8B8 || dd.Format == D3DFMT_X8R8G8B8;
  const bool decode = sd.Format != dd.Format;
  if (decode && !(dst_is_bgra8 && IsFrontBufferReadbackSourceFormat(sd.Format)))
    return D3DERR_INVALIDCALL;
  // A texture-level destination allocates its host mirror lazily; the
  // same materialisation LockRect performs before its pointer check.
  dst->ensureHostMirror();
  if (front->metalTexture().handle == 0 || dst->cpuPtr() == nullptr)
    return D3DERR_INVALIDCALL;

  // Same drain + ring-readback + wait shape as readbackSurfaceMirror:
  // pull the whole backbuffer into an upload-ring block, then finish
  // the placement / conversion on the CPU against the dst mirror.
  FlushDrawBatch();
  flushOpenWork();

  const uint32_t src_w = sd.Width;
  const uint32_t src_h = sd.Height;
  // Equal formats share a texel size by construction; the X8 -> A8 pair
  // is 4 bytes on both sides.
  const uint32_t bpp = D3DFormatBytesPerPixel(sd.Format);
  if (bpp == 0)
    return D3DERR_INVALIDCALL;
  const uint32_t src_pitch = src_w * bpp;
  const size_t total_bytes = static_cast<size_t>(src_pitch) * src_h;
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto span = tryRingAllocate(m_uploadRing, m_currentCmdSeq, coherent_id, total_bytes, 16);
  if (!span)
    return D3DERR_OUTOFVIDEOMEMORY;
  const uint64_t block_offset = span.offset;

  // The COPY front canvas, when rect presents materialised one, is the
  // image the presenter actually blits; read the front from it so the
  // grab reflects composited partial presents. Chains without rect
  // presents keep reading the backbuffer.
  Rc<dxmt::Texture> front_canvas = chain->frontCanvas();
  WMT::Reference<WMT::Texture> src_tex_retain(
      msaa_resolve != nullptr   ? msaa_resolve->current()->texture()
      : front_canvas != nullptr ? front_canvas->current()->texture()
                                : front->metalTexture()
  );
  obj_handle_t src_texture_handle = src_tex_retain.handle;
  obj_handle_t ring_buffer_handle = span.handle;
  uint32_t src_mip = msaa_resolve != nullptr || front_canvas != nullptr ? 0 : front->mipLevel();
  Rc<dxmt::Texture> resolve_src = msaa_resolve != nullptr ? front->dxmtTexture() : nullptr;

  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;

  auto *chunk = m_dxmtQueue->CurrentChunk();
  chunk->emitcc([src_tex_retain = std::move(src_tex_retain), src_texture_handle, ring_buffer_handle, block_offset,
                 src_pitch, src_w, src_h, src_mip, event_handle, signal_seq, resolve_src = std::move(resolve_src),
                 resolve_dst = std::move(msaa_resolve)](ArgumentEncodingContext &ctx) mutable {
    if (resolve_src != nullptr)
      ctx.resolve_texture_cmd.resolve(
          resolve_src, resolve_src->fullView, resolve_dst, resolve_dst->fullView, ResolveTextureMode::Average,
          WMTScissorRect{0, 0, src_w, src_h}, WMTOrigin{0, 0, 0}, WMTSize{src_w, src_h, 1}
      );
    ctx.startBlitPass();
    auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
    cmd.src = src_texture_handle;
    cmd.slice = 0;
    cmd.level = src_mip;
    cmd.origin = WMTOrigin{0, 0, 0};
    cmd.size = WMTSize{src_w, src_h, 1};
    cmd.dst = ring_buffer_handle;
    cmd.offset = block_offset;
    cmd.bytes_per_row = src_pitch;
    cmd.bytes_per_image = src_pitch * src_h;
    ctx.endPass();
    ctx.signalEventByHandle(event_handle, signal_seq);
  });
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();

  uint64_t seq = m_dxmtQueue->CurrentSeqId();
  commitCurrentChunkTimed();
  m_dxmtQueue->WaitCPUFence(seq);
  if (!waitForGpuOrDeviceError(signal_seq))
    return D3DERR_DEVICELOST;

  // Windowed mode places the copy at the window's client-area origin
  // in screen coordinates (DXVK queries ClientToScreen the same way).
  // Fullscreen, and the headless host build, use (0, 0).
  int32_t dst_x = 0;
  int32_t dst_y = 0;
#ifdef _WIN32
  if (chain->windowed()) {
    HWND window = chain->hWindow() ? chain->hWindow() : m_creationParams.hFocusWindow;
    POINT point = {0, 0};
    if (window && ::ClientToScreen(window, &point)) {
      dst_x = point.x;
      dst_y = point.y;
    }
  }
#endif

  const uint32_t dst_pitch = dst->pitch();
  // The destination texel stride, separate from the source bpp: a decode
  // reads a 2- or 4-byte source pixel and writes a 4-byte BGRA8 one.
  const uint32_t dst_bpp = D3DFormatBytesPerPixel(dd.Format);
  uint8_t *dst_base = static_cast<uint8_t *>(dst->cpuPtr());
  const uint8_t *src_base = static_cast<const uint8_t *>(span.host);
  // Anything the source doesn't cover stays deterministic: DXVK
  // zero-clears the destination buffer when the extents mismatch.
  if (dd.Width != src_w || dd.Height != src_h || dst_x != 0 || dst_y != 0)
    std::memset(dst_base, 0, static_cast<size_t>(dst_pitch) * dd.Height);
  // Intersection copy. A window partially off-screen to the left/top
  // clips on the source side; the destination extent clips the
  // right/bottom.
  int64_t src_x = 0;
  int64_t src_y = 0;
  if (dst_x < 0) {
    src_x = -dst_x;
    dst_x = 0;
  }
  if (dst_y < 0) {
    src_y = -dst_y;
    dst_y = 0;
  }
  const int64_t copy_w = std::min<int64_t>(static_cast<int64_t>(src_w) - src_x, static_cast<int64_t>(dd.Width) - dst_x);
  const int64_t copy_h =
      std::min<int64_t>(static_cast<int64_t>(src_h) - src_y, static_cast<int64_t>(dd.Height) - dst_y);
  if (copy_w > 0) {
    const size_t row_bytes = static_cast<size_t>(copy_w) * bpp;
    for (int64_t y = 0; y < copy_h; ++y) {
      const uint8_t *src_row = src_base + static_cast<size_t>(src_y + y) * src_pitch + static_cast<size_t>(src_x) * bpp;
      uint8_t *dst_row = dst_base + static_cast<size_t>(dst_y + y) * dst_pitch + static_cast<size_t>(dst_x) * dst_bpp;
      if (!decode) {
        // Same format: the source row already matches the destination layout.
        std::memcpy(dst_row, src_row, row_bytes);
      } else {
        // Convert each pixel to the destination's 32-bit BGRA8.
        for (int64_t x = 0; x < copy_w; ++x)
          DecodeFrontBufferPixelToBGRA8(sd.Format, src_row + static_cast<size_t>(x) * bpp, dst_row + x * dst_bpp);
      }
    }
  }
  // Record the dirty region so a later UpdateTexture sourcing this SYSTEMMEM
  // texture level picks the grab up (same as GetRenderTargetData; wined3d
  // texture.c). A no-op on a standalone offscreen surface.
  dst->flagContainerDirtyRegion(nullptr);
  return D3D_OK;
}
// StretchRect: DEFAULT->DEFAULT surface blit. Validation per DXVK
// d3d9_device.cpp. MVP path: same-format, same-extent, no MSAA,
// no depth-stencil. Stretch / format-convert / resolve / DS land in
// follow-ups (each routes through a different Metal path: render-pass
// blit, MTLBlitCommandEncoder copy with format reinterpret, or a DS-
// aware copy that respects aspectMask).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::StretchRect(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect,
    D3DTEXTUREFILTERTYPE Filter
) {
  return stretchRectImpl(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter, /* from_readback */ false);
}

// GetRenderTargetData's DEFAULT-pool forward copies a render target into a plain
// offscreen surface, which the public StretchRect rejects (a render target into a
// plain surface is INVALIDCALL on native). It reaches the same blit path through
// here with from_readback set so the planner's plain-destination gate lets it pass.
HRESULT
MTLD3D9Device::stretchRectImpl(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect,
    D3DTEXTUREFILTERTYPE Filter, bool from_readback
) {
  D9DeviceLock lock = LockDevice();
  // No autorelease pool needed here post-chunk-migration. The body
  // only does validation, rect parsing, and QueueBlitOp (a vector
  // push_back); every Metal-touching call moved into the chunk
  // lambda's encoder loop, which runs under the per-flush pool that
  // FlushDrawBatch already wraps. A local pool here would push/pop
  // an empty NSAutoreleasePool on the calling thread per call.
  if (!pSourceSurface || !pDestSurface)
    return D3DERR_INVALIDCALL;
  if (pSourceSurface == pDestSurface)
    return D3DERR_INVALIDCALL;
  auto *src = static_cast<MTLD3D9Surface *>(pSourceSurface);
  auto *dst = static_cast<MTLD3D9Surface *>(pDestSurface);
  if (src->deviceRaw() != this || dst->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  // A mapped participant is INVALIDCALL: wined3d texture.c wined3d_texture_blt
  // returns WINEDDERR_SURFACEBUSY (d3d9 remaps to INVALIDCALL) for a locked
  // source or destination. The same locked() gate UpdateSurface applies (both
  // are GPU copies that must not straddle an outstanding CPU map).
  if (src->locked() || dst->locked())
    return D3DERR_INVALIDCALL;
  const D3DSURFACE_DESC &sd = src->desc();
  const D3DSURFACE_DESC &dd = dst->desc();
  // access<Compute> in the emit paths is load-bearing for fence dependency
  // correctness (see PendingBlitOp), so both participants must carry a
  // dxmt::Texture wrapper; guard defensively against legacy callsites.
  Rc<dxmt::Texture> src_tex = src->dxmtTexture();
  Rc<dxmt::Texture> dst_tex = dst->dxmtTexture();
  if (!src_tex || !dst_tex)
    return D3DERR_INVALIDCALL;

  // Reduce both participants to plain values and let the shared planner
  // validate + pick the kind (pool matrix, dst-usage gate, DS rules, MSAA
  // matrix, rect bounds + BC alignment, X->A alpha routing). The decision table
  // stays a pure function so it can be exercised without a Metal device.
  dxmt::StretchRectSurface plan_src{
      src->metalPixelFormat(),     sd.Format, sd.Pool, sd.Usage, sd.Width, sd.Height, src_tex->sampleCount(),
      src->isTextureSubresource(),
  };
  dxmt::StretchRectSurface plan_dst{
      dst->metalPixelFormat(),     dd.Format, dd.Pool, dd.Usage, dd.Width, dd.Height, dst_tex->sampleCount(),
      dst->isTextureSubresource(),
  };
  dxmt::StretchRectPlan plan =
      dxmt::plan_stretch_rect(plan_src, plan_dst, pSourceRect, pDestRect, Filter, m_inScene, from_readback);
  if (FAILED(plan.hr)) {
    // Leave a breadcrumb for the two multisampled depth-stencil deferrals
    //: the source needs a depth-resolve encoder (Metal depth attachment
    // resolve), the destination needs the resolved-location scheme wined3d
    // tracks per subresource, so a title that trips either is diagnosable
    // instead of getting a silent INVALIDCALL. Both read the lowered sample
    // count the planner decided on, since D3DMULTISAMPLE_NONMASKABLE at quality
    // 0 lowers to a single sample and would otherwise misattribute an unrelated
    // rejection. (Single-sample -> multisample colour is handled via the Stretch
    // broadcast path, so this rejection does not apply to it.)
    if (IsDepthStencilFormat(sd.Format)) {
      if (plan_src.sample_count > 1)
        Logger::warn("d3d9: StretchRect: multisampled depth-stencil resolve is not implemented; rejecting");
      else if (plan_dst.sample_count > 1)
        Logger::warn("d3d9: StretchRect: multisampled depth-stencil destination is not implemented; rejecting");
    }
    return plan.hr;
  }

  // The render-pass Stretch (and the stretch leg of a resolve-then-stretch)
  // samples the source through its D3D9 channel swizzle so a format-converting
  // stretch from a fixup-needing source (L8, A4R4G4B4, V8U8, ATI2, 2-channel, or
  // the X->A alpha fix) reads the shape D3D9 promises rather than raw
  // storage channels, the same fixup the sample-bind path applies. Copy and
  // Resolve ignore it; {Zero,Zero,Zero,Zero} is the identity sentinel.
  WMTTextureSwizzleChannels src_swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha
  };
  {
    WMTTextureSwizzleChannels ssw = D3DFormatSamplerSwizzle(sd.Format);
    if (!(ssw.r == WMTTextureSwizzleZero && ssw.g == WMTTextureSwizzleZero && ssw.b == WMTTextureSwizzleZero &&
          ssw.a == WMTTextureSwizzleZero))
      src_swizzle = ssw;
  }

  // Queue the blit(s) into the arrival-order op stream (no per-call flush).
  switch (plan.kind) {
  case dxmt::StretchRectKind::Copy: {
    PendingBlitOp op;
    op.kind = PendingBlitOp::Kind::Copy;
    op.src_tex = std::move(src_tex);
    op.dst_tex = std::move(dst_tex);
    op.src_mip = src->mipLevel();
    op.dst_mip = dst->mipLevel();
    op.src_slice = src->arraySlice();
    op.dst_slice = dst->arraySlice();
    op.src_origin = WMTOrigin{plan.src_x, plan.src_y, 0};
    op.dst_origin = WMTOrigin{plan.dst_x, plan.dst_y, 0};
    op.size = WMTSize{plan.src_w, plan.src_h, 1};
    QueueBlitOp(std::move(op));
    break;
  }
  case dxmt::StretchRectKind::Stretch: {
    PendingBlitOp op;
    op.kind = PendingBlitOp::Kind::Stretch;
    op.src_tex = std::move(src_tex);
    op.dst_tex = std::move(dst_tex);
    op.src_mip = src->mipLevel();
    op.dst_mip = dst->mipLevel();
    op.src_slice = src->arraySlice();
    op.dst_slice = dst->arraySlice();
    op.src_origin = WMTOrigin{plan.src_x, plan.src_y, 0};
    op.dst_origin = WMTOrigin{plan.dst_x, plan.dst_y, 0};
    op.size = WMTSize{plan.src_w, plan.src_h, 1};
    op.dst_size = WMTSize{plan.dst_w, plan.dst_h, 1};
    op.filter = Filter;
    op.src_swizzle = src_swizzle;
    QueueBlitOp(std::move(op));
    break;
  }
  case dxmt::StretchRectKind::Resolve:
  case dxmt::StretchRectKind::DepthResolve: {
    PendingBlitOp op;
    op.kind = plan.kind == dxmt::StretchRectKind::DepthResolve ? PendingBlitOp::Kind::DepthResolve
                                                               : PendingBlitOp::Kind::Resolve;
    op.src_tex = std::move(src_tex);
    op.dst_tex = std::move(dst_tex);
    op.src_mip = src->mipLevel();
    op.dst_mip = dst->mipLevel();
    op.src_slice = src->arraySlice();
    op.dst_slice = dst->arraySlice();
    op.src_origin = WMTOrigin{plan.src_x, plan.src_y, 0};
    op.dst_origin = WMTOrigin{plan.dst_x, plan.dst_y, 0};
    op.size = WMTSize{plan.src_w, plan.src_h, 1};
    op.dst_size = WMTSize{plan.dst_w, plan.dst_h, 1};
    QueueBlitOp(std::move(op));
    break;
  }
  case dxmt::StretchRectKind::ResolveThenStretch: {
    // Scaled / fixup-source MSAA resolve: resolve the source into a
    // single-sample transient at the source-rect extent (1:1, no scale, no
    // convert), then Stretch that transient into the real destination so the
    // scale, the format convert, and the source channel fixup are all honoured.
    // A bare 1:1 Resolve to the destination would crop instead of scale.
    // References: wined3d surface.c (scale + resolve via the blitter chain),
    // DXVK d3d9_device.cpp (falls off the resolve fast path to blitImageView).
    // Both ops ride the same arrival-order stream; the access<> write on the
    // transient in the resolve and the read in the stretch order them for free.
    Rc<dxmt::Texture> transient = createTransientResolveTarget(src->metalPixelFormat(), plan.src_w, plan.src_h);
    if (!transient) {
      Logger::warn("d3d9: StretchRect: failed to allocate scaled-resolve transient");
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    PendingBlitOp resolve_op;
    resolve_op.kind = PendingBlitOp::Kind::Resolve;
    resolve_op.src_tex = std::move(src_tex);
    resolve_op.dst_tex = transient;
    resolve_op.src_mip = src->mipLevel();
    resolve_op.dst_mip = 0;
    resolve_op.src_slice = src->arraySlice();
    resolve_op.dst_slice = 0;
    resolve_op.src_origin = WMTOrigin{plan.src_x, plan.src_y, 0};
    resolve_op.dst_origin = WMTOrigin{0, 0, 0};
    resolve_op.size = WMTSize{plan.src_w, plan.src_h, 1};
    resolve_op.dst_size = WMTSize{plan.src_w, plan.src_h, 1};
    QueueBlitOp(std::move(resolve_op));

    PendingBlitOp stretch_op;
    stretch_op.kind = PendingBlitOp::Kind::Stretch;
    stretch_op.src_tex = std::move(transient);
    stretch_op.dst_tex = std::move(dst_tex);
    stretch_op.src_mip = 0;
    stretch_op.dst_mip = dst->mipLevel();
    stretch_op.src_slice = 0;
    stretch_op.dst_slice = dst->arraySlice();
    stretch_op.src_origin = WMTOrigin{0, 0, 0};
    stretch_op.dst_origin = WMTOrigin{plan.dst_x, plan.dst_y, 0};
    stretch_op.size = WMTSize{plan.src_w, plan.src_h, 1};
    stretch_op.dst_size = WMTSize{plan.dst_w, plan.dst_h, 1};
    stretch_op.filter = Filter;
    stretch_op.src_swizzle = src_swizzle;
    QueueBlitOp(std::move(stretch_op));
    break;
  }
  }
  // wined3d device.c; flag the dest texture's auto-gen mipmap
  // state after a successful StretchRect. See
  // MTLD3D9Surface::flagContainerAutoGenDirty for the QI-and-downcast
  // shape (no-op for standalone surfaces / swapchain backbuffers).
  dst->flagContainerAutoGenDirty();
  return D3D_OK;
}
// Encode one solid-colour block for a DXT1..5 (BC1/2/3) format from a D3DCOLOR,
// returning the block size in bytes (8 for DXT1, 16 for DXT2..5). Used by the
// compressed ColorFill path. The decoded value is not conformance-pinned (wine
// visual.c: "no driver produces a proper DXT compression block"), so a single
// solid 565 endpoint with zero colour indices is faithful: BC colour blocks
// map index 0 to colour0, so every texel reads back the packed fill colour. The
// DXT3/5 alpha block is filled from the D3DCOLOR alpha byte.
static uint32_t
encode_bc_solid_block(D3DFORMAT format, D3DCOLOR color, uint8_t out[16]) {
  const uint8_t a8 = (color >> 24) & 0xFF;
  const uint8_t r8 = (color >> 16) & 0xFF;
  const uint8_t g8 = (color >> 8) & 0xFF;
  const uint8_t b8 = color & 0xFF;
  const uint16_t c565 = static_cast<uint16_t>(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
  // Colour block: both little-endian 565 endpoints = the fill colour, all 2-bit
  // indices 0 (every texel selects colour0).
  auto write_color_block = [&](uint8_t *p) {
    p[0] = c565 & 0xFF;
    p[1] = (c565 >> 8) & 0xFF;
    p[2] = c565 & 0xFF;
    p[3] = (c565 >> 8) & 0xFF;
    p[4] = p[5] = p[6] = p[7] = 0;
  };
  switch (format) {
  case D3DFMT_DXT1:
    write_color_block(out);
    return 8;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
    // Explicit 4-bit alpha: replicate the top nibble into both texels of each
    // byte ((nibble << 4) | nibble); eight bytes cover the 4x4 block.
    std::memset(out, static_cast<int>((a8 & 0xF0) | (a8 >> 4)), 8);
    write_color_block(out + 8);
    return 16;
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    // Interpolated alpha: alpha0 = alpha1 = a8, all 3-bit indices 0 (= alpha0).
    out[0] = a8;
    out[1] = a8;
    out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = 0;
    write_color_block(out + 8);
    return 16;
  default:
    // Unreachable: the caller gates on IsCompressedFormat && !Is3DcFormat.
    std::memset(out, 0, 16);
    return 16;
  }
}
// ColorFill: solid color via render-pass clear (empty pass, loadAction=Clear)
// for RT-capable surfaces; block-encoded staged upload for compressed offscreen
// surfaces (below). Full-surface takes the cheap loadAction=Clear coalesce, a
// sub-rect takes the scissored render-pass quad.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR Color) {
  D9DeviceLock lock = LockDevice();
  // Wine main thread has no outer NSAutoreleasePool. Clear-encoder
  // chunk emit touches Metal APIs (view, fence) that return
  // autoreleased handles.
  auto pool = WMT::MakeAutoreleasePool();
  if (!pSurface)
    return D3DERR_INVALIDCALL;
  auto *dst = static_cast<MTLD3D9Surface *>(pSurface);
  if (dst->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  const D3DSURFACE_DESC &dd = dst->desc();
  if (dd.Pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  // DXVK gates on `aspectMask != COLOR_BIT`, which is broader than just
  // DS rejection. dxmt's DEFAULT-pool surface-creation paths
  // (CreateRenderTarget, CreateOffscreenPlainSurface, CreateTexture)
  // all reject formats that don't lower via D3DFormatToMetal, so the
  // only non-color DEFAULT surfaces we can construct are DS; making
  // the IsDepthStencilFormat check sufficient at this site.
  if (IsDepthStencilFormat(dd.Format))
    return D3DERR_INVALIDCALL;
  // ColorFill targets render targets and standalone offscreen-plain
  // surfaces; a plain or DYNAMIC texture sub-resource (not a render target)
  // is rejected (wined3d device.c; the test's resource_types matrix). A
  // compressed texture level can never carry the RENDERTARGET bit, so this
  // also keeps the block-fill path below scoped to standalone compressed
  // offscreen surfaces.
  if (dst->isTextureSubresource() && !(dd.Usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_INVALIDCALL;
  // Resolve the fill rect. NULL means full surface; otherwise validate
  // against the surface bounds (wined3d/DXVK both INVALIDCALL on an
  // out-of-bounds or inverted rect). The full-surface shortcut bypasses
  // the render-pass quad path entirely; it stays on the cheap
  // loadAction=Clear coalesce.
  uint32_t fill_x = 0, fill_y = 0;
  uint32_t fill_w = dd.Width, fill_h = dd.Height;
  bool full_surface = true;
  if (pRect) {
    if (pRect->left < 0 || pRect->top < 0 || pRect->right <= pRect->left || pRect->bottom <= pRect->top ||
        (uint32_t)pRect->right > dd.Width || (uint32_t)pRect->bottom > dd.Height)
      return D3DERR_INVALIDCALL;
    fill_x = pRect->left;
    fill_y = pRect->top;
    fill_w = pRect->right - pRect->left;
    fill_h = pRect->bottom - pRect->top;
    full_surface = (fill_x == 0 && fill_y == 0 && fill_w == dd.Width && fill_h == dd.Height);
  }

  // Block-compressed offscreen surface (BC1/2/3): Apple Silicon can't render to
  // BC, so ctx.clearColor's RT render pass is unavailable. Native + wined3d
  // still accept a block-aligned ColorFill (visual.c color_fill_test: full fill
  // and the {4,4,8,8} block-aligned rect succeed, {5,5,7,7} unaligned returns
  // INVALIDCALL; the block VALUE is driver-garbage and unchecked). Encode one
  // solid-colour block and stage it across the rect's block grid through the
  // same upload-ring + BufferToTexture path a DXT LockRect takes. DXVK clears
  // BC through an R32G32_UINT storage view, which Metal forbids on a compressed
  // texture; the CPU block encode is the Metal-shaped equivalent. 3Dc (ATI1/
  // ATI2) keeps its INVALIDCALL: its app-facing layout is a linear fiction over
  // BC4/BC5 and no title ColorFills a normal-map surface.
  if (IsCompressedFormat(dd.Format)) {
    if (Is3DcFormat(dd.Format))
      return D3DERR_INVALIDCALL;
    // wined3d resource.c block-mask rule: origin block-aligned, right/bottom
    // block-aligned or touching the surface edge, else INVALIDCALL.
    if (!dxmt::d3d9_rect_block_aligned(
            dd.Format, fill_x, fill_y, fill_x + fill_w, fill_y + fill_h, dd.Width, dd.Height
        ))
      return D3DERR_INVALIDCALL;
    WMT::Texture dst_metal = dst->metalTexture();
    Rc<dxmt::Texture> comp_tex = dst->dxmtTexture();
    if (dst_metal == nullptr || comp_tex == nullptr)
      return D3DERR_INVALIDCALL;
    uint8_t solid_block[16];
    const uint32_t block_bytes = encode_bc_solid_block(dd.Format, Color, solid_block);
    const uint32_t bw = D3DFormatBlockWidth(dd.Format);
    const uint32_t bh = D3DFormatBlockHeight(dd.Format);
    const uint32_t blocks_x = (fill_w + bw - 1u) / bw;
    const uint32_t blocks_y = (fill_h + bh - 1u) / bh;
    const uint32_t row_bytes = blocks_x * block_bytes;
    std::vector<uint8_t> block_grid(static_cast<size_t>(row_bytes) * blocks_y);
    for (uint32_t by = 0; by < blocks_y; ++by)
      for (uint32_t bx = 0; bx < blocks_x; ++bx)
        std::memcpy(
            block_grid.data() + (static_cast<size_t>(by) * row_bytes + static_cast<size_t>(bx) * block_bytes),
            solid_block, block_bytes
        );
    // Rides the arrival-order op stream (no signal, chunk completion recycles
    // the ring block), the same as a texture upload; no FlushDrawBatch drain.
    stageTextureUpload(
        dst_metal, comp_tex, static_cast<uint32_t>(dst->mipLevel()), static_cast<uint32_t>(dst->arraySlice()),
        WMTOrigin{fill_x, fill_y, 0}, WMTSize{fill_w, fill_h, 1}, block_grid.data(), row_bytes, /*is_compressed=*/true
    );
    dst->flagContainerAutoGenDirty();
    return D3D_OK;
  }

  const double r = ((Color >> 16) & 0xFF) / 255.0;
  const double g = ((Color >> 8) & 0xFF) / 255.0;
  const double b = (Color & 0xFF) / 255.0;
  const double a = ((Color >> 24) & 0xFF) / 255.0;

  // ColorFill posts a chunk lambda that routes through ctx.clearColor.
  // d3d11's ClearRenderTargetView shape. The chunk's ClearEncoderData
  // fast-path coalesces with an immediately-following render pass against
  // the same attachment, matching d3d11's load-action folding. Drain
  // queued draws first so they land on their own attachments before this
  // clear retargets.
  if (!m_pendingOps.empty())
    FlushDrawBatch();
  // A latched Clear on this surface must drain in API order, before the fill.
  // FlushDrawBatch already drains it when there are queued draws (the clear
  // folds into their pass); with no intervening draw the op queue is empty, so
  // the latch would otherwise survive and overwrite the fill at the next flush.
  // flushOpenWork no-ops when nothing is latched.
  flushOpenWork();

  // ColorFill requires a dxmt::Texture wrapper to take the ctx.access
  // path. Every DEFAULT-pool surface (CreateRenderTarget,
  // CreateOffscreenPlainSurface, CreateTexture with the RT-promotion
  // ctor) carries one; guard defensively against legacy callsites.
  Rc<dxmt::Texture> dst_tex = dst->dxmtTexture();
  if (!dst_tex)
    return D3DERR_INVALIDCALL;

  // Per-level + per-slice view of the surface, since ctx.clearColor's
  // ClearEncoderData carries a TextureViewRef. The Rc<>'s fullView is
  // the level-0/slice-0 view; for cube faces and mip surfaces the
  // MTLD3D9Surface's mipLevel() + arraySlice() select the right one.
  uint16_t dst_level = static_cast<uint16_t>(dst->mipLevel());
  uint16_t dst_slice = static_cast<uint16_t>(dst->arraySlice());
  TextureViewKey view_key = dst_tex->createView({
      .format = dst->metalPixelFormat(),
      .type = surface_view_type(dst_tex.ptr()),
      .firstMiplevel = dst_level,
      .miplevelCount = 1,
      .firstArraySlice = dst_slice,
      .arraySize = 1,
  });
  unsigned array_length = 1;

  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;

  auto *chunk = m_dxmtQueue->CurrentChunk();
  if (full_surface) {
    // Whole-attachment loadAction=Clear; coalesces with the next
    // render pass against the same RT into a single encoder.
    chunk->emitcc([dst_tex = std::move(dst_tex), view_key, array_length, r, g, b, a, event_handle,
                   signal_seq](ArgumentEncodingContext &ctx) mutable {
      ctx.clearColor(std::move(dst_tex), view_key, array_length, WMTClearColor{r, g, b, a});
      ctx.signalEventByHandle(event_handle, signal_seq);
    });
  } else {
    // Sub-rect path: load-then-scissored-clear via the render-pass
    // quad in ClearRenderTargetContext. Loses the loadAction=Clear
    // coalesce since the pass has loadAction=Load, but preserves the
    // out-of-rect pixels: which is the whole point of a sub-rect
    // ColorFill. DXVK's clearImageView with an extent maps to this
    // same render-pass-with-scissor pattern.
    std::array<float, 4> color_f = {
        static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), static_cast<float>(a)
    };
    chunk->emitcc([dst_tex = std::move(dst_tex), view_key, fill_x, fill_y, fill_w, fill_h, color_f, event_handle,
                   signal_seq](ArgumentEncodingContext &ctx) mutable {
      ctx.clear_rt_cmd.begin(std::move(dst_tex), view_key);
      ctx.clear_rt_cmd.clear(fill_x, fill_y, fill_w, fill_h, color_f);
      ctx.clear_rt_cmd.end();
      ctx.signalEventByHandle(event_handle, signal_seq);
    });
  }
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();
  // AUTOGENMIPMAP regen: DXVK d3d9_device.cpp calls
  // MarkTextureMipsDirty when IsAutomaticMip after a ColorFill. Routes
  // through the shared surface helper (see flagContainerAutoGenDirty)
  // so the standalone-surface / swapchain-backbuffer cases stay no-op.
  dst->flagContainerAutoGenDirty();
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateOffscreenPlainSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) {
  D9DeviceLock lock = LockDevice();
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;
  if (Width == 0 || Height == 0)
    return D3DERR_INVALIDCALL;
  if (Width > 16384 || Height > 16384)
    return D3DERR_INVALIDCALL;
  if (!isBlockAlignedCreate(Format, Width, Height))
    return D3DERR_INVALIDCALL;

  // wined3d device.c; D3DPOOL_MANAGED on offscreen plain is
  // contract-illegal (managed pool implies a GPU mirror, but offscreen
  // plain has no defined GPU-bind path that would feed the mirror).
  if (Pool == D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;

  // 3Dc (ATI1/ATI2) is a vendor FOURCC the runtime accepts only as a sampled
  // texture format. Native D3D9 rejects it as an offscreen-plain surface in
  // every pool but SCRATCH, where a raw CPU blob backs any format with a byte
  // layout (tests/device.c test_resource_access asserts INVALIDCALL for ATI2 in
  // DEFAULT / SYSTEMMEM; wined3d and DXVK are lenient here and carry a todo_wine).
  if (Is3DcFormat(Format) && Pool != D3DPOOL_SCRATCH)
    return D3DERR_INVALIDCALL;

  // pSharedHandle on SYSTEMMEM is the user-memory overload: the app
  // pointer is the packed CPU storage, aliased in place the same way the
  // texture path does it (offscreen plain surfaces are single level, so
  // there is no level gate). DEFAULT pool is the cross-process shared
  // handle (unimplemented); any other pool with a handle is illegal.
  void *user_memory = nullptr;
  if (pSharedHandle) {
    // Non-extended devices reject any handle with E_NOTIMPL up front, the
    // same gate the texture path uses; the user-memory overload is
    // D3D9Ex-only (wined3d device.c).
    if (!m_isEx)
      return E_NOTIMPL;
    if (Pool == D3DPOOL_SYSTEMMEM) {
      user_memory = *reinterpret_cast<void **>(pSharedHandle);
      pSharedHandle = nullptr;
    } else if (Pool != D3DPOOL_DEFAULT) {
      return D3DERR_INVALIDCALL;
    } else {
      // TODO: cross-process resource share.
      return E_NOTIMPL;
    }
  }

  // A depth-stencil offscreen-plain surface has no GPU attachment role here
  // (CreateDepthStencilSurface owns that), but D3D9 still creates a lockable
  // system-memory surface for it: CheckDeviceFormat reports depth formats
  // available for D3DRTYPE_SURFACE (they carry the blit cap), so the runtime
  // backs the create with a host mirror alone, the same texture-less shape as
  // a packed-YUV SCRATCH surface. MANAGED was already rejected above, matching
  // the wined3d offscreen-plain pool rules.
  // Deliberate divergence: dxmt accepts EVERY depth format here, matching
  // wined3d (its blit-cap surface path, directx.c). DXVK restricts plain
  // surfaces to the LOCKABLE depth formats only (adapter.cpp, a native-derived
  // rule). dxmt sits on the wined3d-primary side; a wine-test oracle covering
  // CreateOffscreenPlainSurface(D24S8) is needed before moving to DXVK's rule
  // (one predicate swap here + in the SURFACE probe arm).
  if (IsDepthStencilFormat(Format)) {
    // A zero lock pitch means the format has no packed byte layout to mirror
    // (no D3DFormatBytesPerPixel entry); it cannot back a lockable surface, so
    // reject it here. This is the filter that keeps the create to the depth
    // formats with a defined CPU layout.
    const uint32_t depth_pitch = D3DFormatLockPitch(Format, Width);
    if (depth_pitch == 0)
      return D3DERR_INVALIDCALL;
    const uint64_t mirror_bytes = static_cast<uint64_t>(depth_pitch) * D3DFormatRowCount(Format, Height);
    void *backing = user_memory;
    if (!backing) {
      backing = wsi::aligned_malloc(mirror_bytes, DXMT_PAGE_SIZE);
      if (!backing)
        return D3DERR_OUTOFVIDEOMEMORY;
      std::memset(backing, 0, mirror_bytes);
    }
    D3DSURFACE_DESC desc{};
    desc.Format = Format;
    desc.Type = D3DRTYPE_SURFACE;
    desc.Usage = 0;
    desc.Pool = Pool;
    desc.MultiSampleType = D3DMULTISAMPLE_NONE;
    desc.MultiSampleQuality = 0;
    desc.Width = Width;
    desc.Height = Height;
    auto *surface = new MTLD3D9Surface(
        this, desc, static_cast<IDirect3DDevice9 *>(this), WMT::Reference<WMT::Texture>{},
        /*mipLevel=*/0, /*selfPin=*/true, WMTTextureType2D, WMT::Reference<WMT::Buffer>{},
        /*cpuPtr=*/backing, depth_pitch, /*arraySlice=*/0,
        /*ownedBacking=*/user_memory ? nullptr : backing, /*dxmtTexture=*/{}
    );
    // DEFAULT-pool offscreen surfaces are losable; the SYSTEMMEM / SCRATCH
    // copies live in CPU pools and never go through Reset's gate. Mirrors the
    // normal offscreen-plain branch below.
    if (Pool == D3DPOOL_DEFAULT)
      surface->markLosable();
    surface->AddRef();
    *ppSurface = surface;
    return D3D_OK;
  }

  // Packed-YUV SCRATCH surfaces: Metal has no 422 format, so there is no
  // placeholder texture to allocate. D3D9 still permits a system-memory copy,
  // backed by a host mirror alone (texture-less), the same shape as a DXTn
  // SCRATCH volume. CheckDeviceFormat reports the format NOTAVAILABLE, so any
  // non-SCRATCH pool stays INVALIDCALL.
  if (IsScratchableUnsupportedFormat(Format)) {
    if (Pool != D3DPOOL_SCRATCH)
      return D3DERR_INVALIDCALL;
    const uint32_t yuv_pitch = D3DFormatLockPitch(Format, Width);
    if (yuv_pitch == 0)
      return D3DERR_INVALIDCALL;
    const uint64_t mirror_bytes = static_cast<uint64_t>(yuv_pitch) * D3DFormatRowCount(Format, Height);
    void *backing = wsi::aligned_malloc(mirror_bytes, DXMT_PAGE_SIZE);
    if (!backing)
      return D3DERR_OUTOFVIDEOMEMORY;
    std::memset(backing, 0, mirror_bytes);
    D3DSURFACE_DESC desc{};
    desc.Format = Format;
    desc.Type = D3DRTYPE_SURFACE;
    desc.Usage = 0;
    desc.Pool = Pool;
    desc.MultiSampleType = D3DMULTISAMPLE_NONE;
    desc.MultiSampleQuality = 0;
    desc.Width = Width;
    desc.Height = Height;
    auto *surface = new MTLD3D9Surface(
        this, desc, static_cast<IDirect3DDevice9 *>(this), WMT::Reference<WMT::Texture>{},
        /*mipLevel=*/0, /*selfPin=*/true, WMTTextureType2D, WMT::Reference<WMT::Buffer>{},
        /*cpuPtr=*/backing, yuv_pitch, /*arraySlice=*/0, /*ownedBacking=*/backing, /*dxmtTexture=*/{}
    );
    surface->AddRef();
    *ppSurface = surface;
    return D3D_OK;
  }

  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture);
  if (pixelFormat == WMTPixelFormatInvalid)
    return D3DERR_INVALIDCALL;

  // Pool -> Metal storage. D3DPOOL_DEFAULT lives GPU-side and is the
  // legal StretchRect destination; SYSTEMMEM/SCRATCH live CPU-side and
  // are the legal UpdateSurface source. Apple Silicon's unified memory
  // makes Shared a zero-copy fit for the CPU pools.
  WMTResourceOptions storage;
  WMTTextureUsage usage;
  switch (Pool) {
  case D3DPOOL_DEFAULT:
    storage = WMTResourceStorageModePrivate;
    // ShaderRead so the surface can be a blit source / sampled texture
    // standin for StretchRect; RenderTarget so it can be a StretchRect
    // destination via render pass when the format gates blit out.
    // BC-compressed formats can't carry the RenderTarget bit on Apple
    // Silicon: same gate as CreateTexture. A DEFAULT-pool DXT
    // offscreen surface stays sampler-only; StretchRect to it goes
    // through the blit-encoder path, not a render pass.
    usage = (IsCompressedFormat(Format) || Is3DcFormat(Format))
                ? WMTTextureUsageShaderRead
                : (WMTTextureUsage)(WMTTextureUsageShaderRead | WMTTextureUsageRenderTarget);
    break;
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_SCRATCH:
    storage = WMTResourceStorageModeShared;
    usage = WMTTextureUsageShaderRead;
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = usage;
  info.options = storage;

  WMT::Reference<WMT::Texture> texture;
  WMT::Reference<WMT::Buffer> buffer;
  void *cpuPtr = nullptr;
  void *ownedBacking = nullptr;
  uint32_t pitch = 0;
  // DEFAULT offscreen: dxmt::Texture keeps MTLTexture alive (EncodingThread). fullView carries intendedUsage for
  // RT-substitution. SYSTEMMEM/SCRATCH: buffer-backed (can't add RenderTarget on Apple Silicon; lacks 32-bit
  // addressing).
  Rc<dxmt::Texture> dxmt_texture;

  if (Pool == D3DPOOL_DEFAULT) {
    dxmt_texture = new dxmt::Texture(info, m_metalDevice);
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
    auto allocation = dxmt_texture->allocate(alloc_flags);
    if (!allocation || !allocation->texture())
      return D3DERR_OUTOFVIDEOMEMORY;
    texture = WMT::Reference<WMT::Texture>(allocation->texture());
    dxmt_texture->rename(std::move(allocation));
    // Clear the GPU-private texture: a DEFAULT offscreen surface is a legal
    // StretchRect source and can be SetTexture-bound, so a copy / sample before
    // the app's first write must not read recycled memory. The zeroed host
    // mirror below is separate lock storage, not the sampled texture. The
    // SYSTEMMEM / SCRATCH / user-memory branches skip this: their Private
    // texture is a never-sampled placeholder and every consumer reads cpuPtr().
    initTextureWithZero(dxmt_texture.ptr());
    // DEFAULT surfaces always lockable (MSDN). Private texture can't carry CPU pointer; buffer-backed loses
    // RenderTarget (Apple Silicon disallows RT on linear textures). Solution: host-side mirror (MANAGED pattern).
    pitch = D3DFormatLockPitch(Format, Width);
    if (pitch == 0)
      return D3DERR_INVALIDCALL;
    const uint64_t mirror_bytes = static_cast<uint64_t>(pitch) * D3DFormatRowCount(Format, Height);
    ownedBacking = wsi::aligned_malloc(mirror_bytes, DXMT_PAGE_SIZE);
    if (!ownedBacking)
      return D3DERR_OUTOFVIDEOMEMORY;
    std::memset(ownedBacking, 0, mirror_bytes);
    cpuPtr = ownedBacking;
    // buffer stays null -> UnlockRect takes the mirror-upload path, not the
    // zero-copy buffer-backed path SYSTEMMEM/SCRATCH use.
  } else if (user_memory) {
    // D3D9Ex user-memory: the app pointer is the lock storage, tightly
    // packed. The Metal texture is a plain CPU-visible allocation; the
    // surface stays a valid UpdateSurface source because that path stages
    // from cpuPtr(). No buffer wrap (the app pointer is neither page-
    // aligned nor dxmt-owned) and no owned backing (the app frees it).
    pitch = D3DFormatRowPitch(Format, Width);
    if (pitch == 0)
      return D3DERR_INVALIDCALL;
    dxmt_texture = new dxmt::Texture(info, m_metalDevice);
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    auto allocation = dxmt_texture->allocate(alloc_flags);
    if (!allocation || !allocation->texture())
      return D3DERR_OUTOFVIDEOMEMORY;
    texture = WMT::Reference<WMT::Texture>(allocation->texture());
    dxmt_texture->rename(std::move(allocation));
    cpuPtr = user_memory;
  } else {
    // SYSTEMMEM / SCRATCH (the CPU pools, any format): a host mirror is the
    // lock storage and the UpdateSurface staging source; the Metal texture is
    // a Private CpuInvisible placeholder that is never sampled (these pools
    // are never a GPU bind or StretchRect endpoint, and every consumer reads
    // cpuPtr()). Backing the surface with an MTLBuffer-aliased linear texture
    // instead would force the reported pitch up to Metal's per-format row
    // alignment, which is a device artifact: LockRect must report the D3D9
    // pitch (D3DFormatLockPitch). Pitch and backing height count in 4x4 blocks
    // for the compressed formats.
    info.options = WMTResourceStorageModePrivate;
    dxmt_texture = new dxmt::Texture(info, m_metalDevice);
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
    auto allocation = dxmt_texture->allocate(alloc_flags);
    if (!allocation || !allocation->texture())
      return D3DERR_OUTOFVIDEOMEMORY;
    texture = WMT::Reference<WMT::Texture>(allocation->texture());
    dxmt_texture->rename(std::move(allocation));
    pitch = D3DFormatLockPitch(Format, Width);
    if (pitch == 0)
      return D3DERR_INVALIDCALL;
    const uint64_t mirror_bytes = static_cast<uint64_t>(pitch) * D3DFormatRowCount(Format, Height);
    // 32-bit WoW64: pre-allocate backing in process address space so LockRect
    // pBits is 32-bit-addressable, and pre-fault it (see CreateVertexBuffer's
    // matching comment for the Rosetta x86_32 first-touch cliff rationale).
    ownedBacking = wsi::aligned_malloc(mirror_bytes, DXMT_PAGE_SIZE);
    if (!ownedBacking)
      return D3DERR_OUTOFVIDEOMEMORY;
    std::memset(ownedBacking, 0, mirror_bytes);
    cpuPtr = ownedBacking;
  }

  D3DSURFACE_DESC desc{};
  desc.Format = Format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = 0;
  desc.Pool = Pool;
  desc.MultiSampleType = D3DMULTISAMPLE_NONE;
  desc.MultiSampleQuality = 0;
  desc.Width = Width;
  desc.Height = Height;

  auto *surface = new MTLD3D9Surface(
      this, desc,
      /*container=*/static_cast<IDirect3DDevice9 *>(this), std::move(texture),
      /*mipLevel=*/0,
      /*selfPin=*/true,
      /*parentTextureType=*/WMTTextureType2D, std::move(buffer), cpuPtr, pitch,
      /*arraySlice=*/0, ownedBacking,
      /*dxmtTexture=*/std::move(dxmt_texture)
  );
  // CreateOffscreenPlainSurface in DEFAULT pool is losable; SYSTEMMEM /
  // SCRATCH copies live in CPU pools and never go through Reset's gate.
  if (Pool == D3DPOOL_DEFAULT)
    surface->markLosable();
  surface->AddRef();
  *ppSurface = surface;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
  D9DeviceLock lock = LockDevice();
  if (RenderTargetIndex >= D3D_MAX_SIMULTANEOUS_RENDERTARGETS)
    return D3DERR_INVALIDCALL;
  // wined3d device.c: slot 0 cannot be unbound. Without a
  // primary RT the rest of the pipeline has nothing to write into,
  // so the runtime hard-rejects the case rather than letting a draw
  // produce no output.
  if (RenderTargetIndex == 0 && pRenderTarget == nullptr)
    return D3DERR_INVALIDCALL;

  auto *surface = static_cast<MTLD3D9Surface *>(pRenderTarget);
  // wined3d device.c: the surface must belong to *this* device.
  // Cross-device binding would break the Metal allocator that owns
  // the texture handle and is meaningless across separate D3D9
  // devices anyway. deviceRaw() avoids the AddRef/Release that the
  // public GetDevice path would require; this is a hot path.
  if (surface && surface->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  // DXVK SetRenderTargetInternal (d3d9_device.cpp) rejects a
  // surface that wasn't created with D3DUSAGE_RENDERTARGET. Apps and
  // tools that probe with an offscreen-plain / SYSTEMMEM surface
  // (anti-cheat fingerprinting, capability tests) expect INVALIDCALL,
  // not an opaque Metal encode-time validation error later. The
  // implicit-DS auto-target path passes through, and the swapchain
  // back-buffer surfaces are created with the usage bit set, so this
  // gate has no effect on dxmt's own creates.
  if (surface && !(surface->desc().Usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_INVALIDCALL;
  // The usage bit alone is not sufficient: a texture created with
  // stray RENDERTARGET usage on a sampler-only format (CreateTexture
  // accepts those, matching wined3d) carries the bit but has no
  // attachable Metal format. Fail the bind here, not in the encoder.
  // NULL-FOURCC targets stay bindable; the render pass drops the slot.
  if (surface && !IsNullFormat(surface->desc().Format) &&
      D3DFormatToMetal(surface->desc().Format, D3D9FormatUsage::RenderTarget) == WMTPixelFormatInvalid)
    return D3DERR_INVALIDCALL;

  // No-op rebind on a non-zero slot. Slot 0 falls through because D3D9
  // spec resets viewport+scissor on every SetRenderTarget(0, ...) call,
  // even with the same surface (DXVK d3d9_device.cpp, wined3d
  // device.c). Slot >0 has no such semantic: pure refcount churn
  // if the surface didn't change.
  if (RenderTargetIndex != 0 && m_renderTargets[RenderTargetIndex].ptr() == surface)
    return D3D_OK;

  // If the bound surface for this slot is actually changing, drain any
  // pending clear onto the OLD RT before m_renderTargets mutates.
  // without it a Clear -> SetRT (no draws between) would land the clear
  // on the new RT instead of the old, and the old RT would carry
  // forward stale content into subsequent frames. drainPendingClear
  // captures the current RT0/DS resources in its emitcc closure, so it
  // remains RT-correct without a FlushDrawBatch here.
  // Queued draws resolve before the SetRef op below is applied, so
  // they still observe the pre-Set binding.
  bool surface_changed = m_renderTargets[RenderTargetIndex].ptr() != surface;
  if (surface_changed) {
    drainPendingClear();
  }

  // Com<,false> assignment drops the previously-bound surface's priv
  // ref and AddRefPrivate's the new one. surface=nullptr is the
  // unbind path (idx>0).
  m_renderTargets[RenderTargetIndex] = surface;
  // Flag an AUTOGENMIPMAP render target for mip regen on bind, matching
  // DXVK (SetRenderTargetInternal: IsAutomaticMip -> SetNeedsMipGen).
  // dxmt already flags on Clear/StretchRect/ColorFill but not the
  // draw-fill path, so a target filled only by draws would never
  // regenerate; the mipsDirty sweep regenerates before the next sampler
  // bind. The usage pre-check keeps the QI off the common RT-bind path.
  if (surface && (surface->desc().Usage & D3DUSAGE_AUTOGENMIPMAP))
    surface->flagContainerAutoGenDirty();
  // D3D9 spec: a successful SetRenderTarget on slot 0 resets viewport
  // and scissor to cover the new RT (DXVK d3d9_device.cpp,
  // wined3d device.c). Apps that swap RTs without re-issuing
  // SetViewport rely on this.
  D3DVIEWPORT9 new_viewport;
  RECT new_scissor;
  bool need_viewport_op = false;
  bool need_scissor_op = false;
  if (RenderTargetIndex == 0 && surface) {
    const D3DSURFACE_DESC &d = surface->desc();
    m_viewport.X = 0;
    m_viewport.Y = 0;
    m_viewport.Width = d.Width;
    m_viewport.Height = d.Height;
    m_viewport.MinZ = 0.0f;
    m_viewport.MaxZ = 1.0f;
    m_scissorRect.left = 0;
    m_scissorRect.top = 0;
    m_scissorRect.right = static_cast<LONG>(d.Width);
    m_scissorRect.bottom = static_cast<LONG>(d.Height);
    new_viewport = m_viewport;
    new_scissor = m_scissorRect;
    need_viewport_op = true;
    need_scissor_op = true;
  }
  // The encode-thread walker applies the SetRef op below in arrival
  // order and bumps m_encodeSideRefsGen, so the next BatchedDraw picks
  // up the new RT slot while earlier queued draws resolve against the
  // pre-Set binding.
  // Op-stream mirror: push a SetRef only when the surface actually
  // changed. SetRenderTarget(0, same_surface) is a documented re-bind
  // that resets viewport/scissor (POD axes, handled separately below)
  // without touching the ref-counted slot; pushing an op there would
  // be a no-op AddRef/Release pair on the encode side.
  if (surface_changed) {
    if (surface)
      surface->AddRefPrivate();
    QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::RenderTarget0 + RenderTargetIndex), surface);
  }
  // viewport/scissor live in the per-draw pod_snapshot now; the
  // SetRenderTarget reset above already wrote to the calling-thread
  // shadows (m_viewport / m_scissorRect), so we just flag the axes
  // dirty so the next QueueBatchedDraw rebuilds them.
  if (need_viewport_op)
    m_encShadowDirty |= dxmt::D9ES_DIRTY_VIEWPORT;
  if (need_scissor_op)
    m_encShadowDirty |= dxmt::D9ES_DIRTY_SCISSOR_RECT;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) {
  D9DeviceLock lock = LockDevice();
  // Mirror wined3d_device_GetRenderTarget shape (and the same shape
  // MTLD3D9Device::GetSwapChain / MTLD3D9SwapChain::GetBackBuffer use):
  // do NOT touch the out-pointer on the INVALIDCALL path. Apps planting
  // a sentinel they expect to survive an out-of-range probe see it
  // preserved on OOR index.
  if (!ppRenderTarget)
    return D3DERR_INVALIDCALL;
  if (RenderTargetIndex >= D3D_MAX_SIMULTANEOUS_RENDERTARGETS)
    return D3DERR_INVALIDCALL;
  // wined3d device.c d3d9_device_GetRenderTarget: returning NOTFOUND
  // when the slot is unbound matches the D3D9 contract. wined3d
  // explicitly sets the out-ptr to null on the NOTFOUND path; replicate.
  MTLD3D9Surface *bound = m_renderTargets[RenderTargetIndex].ptr();
  if (!bound) {
    *ppRenderTarget = nullptr;
    return D3DERR_NOTFOUND;
  }
  *ppRenderTarget = ::dxmt::ref<IDirect3DSurface9>(bound);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
  D9DeviceLock lock = LockDevice();
  // Unlike RT slot 0, depth-stencil is allowed to be NULL; depth-
  // disabled rendering is a valid pipeline configuration. wined3d
  // device.c d3d9_device_SetDepthStencilSurface accepts NULL.
  auto *surface = static_cast<MTLD3D9Surface *>(pNewZStencil);
  if (surface && surface->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  // Symmetric to SetRenderTarget's D3DUSAGE_RENDERTARGET gate: the
  // surface must carry D3DUSAGE_DEPTHSTENCIL or it has no business as
  // a depth attachment. wined3d / DXVK both validate this; Metal
  // would otherwise reject the texture at render-pass build time with
  // an opaque encode-time error. CreateDepthStencilSurface stamps
  // the flag; CreateTexture(D3DUSAGE_DEPTHSTENCIL) propagates it to
  // every level surface so shadow-map texture-as-DS still passes.
  if (surface && !(surface->desc().Usage & D3DUSAGE_DEPTHSTENCIL))
    return D3DERR_INVALIDCALL;
  if (m_depthStencilSurface.ptr() == surface)
    return D3D_OK;
  // DS surface is actually changing: drain any staged depth/stencil
  // clear onto the OLD DS before m_depthStencilSurface mutates, else
  // the clear leaks onto whatever DS the next draw binds.
  drainPendingClear();
  m_depthStencilSurface = surface;
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (surface)
    surface->AddRefPrivate();
  QueueRefOp(PendingRefOp::DepthStencilSurface, surface);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) {
  D9DeviceLock lock = LockDevice();
  if (!ppZStencilSurface)
    return D3DERR_INVALIDCALL;
  *ppZStencilSurface = nullptr;
  MTLD3D9Surface *bound = m_depthStencilSurface.ptr();
  if (!bound)
    return D3DERR_NOTFOUND;
  *ppZStencilSurface = ::dxmt::ref<IDirect3DSurface9>(bound);
  return D3D_OK;
}
// BeginScene / EndScene: pair-bracketed scene marker. DXVK
// (d3d9_device.cpp) and wined3d (device.c) both track an
// in_scene flag and reject misnested calls with INVALIDCALL. The
// bracket is also where DXVK fires an implicit-flush hint at EndScene;
// EndScene drains the batch below, matching that hint.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::BeginScene() {
  D9DeviceLock lock = LockDevice();
  if (m_inScene)
    return D3DERR_INVALIDCALL;
  m_inScene = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EndScene() {
  D9DeviceLock lock = LockDevice();
  if (!m_inScene)
    return D3DERR_INVALIDCALL;
  m_inScene = false;
  // Frame boundary. Drain queued batched draws onto a chunk first so
  // Present + downstream sync paths observe the frame's actual draws.
  // flushOpenWork() then catches any residual sync cmdbuf work: blits
  // queued post-FlushDrawBatch via the legacy path; so its commit
  // serialises against the chunk's commit through Metal queue ordering.
  auto pool = WMT::MakeAutoreleasePool();
  FlushDrawBatch();
  flushOpenWork();
  return D3D_OK;
}
// Clear: validation per DXVK d3d9_device.cpp and wined3d
// device.c. The clear region is the viewport, narrowed by the scissor
// rect when SCISSORTESTENABLE, then intersected with each pRect
// (wined3d cs.c wined3d_cs_emit_clear's draw_rect order). A region
// covering every involved attachment whole stays on the *lazy* path:
// the colour / depth / stencil values land in m_pendingClear and the
// next render pass opened by StartRenderPassForBatch_d9 (or
// drainPendingClear on the lone-Clear-then-Present path) folds them
// into its loadAction. Partial regions can't ride a loadAction (Metal
// clears the whole attachment), so they go through emitClippedClear.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
  D9DeviceLock lock = LockDevice();
  // DXVK: Count==0 with a non-null rect array is a documented
  // no-op, not an error.
  if (Count == 0 && pRects != nullptr)
    return D3D_OK;
  // Non-zero Count + NULL rects: both references normalize to full-target clear.
  // Prior INVALIDCALL broke legal calls (dropped frame-clear -> visual garbage).
  if (Count != 0 && pRects == nullptr)
    Count = 0;

  MTLD3D9Surface *ds = m_depthStencilSurface.ptr();
  // No DS bound + Z/Stencil flag -> INVALIDCALL (DXVK).
  if (!ds && (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
    return D3DERR_INVALIDCALL;
  // wined3d device.c wined3d_device_clear: a combined TARGET +
  // ZBUFFER/STENCIL clear against a DS smaller than RT0 is silently
  // dropped whole ("mismatching sizes"), returning OK.
  if (ds && (Flags & D3DCLEAR_TARGET) && (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))) {
    MTLD3D9Surface *rt0 = m_renderTargets[0].ptr();
    if (rt0 && (ds->desc().Width < rt0->desc().Width || ds->desc().Height < rt0->desc().Height))
      return D3D_OK;
  }
  // Drop stencil if the bound DS format has no stencil aspect; D3D9
  // silently masks; Metal would reject the encoder. DXVK does
  // the same via lookupFormatInfo->aspectMask.
  if (ds && (Flags & D3DCLEAR_STENCIL) && !HasStencilAspect(ds->desc().Format))
    Flags &= ~D3DCLEAR_STENCIL;
  // No-flags-set is a no-op.
  if (!(Flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
    return D3D_OK;

  // Drain any queued draws first so they land on the pre-Clear pass.
  // the chunk's startRenderPass sees loadAction=Load for the matching
  // attachments. After this returns m_pendingDraws is empty; the next
  // staged Clear will be picked up by the next batch's startRenderPass
  // (FlushDrawBatch snapshots m_pendingClear into the first BatchedDraw
  // when it next drains).
  FlushDrawBatch();
  // Catch any residual sync cmdbuf work that bypassed FlushDrawBatch
  // (mip-gen, blit uploads). The chunk has already been enqueued, so
  // its Metal commit will land before whatever flushOpenWork commits.
  flushOpenWork();

  // Viewport first, then scissor when enabled; the helper applies
  // exactly that order (wined3d cs.c wined3d_cs_emit_clear).
  const WMTScissorRect clip =
      wmt_scissor_from_d3d9(m_scissorRect, m_viewport, m_renderStates[D3DRS_SCISSORTESTENABLE] != 0);
  // DXVK: a first rect that encompasses the whole clip region degrades
  // to the no-rect case (the remaining rects are redundant).
  if (Count && pRects[0].x1 <= static_cast<int64_t>(clip.x) && pRects[0].y1 <= static_cast<int64_t>(clip.y) &&
      pRects[0].x2 >= static_cast<int64_t>(clip.x + clip.width) &&
      pRects[0].y2 >= static_cast<int64_t>(clip.y + clip.height))
    Count = 0;

  // The staging fast path only fits when the single region covers
  // every involved attachment whole; the loadAction it later becomes
  // has no sub-rect form.
  auto clip_covers = [&clip](uint32_t width, uint32_t height) {
    return clip.x == 0 && clip.y == 0 && clip.width >= width && clip.height >= height;
  };
  bool full_cover = Count == 0;
  if (full_cover && (Flags & D3DCLEAR_TARGET)) {
    for (auto &rt : m_renderTargets) {
      MTLD3D9Surface *surface = rt.ptr();
      if (surface && !IsNullFormat(surface->desc().Format) && surface->dxmtTexture())
        full_cover = full_cover && clip_covers(surface->desc().Width, surface->desc().Height);
    }
  }
  if (full_cover && ds && (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
    full_cover = clip_covers(ds->desc().Width, ds->desc().Height);

  if (full_cover) {
    if (Flags & D3DCLEAR_TARGET) {
      // Decode D3DCOLOR (0xAARRGGBB). DXVK DecodeD3DCOLOR same shape.
      m_pendingClear.color_valid = true;
      m_pendingClear.color[0] = ((Color >> 16) & 0xFF) / 255.0;
      m_pendingClear.color[1] = ((Color >> 8) & 0xFF) / 255.0;
      m_pendingClear.color[2] = (Color & 0xFF) / 255.0;
      m_pendingClear.color[3] = ((Color >> 24) & 0xFF) / 255.0;
    }
    if (Flags & D3DCLEAR_ZBUFFER) {
      m_pendingClear.depth_valid = true;
      m_pendingClear.depth = Z;
    }
    if (Flags & D3DCLEAR_STENCIL) {
      m_pendingClear.stencil_valid = true;
      m_pendingClear.stencil = static_cast<uint8_t>(Stencil);
    }
  } else if (clip.width && clip.height) {
    // Region list in render-target space, shared by every attachment;
    // emitClippedClear clamps per attachment. Degenerate or
    // out-of-clip rects drop here (DXVK skips them the same way).
    std::vector<WMTScissorRect> regions;
    if (Count == 0) {
      regions.push_back(clip);
    } else {
      regions.reserve(Count);
      for (DWORD i = 0; i < Count; i++) {
        const int64_t left = std::max<int64_t>(pRects[i].x1, clip.x);
        const int64_t top = std::max<int64_t>(pRects[i].y1, clip.y);
        const int64_t right = std::min<int64_t>(pRects[i].x2, clip.x + clip.width);
        const int64_t bottom = std::min<int64_t>(pRects[i].y2, clip.y + clip.height);
        if (right <= left || bottom <= top)
          continue;
        regions.push_back({
            static_cast<uint64_t>(left),
            static_cast<uint64_t>(top),
            static_cast<uint64_t>(right - left),
            static_cast<uint64_t>(bottom - top),
        });
      }
    }
    if (!regions.empty())
      emitClippedClear(regions, Flags, Color, Z, Stencil);
  }
  // wined3d device.c calls d3d9_rts_flag_auto_gen_mipmap after a
  // successful Clear: every bound RT whose container is a texture
  // gets its mip chain flagged for regen so subsequent sampler binds
  // see the cleared level-0 propagated through the chain. Iterate
  // unconditionally (flagAutoGenDirty gates on D3DUSAGE_AUTOGENMIPMAP),
  // matching wined3d's d3d9_rts_flag_auto_gen_mipmap shape (device.c).
  for (auto &rt : m_renderTargets) {
    if (MTLD3D9Surface *surface = rt.ptr())
      surface->flagContainerAutoGenDirty();
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  // The index compaction and its table size live in d3d9_matrix.hpp
  // (transform_index / kTransformStateCount) so the host unit tier can pin the
  // mapping; keep the class-local storage count in step with it.
  static_assert(kMaxTransforms == dxmt::kTransformStateCount, "transform table size must match transform_index");
  D9DeviceLock lock = LockDevice();
  // Validate before flipping the StateBlock-recording mask. wined3d
  // device.c sets the per-category dirty bit only after the underlying
  // wined3d_state_X call succeeds; recording a category whose Set
  // failed makes Apply restore stale snapshot values for a state the
  // app never touched.
  if (!pMatrix)
    return D3DERR_INVALIDCALL;
  uint32_t idx = transform_index(State);
  if (idx >= kMaxTransforms)
    return D3DERR_INVALIDCALL;
  // Recording arm: route the value into the block's snapshot and
  // leave live state untouched (wine update_state / DXVK m_recorder
  // shape; same for every recording arm below).
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapTransforms[idx] = *pMatrix;
    m_recordingBlock->m_changes.transforms = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit. D3DX-style engines re-set the same
  // world/view/projection matrix every pass; the memcmp here saves the
  // 64-byte memcpy on a hot setter that may fire per-draw.
  if (std::memcmp(&m_transforms[idx], pMatrix, sizeof(D3DMATRIX)) == 0)
    return D3D_OK;
  m_transforms[idx] = *pMatrix;
  // The world*view*projection product is recomputed lazily at snapshot
  // capture; texture transforms join the product's consumers with the
  // texcoord-transform milestone, so every index dirties the axis.
  m_ffpWVPStale = true;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_FFP;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) {
  D9DeviceLock lock = LockDevice();
  if (!pMatrix)
    return D3DERR_INVALIDCALL;
  uint32_t idx = transform_index(State);
  if (idx >= kMaxTransforms)
    return D3DERR_INVALIDCALL;
  *pMatrix = m_transforms[idx];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  D9DeviceLock lock = LockDevice();
  if (!pMatrix)
    return D3DERR_INVALIDCALL;
  uint32_t idx = transform_index(State);
  if (idx >= kMaxTransforms)
    return D3DERR_INVALIDCALL;
  // Compose pMatrix * current (pMatrix applied innermost: v * pMatrix *
  // current), the wined3d multiply_matrix argument order and MSDN's
  // "pMatrix times State" convention. Reversing the operands scales the
  // existing translation instead of preserving it, so a scene graph built
  // through MultiplyTransform would render misplaced.
  m_transforms[idx] = mat4_multiply(*pMatrix, m_transforms[idx]);
  // Same latch as SetTransform: the precomputed product goes stale and
  // the snapshot axis must recapture.
  m_ffpWVPStale = true;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_FFP;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetViewport(const D3DVIEWPORT9 *pViewport) {
  D9DeviceLock lock = LockDevice();
  if (!pViewport)
    return D3DERR_INVALIDCALL;
  D3DVIEWPORT9 vp = *pViewport;
  // DXVK normalises inverted Z (d3d9_device.cpp); Metal's
  // viewport rejects MaxZ <= MinZ at draw time.
  if (!(vp.MinZ < vp.MaxZ))
    vp.MaxZ = vp.MinZ + 0.001f;
  // Record the normalised viewport so Apply restores exactly what a
  // live SetViewport would have stored.
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapViewport = vp;
    m_recordingBlock->m_changes.viewport = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit: D3D9 effect frameworks re-set the
  // same viewport every pass.
  if (std::memcmp(&m_viewport, &vp, sizeof(D3DVIEWPORT9)) == 0)
    return D3D_OK;
  m_viewport = vp;
  // The "scissor disabled" branch in wmt_scissor_from_d3d9 returns
  // viewport bounds, so a viewport change implicitly invalidates the
  // applied scissor too.
  m_encShadowDirty |= dxmt::D9ES_DIRTY_VIEWPORT | dxmt::D9ES_DIRTY_SCISSOR_RECT;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetViewport(D3DVIEWPORT9 *pViewport) {
  D9DeviceLock lock = LockDevice();
  if (!pViewport)
    return D3DERR_INVALIDCALL;
  *pViewport = m_viewport;
  return D3D_OK;
}
// FFP material / light bookkeeping. wined3d device.c
// d3d9_device_SetMaterial / SetLight / LightEnable. The FFP shader
// generator reads m_material / m_lights / m_lightEnables when it
// lands; until then these are bookkeeping calls; apps still issue
// them with a programmable PS bound, and a STUB_HR (E_NOTIMPL)
// trips apps that don't hr-check.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetMaterial(const D3DMATERIAL9 *pMaterial) {
  D9DeviceLock lock = LockDevice();
  if (!pMaterial)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapMaterial = *pMaterial;
    m_recordingBlock->m_changes.material = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit. Same FFP-bookkeeping-only rationale
  // as SetTransform; no encShadowGen bump today (no Resolve reader),
  // but the memcpy of D3DMATERIAL9 (68 bytes) still costs on a setter
  // that hr-strict apps issue every frame even without an FFP draw.
  if (std::memcmp(&m_material, pMaterial, sizeof(D3DMATERIAL9)) == 0)
    return D3D_OK;
  m_material = *pMaterial;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_FFP;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetMaterial(D3DMATERIAL9 *pMaterial) {
  D9DeviceLock lock = LockDevice();
  if (!pMaterial)
    return D3DERR_INVALIDCALL;
  *pMaterial = m_material;
  return D3D_OK;
}

// SetLight at index Idx. wined3d device.c d3d9_device_SetLight
// grows the underlying light array on demand; new slots default to
// disabled. Negative Type is INVALIDCALL.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetLight(DWORD Index, const D3DLIGHT9 *pLight) {
  D9DeviceLock lock = LockDevice();
  if (!pLight)
    return D3DERR_INVALIDCALL;
  if (pLight->Type < D3DLIGHT_POINT || pLight->Type > D3DLIGHT_DIRECTIONAL)
    return D3DERR_INVALIDCALL;
  // Per wined3d stateblock.c, attenuation < 0 is INVALIDCALL
  // for POINT/SPOT (DIRECTIONAL ignores attenuation entirely). wined3d
  // notes that some titles set junk light data that confuses the GL
  // driver; on Metal the symptom would be NaN-poisoned FFP lighting once
  // the FFP generator lands. Cheap gate, prevents bad state from being
  // captured into StateBlocks.
  if (pLight->Type == D3DLIGHT_POINT || pLight->Type == D3DLIGHT_SPOT) {
    if (pLight->Attenuation0 < 0.0f || pLight->Attenuation1 < 0.0f || pLight->Attenuation2 < 0.0f)
      return D3DERR_INVALIDCALL;
  }
  // Recording: the seed-captured snapshot vectors get the same
  // grow-on-demand treatment the live vectors do below.
  std::vector<D3DLIGHT9> &lights = m_inStateBlockRecord ? m_recordingBlock->m_snapLights : m_lights;
  std::vector<BOOL> &enables = m_inStateBlockRecord ? m_recordingBlock->m_snapLightEnables : m_lightEnables;
  if (Index >= lights.size()) {
    lights.resize(Index + 1, D3DLIGHT9{});
    enables.resize(Index + 1, FALSE);
  }
  lights[Index] = *pLight;
  if (!m_inStateBlockRecord)
    m_encShadowDirty |= dxmt::D9ES_DIRTY_FFP;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_changes.lights = true;
    // Track this light index so Apply restores only the touched lights, not the
    // whole seed-captured set (wined3d records into changed.changed_lights).
    auto &idxs = m_recordingBlock->m_snapLightIndices;
    if (std::find(idxs.begin(), idxs.end(), Index) == idxs.end())
      idxs.push_back(Index);
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetLight(DWORD Index, D3DLIGHT9 *pLight) {
  D9DeviceLock lock = LockDevice();
  if (!pLight)
    return D3DERR_INVALIDCALL;
  // wined3d returns INVALIDCALL when the index was never set
  // (stateblock.c, wined3d_stateblock_get_light path).
  // Sparse-grown vector slots default to
  // zero-init D3DLIGHT9 with Type=0, which sits below the valid
  // D3DLIGHT_POINT..DIRECTIONAL (1..3) range; Type==0 is the
  // "implicitly grown but never Set" sentinel.
  if (Index >= m_lights.size() || m_lights[Index].Type == 0)
    return D3DERR_INVALIDCALL;
  *pLight = m_lights[Index];
  return D3D_OK;
}

// LightEnable on an unset index implicitly creates a default
// directional light there; wined3d device.c mirrors this so
// apps can LightEnable(0, TRUE) without first SetLight'ing.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::LightEnable(DWORD Index, BOOL Enable) {
  D9DeviceLock lock = LockDevice();
  // Recording targets the block's seed-captured vectors instead of
  // live state; the implicit default-light creation applies the same
  // way on either side.
  std::vector<D3DLIGHT9> &lights = m_inStateBlockRecord ? m_recordingBlock->m_snapLights : m_lights;
  std::vector<BOOL> &enables = m_inStateBlockRecord ? m_recordingBlock->m_snapLightEnables : m_lightEnables;
  if (Index >= lights.size()) {
    lights.resize(Index + 1, D3DLIGHT9{});
    enables.resize(Index + 1, FALSE);
  }
  // A slot grown only by a higher-index Set (the Type==0 sentinel) does not yet
  // exist as a light; LightEnable on it defines the default directional light,
  // matching wined3d's get-light-or-define path (device.c). Without this, the
  // enable would land on a Type==0 hole that GetLight / GetLightEnable still
  // report as nonexistent.
  if (lights[Index].Type == 0) {
    D3DLIGHT9 def{};
    def.Type = D3DLIGHT_DIRECTIONAL;
    def.Diffuse = {1.0f, 1.0f, 1.0f, 0.0f};
    def.Direction = {0.0f, 0.0f, 1.0f};
    lights[Index] = def;
  }
  enables[Index] = Enable ? TRUE : FALSE;
  if (!m_inStateBlockRecord)
    m_encShadowDirty |= dxmt::D9ES_DIRTY_FFP;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_changes.lights = true;
    // Track this light index so Apply restores only the touched lights, not the
    // whole seed-captured set (wined3d records into changed.changed_lights).
    auto &idxs = m_recordingBlock->m_snapLightIndices;
    if (std::find(idxs.begin(), idxs.end(), Index) == idxs.end())
      idxs.push_back(Index);
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetLightEnable(DWORD Index, BOOL *pEnable) {
  D9DeviceLock lock = LockDevice();
  if (!pEnable)
    return D3DERR_INVALIDCALL;
  // Same sparse-grown sentinel as GetLight; Type==0 means the slot
  // exists in the underlying vector only because a higher-index Set
  // resized it, not because the app ever touched this index.
  if (Index >= m_lights.size() || m_lights[Index].Type == 0)
    return D3DERR_INVALIDCALL;
  // Native returns 128, never 1, for an enabled light: a documented D3D9
  // quirk that both wined3d (stateblock.c) and DXVK (d3d9_device.cpp) match.
  // Internal storage stays a normalized bool.
  *pEnable = m_lightEnables[Index] ? 128 : 0;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetClipPlane(DWORD Index, const float *pPlane) {
  D9DeviceLock lock = LockDevice();
  if (!pPlane)
    return D3DERR_INVALIDCALL;
  // D3D9 caps higher indices to the last valid slot rather than
  // erroring. cf. DXVK d3d9_device.cpp.
  if (Index >= 8)
    Index = 7;
  if (m_inStateBlockRecord) {
    for (uint32_t i = 0; i < 4; ++i)
      m_recordingBlock->m_snapClipPlanes[Index][i] = pPlane[i];
    m_recordingBlock->m_changes.clip_planes = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit. The clip-plane array is in
  // pod_snapshot so a no-op rewrite would otherwise force a fresh
  // D9EncodingState COW on the next QueueBatchedDraw.
  if (std::memcmp(&m_clipPlanes[Index][0], pPlane, sizeof(float) * 4) == 0)
    return D3D_OK;
  for (uint32_t i = 0; i < 4; ++i)
    m_clipPlanes[Index][i] = pPlane[i];
  m_encShadowDirty |= dxmt::D9ES_DIRTY_CLIP_PLANES;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetClipPlane(DWORD Index, float *pPlane) {
  D9DeviceLock lock = LockDevice();
  if (!pPlane)
    return D3DERR_INVALIDCALL;
  if (Index >= 8)
    Index = 7;
  for (uint32_t i = 0; i < 4; ++i)
    pPlane[i] = m_clipPlanes[Index][i];
  return D3D_OK;
}
// Hot path: pure DWORD store/load (no Value validation; rasterizer clamps).
// State range: 0,7..255 valid; 1..6 D3D8-era no-ops; 256+ out-of-enum.
//
// Vendor-magic render-state values are stored as plain state and their
// hacks are not implemented, all inert on our Apple vendor id (0x106B):
//  - RESZ (D3DRS_POINTSIZE == 0x7FA05000) triggers an MSAA depth resolve.
//    It is an AMD-lineage hack that wined3d fires for every vendor and DXVK
//    gates on AMD; dxmt does neither. With the RESZ FOURCC probe rejected in
//    CheckDeviceFormat and a non-AMD vendor id, no conformant app can
//    discover it, so games fall back to non-MSAA depth readback (via INTZ).
//    A full implementation would accept the RESZ probe and resolve into the
//    bound INTZ texture (the INTZ plumbing exists per the shadow-sampling
//    arc); deferred as a capability, not a correctness gap.
//  - ATOC alpha-to-coverage (D3DRS_ADAPTIVETESS_Y == 'ATOC', or the AMD
//    D3DRS_POINTSIZE == 'A2M1'/'A2M0' dialect) smooths alpha-tested cutouts.
//    Both refs implement it and Metal exposes the exact knob
//    (WMTRenderPipelineInfo alpha-to-coverage), but the ATOC/A2M1 format
//    probes are rejected and the vendor id disables the engine vendor paths,
//    so games fall back to plain alpha test: correct image, aliased cutouts.
//    Deferred enhancement (one PSO key bit + accepting the FOURCC probes) for
//    if a real MSAA title ever needs the smoothing.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
  D9DeviceLock lock = LockDevice();
  // One of the hottest entry points in D3D9 (D3DX effect frameworks
  // set every state per draw); keep the caller-thread cost minimal.
  if (State > 255 || (State < D3DRS_ZENABLE && State != 0))
    return D3D_OK;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapRenderStates[State] = Value;
    m_recordingBlock->m_changes.render_states[State] = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit (DXVK d3d9_device.cpp). D3DX
  // effect frameworks re-set identical state thousands of times per
  // frame; the no-change fast path skips both the per-setter
  // FlushDrawBatch (which would break encoder batching on AGX TBDR)
  // and the D9EmitOP queue write.
  if (m_renderStates[State] == Value)
    return D3D_OK;
  // SRGBWRITEENABLE flips the colour-attachment pixel format
  // (linear <-> sRGB-aliased view). The PSO and the render pass must
  // agree on attachment format; under the chunk path the per-batch
  // ResolveBatchedDrawForChunk reads D3DRS_SRGBWRITEENABLE at queue
  // time and StartRenderPassForBatch_d9 splits the render pass when
  // the attachment binding changes. No explicit encoder-end is needed
  // here; the next BatchedDraw against the new value naturally opens
  // a sibling render pass within the chunk.
  m_renderStates[State] = Value;
  // Invalidate the COW snapshot so the next QueueBatchedDraw captures
  // the new value. POD setters do not need to FlushDrawBatch; each
  // pending BatchedDraw already references its own frozen pod_snapshot.
  m_encShadowDirty |= dxmt::D9ES_DIRTY_RENDER_STATES;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) {
  D9DeviceLock lock = LockDevice();
  if (!pValue)
    return D3DERR_INVALIDCALL;
  // DXVK d3d9_device.cpp: out of the live-storage range is
  // INVALIDCALL on Get (asymmetric with Set, which silently no-ops).
  if (State > 255 || (State < D3DRS_ZENABLE && State != 0))
    return D3DERR_INVALIDCALL;
  // DXVK d3d9_device.cpp: slots inside the live-storage range
  // but outside the D3DRS_ZENABLE..D3DRS_BLENDOPALPHA enum (state 0,
  // 210..255 reserved) always read back as 0,
  // regardless of whether Set wrote to them. The asymmetry is part
  // of D3D9's contract; apps observe it.
  if (State < D3DRS_ZENABLE || State > D3DRS_BLENDOPALPHA)
    *pValue = 0;
  else
    *pValue = m_renderStates[State];
  return D3D_OK;
}
// State-block creation. wined3d device.c d3d9_device_CreateStateBlock
// gates Type to D3DSBT_ALL / VERTEXSTATE / PIXELSTATE; anything else
// is INVALIDCALL. The block round-trips every D3D9 state-block
// category on Apply.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) {
  D9DeviceLock lock = LockDevice();
  if (!ppSB)
    return D3DERR_INVALIDCALL;
  *ppSB = nullptr;
  if (Type != D3DSBT_ALL && Type != D3DSBT_VERTEXSTATE && Type != D3DSBT_PIXELSTATE)
    return D3DERR_INVALIDCALL;
  // Issuing CreateStateBlock between Begin/EndStateBlock is an error;
  // the runtime is mid-recording and conflating the two would
  // corrupt the recorded mask. wined3d returns INVALIDCALL.
  if (m_inStateBlockRecord)
    return D3DERR_INVALIDCALL;
  auto *sb = new MTLD3D9StateBlock(this, Type);
  // D3D9: CreateStateBlock captures state immediately (wined3d pattern).
  // Mask drives which categories Apply restores (D3DSBT_ALL/PIXELSTATE/VERTEXSTATE).
  D3D9StateBlockChanges changes;
  switch (Type) {
  case D3DSBT_ALL:
    changes.markAll();
    break;
  case D3DSBT_PIXELSTATE:
    changes.markPixelStateSubset();
    break;
  case D3DSBT_VERTEXSTATE:
    changes.markVertexStateSubset();
    break;
  default:
    break; // Unreachable; Type was already validated above.
  }
  sb->setChanges(changes);
  // Predefined blocks record every existing light index at creation (wined3d
  // stateblock_init_lights); the initial Capture then refreshes only these.
  if (changes.lights)
    sb->seedLightsFromDevice();
  sb->Capture();
  // Freeze the stream offset after the create-time capture: a later Capture on
  // this block updates the bound buffer + stride but keeps the offset frozen
  // (wined3d store_stream_offset). Recorded blocks never take this path.
  sb->freezeStreamOffset();
  sb->AddRef();
  sb->markLosable();
  *ppSB = sb;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::BeginStateBlock() {
  D9DeviceLock lock = LockDevice();
  if (m_inStateBlockRecord)
    return D3DERR_INVALIDCALL;
  // Recording redirects every Set* into this block's snapshot storage
  // so live device state stays untouched until the app Apply()s the
  // returned block (wine d3d9 device.c repoints device->update_state;
  // DXVK allocates m_recorder the same way at BeginStateBlock).
  //
  // Seed-capture the coarse-masked categories from live state. KNOWN
  // DIVERGENCE from the per-element tracking wined3d / DXVK do: where
  // the changed mask is one bit per category (sampler states, texture
  // stage states, transforms, clip planes, lights, VS/PS I+B constant
  // files, gaps inside the recorded F-constant range), recording ONE
  // element marks the whole category and Apply restores the
  // un-recorded siblings to these Begin-time values, not the live
  // values at Apply time. Render states are per-state exact; textures
  // and streams are per-slot; F constants are range-tracked. The
  // ref-pinned single-slot categories (textures, streams, index
  // buffer, decl, shaders) are NOT seeded: a recorded Set wholly
  // overwrites those snapshot slots, and skipping the seed keeps the
  // recording block from pinning every object bound at Begin time.
  auto *sb = new MTLD3D9StateBlock(this, D3DSBT_ALL);
  D3D9StateBlockChanges seed;
  seed.markAll();
  seed.textures = 0;
  seed.stream_source = 0;
  seed.stream_freq = 0;
  seed.index_buffer = false;
  seed.vertex_declaration = false;
  seed.vertex_shader = false;
  seed.pixel_shader = false;
  sb->setChanges(seed);
  sb->Capture();
  sb->setChanges(D3D9StateBlockChanges{});
  // A recorded block tracks only the lights its Set / Enable arms touch, so its
  // light-index set starts empty (unlike a predefined block, which seeds every
  // existing index at creation). The seed capture above skips lights entirely
  // because it never calls seedLightsFromDevice; this clear keeps the empty
  // invariant explicit.
  sb->m_snapLightIndices.clear();
  m_recordingBlock = sb;
  m_inStateBlockRecord = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EndStateBlock(IDirect3DStateBlock9 **ppSB) {
  D9DeviceLock lock = LockDevice();
  // End-without-Begin must leave the out-pointer untouched (wine
  // dlls/d3d9/tests/device.c test_begin_end_state_block asserts the
  // caller's sentinel survives), so the recording gate runs before any
  // write through ppSB.
  if (!ppSB || !m_inStateBlockRecord)
    return D3DERR_INVALIDCALL;
  m_inStateBlockRecord = false;
  // Hand the recording block out as-is: it already carries the
  // recorded values and the touched-state mask, and a capture-from-
  // live here would overwrite the recorded fields with live state the
  // recording deliberately never modified.
  auto *sb = m_recordingBlock;
  m_recordingBlock = nullptr;
  sb->AddRef();
  sb->markLosable();
  *ppSB = sb;
  return D3D_OK;
}
// SetClipStatus/GetClipStatus: vestigial FFP-era occlusion bookkeeping.
// wined3d device.c stubs wined3d_device_set_clip_status /
// get_clip_status (FIXME: store nothing, return OK). Apps still call these
// and don't always check the hr; E_NOTIMPL trips them. Spec-correct shape:
// round-trip the struct so a read-back is consistent, return D3D_OK.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus) {
  D9DeviceLock lock = LockDevice();
  if (!pClipStatus)
    return D3DERR_INVALIDCALL;
  m_clipStatus = *pClipStatus;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) {
  D9DeviceLock lock = LockDevice();
  if (!pClipStatus)
    return D3DERR_INVALIDCALL;
  *pClipStatus = m_clipStatus;
  return D3D_OK;
}
// Stage layout (wined3d pattern): PS 0..15 (slots 0..15), VS samplers 257..260->16..19.
// Out-of-range: GetTexture->NULL/D3D_OK; SetTexture->silent no-op (DMAP without cap check).
// GPU-side mapping at draw time (sampler slot N -> m_textures[N]).
namespace {
// Returns 0..19 for a valid stage, or UINT32_MAX for stages that the
// runtime ignores (D3DDMAPSAMPLER and out-of-range values).
inline uint32_t
texture_stage_to_slot(DWORD stage) {
  if (stage < 16)
    return stage;
  if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3)
    return 16 + (stage - D3DVERTEXTEXTURESAMPLER0);
  return UINT32_MAX;
}
} // namespace

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) {
  D9DeviceLock lock = LockDevice();
  if (!ppTexture)
    return D3DERR_INVALIDCALL;
  *ppTexture = nullptr;
  uint32_t slot = texture_stage_to_slot(Stage);
  if (slot == UINT32_MAX)
    return D3D_OK;
  MTLD3D9CommonTexture *bound = m_textures[slot].ptr();
  if (!bound)
    return D3D_OK;
  // Hand back the IDirect3DBaseTexture9 view of the leaf. The leaf
  // type tag picks which IDirect3D*Texture9 sub-interface the bound
  // pointer is castable to; static_cast to that, then to the base.
  IDirect3DBaseTexture9 *iface = nullptr;
  switch (bound->commonTextureType()) {
  case D3DRTYPE_TEXTURE:
    iface = static_cast<IDirect3DTexture9 *>(static_cast<MTLD3D9Texture *>(bound));
    break;
  case D3DRTYPE_CUBETEXTURE:
    iface = static_cast<IDirect3DCubeTexture9 *>(static_cast<MTLD3D9CubeTexture *>(bound));
    break;
  case D3DRTYPE_VOLUMETEXTURE:
    iface = static_cast<IDirect3DVolumeTexture9 *>(static_cast<MTLD3D9VolumeTexture *>(bound));
    break;
  default:
    // Defensive branch; every concrete commonTextureType() returns
    // one of the three D3DRTYPE_* values, so this is dead code in
    // practice. Match wined3d's looser shape (device.c
    // unconditionally hands back the parent regardless of type tag);
    // silent OK + null output keeps the contract uniform with the
    // unbound-slot branch above.
    return D3D_OK;
  }
  *ppTexture = ::dxmt::ref(iface);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
  D9DeviceLock lock = LockDevice();
  uint32_t slot = texture_stage_to_slot(Stage);
  if (slot == UINT32_MAX)
    return D3D_OK;

  // Identify the texture without calling through its app-visible vtable (which an
  // overlay may have wrapped, or an app deliberately corrupted): an unrecognised
  // non-null pointer is treated as "no texture", matching wined3d.
  MTLD3D9CommonTexture *common = commonTextureFromBound(pTexture);
  if (common) {
    // Cross-device check matches Set(RT|DepthStencilSurface). Same
    // reasoning: deviceRaw() avoids an AddRef/Release cycle that
    // GetDevice would force on a hot path.
    if (common->deviceRaw() != this)
      return D3DERR_INVALIDCALL;
    // A D3DPOOL_SCRATCH texture binds without error: the runtime samples its
    // CPU-resident contents (the Shared-storage Metal texture the SCRATCH
    // create allocates). Neither wined3d nor DXVK rejects it at the d3d9
    // layer, and the conformance suite expects D3D_OK, so dxmt does not
    // invent a stricter gate here.
  }
  // Recording arm AFTER the validation gates above (DXVK validates
  // then records). The Com<,false> assignment pins the target exactly
  // like Capture's snapshot does; the per-slot mask bit keeps Apply
  // away from slots the recording never touched.
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapTextures[slot] = common;
    m_recordingBlock->m_changes.textures |= 1u << slot;
    return D3D_OK;
  }
  // Defensive same-slot rebind; common in D3D9 engines that re-issue
  // every per-draw state-set unconditionally; would otherwise force a
  // fresh D9EncodingRefs COW snapshot at the next QueueBatchedDraw
  // (~50 AddRefPrivate ops walking every bound slot).
  if (m_textures[slot].ptr() == common)
    return D3D_OK;
  m_textures[slot] = common;
  // Binding a texture whose mip chain is already dirty (dirtied while unbound)
  // must move the sweep epoch: the mark itself bumped it earlier, but that
  // draw already swept and advanced past it, so without this the next draw
  // would sample the stale chain. mipsDirty is a single atomic-bool load.
  if (common && common->mipsDirty())
    markAutogenMipsDirty();
  // Same second-binding-path move for a MANAGED texture that gained pending
  // upload levels while unbound (level written but never individually Unlocked,
  // or re-armed by EvictManagedResources): the mark ran while it was unbound and
  // an intervening draw already swept past the epoch, so without this the next
  // draw would sample its stale GPU copy. A single mask load, like mipsDirty.
  if (common && common->hasPendingManagedUpload())
    markManagedUploadPending();
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (common)
    common->AddRefPrivate();
  QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::Texture0 + slot), common);
  return D3D_OK;
}

// FFP texture-blend: stage 0..7, type D3DTSS_COLOROP..CONSTANT (1..32).
// Out-of-range: INVALIDCALL (strict gate, no DMAP-style ignore).
// Programmable-PS apps call even with active shaders; return OK (not E_NOTIMPL) matching DXVK.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  D9DeviceLock lock = LockDevice();
  // wined3d d3d9/device.c returns D3D_OK silently for out-of-range
  // Type and does NOT bound Stage at all; DXVK d3d9_device.cpp
  // clamps and also returns D3D_OK. Silently ignore OOR: hr-strict
  // app init paths fail on an INVALIDCALL here.
  if (Stage >= 8 || Type == 0 || Type > D3DTSS_CONSTANT)
    return D3D_OK;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapTextureStageStates[Stage][Type] = Value;
    m_recordingBlock->m_changes.texture_stage_states = true;
    return D3D_OK;
  }
  if (m_textureStageStates[Stage][Type] == Value)
    return D3D_OK;
  m_textureStageStates[Stage][Type] = Value;
  // Resolve reads texture-stage state on the encode thread (PS bump-env
  // constants, the SM1.x projected-texturing mask), so the per-draw POD
  // snapshot must pick up the change: dirty the axis exactly like the
  // render-state and sampler-state setters. The device member stays the
  // source of truth for Get and state-block capture (calling-thread only).
  m_encShadowDirty |= dxmt::D9ES_DIRTY_TEXTURE_STAGE_STATES;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
  D9DeviceLock lock = LockDevice();
  if (!pValue)
    return D3DERR_INVALIDCALL;
  *pValue = 0;
  // Match the loose Set shape; wined3d d3d9/device.c returns
  // D3D_OK with *pValue=0 for OOR Type and doesn't bound Stage.
  if (Stage >= 8 || Type == 0 || Type > D3DTSS_CONSTANT)
    return D3D_OK;
  *pValue = m_textureStageStates[Stage][Type];
  return D3D_OK;
}
// One of hottest entry points: pure DWORD store/load.
// Stage layout matches SetTexture: PS 0..15, VS 257..260->16..19, out-of-range->no-op.
// D3DSAMP_INVALID slot (index 0) accepted; indices >14 out-of-enum rejected.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) {
  D9DeviceLock lock = LockDevice();
  if (!pValue)
    return D3DERR_INVALIDCALL;
  *pValue = 0;
  if (Type > D3DSAMP_DMAPOFFSET)
    return D3DERR_INVALIDCALL;
  uint32_t slot = texture_stage_to_slot(Sampler);
  if (slot == UINT32_MAX)
    return D3D_OK;
  *pValue = m_samplerStates[slot][Type];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  D9DeviceLock lock = LockDevice();
  // wined3d and DXVK size their sampler-state arrays at exactly
  // D3DSAMP_DMAPOFFSET + 1 and do not range-check Type, so a Type past the end
  // is an undefined out-of-bounds write there. dxmt rejects it with INVALIDCALL
  // instead (each slot is per-stage POD; no shader path consumes Type > 13).
  // hr-strict apps that expect D3D_OK on bogus Type bits land here.
  if (Type > D3DSAMP_DMAPOFFSET)
    return D3DERR_INVALIDCALL;
  uint32_t slot = texture_stage_to_slot(Sampler);
  if (slot == UINT32_MAX)
    return D3D_OK;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapSamplerStates[slot][Type] = Value;
    m_recordingBlock->m_changes.sampler_states = true;
    return D3D_OK;
  }
  // FETCH4 magic rides the LOD-bias state: 'GET4' arms the sampler's
  // latch, 'GET1' disarms it, any other value leaves it alone and lands
  // as a plain bias. Slots 16+ (vertex samplers) never fetch4.
  if (Type == D3DSAMP_MIPMAPLODBIAS && slot < 16) {
    if (Value == MAKEFOURCC('G', 'E', 'T', '4'))
      m_fetch4Latch |= (uint16_t)(1u << slot);
    else if (Value == MAKEFOURCC('G', 'E', 'T', '1'))
      m_fetch4Latch &= (uint16_t)~(1u << slot);
  }
  if (m_samplerStates[slot][Type] == Value)
    return D3D_OK;
  m_samplerStates[slot][Type] = Value;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_SAMPLER_STATES;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ValidateDevice(DWORD *pNumPasses) {
  D9DeviceLock lock = LockDevice();
  // Texture filtering has to be valid for every active fixed-function stage,
  // the way wined3d validates it (DXVK skips this and always returns OK). The
  // blend chain runs from stage 0 until a disabled colour op: each stage must
  // magnify and minify with at least a point filter, and a bound texture whose
  // format Metal cannot linearly filter rejects any linear filter the sampler
  // requests. Stages 0..7 map straight to sampler / texture slots 0..7. D3D9
  // leaves *pNumPasses untouched on these failures, so this precedes the
  // single-pass write below.
  for (uint32_t stage = 0; stage < 8; ++stage) {
    if (m_textureStageStates[stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
      break;
    DWORD mag_filter = m_samplerStates[stage][D3DSAMP_MAGFILTER];
    DWORD min_filter = m_samplerStates[stage][D3DSAMP_MINFILTER];
    DWORD mip_filter = m_samplerStates[stage][D3DSAMP_MIPFILTER];
    if (mag_filter == D3DTEXF_NONE || min_filter == D3DTEXF_NONE)
      return D3DERR_UNSUPPORTEDTEXTUREFILTER;
    MTLD3D9CommonTexture *tex = m_textures[stage].ptr();
    if (tex && IsMetalNonFilterableFormat(tex->d3dFormat()) &&
        (mag_filter == D3DTEXF_LINEAR || min_filter == D3DTEXF_LINEAR || mip_filter == D3DTEXF_LINEAR))
      return E_FAIL;
  }
  // Apps query: can current state render in single pass? Metal validates at PSO build.
  // Always claim single-pass. Accept NULL out-pointer (DXVK/wined3d pattern).
  if (pNumPasses)
    *pNumPasses = 1;
  // A non-Ex device that lost its display fails the query with DEVICELOST; the
  // single-pass write above still lands, matching DXVK d3d9_device.cpp
  // ValidateDevice (the write precedes the lost check). Ex never enters Lost.
  if (!m_isEx && m_deviceState.load(std::memory_order_relaxed) == DeviceState::Lost)
    return D3DERR_DEVICELOST;
  return D3D_OK;
}
// Texture-palette state: storage-only port of DXVK
// D3D9DeviceEx::Set/GetPaletteEntries / Set/GetCurrentTexturePalette
// (d3d9_device.cpp). dxmt has no FFP P8 sampler yet, so
// SetCurrentTexturePalette doesn't translate paletted reads; the
// palette state still needs a faithful round-trip per spec, since
// apps' init paths hr-check these.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries) {
  D9DeviceLock lock = LockDevice();
  if (pEntries == nullptr)
    return D3DERR_INVALIDCALL;
  // 256 entries per D3D9 spec; emplace-or-overwrite the map slot.
  auto it = m_texturePalettes.find(PaletteNumber);
  if (it == m_texturePalettes.end()) {
    std::array<PALETTEENTRY, 256> palette;
    std::memcpy(palette.data(), pEntries, sizeof(PALETTEENTRY) * 256);
    m_texturePalettes.emplace(PaletteNumber, palette);
  } else {
    std::memcpy(it->second.data(), pEntries, sizeof(PALETTEENTRY) * 256);
  }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) {
  D9DeviceLock lock = LockDevice();
  if (pEntries == nullptr)
    return D3DERR_INVALIDCALL;
  auto it = m_texturePalettes.find(PaletteNumber);
  if (it == m_texturePalettes.end())
    return D3DERR_INVALIDCALL;
  std::memcpy(pEntries, it->second.data(), sizeof(PALETTEENTRY) * 256);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetCurrentTexturePalette(UINT PaletteNumber) {
  D9DeviceLock lock = LockDevice();
  // DXVK note: when FFP P8 sampler lands, this should kick a texture
  // re-translate pass for all active paletted stages. Storage-only
  // for now matches DXVK's TODO at d3d9_device.cpp.
  m_currentTexturePalette = PaletteNumber;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetCurrentTexturePalette(UINT *PaletteNumber) {
  D9DeviceLock lock = LockDevice();
  if (PaletteNumber == nullptr)
    return D3DERR_INVALIDCALL;
  *PaletteNumber = m_currentTexturePalette;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetScissorRect(const RECT *pRect) {
  D9DeviceLock lock = LockDevice();
  if (!pRect)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapScissorRect = *pRect;
    m_recordingBlock->m_changes.scissor = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit (DXVK d3d9_device.cpp).
  if (std::memcmp(&m_scissorRect, pRect, sizeof(RECT)) == 0)
    return D3D_OK;
  m_scissorRect = *pRect;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_SCISSOR_RECT;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetScissorRect(RECT *pRect) {
  D9DeviceLock lock = LockDevice();
  if (!pRect)
    return D3DERR_INVALIDCALL;
  *pRect = m_scissorRect;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetSoftwareVertexProcessing(BOOL bSoftware) {
  D9DeviceLock lock = LockDevice();
  // Pure state echo (DXVK D3D9DeviceEx::SetSoftwareVertexProcessing): the mode
  // has no effect on Metal (always hardware-VP), but the value must round-trip
  // and reject the two illegal transitions the runtime forbids. A pure-HWVP
  // device (created without SOFTWARE or MIXED) cannot switch to software, and a
  // pure-SWVP device (created with SOFTWARE) cannot switch to hardware. wined3d
  // accepts unconditionally; DXVK's two rejects match native and the MSDN
  // contract, so mirror those.
  const DWORD flags = m_creationParams.BehaviorFlags;
  const bool canSWVP = (flags & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)) != 0;
  if (bSoftware && !canSWVP)
    return D3DERR_INVALIDCALL;
  if (!bSoftware && (flags & D3DCREATE_SOFTWARE_VERTEXPROCESSING))
    return D3DERR_INVALIDCALL;
  m_isSWVP = bSoftware != FALSE;
  // The mode is captured per draw so the encode side can force fixed-function
  // for a hardware-unrunnable shader; mark it dirty so the next draw re-freezes
  // it. A switch of mode also re-arms the one-shot draw gate.
  m_encShadowDirty |= dxmt::D9ES_DIRTY_SWVP;
  m_swvpDrawRejected = false;
  return D3D_OK;
}
BOOL STDMETHODCALLTYPE
MTLD3D9Device::GetSoftwareVertexProcessing() {
  D9DeviceLock lock = LockDevice();
  // Seeded TRUE on a pure-SWVP device (MSDN + DXVK m_isSWVP); tracks
  // SetSoftwareVertexProcessing thereafter.
  return m_isSWVP ? TRUE : FALSE;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetNPatchMode(float nSegments) {
  D9DeviceLock lock = LockDevice();
  // Pure device state (DXVK stores m_state.nPatchSegments, wined3d
  // set_npatch_mode): native records the segment count regardless of
  // tessellation support, so GetNPatchMode reads it back. The value has no
  // rendering effect here (N-patch tessellation is not advertised; caps report
  // MaxNpatchTessellationLevel 0), only the round-trip contract matters.
  m_nPatchMode = nSegments;
  return D3D_OK;
}
float STDMETHODCALLTYPE
MTLD3D9Device::GetNPatchMode() {
  D9DeviceLock lock = LockDevice();
  return m_nPatchMode;
}
bool
MTLD3D9Device::swvpDrawGateRejects() {
  // Only a device that exposes the extended file (software / mixed VP) can
  // reach this state; a pure hardware-VP device never gates, so its shaders
  // (which never reference c256..) take the path they always did.
  if (m_vsConstFCount <= D3D9_MAX_VS_CONST_F)
    return false;
  MTLD3D9VertexShader *vs = m_vertexShader.ptr();
  // Fixed-function, or a device already in software VP, always runs.
  if (!vs || m_isSWVP)
    return false;
  // A shader that stays within the hardware constant file (c0..c255) runs on
  // hardware VP; only one referencing the extended file (c256..) cannot.
  if (vs->metadata().max_float_const_index <= D3D9_MAX_VS_CONST_F)
    return false;
  // Needs software VP but the device is in hardware VP: native rejects the
  // FIRST such draw (INVALIDCALL, nothing rendered) and falls back to
  // fixed-function vertex processing for every draw after. Latch so only the
  // first draw rejects; Resolve forces the FFP path for the rest from the
  // per-draw captured mode.
  if (m_swvpDrawRejected)
    return false;
  m_swvpDrawRejected = true;
  return true;
}

// All entry points (DP/DIP/DPUP/DIPUP) queue: BuildDrawCapture->QueueBatchedDraw.
// Encode-side: ResolveBatchedDrawForChunk + EmitCommonRenderSetup_d9 + EmitDrawCommand_d9.
// Per-(RT,DS) encoder batching avoids tile-store/load; BatchedDraw POD-COW is DXVK m_dirty analogue.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::addFrameCounter(frameStats(), &dxmt::FrameStatistics::frame_draw_issue_count);
  // Caller-thread cost is queue-into-chunk only; encode/dispatch happen on the encode thread.
  // wined3d gates on vertex_declaration only; no BeginScene gate; stream 0 not required (multi-stream use
  // [[vertex_id]]).
  if (!m_vertexDeclaration.ptr())
    return D3DERR_INVALIDCALL;
  if (PrimitiveCount == 0)
    return D3D_OK;
  // Software-VP-only shader bound in hardware VP: reject the first draw.
  if (swvpDrawGateRejects())
    return D3DERR_INVALIDCALL;
  // No autorelease pool (post-migration: all handles retained +1 in WMT::Reference).
  // Per-pool cost: 2 syscalls (~2ms per frame at modest draw counts); wined3d GL backend no analogue.
  // Fan emulation: synthesise (0,k+1,k+2) IB from m_constRing, route as TRIANGLELIST indexed.
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto [ib_handle, ib_offset_u32] = BuildFanIndexBuffer(PrimitiveCount, nullptr, 0);
    // Zero is the "no override" sentinel the resolve path tests, so a fan
    // buffer that could not be allocated has to skip the draw: resolving it
    // without the override would bind whatever index buffer is currently set
    // and read it at the remapped triangle-list count. Skipping rather than
    // failing the call follows the ring-allocation policy of the UP paths.
    if (ib_handle == 0)
      return D3D_OK;
    BatchedDraw draw{};
    draw.cap = BuildDrawCapture();
    draw.type = BatchedDraw::kIndexed;
    draw.primitive_type = D3DPT_TRIANGLELIST;
    draw.vertex_or_index_count = PrimitiveCount * 3;
    draw.base_vertex = static_cast<INT>(StartVertex);
    draw.override_ib_buffer = ib_handle;
    draw.override_ib_offset = ib_offset_u32;
    draw.override_ib_format = D3DFMT_INDEX32;
    QueueBatchedDraw(std::move(draw));
    // Accumulate. State-change handlers + Present drain m_pendingDraws.
    return D3D_OK;
  }
  UINT vertex_count = prim_to_vertex_count(PrimitiveType, PrimitiveCount);
  BatchedDraw draw{};
  draw.cap = BuildDrawCapture();
  draw.type = BatchedDraw::kNonIndexed;
  draw.primitive_type = PrimitiveType;
  draw.vertex_or_index_count = vertex_count;
  draw.start_vertex_or_index = StartVertex;
  QueueBatchedDraw(std::move(draw));
  // Accumulate. State-change handlers + Present drain m_pendingDraws.
  return D3D_OK;
}

// DrawIndexedPrimitive: same as DrawPrimitive + bound IB + BaseVertexIndex.
// Metal resolves indices + adds baseVertex; manual-fetch lowering consumes value as-is.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex,
    UINT PrimitiveCount
) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::addFrameCounter(frameStats(), &dxmt::FrameStatistics::frame_draw_issue_count);
  // wined3d d3d9_device_DrawIndexedPrimitive (device.c) gates on
  // vertex_declaration AND index_buffer; no BeginScene gate, no
  // stream-0 gate (see DrawPrimitive for the multi-stream rationale).
  if (!m_vertexDeclaration.ptr())
    return D3DERR_INVALIDCALL;
  // A NULL index buffer has nothing to index, so Windows draws nothing and
  // returns D3D_OK rather than rejecting (wined3d marks this todo_wine). Return
  // before the null index reaches the draw below.
  if (!m_indexBuffer.ptr())
    return D3D_OK;
  // DXVK D3D9DeviceEx::DrawIndexedPrimitive early-outs D3D_OK on
  // (!PrimitiveCount || !NumVertices); a zero-vertex range is a
  // degenerate no-op, matching the DrawIndexedPrimitiveUP sibling below.
  if (PrimitiveCount == 0 || NumVertices == 0)
    return D3D_OK;
  // Software-VP-only shader bound in hardware VP: reject the first draw.
  if (swvpDrawGateRejects())
    return D3DERR_INVALIDCALL;
  // No autorelease pool; see DrawPrimitive for the rationale.
  // Fan emulation against a bound IB; read the source indices through
  // the host pointer at (currentOffset() + StartIndex * indexSize) and
  // remap into a fresh u32 list. m_hostPtr is null only for pool
  // combinations that have no sysmem mirror (a future DEFAULT-static
  // path); we reject those rather than silently mis-rendering. The
  // resulting IB rides m_constRing; pinned to m_completionEvent via
  // the chunk lambda's signal_seq tail.
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto *ib_obj = m_indexBuffer.ptr();
    const void *src_base = ib_obj->hostPointer();
    if (!src_base)
      return D3DERR_INVALIDCALL;
    uint32_t src_idx_size = (ib_obj->indexFormat() == D3DFMT_INDEX32) ? 4u : 2u;
    // Clamp the fan remap to what the index mirror actually holds: the read
    // below touches PrimitiveCount + 2 indices from src_base + StartIndex, and
    // dxmt reads it host-side where the references read GPU-side (see
    // fan_index_prim_clamp). A fully-supplied draw clamps to itself.
    uint32_t fan_prim_count = fan_index_prim_clamp(ib_obj->size(), src_idx_size, StartIndex, PrimitiveCount);
    if (fan_prim_count == 0)
      return D3D_OK;
    const void *src = static_cast<const char *>(src_base) + static_cast<size_t>(StartIndex) * src_idx_size;
    auto [ib_handle, ib_offset_u32] = BuildFanIndexBuffer(fan_prim_count, src, src_idx_size);
    // See DrawPrimitive: a zero handle reads as "no override" downstream.
    if (ib_handle == 0)
      return D3D_OK;
    BatchedDraw draw{};
    draw.cap = BuildDrawCapture();
    draw.type = BatchedDraw::kIndexed;
    draw.primitive_type = D3DPT_TRIANGLELIST;
    draw.vertex_or_index_count = fan_prim_count * 3;
    draw.base_vertex = BaseVertexIndex;
    draw.override_ib_buffer = ib_handle;
    draw.override_ib_offset = ib_offset_u32;
    draw.override_ib_format = D3DFMT_INDEX32;
    QueueBatchedDraw(std::move(draw));
    // Accumulate. State-change handlers + Present drain m_pendingDraws.
    return D3D_OK;
  }
  // Metal always enables primitive restart for strip topologies, cutting the
  // strip at the max index value (0xffff for 16-bit indices); D3D9 has no
  // primitive restart, so 0xffff must draw as an ordinary index. When a 16-bit
  // strip draw contains the sentinel, widen its index range to 32-bit (where
  // 0xffff becomes 0x0000ffff, not the 0xffffffff cut) so Metal keeps the strip
  // whole. Lists and points are immune (Metal restarts strips only), and the
  // scan is skipped for 32-bit index buffers. wined3d/DXVK disable restart on
  // Vulkan, an option Metal does not expose. A per-buffer max index tracked at
  // Unlock would make the common no-sentinel case O(1); the scan is gated to
  // 16-bit strips so mesh lists never pay it.
  if (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_LINESTRIP) {
    auto *ib_obj = m_indexBuffer.ptr();
    const void *src_base = ib_obj->indexFormat() != D3DFMT_INDEX32 ? ib_obj->hostPointer() : nullptr;
    if (src_base) {
      UINT strip_count = prim_to_vertex_count(PrimitiveType, PrimitiveCount);
      const size_t start_bytes = static_cast<size_t>(StartIndex) * 2u;
      const uint32_t avail = ib_obj->size() > start_bytes ? (ib_obj->size() - start_bytes) / 2u : 0u;
      if (strip_count > avail)
        strip_count = avail;
      const uint16_t *src16 = reinterpret_cast<const uint16_t *>(static_cast<const char *>(src_base) + start_bytes);
      bool has_sentinel = false;
      for (UINT i = 0; i < strip_count; ++i)
        if (src16[i] == 0xffffu) {
          has_sentinel = true;
          break;
        }
      if (has_sentinel) {
        auto [ib_handle, ib_offset_u32] = BuildWidenedIndexBuffer(src16, strip_count);
        // See DrawPrimitive for the sentinel. Falling through to the ordinary
        // path is not an alternative here: it would draw the 16-bit indices
        // whose sentinel value this widening exists to escape.
        if (ib_handle == 0)
          return D3D_OK;
        BatchedDraw draw{};
        draw.cap = BuildDrawCapture();
        draw.type = BatchedDraw::kIndexed;
        draw.primitive_type = PrimitiveType;
        draw.vertex_or_index_count = strip_count;
        draw.base_vertex = BaseVertexIndex;
        draw.override_ib_buffer = ib_handle;
        draw.override_ib_offset = ib_offset_u32;
        draw.override_ib_format = D3DFMT_INDEX32;
        QueueBatchedDraw(std::move(draw));
        return D3D_OK;
      }
    }
  }
  UINT index_count = prim_to_vertex_count(PrimitiveType, PrimitiveCount);
  BatchedDraw draw{};
  draw.cap = BuildDrawCapture();
  draw.type = BatchedDraw::kIndexed;
  draw.primitive_type = PrimitiveType;
  draw.vertex_or_index_count = index_count;
  draw.start_vertex_or_index = StartIndex;
  draw.base_vertex = BaseVertexIndex;
  QueueBatchedDraw(std::move(draw));
  // Accumulate. State-change handlers + Present drain m_pendingDraws.
  return D3D_OK;
}

// Shared body: bound-stream differ in IB/count/BaseVertexIndex; UP inject transient slot-0.
// Validation gate reads from capture (caller-provided or UP-built at queue time).
// UP build at queue time ensures validation sees shader/RT bindings from draw thread, not FlushDrawBatch thread.
MTLD3D9Device::D3D9DrawCapture
MTLD3D9Device::BuildDrawCapture() {
  // Bring every bound buffer's device copy up to date before the record timer
  // starts. The refresh charges itself to the staging axis, so timing it here as
  // well would count it on two axes that the frontend total adds together, and
  // that total would then exceed the frame wall. Each refresh is a no-op when the
  // cache is current, and refreshing every stream before any is frozen still
  // leaves each rename ahead of the freeze that names it. The pre-draw sweeps in
  // QueueBatchedDraw sit outside their timer for the same reason.
  for (uint32_t s = 0; s < D3D9_MAX_VERTEX_STREAMS; ++s) {
    if (auto *vb = m_vertexBuffers[s].ptr())
      vb->refreshWholeMirror();
  }
  if (m_indexBuffer.ptr() != nullptr)
    m_indexBuffer->refreshWholeMirror();

  // Per-draw capture cost (paired with QueueBatchedDraw below).
  dxmt::perf::ScopedFrameDuration _record_timer(frameStats(), &dxmt::FrameStatistics::frame_draw_record_interval);
  // POD state read by Resolve from D9EncodingState; setter-flush invariant ensures batch shares one snapshot.
  // Ref-counted state via setter ops into m_encodeSideRefs.
  // BuildDrawCapture must freeze per-draw rename cursors (gpu_address/currentOffset advance on Lock(DISCARD)).
  D3D9DrawCapture cap;

  // vb_slots is value-initialized (zero-filled) by the struct default
  // ctor (= {}), so unbound slots already report buffer=0,gpu_address=0.
  // The bound buffer pointer is the single source of truth for stream
  // liveness; wined3d (context.c wined3d_stream_info_from_declaration)
  // and DXVK both derive it per draw rather than trusting a cached
  // mask.
  for (uint32_t s = 0; s < D3D9_MAX_VERTEX_STREAMS; ++s) {
    auto *vb = m_vertexBuffers[s].ptr();
    if (!vb)
      continue;
    cap.vb_slots[s].offset = m_streamOffsets[s];
    cap.vb_slots[s].stride = m_streamStrides[s];
    // Freeze the handle, gpu_address, and the tracked allocation from ONE
    // immediateName() read, taken AFTER the refresh so all three name the
    // allocation the refresh renamed to: the emit binds
    // cap.vb_slots[].buffer / gpu_address and fence-tracks
    // cap.vb_slots[].alloc, and a split read could straddle a rename.
    Rc<dxmt::BufferAllocation> alloc = vb->immediateAllocation();
    cap.vb_slots[s].buffer = alloc->buffer().handle;
    cap.vb_slots[s].gpu_address = alloc->gpuAddress();
    cap.vb_slots[s].alloc = std::move(alloc);
  }
  if (m_indexBuffer.ptr() != nullptr) {
    // Single immediateName() read freeze; see the vertex-stream branch above.
    Rc<dxmt::BufferAllocation> alloc = m_indexBuffer->immediateAllocation();
    cap.ib_buffer = alloc->buffer().handle;
    cap.ib_alloc = std::move(alloc);
    cap.ib_offset = m_indexBuffer->currentOffset();
    cap.ib_format = m_indexBuffer->indexFormat();
  } else {
    cap.ib_buffer = 0;
    cap.ib_offset = 0;
    cap.ib_format = D3DFMT_UNKNOWN;
  }

  return cap;
}

void
MTLD3D9Device::QueueBatchedDraw(BatchedDraw &&draw) {
  // Regenerate any bound AUTOGENMIPMAP texture's dirty mip chain before this
  // draw is pushed, so the mip-gen op precedes the draws that sample it in the
  // op stream (DXVK PrepareDraw shape). The hot path pays a single relaxed load
  // when nothing changed; the sweep only runs when the epoch has moved.
  uint32_t autogen_epoch = m_autogenDirtyEpoch.load(std::memory_order_relaxed);
  if (autogen_epoch != m_autogenSweptEpoch) {
    sweepBoundAutogenMips();
    m_autogenSweptEpoch = autogen_epoch;
  }
  // Re-push any bound MANAGED texture's pending sysmem bytes before this draw, so
  // the upload op precedes the draws that sample it (the DXVK PrepareDraw upload
  // shape). Same one-relaxed-load-when-idle gate as the autogen sweep above.
  uint32_t managed_epoch = m_managedUploadEpoch.load(std::memory_order_relaxed);
  if (managed_epoch != m_managedUploadSweptEpoch) {
    sweepBoundManagedUploads();
    m_managedUploadSweptEpoch = managed_epoch;
  }
  // Per-draw queue cost (paired with BuildDrawCapture above).
  dxmt::perf::ScopedFrameDuration _record_timer(frameStats(), &dxmt::FrameStatistics::frame_draw_record_interval);
  // Freeze POD state: m_encShadowDirty==0 means all draws in the chunk share
  // one snapshot. Non-zero: build a fresh snapshot, overwriting only dirty
  // axes. Resolve reads draw.pod_snapshot, letting setters skip
  // FlushDrawBatch (each frozen independently). Storage comes from the
  // queue's command-data ring rather than the process heap: the snapshot is
  // recycled wholesale once its chunk retires, so the ~200 clusters a heavy
  // frame produces cost no allocator locking and no cross-thread frees. A
  // snapshot only stays valid for its own chunk; when the chunk has moved
  // on, the previous block may already be recycled, so a chunk change
  // rebuilds every axis from the device shadows instead of copying the old
  // snapshot forward.
  static_assert(
      std::is_trivially_copyable_v<dxmt::D9EncodingState> && std::is_trivially_destructible_v<dxmt::D9EncodingState>,
      "ring-allocated snapshots are recycled without destruction"
  );
  const uint64_t snap_chunk = m_dxmtQueue->CurrentSeqId();
  const bool snap_reusable = m_encShadowLastSnap != nullptr && m_encShadowLastSnapChunk == snap_chunk;
  if (m_encShadowDirty != 0 || !snap_reusable) {
    auto *snap = static_cast<dxmt::D9EncodingState *>(
        m_dxmtQueue->AllocateCommandData(sizeof(dxmt::D9EncodingState), alignof(dxmt::D9EncodingState))
    );
    // Without a snapshot the encode side has no state to resolve this draw
    // against, so drop it rather than construct through a pointer the
    // command-data ring could not back. This catches exhaustion only at a
    // block boundary: the ring reports a failed block as a null first
    // suballocation, while a later suballocation of that same block comes back
    // as a small offset from null and passes the test. Losing a draw beats
    // writing through the pointer in the case we can see.
    if (snap == nullptr) {
      static std::once_flag warned;
      std::call_once(warned, []() {
        Logger::warn("d3d9: out of memory for a draw state snapshot; the draw is dropped");
      });
      return;
    }
    uint32_t dirty = m_encShadowDirty;
    if (snap_reusable)
      new (snap) dxmt::D9EncodingState(*m_encShadowLastSnap);
    else {
      new (snap) dxmt::D9EncodingState();
      dirty = dxmt::D9ES_DIRTY_ALL;
    }
    if (dirty & dxmt::D9ES_DIRTY_RENDER_STATES)
      std::memcpy(snap->render_states, m_renderStates, sizeof(snap->render_states));
    if (dirty & dxmt::D9ES_DIRTY_SAMPLER_STATES) {
      std::memcpy(snap->sampler_states, m_samplerStates, sizeof(snap->sampler_states));
      // The FETCH4 latch only flips in a SetSamplerState path that also
      // rewrites the stored LOD bias, so it always co-moves with this axis.
      snap->fetch4_latch = m_fetch4Latch;
    }
    if (dirty & dxmt::D9ES_DIRTY_TEXTURE_STAGE_STATES) {
      static_assert(
          sizeof(dxmt::D9EncodingState::texture_stage_states) == sizeof(m_textureStageStates),
          "TSS snapshot shape must match the device member"
      );
      std::memcpy(snap->texture_stage_states, m_textureStageStates, sizeof(snap->texture_stage_states));
    }
    if (dirty & dxmt::D9ES_DIRTY_CLIP_PLANES)
      std::memcpy(snap->clip_planes, m_clipPlanes, sizeof(snap->clip_planes));
    if (dirty & dxmt::D9ES_DIRTY_STREAM_FREQ)
      std::memcpy(snap->stream_freq, m_streamFreq, sizeof(snap->stream_freq));
    if (dirty & dxmt::D9ES_DIRTY_VS_CONST_F) {
      std::memcpy(snap->vs_const_F, m_vsConstantsF, sizeof(snap->vs_const_F));
      // A software / mixed-VP device with a bound shader that reaches the
      // extended constant file (c256..) needs it uploaded: either through
      // relative addressing (c[a0+N], any index) or a direct read/def past
      // c255 (max_float_const_index > 256). Freeze it into the queue's
      // command-data ring alongside the snapshot (same recycle lifetime) so
      // the encode side can upload it without racing the device store, and so
      // the vertex constant buffer is sized to cover the reads (a direct c256+
      // read with the file left at 256 would fault the GPU). A hardware-VP
      // device, or a shader that stays within c0..c255, keeps the pointer null
      // and pays nothing.
      snap->vs_const_F_overflow = nullptr;
      snap->vs_const_F_overflow_count = 0;
      if (m_vsConstFCount > D3D9_MAX_VS_CONST_F && m_vsConstantsFOverflow) {
        MTLD3D9VertexShader *bvs = m_vertexShader.ptr();
        if (bvs &&
            (bvs->metadata().uses_relative_const || bvs->metadata().max_float_const_index > D3D9_MAX_VS_CONST_F)) {
          const uint32_t ext = m_vsConstFCount - D3D9_MAX_VS_CONST_F;
          void *ov = m_dxmtQueue->AllocateCommandData(static_cast<size_t>(ext) * 4u * sizeof(float), alignof(float));
          // The extended constant file stays unbound if the ring could not back
          // it; the shader then reads the defaults rather than through a
          // pointer the ring never provided.
          if (ov != nullptr) {
            std::memcpy(ov, m_vsConstantsFOverflow.get(), static_cast<size_t>(ext) * 4u * sizeof(float));
            snap->vs_const_F_overflow = static_cast<const float (*)[4]>(ov);
            snap->vs_const_F_overflow_count = ext;
          }
        }
      }
    }
    if (dirty & dxmt::D9ES_DIRTY_VS_CONST_I)
      std::memcpy(snap->vs_const_I, m_vsConstantsI, sizeof(snap->vs_const_I));
    if (dirty & dxmt::D9ES_DIRTY_VS_CONST_B)
      std::memcpy(snap->vs_const_B, m_vsConstantsB, sizeof(snap->vs_const_B));
    if (dirty & dxmt::D9ES_DIRTY_PS_CONST_F)
      std::memcpy(snap->ps_const_F, m_psConstantsF, sizeof(snap->ps_const_F));
    if (dirty & dxmt::D9ES_DIRTY_PS_CONST_I)
      std::memcpy(snap->ps_const_I, m_psConstantsI, sizeof(snap->ps_const_I));
    if (dirty & dxmt::D9ES_DIRTY_PS_CONST_B)
      std::memcpy(snap->ps_const_B, m_psConstantsB, sizeof(snap->ps_const_B));
    if (dirty & dxmt::D9ES_DIRTY_FFP) {
      if (m_ffpWVPStale) {
        // Row-vector convention: out = v * world * view * projection.
        // The intermediate world*view product's z column also feeds the
        // generated vertex fog (view-space depth).
        D3DMATRIX wv = mat4_multiply(m_transforms[10], m_transforms[0]);
        D3DMATRIX wvp = mat4_multiply(wv, m_transforms[1]);
        static_assert(sizeof(m_ffpWVP) == sizeof(wvp), "");
        std::memcpy(m_ffpWVP, &wvp, sizeof(m_ffpWVP));
        // Inverse of view*projection for the world-space FFP clip-plane
        // transform (only consumed by an FFP draw with clip planes enabled).
        D3DMATRIX vpInv = mat4_inverse(mat4_multiply(m_transforms[0], m_transforms[1]));
        std::memcpy(m_ffpVPInv, &vpInv, sizeof(m_ffpVPInv));
        for (int b = 0; b < 3; ++b) {
          D3DMATRIX bwv = mat4_multiply(m_transforms[11 + b], m_transforms[0]);
          D3DMATRIX bwvp = mat4_multiply(bwv, m_transforms[1]);
          std::memcpy(m_ffpWVPBlend[b], &bwvp, sizeof(m_ffpWVPBlend[b]));
          // The blend matrix's world*view columns for the eye-space blend
          // (x, y, z as four floats each), matching the matrix-0 packing
          // below so the shader dots the model position against them.
          for (int r = 0; r < 4; ++r) {
            m_ffpWVBlend[b][0 + r] = bwv.m[r][0];
            m_ffpWVBlend[b][4 + r] = bwv.m[r][1];
            m_ffpWVBlend[b][8 + r] = bwv.m[r][2];
          }
        }
        for (int r = 0; r < 4; ++r) {
          m_ffpWVX[r] = wv.m[r][0];
          m_ffpWVY[r] = wv.m[r][1];
          m_ffpWVZ[r] = wv.m[r][2];
        }
        // The eye-space normal transform is the inverse-transpose of the
        // world*view (wined3d compute_normal_matrix): pack the x/y/z rows
        // of inverse(WV), so the shader's dot4(normal, row_j) yields
        // eye_normal.j = sum_i normal_i * inverse(WV)[j][i], the transpose
        // of the inverse applied to a row-vector normal. Lighting then
        // holds under non-uniform scale where plain WV would skew it. A
        // singular WV falls back to identity (mat4_inverse), an acceptable
        // degenerate. The VERTEXBLEND arm keeps plain per-matrix WV.
        D3DMATRIX wvInv = mat4_inverse(wv);
        for (int j = 0; j < 3; ++j)
          for (int k = 0; k < 4; ++k)
            m_ffpNormal[j][k] = wvInv.m[j][k];
        // Table-fog coordinate choice: device z only when the projection
        // cannot produce a non-unit w (fourth column exactly 0,0,0,1);
        // any other projection fogs against eye-space w. test_fog pins
        // both sides, including a near-identity matrix with a scaled
        // last element taking the w side.
        const D3DMATRIX &proj = m_transforms[1];
        m_ffpFogCoordW =
            !(proj.m[0][3] == 0.0f && proj.m[1][3] == 0.0f && proj.m[2][3] == 0.0f && proj.m[3][3] == 1.0f);
        m_ffpWVPStale = false;
      }
      std::memcpy(snap->ffp_wvp, m_ffpWVP, sizeof(snap->ffp_wvp));
      std::memcpy(snap->ffp_vp_inv, m_ffpVPInv, sizeof(snap->ffp_vp_inv));
      std::memcpy(snap->ffp_wvp_blend, m_ffpWVPBlend, sizeof(snap->ffp_wvp_blend));
      std::memcpy(snap->ffp_wv_z, m_ffpWVZ, sizeof(snap->ffp_wv_z));
      std::memcpy(snap->ffp_wv_x, m_ffpWVX, sizeof(snap->ffp_wv_x));
      std::memcpy(snap->ffp_wv_y, m_ffpWVY, sizeof(snap->ffp_wv_y));
      std::memcpy(snap->ffp_wv_blend, m_ffpWVBlend, sizeof(snap->ffp_wv_blend));
      std::memcpy(snap->ffp_normal, m_ffpNormal, sizeof(snap->ffp_normal));
      snap->ffp_fog_coord_w = m_ffpFogCoordW ? 1u : 0u;
      static_assert(sizeof(snap->ffp_material) <= sizeof(D3DMATERIAL9), "");
      std::memcpy(snap->ffp_material, &m_material, sizeof(snap->ffp_material));
      uint32_t li = 0;
      for (size_t i = 0; i < m_lights.size() && li < 8; ++i) {
        if (!m_lightEnables[i])
          continue;
        static_assert(sizeof(D3DLIGHT9) <= sizeof(snap->ffp_lights[0]), "");
        std::memcpy(snap->ffp_lights[li], &m_lights[i], sizeof(D3DLIGHT9));
        // D3D9 light position/direction are world space, but the vertex pipe
        // lights in view space (eye position/normal come from the world*view
        // columns). Both references upload the light already view-transformed;
        // pre-multiply here by D3DTS_VIEW (m_transforms[0]) so a non-identity
        // camera does not leave point lights displaced by V^-1 and directional
        // lights rotating with the view. The shader normalizes directions, so
        // leaving the view's scale in Direction is harmless.
        auto *snap_light = reinterpret_cast<D3DLIGHT9 *>(snap->ffp_lights[li]);
        const float pos_in[3] = {m_lights[i].Position.x, m_lights[i].Position.y, m_lights[i].Position.z};
        const float dir_in[3] = {m_lights[i].Direction.x, m_lights[i].Direction.y, m_lights[i].Direction.z};
        transform_row_vec3(m_transforms[0], pos_in, 1.0f, &snap_light->Position.x);
        transform_row_vec3(m_transforms[0], dir_in, 0.0f, &snap_light->Direction.x);
        ++li;
      }
      snap->ffp_light_count = li;
      static_assert(sizeof(snap->ffp_tex_mats) == sizeof(D3DMATRIX) * 8, "");
      std::memcpy(snap->ffp_tex_mats, &m_transforms[2], sizeof(snap->ffp_tex_mats));
    }
    if (dirty & dxmt::D9ES_DIRTY_VS_CONST_F_MAX)
      snap->vs_const_f_max = m_vsConstFMax;
    if (dirty & dxmt::D9ES_DIRTY_PS_CONST_F_MAX)
      snap->ps_const_f_max = m_psConstFMax;
    if (dirty & dxmt::D9ES_DIRTY_SWVP) {
      snap->is_swvp = m_isSWVP ? 1u : 0u;
      snap->sw_vp_capable = m_vsConstFCount > D3D9_MAX_VS_CONST_F ? 1u : 0u;
    }
    if (dirty & dxmt::D9ES_DIRTY_VIEWPORT)
      snap->viewport = m_viewport;
    if (dirty & dxmt::D9ES_DIRTY_SCISSOR_RECT)
      snap->scissor_rect = m_scissorRect;
    m_encShadowLastSnap = snap;
    m_encShadowLastSnapChunk = snap_chunk;
    m_encShadowDirty = 0;
  }
  draw.pod_snapshot = m_encShadowLastSnap;

  // No per-draw ref snapshot; ref-counted state lives on the
  // device-side m_encodeSideRefs mirror that the chunk walker mutates
  // as it processes SetRef ops in arrival order. Resolve reads from
  // there directly. The migration deletes the 40-Com<>-slot AddRef
  // pair the COW model paid per cluster boundary (~4400 ref setters/frame
  // in a heavy draw scene, clustered such that effectively every draw
  // rebuilt the snapshot under COW). wined3d CS shape.

  // Push to the arrival-order op stream FIRST so the index field
  // points at the slot we're about to occupy in m_pendingDraws.
  m_pendingOps.push_back({PendingOpRef::Draw, static_cast<uint32_t>(m_pendingDraws.size())});
  m_pendingDraws.push_back(std::move(draw));
  // Submit once the renamed allocations this command buffer pins outgrow the
  // threshold. A draw is the boundary to do it on: the recording is complete,
  // and unlike Lock no application-held pointer is outstanding. See
  // kRenameBytesBeforeImplicitCommit for why an unsubmitted scene is the case
  // that needs this.
  if (m_renamedBytesSinceCommit >= kRenameBytesBeforeImplicitCommit)
    forceFlushAndCommit();
  // Per-frame draw rate. All four Draw* entry points funnel through
  // here (DrawPrimitive / DrawIndexedPrimitive and the UP siblings),
  // so a single bump here covers the full draw stream.
}

void
MTLD3D9Device::QueueBlitOp(PendingBlitOp &&op) {
  // Same arrival-order discipline as QueueBatchedDraw; record the
  // ref before pushing the payload so the index field stays consistent.
  // Blits ride the same chunk lambda as draws; arrival-order across
  // kinds matters for sequencing a blit's GPU writes against the
  // draws that read its destination.
  m_pendingOps.push_back({PendingOpRef::Blit, static_cast<uint32_t>(m_pendingBlits.size())});
  m_pendingBlits.push_back(std::move(op));
}

Rc<dxmt::Texture>
MTLD3D9Device::createTransientResolveTarget(WMTPixelFormat format, uint32_t width, uint32_t height) {
  WMTTextureInfo info{};
  info.pixel_format = format;
  info.width = width;
  info.height = height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  // RenderTarget: the resolve pass writes it. ShaderRead: the Stretch samples
  // it. PixelFormatView: the Stretch source view carries the D3D9 channel
  // swizzle of a fixup source. Private, GPU-only; never locked or presented.
  info.usage = static_cast<WMTTextureUsage>(
      WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead | WMTTextureUsagePixelFormatView
  );
  info.options = WMTResourceStorageModePrivate;
  Rc<dxmt::Texture> tex = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = tex->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return nullptr;
  tex->rename(std::move(allocation));
  return tex;
}

void
MTLD3D9Device::QueueRefOp(PendingRefOp::Slot slot, void *new_com) {
  // Same arrival-order discipline as the Draw / Blit queues. The caller
  // (the ref-state setter) AddRefPrivate'd new_com exactly once before
  // calling; that single ref is the lifetime guarantee until the chunk
  // walker installs it into m_encodeSideRefs via ApplyRefOp_d9.
  m_pendingOps.push_back({PendingOpRef::SetRef, static_cast<uint32_t>(m_pendingRefOps.size())});
  m_pendingRefOps.push_back({slot, new_com});
}

// Encode-thread walker hook: install one SetRef op into m_encodeSideRefs.
// The op carries one outstanding AddRefPrivate (or nullptr); the static_cast
// + Com<,false>::operator=(T*) path would AddRef again, so we manage the
// install manually: take_old via prvRef() pattern (release the prior slot)
// then poke the raw pointer into the slot's Com<,false>. The
// implementation lives in d3d9_device.cpp (here) rather than as a free
// helper because it needs the slot enum + every D9 resource type
// definition in scope, all of which are already known to this TU.
void
MTLD3D9Device::ApplyRefOp_d9(const PendingRefOp &op) {
  // Helper: install a raw pointer (with one outstanding AddRefPrivate)
  // into a Com<,false> slot. Releases the prior slot value's private
  // ref, takes the new ref by raw assignment (no further AddRef; the
  // setter's AddRef is the lifetime). nullptr is a valid unbind.
  auto install = [](auto &slot_ref, void *new_com) {
    using ComT = std::remove_reference_t<decltype(slot_ref)>;
    using T = typename std::remove_pointer<decltype(slot_ref.ptr())>::type;
    auto *prev = slot_ref.ptr();
    // Reset slot to null while releasing the prior ref. Com<,false>::
    // operator=(nullptr) does decRef() on m_ptr.
    slot_ref = nullptr;
    // Move the new pointer in WITHOUT re-AddRef. Move-assign from a
    // Com<,false> built via takeOwnership idiom: construct a temporary
    // Com<,false> that holds the pointer with zero outstanding refs,
    // then move-assign; move-assign skips both decRef-and-incRef.
    if (new_com) {
      ComT tmp;
      // Bypass public ctor via move-assign of temporary built with
      // takeOwnership idiom: move-assign skips incRef on the source.
      *(&tmp) = static_cast<T *>(new_com);
      slot_ref = std::move(tmp);
    }
    (void)prev; // prev was already released by the `= nullptr` above
  };

  if (op.slot >= PendingRefOp::Texture0 && op.slot <= PendingRefOp::Texture19) {
    unsigned i = op.slot - PendingRefOp::Texture0;
    install(m_encodeSideRefs.textures[i], op.com_ptr);
    return;
  }
  if (op.slot >= PendingRefOp::VertexBuffer0 && op.slot <= PendingRefOp::VertexBuffer15) {
    unsigned i = op.slot - PendingRefOp::VertexBuffer0;
    install(m_encodeSideRefs.vertex_buffers[i], op.com_ptr);
    return;
  }
  if (op.slot >= PendingRefOp::RenderTarget0 && op.slot <= PendingRefOp::RenderTarget3) {
    unsigned i = op.slot - PendingRefOp::RenderTarget0;
    install(m_encodeSideRefs.render_targets[i], op.com_ptr);
    return;
  }
  switch (op.slot) {
  case PendingRefOp::VertexShader:
    install(m_encodeSideRefs.vertex_shader, op.com_ptr);
    return;
  case PendingRefOp::PixelShader:
    install(m_encodeSideRefs.pixel_shader, op.com_ptr);
    return;
  case PendingRefOp::VertexDeclaration:
    install(m_encodeSideRefs.vertex_declaration, op.com_ptr);
    return;
  case PendingRefOp::DepthStencilSurface:
    install(m_encodeSideRefs.depth_stencil_surface, op.com_ptr);
    return;
  case PendingRefOp::IndexBuffer:
    install(m_encodeSideRefs.index_buffer, op.com_ptr);
    return;
  default:
    return;
  }
}

// ===========================================================================
// chunk-emit helpers
// ===========================================================================
// Run on dxmt's encode thread, calling ArgumentEncodingContext primitives
// with pre-captured BatchedDraw state. File-level static to avoid vtable.

namespace {

// Whether two draws can share one render-pass encoder: identical RT/DS
// attachments and views, and the same depth read-only state. The pass bakes
// its depth store action and read-only flags from its FIRST draw
// (StartRenderPassForBatch_d9), so a read-only transition must close the
// encoder. A read-only-depth pass stores DontCare, so a later depth-writing
// draw merged into it would lose its writes at pass end; the reverse would host
// a depth-sampling draw inside a writable-depth pass, an in-pass sample-vs-write
// hazard. DXVK splits the framebuffer on the same transition (it binds a
// distinct read-only DSV per state).
inline bool
RtDsAttachmentsMatch(const MTLD3D9Device::BatchedDraw &a, const MTLD3D9Device::BatchedDraw &b) {
  if (a.resolved_rt_count != b.resolved_rt_count)
    return false;
  if (a.resolved_ds_handle != b.resolved_ds_handle)
    return false;
  for (unsigned i = 0; i < a.resolved_rt_count; ++i) {
    if (a.resolved_rt_handles[i] != b.resolved_rt_handles[i])
      return false;
    if (a.resolved_rt_level[i] != b.resolved_rt_level[i])
      return false;
    if (a.resolved_rt_slice[i] != b.resolved_rt_slice[i])
      return false;
    // View key carries sRGB aliasing chosen by Resolve; SRGBWRITEENABLE
    // bit flips select different views for same handle/level/slice.
    // Encoder must close when view changes (PSO compiled for one format).
    if (a.resolved_rt_view[i] != b.resolved_rt_view[i])
      return false;
  }
  if (a.resolved_ds_handle) {
    if (a.resolved_ds_level != b.resolved_ds_level)
      return false;
    if (a.resolved_ds_slice != b.resolved_ds_slice)
      return false;
    if (a.resolved_ds_view != b.resolved_ds_view)
      return false;
    // Store action and read-only flags are baked per pass; see above.
    if (a.resolved_ds_readonly != b.resolved_ds_readonly)
      return false;
  }
  return true;
}

inline void
StartRenderPassForBatch_d9(ArgumentEncodingContext &ctx, const MTLD3D9Device::BatchedDraw &bd) {
  uint8_t dsv_planar = 0;
  if (bd.resolved_ds_dxmt) {
    dsv_planar = 1 | (bd.resolved_ds_has_stencil ? 2 : 0);
  }
  uint8_t dsv_readonly_flags = bd.resolved_ds_readonly ? dsv_planar : 0;
  auto *info = ctx.startRenderPass(dsv_planar, dsv_readonly_flags, bd.resolved_rt_count, /*argbuf_size=*/0);

  for (unsigned i = 0; i < bd.resolved_rt_count; ++i) {
    auto &color = info->colors[i];
    // resolved_rt_dxmt[i] is the universal predicate now; every
    // surface, including buffer-backed ones, routes through a
    // dxmt::Texture wrapper after the unified-allocation refactor.
    // ctx.access does both fence tracking and Metal handle resolution.
    if (!bd.resolved_rt_dxmt[i])
      continue;
    color.attachment =
        ctx.access<PipelineStage::Pixel>(bd.resolved_rt_dxmt[i], bd.resolved_rt_view[i], ResourceAccess::ReadWrite);
    color.level = bd.resolved_rt_level[i];
    color.slice = bd.resolved_rt_slice[i];
    color.depth_plane = 0;
    // loadAction=Load is the right default; any pending Clear was
    // emitted as a standalone Clear chunk by drainPendingClear, and
    // the coalescer's Clear->Render fold (dxmt_context.cpp)
    // will upgrade this attachment's load_action to Clear and import
    // the Clear encoder's color when the targets match.
    color.load_action = WMTLoadActionLoad;
    color.store_action = WMTStoreActionStore;
  }

  if (bd.resolved_ds_dxmt) {
    // Read-only when the DS is also sampled this draw (see the resolve): Read
    // access keeps the dependency tracker from ordering it as a write, and
    // DontCare leaves the texture genuinely unwritten so the in-pass sample is
    // hazard-free. Device memory keeps the prior depth for later passes.
    auto ds_access = bd.resolved_ds_readonly ? ResourceAccess::Read : ResourceAccess::ReadWrite;
    auto ds_store = bd.resolved_ds_readonly ? WMTStoreActionDontCare : WMTStoreActionStore;
    auto &depth = info->depth;
    depth.attachment = ctx.access<PipelineStage::Pixel>(bd.resolved_ds_dxmt, bd.resolved_ds_view, ds_access);
    depth.level = bd.resolved_ds_level;
    depth.slice = bd.resolved_ds_slice;
    depth.depth_plane = 0;
    // loadAction=Load; pending depth/stencil clears flow through
    // drainPendingClear's standalone Clear chunk and get folded into
    // this attachment by the coalescer (dxmt_context.cpp).
    depth.load_action = WMTLoadActionLoad;
    depth.store_action = ds_store;
    if (bd.resolved_ds_has_stencil) {
      auto &stencil = info->stencil;
      stencil.attachment = ctx.access<PipelineStage::Pixel>(bd.resolved_ds_dxmt, bd.resolved_ds_view, ds_access);
      stencil.level = bd.resolved_ds_level;
      stencil.slice = bd.resolved_ds_slice;
      stencil.depth_plane = 0;
      stencil.load_action = WMTLoadActionLoad;
      stencil.store_action = ds_store;
    }
  }

  info->render_target_width = bd.resolved_rt_width;
  info->render_target_height = bd.resolved_rt_height;
  info->render_target_array_length = 1;
  // Match the PSO's raster_sample_count resolved in ResolveBatchedDrawForChunk.
  // Metal validates equality at setRenderPipelineState; a mismatch
  // hard-errors under MTL_DEBUG_LAYER.
  info->default_raster_sample_count = bd.resolved_raster_sample_count;
}

inline void
EmitCommonRenderSetup_d9(
    ArgumentEncodingContext &ctx, const MTLD3D9Device::BatchedDraw &bd, MTLD3D9Device::ChunkEmitState &s
) {
  // Per-draw POD state lives on bd.pod_snapshot now; Resolve already read the
  // same frozen snapshot pointer above to populate bd.resolved_*, so reading
  // rs here observes the same snapshot.
  const DWORD *rs = bd.pod_snapshot->render_states;

  // Emit setVertex/FragmentBufferOffset when only the offset changed
  // (same buffer handle). Metal's offset-only update is roughly half
  // the cost of a full setBuffer. With all 8 const-buffer slots
  // sharing one ring allocation per draw, the handle changes only on
  // m_constRing block rotation, so the offset-only path catches the
  // steady state.
  auto enc_setbuffer = [&](WMTRenderCommandType ty, obj_handle_t buf, uint64_t off, uint8_t idx) {
    obj_handle_t *handle_shadow = (ty == WMTRenderCommandSetVertexBuffer) ? s.vs_buf_handle : s.fs_buf_handle;
    uint64_t *offset_shadow = (ty == WMTRenderCommandSetVertexBuffer) ? s.vs_buf_offset : s.fs_buf_offset;
    if (handle_shadow[idx] == buf && buf != 0) {
      // (buffer, offset) match; encoder already has the right binding.
      // Post P1b, the 7 const-upload slots hit this every cluster-hit
      // draw because their (buffer, offset) are reused verbatim.
      if (offset_shadow[idx] == off)
        return;
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setbufferoffset>();
      cmd.type = (ty == WMTRenderCommandSetVertexBuffer) ? WMTRenderCommandSetVertexBufferOffset
                                                         : WMTRenderCommandSetFragmentBufferOffset;
      cmd.offset = off;
      cmd.index = idx;
      offset_shadow[idx] = off;
      return;
    }
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setbuffer>();
    cmd.type = ty;
    cmd.buffer = buf;
    cmd.offset = off;
    cmd.index = idx;
    handle_shadow[idx] = buf;
    offset_shadow[idx] = off;
  };

  // useResource hints for each active VS stream (manual-fetch from the
  // vbuf-table reads through these by GPU address).
  for (uint32_t slot = 0; slot < D3D9_MAX_VERTEX_STREAMS; ++slot) {
    obj_handle_t h = bd.resolved_vs_resident_handles[slot];
    if (!h)
      continue;
    // Register a Vertex-stage read on the stream's tracked allocation and
    // retain it for the chunk, alongside the Rc the draw already holds.
    // Nothing writes these allocations on the GPU, so the read has no
    // counterpart to order against, and a refresh renames rather than
    // overwriting a name a recorded draw already froze. Only override (UP)
    // streams
    // carry no tracked allocation and skip this; the per-encoder fence set
    // collapses repeats within an encoder and retainAllocation dedups within
    // the chunk. Runs before the resident dedup so a new encoder after a copy
    // re-establishes the dependency.
    if (auto *vb_alloc = bd.resolved_vb_dxmt[slot].ptr())
      ctx.access<PipelineStage::Vertex>(
          bd.resolved_vb_dxmt[slot], 0, static_cast<unsigned>(vb_alloc->length()), ResourceAccess::Read
      );
    if (s.vs_resident[slot] == h)
      continue;
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_useresource>();
    cmd.type = WMTRenderCommandUseResource;
    cmd.resource = h;
    cmd.usage = WMTResourceUsageRead;
    cmd.stages = WMTRenderStageVertex;
    s.vs_resident[slot] = h;
  }
  // Same Vertex-stage read dependency for the index buffer (either map mode).
  if (auto *ib_alloc = bd.resolved_ib_dxmt.ptr())
    ctx.access<PipelineStage::Vertex>(
        bd.resolved_ib_dxmt, 0, static_cast<unsigned>(ib_alloc->length()), ResourceAccess::Read
    );

  // PSO bind.
  if (s.pso != bd.resolved_pso) {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setpso>();
    cmd.type = WMTRenderCommandSetPSO;
    cmd.pso = bd.resolved_pso;
    s.pso = bd.resolved_pso;
  }

  // VS/PS constant buffers + vbuf table; these always re-bind because
  // m_constRing returns a fresh offset every draw.
  const auto &cu = bd.resolved_const_uploads;
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[0].buffer, cu[0].offset, 0);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[1].buffer, cu[1].offset, 1);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[2].buffer, cu[2].offset, 2);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[6].buffer, cu[6].offset, 3);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[7].buffer, cu[7].offset, 4);
  // Pre-transform viewport remap at VS buffer 5: only the POSITIONT VS variant
  // declares this binding, so only bind it for those draws.
  if (bd.resolved_position_transformed)
    enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[8].buffer, cu[8].offset, 5);
  // Point-size uniform at VS buffer 6: only the injecting point-size VS
  // variant declares this binding, so only bind it for those draws.
  if (bd.resolved_inject_point_size)
    enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[9].buffer, cu[9].offset, 6);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, bd.resolved_vbuf_table_buffer, bd.resolved_vbuf_table_offset, 16);
  enc_setbuffer(WMTRenderCommandSetFragmentBuffer, cu[3].buffer, cu[3].offset, 0);
  enc_setbuffer(WMTRenderCommandSetFragmentBuffer, cu[4].buffer, cu[4].offset, 1);
  enc_setbuffer(WMTRenderCommandSetFragmentBuffer, cu[5].buffer, cu[5].offset, 2);
  if (bd.override_vb_buffer)
    enc_setbuffer(WMTRenderCommandSetVertexBuffer, bd.override_vb_buffer, 0, 29);
  if (bd.override_ib_buffer)
    enc_setbuffer(WMTRenderCommandSetVertexBuffer, bd.override_ib_buffer, 0, 28);

  // Viewport / scissor: emit on change only. Draws within one encoder
  // can carry different viewports/scissors, and re-setting an
  // unchanged one is flagged redundant by the Metal debug layer.
  // Matches the rasterizer / DSSO / blend skips below.
  if (!s.viewport_set || std::memcmp(&s.viewport, &bd.resolved_viewport, sizeof(WMTViewport)) != 0) {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setviewport>();
    cmd.type = WMTRenderCommandSetViewport;
    cmd.viewport = bd.resolved_viewport;
    s.viewport = bd.resolved_viewport;
    s.viewport_set = true;
  }
  if (!s.scissor_set || std::memcmp(&s.scissor, &bd.resolved_scissor, sizeof(WMTScissorRect)) != 0) {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setscissorrect>();
    cmd.type = WMTRenderCommandSetScissorRect;
    cmd.scissor_rect = bd.resolved_scissor;
    s.scissor = bd.resolved_scissor;
    s.scissor_set = true;
  }

  // Rasterizer state.
  {
    auto fm = to_mtl_fill_mode(rs[D3DRS_FILLMODE]);
    auto cm = to_mtl_cull_mode(rs[D3DRS_CULLMODE]);
    uint32_t db_bits = rs[D3DRS_DEPTHBIAS];
    uint32_t ss_bits = rs[D3DRS_SLOPESCALEDEPTHBIAS];
    // Only a pretransformed (RHW) draw with no live depth test escapes near/far
    // clipping: D3D9 depth-clamps and rasterizes it, feeding the raw device z to
    // table fog. test_negative_fixedfunction_fog draws an RHW quad at z=-0.5 with
    // ZENABLE off and expects it fogged by that z, not clipped away. DXVK reaches
    // the same result by zeroing the pretransformed clip-space z only when the
    // depth test is off (inverseExtent.z = IsZTestEnabled ? 1 : 0, d3d9_device.cpp),
    // so an untested TL vertex never clips; both refs otherwise leave depth clip
    // on unconditionally (wined3d desc.depth_clip = TRUE, stateblock.c; DXVK
    // depthClipEnable = true, d3d9_device.cpp). A pretransformed draw WITH a live
    // depth test must still clip, honoring the advertised D3DPMISCCAPS_CLIPTLVERTS:
    // z_range_test (ZENABLE on) clips RHW z outside [0,1] and depth_clamp_test
    // (ZENABLE on) clips RHW z=5/10. Metal has one clip/clamp knob, so this is
    // per-draw encoder state shadowed like fill/cull: a clipping draw after a
    // clamped RHW draw must re-emit Clip, or it would inherit Clamp.
    const bool z_test_live = bd.resolved_ds_handle != 0 && rs[D3DRS_ZENABLE] != D3DZB_FALSE;
    WMTDepthClipMode dcm =
        (bd.resolved_position_transformed && !z_test_live) ? WMTDepthClipModeClamp : WMTDepthClipModeClip;
    if (s.fill_mode != static_cast<int>(fm) || s.cull_mode != static_cast<int>(cm) ||
        s.depth_clip_mode != static_cast<int>(dcm) || s.depth_bias_bits != db_bits || s.slope_scale_bits != ss_bits) {
      float depth_bias;
      float slope_scale;
      std::memcpy(&depth_bias, &db_bits, sizeof(float));
      std::memcpy(&slope_scale, &ss_bits, sizeof(float));
      // D3D9 specifies bias in normalized depth space; Metal applies
      // `depth_bias * r` where r is the DS format's minimum resolvable
      // difference. Multiply by 1/r baked into resolved_depth_bias_scale
      // to restore D3D9 semantics. Slope-scale needs no scaling; both
      // APIs define it as a multiplier of dz/dx.
      depth_bias *= bd.resolved_depth_bias_scale;
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      cmd.type = WMTRenderCommandSetRasterizerState;
      cmd.fill_mode = fm;
      cmd.cull_mode = cm;
      cmd.depth_clip_mode = dcm;
      cmd.winding = WMTWindingClockwise;
      cmd.depth_bias = depth_bias;
      cmd.scole_scale = slope_scale; // sic; typo in winemetal.h
      cmd.depth_bias_clamp = 0.0f;
      s.fill_mode = static_cast<int>(fm);
      s.cull_mode = static_cast<int>(cm);
      s.depth_clip_mode = static_cast<int>(dcm);
      s.depth_bias_bits = db_bits;
      s.slope_scale_bits = ss_bits;
    }
    // D3DRS_MULTISAMPLEMASK is applied through the PS coverage output, not
    // here: Metal has no encoder- or PSO-level sample mask, only a shader-side
    // [[sample_mask]] output. The PS resolve mints a coverage-emitting variant
    // (enable bit in the PS key) that ANDs the mask word (buffer(2) tail, uint32
    // index 29) into hardware coverage, so it never touches the rasterizer
    // state built here. The enable bit gates on a genuinely multisampled RT and
    // a non-all-ones mask; both refs honor the mask the same way (wined3d
    // set_blend_state, DXVK setSampleMask on a maskable MSAA target).
    //
    // D3DRS_MULTISAMPLEANTIALIAS=FALSE on an MSAA RT is likewise ignored.
    // Here both refs honor it (wined3d disables GL_MULTISAMPLE, DXVK forces
    // the rasterizer sample count to 1) but Metal genuinely cannot: a PSO's
    // rasterSampleCount must equal the attachment sample count, so 1-sample
    // rasterization into an N-sample pass is not expressible. A hard Metal
    // limit, not a DXVK parity claim.
  }

  // DSSO + stencil ref.
  if (bd.resolved_dsso && (s.dsso != bd.resolved_dsso || s.stencil_ref != static_cast<int>(bd.resolved_stencil_ref))) {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setdsso>();
    cmd.type = WMTRenderCommandSetDSSO;
    cmd.dsso = bd.resolved_dsso;
    cmd.stencil_ref = bd.resolved_stencil_ref;
    s.dsso = bd.resolved_dsso;
    s.stencil_ref = static_cast<int>(bd.resolved_stencil_ref);
  }

  // Blend color from D3DRS_BLENDFACTOR.
  {
    DWORD bf = rs[D3DRS_BLENDFACTOR];
    if (!s.blend_color_set || s.blend_color_bits != bf) {
      float r = static_cast<float>((bf >> 16) & 0xFF) / 255.0f;
      float g = static_cast<float>((bf >> 8) & 0xFF) / 255.0f;
      float b = static_cast<float>(bf & 0xFF) / 255.0f;
      float a = static_cast<float>((bf >> 24) & 0xFF) / 255.0f;
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setblendcolor_only>();
      cmd.type = WMTRenderCommandSetBlendColor;
      cmd.red = r;
      cmd.green = g;
      cmd.blue = b;
      cmd.alpha = a;
      s.blend_color_bits = bf;
      s.blend_color_set = true;
    }
  }

  // Per-stage textures + samplers. Bind every stage every draw; the
  // accumulated-batch path puts many BatchedDraws into one encoder, and
  // shadow-skipping a stage whose handle didn't change is fine, but
  // shadow-skipping a stage whose handle dropped to null leaves the
  // PREVIOUS draw's texture bound in the encoder. The next draw whose
  // PSO actually samples that stage then reads stale data. Track the
  // bound handle including the null state to catch the unbind transition.
  for (uint32_t stage = 0; stage < 16; ++stage) {
    const auto &rc = bd.resolved_frag_texture_dxmt[stage];
    dxmt::Texture *rc_ptr = rc.ptr();
    obj_handle_t mt;
    if (rc_ptr) {
      // Access retains allocation owning view (survives wrapper Reset via ownership).
      // Re-access on SetLOD / sRGB-toggle / swizzle change.
      uint64_t vkey = bd.resolved_frag_view[stage];
      if (rc_ptr != s.frag_tex_access[stage] || vkey != s.frag_view[stage]) {
        auto &view = ctx.access<PipelineStage::Pixel>(rc, vkey, ResourceAccess::Read);
        s.frag_tex_access[stage] = rc_ptr;
        s.frag_view[stage] = vkey;
        mt = view.texture.handle;
      } else {
        mt = s.frag_tex[stage]; // unchanged since last bind this encoder
      }
    } else {
      // No app texture: immutable device-owned dummy placeholder, bound
      // by raw handle (no fence tracking). Clear the access shadow so a
      // later app-texture rebind at this stage re-accesses; the dummy
      // bind in between moved s.frag_tex off the app handle.
      mt = bd.resolved_frag_textures[stage];
      s.frag_tex_access[stage] = nullptr;
      s.frag_view[stage] = 0;
    }
    if (s.frag_tex[stage] != mt) {
      if (mt) {
        auto &uc = ctx.encodeRenderCommand<wmtcmd_render_useresource>();
        uc.type = WMTRenderCommandUseResource;
        uc.resource = mt;
        uc.usage = WMTResourceUsageRead;
        uc.stages = WMTRenderStageFragment;
      }
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_settexture>();
      cmd.type = WMTRenderCommandSetFragmentTexture;
      cmd.texture = mt;
      cmd.index = static_cast<uint8_t>(stage);
      s.frag_tex[stage] = mt;
    }
    obj_handle_t smp = bd.resolved_frag_samplers[stage];
    if (s.frag_smp[stage] != smp) {
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setsamplerstate>();
      cmd.type = WMTRenderCommandSetFragmentSamplerState;
      cmd.sampler = smp;
      cmd.index = static_cast<uint8_t>(stage);
      s.frag_smp[stage] = smp;
    }
  }

  // Vertex texture fetch (VTF): bind the VS-sampled textures on the vertex
  // stage (D3DVERTEXTEXTURESAMPLER0-3 -> Metal vertex index 0-3). A bound slot
  // goes through ctx.access<Vertex> (read dependency + residency); a slot the
  // VS declared but the app left unbound carries an opaque-black dummy from
  // Resolve, bound by raw handle so the VS never samples a stale texture. A
  // slot the VS does not declare stays 0 and is left untouched (never sampled).
  // VTF draws are rare, so bind directly each draw (no per-slot shadow).
  for (uint32_t vslot = 0; vslot < 4; ++vslot) {
    const auto &rc = bd.resolved_vert_texture_dxmt[vslot];
    obj_handle_t mt;
    if (rc.ptr()) {
      auto &view = ctx.access<PipelineStage::Vertex>(rc, bd.resolved_vert_view[vslot], ResourceAccess::Read);
      mt = view.texture.handle;
    } else {
      // Device-owned dummy for a declared-but-unbound slot, or 0 when the VS
      // does not declare this slot.
      mt = bd.resolved_vert_textures[vslot];
    }
    if (!mt)
      continue;
    auto &uc = ctx.encodeRenderCommand<wmtcmd_render_useresource>();
    uc.type = WMTRenderCommandUseResource;
    uc.resource = mt;
    uc.usage = WMTResourceUsageRead;
    uc.stages = WMTRenderStageVertex;
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_settexture>();
    cmd.type = WMTRenderCommandSetVertexTexture;
    cmd.texture = mt;
    cmd.index = static_cast<uint8_t>(vslot);
    auto &scmd = ctx.encodeRenderCommand<wmtcmd_render_setsamplerstate>();
    scmd.type = WMTRenderCommandSetVertexSamplerState;
    scmd.sampler = bd.resolved_vert_samplers[vslot];
    scmd.index = static_cast<uint8_t>(vslot);
  }
}

inline void
EmitDrawCommand_d9(ArgumentEncodingContext &ctx, const MTLD3D9Device::BatchedDraw &bd) {
  // "Instancing is ignored for non-indexed draws" is native (MSDN, wined3d
  // device.c and DXVK d3d9_device.cpp all yield instance_count = 1 for
  // non-indexed). Gating on the D3DSTREAMSOURCE_INDEXEDDATA flag below is dxmt's
  // own tightening: wined3d and DXVK apply stream-0's frequency on every indexed
  // draw without checking that flag, so a bare SetStreamSourceFreq(0, N) with no
  // flag draws N instances there but 1 here. Real apps set the flag when
  // instancing, so the two agree in practice.
  uint32_t instance_count = 1;
  if (bd.type == MTLD3D9Device::BatchedDraw::kIndexed) {
    UINT s0_freq = bd.pod_snapshot->stream_freq[0];
    if (s0_freq & D3DSTREAMSOURCE_INDEXEDDATA)
      instance_count = std::max(s0_freq & 0x007FFFFFu, 1u);
  }

  if (bd.type == MTLD3D9Device::BatchedDraw::kIndexed) {
    uint32_t index_size = (bd.resolved_ib_fmt == static_cast<uint32_t>(DXSO_INDEX_BUFFER_FORMAT_UINT32)) ? 4u : 2u;
    WMTIndexType index_type = (bd.resolved_ib_fmt == static_cast<uint32_t>(DXSO_INDEX_BUFFER_FORMAT_UINT32))
                                  ? WMTIndexTypeUInt32
                                  : WMTIndexTypeUInt16;
    obj_handle_t ib_handle = bd.resolved_ib_handle;
    uint64_t ib_base = bd.resolved_ib_base_offset;
    uint64_t index_offset = ib_base + static_cast<uint64_t>(bd.start_vertex_or_index) * index_size;
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_draw_indexed>();
    cmd.type = WMTRenderCommandDrawIndexed;
    cmd.primitive_type = to_mtl_prim_type(bd.primitive_type);
    cmd.index_type = index_type;
    cmd.index_count = bd.vertex_or_index_count;
    cmd.index_buffer = ib_handle;
    cmd.index_buffer_offset = index_offset;
    cmd.instance_count = instance_count;
    cmd.base_vertex = bd.base_vertex;
    cmd.base_instance = 0;
  } else {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_draw>();
    cmd.type = WMTRenderCommandDraw;
    cmd.primitive_type = to_mtl_prim_type(bd.primitive_type);
    cmd.vertex_start = bd.start_vertex_or_index;
    cmd.vertex_count = bd.vertex_or_index_count;
    cmd.instance_count = instance_count;
    cmd.base_instance = 0;
  }
}

inline void
EmitBlitOp_d9(ArgumentEncodingContext &ctx, MTLD3D9Device::PendingBlitOp &op) {
  // Register src/dst access for cross-encoder dependency tracking.
  // Without them, same-RT Render-merge folds across blit, executing blit before renders.
  auto src_tex = ctx.access<PipelineStage::Compute>(op.src_tex, op.src_mip, op.src_slice, ResourceAccess::Read);
  auto dst_tex = ctx.access<PipelineStage::Compute>(op.dst_tex, op.dst_mip, op.dst_slice, ResourceAccess::Write);
  auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
  cmd.type = WMTBlitCommandCopyFromTextureToTexture;
  cmd.src = src_tex.handle;
  cmd.src_slice = op.src_slice;
  cmd.src_level = op.src_mip;
  cmd.src_origin = op.src_origin;
  cmd.src_size = op.size;
  cmd.dst = dst_tex.handle;
  cmd.dst_slice = op.dst_slice;
  cmd.dst_level = op.dst_mip;
  cmd.dst_origin = op.dst_origin;
}

// Copy a mirror-backed texture level's staged bytes from an upload-ring block
// into its Private texture. The access<Compute> write registers the destination
// subresource so the encoder scheduler keeps this upload ahead of any later
// pass that samples the texture; without it the blit-merge step floats the
// upload past a same-chunk render pass that reads the level (stale read).
inline void
EmitBufferToTextureOp_d9(ArgumentEncodingContext &ctx, MTLD3D9Device::PendingBlitOp &op) {
  auto dst_tex = ctx.access<PipelineStage::Compute>(op.dst_tex, op.dst_mip, op.dst_slice, ResourceAccess::Write);
  auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
  cmd.type = WMTBlitCommandCopyFromBufferToTexture;
  cmd.src = op.buf_src_handle;
  cmd.src_offset = op.buf_src_offset;
  cmd.bytes_per_row = op.tex_src_pitch;
  cmd.bytes_per_image = op.tex_bytes_per_image;
  cmd.size = op.size;
  cmd.dst = dst_tex.handle;
  cmd.slice = op.dst_slice;
  cmd.level = op.dst_mip;
  cmd.origin = op.dst_origin;
}

// AUTOGENMIPMAP down-filter for a whole texture. Registers level 0 read and
// levels 1..N write per slice so the encoder scheduler keeps this pass ahead
// of a later one that samples the freshly-generated chain (stale sample), the
// same hazard the upload and StretchRect paths guard. The per-subresource
// access handle resolves to the shared allocation handle, which is what the
// whole-texture generate command wants.
inline void
EmitGenerateMipmapsOp_d9(ArgumentEncodingContext &ctx, MTLD3D9Device::PendingBlitOp &op) {
  const uint32_t mip_count = op.dst_tex->miplevelCount();
  const uint32_t slices = op.dst_tex->arrayLength();
  obj_handle_t tex_handle = 0;
  for (uint32_t s = 0; s < slices; ++s) {
    tex_handle = ctx.access<PipelineStage::Compute>(op.dst_tex, 0, s, ResourceAccess::Read).handle;
    for (uint32_t l = 1; l < mip_count; ++l)
      ctx.access<PipelineStage::Compute>(op.dst_tex, l, s, ResourceAccess::Write);
  }
  auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_generate_mipmaps>();
  cmd.type = WMTBlitCommandGenerateMipmaps;
  cmd.texture = tex_handle;
}

// Render-pass StretchRect for different extents/format aliases.
// ctx.stretchBlit opens its own encoder; TextureViewKey selects mip level.
inline void
EmitStretchBlitOp_d9(StretchBlitContext &stretch_cmd, MTLD3D9Device::PendingBlitOp &op) {
  // The source stays 2D: fs_blit_quad samples it, and Metal has no sampler for
  // a multisampled texture, so the planner never routes a multisampled source
  // here (it resolves first).
  TextureViewKey src_view = op.src_tex->createView({
      .format = op.src_tex->pixelFormat(),
      .type = WMTTextureType2D,
      .firstMiplevel = static_cast<uint32_t>(op.src_mip),
      .miplevelCount = 1,
      .firstArraySlice = static_cast<uint32_t>(op.src_slice),
      .arraySize = 1,
      // Per-format channel fixup for a converting stretch; identity for
      // same-shape sources. Set on the calling thread from D3DFormatSamplerSwizzle.
      .swizzle = op.src_swizzle,
  });
  // The destination is the one side whose sample count varies: the single-sample
  // to multisample broadcast renders into a multisampled target, and
  // Metal rejects a plain 2D view over one.
  TextureViewKey dst_view = op.dst_tex->createView({
      .format = op.dst_tex->pixelFormat(),
      .type = surface_view_type(op.dst_tex.ptr()),
      .firstMiplevel = static_cast<uint32_t>(op.dst_mip),
      .miplevelCount = 1,
      .firstArraySlice = static_cast<uint32_t>(op.dst_slice),
      .arraySize = 1,
  });
  auto filter = (op.filter == D3DTEXF_LINEAR) ? StretchBlitContext::Filter::Linear : StretchBlitContext::Filter::Point;
  stretch_cmd.blit(
      op.src_tex, src_view, op.dst_tex, dst_view, filter, op.src_origin, op.size, op.dst_origin, op.dst_size
  );
}

// MSAA-resolve via StretchRect: src is multisampled, dst is single-
// sampled. Routes through ResolveTextureContext, which builds a per-format
// PSO (keyed on the dst format) that averages the samples in the fragment
// shader, so a same-extent format-converting resolve is handled here; a
// scaled or fixup-source resolve is split into a resolve-then-stretch on the
// calling thread (StretchRectKind::ResolveThenStretch) and never reaches this
// path with mismatched extents. DXMTResolveMetadata src_origin + size give the
// scissor. The encoder opens its own render pass; like Stretch, the walker
// must end any open pass first.
inline void
EmitResolveBlitOp_d9(ResolveTextureContext &resolve_cmd, MTLD3D9Device::PendingBlitOp &op) {
  // Both sides are fixed here by what the resolve does rather than by the
  // textures: Resolve is only planned for a multisampled source, and it renders
  // the averaged samples into a single-sample destination.
  TextureViewKey src_view = op.src_tex->createView({
      .format = op.src_tex->pixelFormat(),
      .type = WMTTextureType2DMultisample,
      .firstMiplevel = static_cast<uint32_t>(op.src_mip),
      .miplevelCount = 1,
      .firstArraySlice = static_cast<uint32_t>(op.src_slice),
      .arraySize = 1,
  });
  TextureViewKey dst_view = op.dst_tex->createView({
      .format = op.dst_tex->pixelFormat(),
      .type = WMTTextureType2D,
      .firstMiplevel = static_cast<uint32_t>(op.dst_mip),
      .miplevelCount = 1,
      .firstArraySlice = static_cast<uint32_t>(op.dst_slice),
      .arraySize = 1,
  });
  WMTScissorRect src_rect = {
      op.src_origin.x,
      op.src_origin.y,
      op.size.width,
      op.size.height,
  };
  if (op.kind == MTLD3D9Device::PendingBlitOp::Kind::DepthResolve) {
    resolve_cmd.resolveDepth(op.src_tex, src_view, op.dst_tex, dst_view, src_rect, op.dst_origin, op.dst_size);
    return;
  }
  resolve_cmd.resolve(
      op.src_tex, src_view, op.dst_tex, dst_view, ResolveTextureMode::Average, src_rect, op.dst_origin, op.dst_size
  );
}

// Per-format sample-view derivation shared by the fragment sample-bind and the
// vs_3_0 vertex-texture (VTF) bind. Base Metal textures are always identity-
// swizzle (winemetal_unix.c fill_texture_descriptor), so a fixup-needing format
// (luminance L,L,L,1 / X-alpha=1 / A4R4G4B4 permute / depth RRRR / 2-channel
// .b=1) samples the wrong channel shape unless the view carries the
// D3DFormatSamplerSwizzle. This also applies the optional D3DSAMP_SRGBTEXTURE
// format alias (skipping D3DFMT_A8L8, whose RG8Unorm_sRGB sibling would decode
// the alpha lane) and the SetLOD mip clamp. Both stages must resolve the same
// view or a format samples differently in a VS than in a PS (DXVK binds one
// swizzled image view in every stage). Views are cached per allocation, so the
// steady-state cost is a key compare.
static uint64_t
deriveSampleView(const Rc<dxmt::Texture> &rc, MTLD3D9CommonTexture *tex, const DWORD *samp_row) {
  uint64_t view = rc->fullView;
  // {Zero,Zero,Zero,Zero} is D3DFormatSamplerSwizzle's "no override" sentinel;
  // translate to identity so it dedups to fullView's default identity swizzle.
  WMTTextureSwizzleChannels sw = D3DFormatSamplerSwizzle(tex->d3dFormat());
  if (sw.r == WMTTextureSwizzleZero && sw.g == WMTTextureSwizzleZero && sw.b == WMTTextureSwizzleZero &&
      sw.a == WMTTextureSwizzleZero)
    sw = {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha};
  view = rc->checkViewUseSwizzle(view, sw);
  // Only the low bit of D3DSAMP_SRGBTEXTURE selects sRGB decode (see
  // d3d9_srgb_texture_enabled: the Might & Magic Heroes VI garbage-value rule
  // wined3d and DXVK both mask). A plain truthiness test would turn sRGB on and
  // wash the textures out.
  if (d3d9_srgb_texture_enabled(samp_row[D3DSAMP_SRGBTEXTURE]) && !D3D9FormatSuppressSRGBRead(tex->d3dFormat())) {
    auto base_fmt = tex->metalPixelFormat();
    auto srgb_fmt = Recall_sRGB(base_fmt);
    if (srgb_fmt != base_fmt)
      view = rc->checkViewUseFormat(view, srgb_fmt);
  }
  // D3D9 SetLOD(N) clamps sampling to mips N..(level_count-1). lod 0 (apps that
  // never call SetLOD) leaves the full mip range.
  uint32_t lod = tex->commonTextureLod();
  if (lod > 0) {
    uint32_t total = rc->miplevelCount();
    if (lod < total)
      view = rc->checkViewUseMipRange(view, lod, total - lod);
  }
  return view;
}

// Pass-kind discriminant used by the op-stream walker below to track
// the currently-open encoder (Render vs Blit vs None). Lifted out of
// the inline walker so the walker body (which lives inside the
// chunk->emitcc lambda in FlushDrawBatch) can stay flat.
enum class D9PassKind { None, Render, Blit };

// Advances the device's cached completion watermark to the batch this target
// was registered on. Monotonic because a chunk may retire out of order with
// respect to the sequence under encoder coalescing, and the watermark is read
// to decide whether ring storage is reusable: moving it backwards would hand
// back memory the GPU still reads. Runs on the finish thread, so it touches
// nothing but the atomic.
class D9BatchWatermark final : public GpuCompletionTarget {
public:
  D9BatchWatermark(std::atomic<uint64_t> &watermark, uint64_t batch_seq) :
      watermark_(watermark),
      batch_seq_(batch_seq) {}

  void
  CompleteGpuWork(GpuCompletionStatus) noexcept final {
    uint64_t prev = watermark_.load(std::memory_order_relaxed);
    while (prev < batch_seq_ && !watermark_.compare_exchange_weak(prev, batch_seq_, std::memory_order_relaxed))
      ;
  }

private:
  std::atomic<uint64_t> &watermark_;
  uint64_t batch_seq_;
};

} // namespace

bool
MTLD3D9Device::ResolveBatchedDrawForChunk(
    BatchedDraw &bd, uint64_t chunk_seq, uint64_t chunk_coherent_id, ConstUploadCache &const_cache,
    ResolveCache &resolve_cache
) {
  // No per-draw autorelease pool here; the calling chunk->emitcc lambda
  // hoists one pool to span every draw resolve + EmitDrawBatch_d9_chunk,
  // since per-draw pool churn is pure WoW64 round-trip overhead. The
  // chunk-level pool drains at chunk end, so the autoreleased transient
  // working set is bounded by the chunk's draw count.
  auto &cap = bd.cap;

  // Encode-side ref-state mirror. Mutated by SetRef ops on the same op
  // stream this Draw was queued on; the walker applies the preceding
  // SetRef ops before this Resolve call, so refs reflects the calling-
  // thread state as of the moment this draw was queued; same temporal
  // ordering wined3d's CS thread / d3d11 EmitOP achieve without a per-
  // draw COW snapshot.
  const D9EncodingRefs &refs = m_encodeSideRefs;
  auto *vs = refs.vertex_shader.ptr();
  auto *ps = refs.pixel_shader.ptr();
  auto *decl = refs.vertex_declaration.ptr();
  auto *rt0 = refs.render_targets[0].ptr();
  // Null vs / ps mean a fixed-function stage: the generated shader pair
  // covers them. Declaration and a color target stay required. A
  // pre-transformed declaration additionally bypasses the bound vertex
  // shader entirely (wined3d use_vs gates on position_transformed), so
  // every POSITIONT draw takes the generated path whatever is bound.
  if (!decl || !rt0)
    return false;
  bool decl_pretransformed = false;
  if (decl && vs) {
    for (UINT i = 0; i < decl->elementCount(); ++i) {
      const D3DVERTEXELEMENT9 &e = decl->elements()[i];
      if (e.Stream != 0xFF && e.Usage == D3DDECLUSAGE_POSITIONT) {
        decl_pretransformed = true;
        break;
      }
    }
  }
  // A bound vertex shader that references the extended constant file (c256..)
  // cannot run in hardware vertex processing. The caller-side gate rejects the
  // first such draw; every draw after falls back to fixed-function vertex
  // processing (native renders the vertex data's own colour), which the
  // per-draw captured software-VP mode drives here. Never fires on a pure
  // hardware-VP device (no shader can reference c256..) so the common path is
  // byte-identical.
  const bool vs_needs_unavailable_swvp = vs != nullptr && bd.pod_snapshot->sw_vp_capable && !bd.pod_snapshot->is_swvp &&
                                         vs->metadata().max_float_const_index > D3D9_MAX_VS_CONST_F;
  const bool ffp_vs = vs == nullptr || decl_pretransformed || vs_needs_unavailable_swvp;
  const bool ffp_ps = ps == nullptr;
  // Component width of each texcoord set in the declaration; the
  // texture-matrix preprocessing consumes it at upload.
  uint32_t ffp_texcoord_width[8] = {2, 2, 2, 2, 2, 2, 2, 2};

  // POD state lives on the per-draw pod_snapshot so POD
  // setters never need to FlushDrawBatch. Every call below reads
  // through `*bd.pod_snapshot`; guaranteed non-null because
  // QueueBatchedDraw populates it before push_back.
  const dxmt::D9EncodingState &pod = *bd.pod_snapshot;
  const DWORD *rs = pod.render_states;
  const DWORD(*samp_states)[D3DSAMP_DMAPOFFSET + 1] = pod.sampler_states;
  const UINT *stream_freq = pod.stream_freq;

  // Cluster cache: pod/ref pointer-equality implies byte-equality.
  // ~80% hit rate; per-hit saves PSO lookup, 16 per-stage sampler/view operations, compiles.
  bool up_vb = bd.override_vb_buffer != 0;
  bool up_ib = bd.override_ib_buffer != 0;
  bool indexed = (bd.type == BatchedDraw::kIndexed);
  bool cluster_hit = resolve_cache.pod_ptr == bd.pod_snapshot && resolve_cache.pod_ptr != nullptr &&
                     resolve_cache.ref_gen == m_encodeSideRefsGen && resolve_cache.ref_gen != 0 &&
                     resolve_cache.up_vb == up_vb && resolve_cache.up_ib == up_ib &&
                     resolve_cache.up_ib_format == bd.override_ib_format &&
                     resolve_cache.primitive_type == bd.primitive_type && resolve_cache.draw_type == bd.type;

  if (cluster_hit) {
    // Cluster-stable resolved fields; copy from cache.
    bd.resolved_pso = resolve_cache.resolved_pso;
    bd.resolved_pso_task = resolve_cache.resolved_pso_task;
    bd.resolved_pso_first_use = false;
    bd.resolved_dsso = resolve_cache.resolved_dsso;
    bd.resolved_stencil_ref = resolve_cache.resolved_stencil_ref;
    bd.resolved_slot_mask = resolve_cache.resolved_slot_mask;
    bd.resolved_ib_fmt = resolve_cache.resolved_ib_fmt;
    bd.resolved_raster_sample_count = resolve_cache.resolved_raster_sample_count;
    bd.resolved_depth_bias_scale = resolve_cache.resolved_depth_bias_scale;
    bd.resolved_ds_has_stencil = resolve_cache.resolved_ds_has_stencil;
    bd.resolved_rt_count = resolve_cache.resolved_rt_count;
    bd.resolved_rt_width = resolve_cache.resolved_rt_width;
    bd.resolved_rt_height = resolve_cache.resolved_rt_height;
    bd.resolved_ds_handle = resolve_cache.resolved_ds_handle;
    bd.resolved_ds_view = resolve_cache.resolved_ds_view;
    bd.resolved_ds_level = resolve_cache.resolved_ds_level;
    bd.resolved_ds_slice = resolve_cache.resolved_ds_slice;
    bd.resolved_viewport = resolve_cache.resolved_viewport;
    bd.resolved_position_transformed = resolve_cache.resolved_position_transformed;
    bd.resolved_inject_point_size = resolve_cache.resolved_inject_point_size;
    std::memcpy(ffp_texcoord_width, resolve_cache.ffp_texcoord_width, sizeof(ffp_texcoord_width));
    bd.resolved_scissor = resolve_cache.resolved_scissor;
    std::memcpy(bd.resolved_rt_handles, resolve_cache.resolved_rt_handles, sizeof(bd.resolved_rt_handles));
    std::memcpy(bd.resolved_rt_view, resolve_cache.resolved_rt_view, sizeof(bd.resolved_rt_view));
    std::memcpy(bd.resolved_rt_level, resolve_cache.resolved_rt_level, sizeof(bd.resolved_rt_level));
    std::memcpy(bd.resolved_rt_slice, resolve_cache.resolved_rt_slice, sizeof(bd.resolved_rt_slice));
    std::memcpy(bd.resolved_frag_textures, resolve_cache.resolved_frag_textures, sizeof(bd.resolved_frag_textures));
    std::memcpy(bd.resolved_frag_view, resolve_cache.resolved_frag_view, sizeof(bd.resolved_frag_view));
    std::memcpy(bd.resolved_frag_samplers, resolve_cache.resolved_frag_samplers, sizeof(bd.resolved_frag_samplers));
    for (uint32_t i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i)
      bd.resolved_rt_dxmt[i] = resolve_cache.resolved_rt_dxmt[i];
    bd.resolved_ds_dxmt = resolve_cache.resolved_ds_dxmt;
    for (uint32_t i = 0; i < 16; ++i)
      bd.resolved_frag_texture_dxmt[i] = resolve_cache.resolved_frag_texture_dxmt[i];
  } else {
    // Pre-convert viewport / scissor to Metal shape so per-draw emit does not
    // re-run the helpers.
    bd.resolved_viewport = wmt_viewport_from_d3d9(pod.viewport);
    bd.resolved_scissor = wmt_scissor_from_d3d9(pod.scissor_rect, pod.viewport, rs[D3DRS_SCISSORTESTENABLE] != 0);

    // ---- IA layout ----
    // D3D9 caps vertex declarations at MAX_FVF_DECL_SIZE = 64 elements
    // (D3DDECL_END terminator brings the typical cap to ~16 active);
    // a stack-resident array avoids the per-draw std::vector heap alloc
    // entirely. decl->elementCount() includes the terminator, so the
    // bound here is a generous 64.
    DXSO_IA_INPUT_ELEMENT elements[64];
    uint32_t element_count = 0;
    uint32_t slot_mask = 0;
    bool decl_position_transformed = false;
    bool ffp_has_diffuse = false;
    bool ffp_has_texcoord0 = false;
    bool ffp_has_specular = false;
    bool ffp_has_normal = false;
    bool ffp_has_psize = false;
    uint32_t ffp_texcoord_mask = 0;
    bool ffp_decl_has_diffuse = false;
    bool ffp_decl_has_specular = false;
    for (UINT i = 0; i < decl->elementCount(); ++i) {
      const D3DVERTEXELEMENT9 &e = decl->elements()[i];
      if (e.Stream == 0xFF)
        continue;
      // Filter elements past the 16-stream cap wholesale, before any per-stream
      // read (wined3d vertexdeclaration.c, "filter tessellation pseudo streams"):
      // such a stream has no vertex_buffers[]/stream_freq[] slot, so it must not
      // reach the material-source bookkeeping below, and 1u << Stream would be an
      // out-of-range shift. CreateVertexDeclaration accepts the decl (both refs
      // do); the draw drops the element. should_skip_ia_element encodes the same
      // past-cap rule, but the guard here also protects the reads that precede it.
      if (e.Stream >= D3D9_MAX_VERTEX_STREAMS)
        continue;
      // Declaration presence, independent of stream liveness: the material
      // source validation below keys on whether the declaration carries the
      // color at all (wined3d validate_material_colour_source); an element
      // declared on an unbound stream keeps its selector and zero-fills
      // through the fetch instead.
      if (ffp_vs && e.Usage == D3DDECLUSAGE_COLOR) {
        if (e.UsageIndex == 0)
          ffp_decl_has_diffuse = true;
        else if (e.UsageIndex == 1)
          ffp_decl_has_specular = true;
      }
      // An element on a stream with no bound vertex buffer drops out of
      // the layout instead of failing the draw: wined3d derives stream
      // liveness per draw (context.c wined3d_stream_info_from_declaration)
      // and still renders, with the unfed shader input reading its
      // zero-fill default. A declaration-only stream reference is common
      // in runner-style harnesses that always declare a position element.
      // Stream is in-cap here (guarded above), so the per-stream read is safe.
      const bool has_live_stream =
          refs.vertex_buffers[e.Stream].ptr() != nullptr || (e.Stream == 0 && bd.override_vb_buffer != 0);
      if (should_skip_ia_element(e.Stream, has_live_stream)) {
        // A PSIZE declared on an unfed stream still sets the point size: D3D9
        // reads the missing per-vertex size as 1 (AMD / WARP), not the render
        // state. Mark the per-vertex path even though the element drops out of
        // the layout; with no element backing it the generated VS emits size 1.
        if (e.Usage == D3DDECLUSAGE_PSIZE)
          ffp_has_psize = true;
        continue;
      }
      // A pre-transformed position element (D3DDECLUSAGE_POSITIONT) carries
      // window-space coordinates. D3D9 routes such a draw through the fixed-
      // function pre-transform and ignores the bound vertex shader (per spec),
      // which is why ffp_vs was forced true above for a pre-transformed decl.
      // The generated FFP vertex shader consumes the POSITIONT element as its
      // position input (matched to reg 0 below) and applies the screen->clip
      // remap from the viewport uniform (vp_remap).
      uint32_t match_usage = e.Usage;
      if (e.Usage == D3DDECLUSAGE_POSITIONT)
        match_usage = D3DDECLUSAGE_POSITION;
      int vs_reg = -1;
      if (ffp_vs) {
        // Generated-VS register contract (ffp_input_register, airconv_public.h):
        // the injective (usage, index) -> input register map. Derive the material-
        // source flags and texcoord widths off the resolved register; the map is
        // injective, so each register unambiguously identifies its semantic.
        vs_reg = ffp_input_register(match_usage, e.UsageIndex);
        switch (vs_reg) {
        case 1:
          ffp_has_diffuse = true;
          break;
        case 3:
          ffp_has_specular = true;
          break;
        case 4:
          ffp_has_normal = true;
          break;
        case 13:
          ffp_has_psize = true;
          break;
        case 2:
          ffp_has_texcoord0 = true;
          ffp_texcoord_mask |= 1u;
          ffp_texcoord_width[0] = texcoord_component_count(e.Type);
          break;
        default:
          // Registers 5..11 are texcoord sets 1..7 (reg = 4 + UsageIndex).
          if (vs_reg >= 5 && vs_reg <= 11) {
            const uint32_t tex_set = static_cast<uint32_t>(vs_reg) - 4u;
            ffp_texcoord_mask |= 1u << tex_set;
            ffp_texcoord_width[tex_set] = texcoord_component_count(e.Type);
          }
          break;
        }
      } else
        for (const auto &d : vs->metadata().dcls) {
          if (d.bound_to.type == DxsoRegisterType::Input && static_cast<uint32_t>(d.dcl.usage) == match_usage &&
              d.dcl.usage_index == e.UsageIndex) {
            vs_reg = static_cast<int>(d.bound_to.num);
            break;
          }
        }
      if (vs_reg < 0)
        continue;
      // Flag the draw only once the POSITIONT element actually feeds a VS input,
      // so a declared-but-unconsumed position doesn't force the transformed variant.
      if (e.Usage == D3DDECLUSAGE_POSITIONT)
        decl_position_transformed = true;
      if (element_count >= 64)
        break;
      DXSO_IA_INPUT_ELEMENT &elem = elements[element_count++];
      elem = DXSO_IA_INPUT_ELEMENT{};
      elem.reg = static_cast<uint32_t>(vs_reg);
      elem.slot = e.Stream;
      elem.aligned_byte_offset = e.Offset;
      elem.format = to_mtl_attr_format(e.Type);
      UINT freq = stream_freq[e.Stream];
      if (freq & D3DSTREAMSOURCE_INSTANCEDATA) {
        elem.step_function = 1;
        // INSTANCEDATA | 0 is a legal API input (matches native D3D9);
        // Metal cannot encode a per-instance stepRate of 0, so clamp the
        // divider to >= 1 the same way instance_count is clamped above.
        elem.step_rate = std::max(freq & 0x007FFFFFu, 1u);
      } else {
        elem.step_function = 0;
        elem.step_rate = 0;
      }
      slot_mask |= (1u << e.Stream);
    }
    // element_count of zero is a legal draw: a constant-output VS with a
    // declaration whose only elements sit on unbound streams (filtered
    // above) fetches nothing and every dcl'd input zero-fills.
    bd.resolved_slot_mask = slot_mask;

    DXSO_INDEX_BUFFER_FORMAT ib_fmt = DXSO_INDEX_BUFFER_FORMAT_NONE;
    if (indexed) {
      D3DFORMAT d3d_ib_format;
      if (bd.override_ib_buffer != 0) {
        d3d_ib_format = bd.override_ib_format;
      } else {
        // cap.ib_format / cap.ib_buffer were frozen at BuildDrawCapture
        // time so Lock(DISCARD) between queue and execute can't move the
        // index data out from under this draw.
        if (cap.ib_buffer == 0)
          return false;
        d3d_ib_format = cap.ib_format;
      }
      ib_fmt = (d3d_ib_format == D3DFMT_INDEX32) ? DXSO_INDEX_BUFFER_FORMAT_UINT32 : DXSO_INDEX_BUFFER_FORMAT_UINT16;
    }
    bd.resolved_ib_fmt = static_cast<uint32_t>(ib_fmt);

    DXSO_SHADER_IA_INPUT_LAYOUT_DATA layout{};
    layout.slot_mask = slot_mask;
    layout.num_elements = element_count;
    layout.elements = elements;
    layout.index_buffer_format = ib_fmt;
    layout.position_transformed = decl_position_transformed ? 1u : 0u;
    bd.resolved_position_transformed = decl_position_transformed;

    // D3DRS_POINTSIZE auto-injection: the injecting VS variant emits
    // [[point_size]] for a point-list draw and reads the size + clamp
    // bounds from a per-draw uniform (VS buffer 6, filled below), so
    // distinct sizes share one MTLFunction instead of minting a cold PSO
    // link per value. A VS that writes its own oPts always injects (the
    // epilogue clamps its output against the uniform); one that doesn't
    // injects only when the clamped render-state size leaves the 1.0
    // default, keeping ordinary point draws on the base variant. Mirrors
    // DXVK src/d3d9/d3d9_fixed_function.cpp GetPointSizeInfoVS + the
    // dxso_compiler.cpp emitPsize clamp. The decision is invariant under
    // the numeric size beyond that one default test (see d3d9_point_size.hpp).
    bool vs_inject_point_size = inject_point_size(
        bd.primitive_type == D3DPT_POINTLIST, !ffp_vs && vs->metadata().writes_point_size, rs[D3DRS_POINTSIZE],
        rs[D3DRS_POINTSIZE_MIN], rs[D3DRS_POINTSIZE_MAX]
    );
    bd.resolved_inject_point_size = vs_inject_point_size;
    // Fixed-function vertex fog: active when fog is enabled and table
    // fog is off (table fog computes per fragment and takes priority).
    // The D3DFOG_* value keys the generated VS directly.
    uint32_t ffp_vs_fog_mode = 0;
    if (ffp_vs && rs[D3DRS_FOGENABLE] != FALSE && rs[D3DRS_FOGTABLEMODE] == D3DFOG_NONE) {
      ffp_vs_fog_mode = rs[D3DRS_FOGVERTEXMODE] <= D3DFOG_LINEAR ? rs[D3DRS_FOGVERTEXMODE] : 0;
      // With no vertex-fog formula and no table fog, the D3D9 fog factor is the
      // vertex specular alpha. It must interpolate smoothly even under FLAT
      // shading (the specular color flat-shades, but the fog factor does not),
      // so route it through the smooth oFog varying (a distinct mode 4) rather
      // than let the pixel stage sample the flat COLOR1 alpha. DXVK emits
      // specular.w to oFog the same way (DoFixedFunctionFog, D3DFOG_NONE).
      // Pretransformed draws keep the pixel-stage specular-alpha path.
      if (ffp_vs_fog_mode == 0 && ffp_has_specular && !bd.resolved_position_transformed)
        ffp_vs_fog_mode = 4;
    }
    // D3DRS_RANGEFOGENABLE switches vertex fog from planar (view-space z) to
    // radial (true eye-space distance), so objects at the screen edge fog by
    // distance instead of depth and stop swimming on camera rotation. One key
    // bit onto the fog axis (both refs implement it: wined3d
    // WINED3D_FFP_VS_FOG_RANGE = length(ec_pos.xyz), DXVK RangeFog VS key).
    // Range fog only affects vertex fog, never table fog; programmable-VS
    // draws are unaffected (their fog rides oFog). Without this the advertised
    // D3DPRASTERCAPS_FOGRANGE cap is a lie.
    bool ffp_vs_range_fog = ffp_vs_fog_mode >= 1u && ffp_vs_fog_mode <= 3u && rs[D3DRS_RANGEFOGENABLE] != FALSE;
    // The clamped point size joins the generated VS's key for point
    // lists (the same injected-constant policy the bytecode variants
    // use; distance attenuation stays a marked gap).
    // Lighting key: enabled + normal presence + the specular/normalize/
    // local-viewer/color-vertex render states + the four material source
    // selectors (values 0..2 per D3DMCS_*).
    uint32_t ffp_lighting_key = 0;
    // A pre-transformed (XYZRHW) draw is never lit: its position is already in
    // clip space, so there is no world/view to light in. Native and both
    // references bypass lighting for transformed vertices (wined3d's
    // transformed vertex pipe emits no lighting, DXVK gates lighting on
    // !VertexHasPositionT); without this an XYZRHW draw left at the default
    // LIGHTING=TRUE replaces its vertex color with a zero light accumulation
    // and renders black. Same carve-out the table-fog selection already makes.
    if (ffp_vs && rs[D3DRS_LIGHTING] != FALSE && !bd.resolved_position_transformed) {
      auto src_sel = [&](DWORD v) -> uint32_t { return v <= 2 ? v : 0; };
      ffp_lighting_key = 1u | (rs[D3DRS_SPECULARENABLE] != FALSE ? 4u : 0u) |
                         (rs[D3DRS_NORMALIZENORMALS] != FALSE ? 8u : 0u) | (rs[D3DRS_LOCALVIEWER] != FALSE ? 16u : 0u) |
                         (rs[D3DRS_COLORVERTEX] != FALSE ? 32u : 0u);
      const bool cv = rs[D3DRS_COLORVERTEX] != FALSE;
      uint32_t sd = cv ? src_sel(rs[D3DRS_DIFFUSEMATERIALSOURCE]) : 0;
      uint32_t ss = cv ? src_sel(rs[D3DRS_SPECULARMATERIALSOURCE]) : 0;
      uint32_t sa = cv ? src_sel(rs[D3DRS_AMBIENTMATERIALSOURCE]) : 0;
      uint32_t se = cv ? src_sel(rs[D3DRS_EMISSIVEMATERIALSOURCE]) : 0;
      // A source pointing at a color the declaration does not carry falls
      // back to the material (wined3d validate_material_colour_source).
      if (!ffp_decl_has_diffuse) {
        if (sd == 1)
          sd = 0;
        if (sa == 1)
          sa = 0;
        if (se == 1)
          se = 0;
        if (ss == 1)
          ss = 0;
      }
      if (!ffp_decl_has_specular) {
        if (sd == 2)
          sd = 0;
        if (ss == 2)
          ss = 0;
        if (sa == 2)
          sa = 0;
        if (se == 2)
          se = 0;
      }
      ffp_lighting_key |= (sd | (ss << 2) | (sa << 4) | (se << 6)) << 8;
      if (ffp_has_normal)
        ffp_lighting_key |= 2u;
    }
    // Per-stage texcoord transforms: enabled when the stage's flags are
    // not DISABLE, with the count and projection bits packed for the
    // generated shader.
    // Per-stage texcoord transforms: the enable bit keys the generated
    // shader; the count, projection and attribute-width semantics fold
    // into the matrix at upload (wined3d utils.c compute_texture_matrix)
    // and the projective divide rides the combiner stage flags.
    uint32_t ffp_tt_key = 0;
    uint32_t ffp_texgen_key = 0;
    uint32_t ffp_texcoord_index_key = 0;
    if (ffp_vs) {
      for (uint32_t s = 0; s < 8; ++s) {
        DWORD ttf = pod.texture_stage_states[s][D3DTSS_TEXTURETRANSFORMFLAGS];
        // A bare PROJECTED flag with a zero count still transforms (the
        // identity-with-divisor arm of the matrix preprocessing). A pre-
        // transformed (XYZRHW) draw never applies the texcoord matrix:
        // wined3d gates the shader multiply on !transformed (glsl_shader.c)
        // and DXVK on !VertexHasPositionT, so those texcoords reach the
        // sampler raw. Leave the enable bit clear rather than warp them.
        if (ttf != D3DTTFF_DISABLE && !bd.resolved_position_transformed)
          ffp_tt_key |= 1u << (s * 4);
        // D3DTSS_TCI_* texture generation, keyed by stage: wined3d
        // utils.c copies the raw TEXCOORDINDEX per stage and a
        // generated stage ignores the low coordinate index, writing
        // the stage's own varying.
        uint32_t tci_mode = (pod.texture_stage_states[s][D3DTSS_TEXCOORDINDEX] >> 16) & 0xFFFFu;
        if (tci_mode >= 1 && tci_mode <= 4)
          ffp_texgen_key |= tci_mode << (s * 3);
        ffp_texcoord_index_key |= (pod.texture_stage_states[s][D3DTSS_TEXCOORDINDEX] & 7u) << (s * 3);
      }
      if (ffp_texgen_key != 0 && rs[D3DRS_NORMALIZENORMALS] != FALSE)
        ffp_texgen_key |= 1u << 24;
    }
    // Fixed-function point size: emit the [[point_size]] output for every
    // point-list draw. The size, clamp bounds and scale factors ride the
    // uniforms block (float4 8/9), so only the point-vs-nonpoint /
    // POINTSCALEENABLE / per-vertex gates key the generated variant.
    bool ffp_point_size = false;
    bool ffp_point_scale = false;
    bool ffp_point_size_per_vertex = false;
    if (ffp_vs && bd.primitive_type == D3DPT_POINTLIST) {
      ffp_point_size = true;
      ffp_point_scale = rs[D3DRS_POINTSCALEENABLE] != FALSE;
      // A declared PSIZE attribute overrides the render-state size
      // (wined3d per_vertex_point_size).
      ffp_point_size_per_vertex = ffp_has_psize;
    }
    // D3DRS_VERTEXBLEND declared weight count for the generated VS.
    // Tweening and the zero-weight arm collapse to disabled (world
    // matrix 0 only), the same 1..3 support the wined3d vertex pipe
    // implements; a pre-transformed position never blends.
    uint32_t ffp_vertex_blend = 0;
    if (ffp_vs && !bd.resolved_position_transformed) {
      DWORD vb = rs[D3DRS_VERTEXBLEND];
      if (vb >= D3DVBF_1WEIGHTS && vb <= D3DVBF_3WEIGHTS)
        ffp_vertex_blend = vb;
    }
    // Fetch (find-or-create + submit) the async vertex-function compile
    // task; the LLVM AIR emit runs on a pool thread, not here. A cold
    // variant does not stall the encode thread; the PSO task below waits
    // on this task off-thread and the null-state skip drops a failed compile.
    D3D9CompiledFunction *vs_fn =
        ffp_vs ? ffpVertexFunction(
                     layout, ffp_has_diffuse, ffp_has_texcoord0, ffp_has_specular, ffp_vs_fog_mode, ffp_vs_range_fog,
                     ffp_point_size, ffp_point_scale, ffp_lighting_key, ffp_texcoord_mask, ffp_tt_key, ffp_vertex_blend,
                     ffp_texgen_key, ffp_texcoord_index_key, ffp_point_size_per_vertex, ffp_decl_has_diffuse
                 )
               : vs->getVariantTask(layout, vs_inject_point_size);

    // SM 1.0..1.3 PS lack dcl_2d/dcl_cube tokens; infer kinds from bound textures.
    // dxso_compile defaults to Texture2D, causing Metal validation and cube-map flicker.
    uint8_t ps_samp_kinds[16] = {};
    for (uint32_t stage = 0; stage < 16; ++stage) {
      auto *tex = refs.textures[stage].ptr();
      if (!tex)
        continue;
      switch (tex->commonTextureType()) {
      case D3DRTYPE_TEXTURE:
        // INTZ/DF24/DF16 are depth textures but bound as D3DRTYPE_TEXTURE.
        // Force depth2d<float> codegen; MSL texture2d<float> leaves .gba undefined.
        switch (tex->metalPixelFormat()) {
        case WMTPixelFormatDepth16Unorm:
        case WMTPixelFormatDepth32Float:
        case WMTPixelFormatDepth32Float_Stencil8:
        case WMTPixelFormatDepth24Unorm_Stencil8:
          // INTZ and the HW-shadow depth formats both land on a Metal
          // depth texture; the D3DFORMAT picks the sample op. INTZ ->
          // raw depth replicated (in-shader compare); D24S8/DF24/DF16 ->
          // hardware PCF (sample_compare). See IsHardwarePCFDepthFormat.
          // The raw-depth trio with the FETCH4 latch armed gathers the
          // neighbourhood instead (DXVK lists the same three among its
          // FETCH4-compatible formats); the PCF formats never gather.
          if (!IsHardwarePCFDepthFormat(tex->d3dFormat()) && (pod.fetch4_latch & (1u << stage)) &&
              samp_states[stage][D3DSAMP_MAGFILTER] == D3DTEXF_POINT)
            ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH_FETCH4;
          else if (IsHardwarePCFDepthFormat(tex->d3dFormat()))
            ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH_COMPARE;
          else
            // Raw depth: INTZ replicates, the DF formats read red only.
            ps_samp_kinds[stage] = tex->d3dFormat() == D3DFMT_INTZ ? DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH
                                                                   : DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH_DF;
          break;
        default:
          // Two-channel signed formats take the snorm rescale kinds.
          if (tex->d3dFormat() == D3DFMT_V8U8) {
            ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_2D_SNORM2_8;
            break;
          }
          if (tex->d3dFormat() == D3DFMT_V16U16) {
            ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_2D_SNORM2_16;
            break;
          }
          // FETCH4: armed latch + point magnification + a single-channel
          // colour format gathers instead of sampling (DXVK gates on the
          // same trio; its format list is the source of this one).
          if (stage < 16 && (pod.fetch4_latch & (1u << stage)) &&
              samp_states[stage][D3DSAMP_MAGFILTER] == D3DTEXF_POINT) {
            switch (tex->d3dFormat()) {
            case D3DFMT_R16F:
            case D3DFMT_R32F:
            case D3DFMT_A8:
            case D3DFMT_L8:
            case D3DFMT_L16:
              ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_2D_FETCH4;
              break;
            case D3DFMT_ATI1:
              // Block-compressed: the hardware replicates the sampled
              // red instead of gathering across the block.
              ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_2D_FETCH4_REPLICATE;
              break;
            default:
              // An armed latch on a format outside the single-channel
              // set: the vendor hardware returns zero for the plain
              // sample forms and only the projected form degrades to a
              // normal sample; wine's fetch4 rows pin both sides.
              ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_2D_FETCH4_BROKEN;
              break;
            }
            break;
          }
          ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_2D;
          break;
        }
        break;
      case D3DRTYPE_CUBETEXTURE:
        ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_CUBE;
        break;
      case D3DRTYPE_VOLUMETEXTURE:
        ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_TEXTURE_3D;
        break;
      default:
        ps_samp_kinds[stage] = DXSO_PS_SAMPLER_KIND_UNKNOWN;
        break;
      }
    }
    // Unbound slots the PS still declares a sampler for: take the kind
    // from the dcl so the compiled variant and the bound dummy agree on
    // texture type. Without this a dcl_volume / dcl_cube slot with no app
    // texture bound compiles the PS as 3D/cube (airconv's dcl fallback)
    // while the resolve binds a 2D dummy: a Metal type mismatch that
    // samples undefined (black). Host-authoritative, mirroring DXVK's
    // per-slot texture-type tracking (D3D9TextureSlotTracking) + wined3d's
    // per-type dummy textures.
    if (!ffp_ps)
      for (const auto &d : ps->metadata().dcls) {
        if (d.bound_to.type != DxsoRegisterType::Sampler || d.bound_to.num >= 16)
          continue;
        if (refs.textures[d.bound_to.num].ptr())
          continue; // bound: kind already set from the actual texture above
        switch (d.dcl.texture_type) {
        case DxsoTextureType::TextureCube:
          ps_samp_kinds[d.bound_to.num] = DXSO_PS_SAMPLER_KIND_TEXTURE_CUBE;
          break;
        case DxsoTextureType::Texture3D:
          ps_samp_kinds[d.bound_to.num] = DXSO_PS_SAMPLER_KIND_TEXTURE_3D;
          break;
        default:
          ps_samp_kinds[d.bound_to.num] = DXSO_PS_SAMPLER_KIND_TEXTURE_2D;
          break;
        }
      }
    // Only the alpha compare FUNC keys the variant (D3DCMP_ALWAYS = no
    // discard emit); the ref rides the shared PS uniform tail, written into
    // ps_b_blob at the const-upload below and read at runtime.
    DWORD alpha_func = rs[D3DRS_ALPHAFUNC];
    // An out-of-range compare func (uninitialized app state) kills every
    // fragment on both refs (their DecodeCompareOp default arm is NEVER,
    // the same rule to_mtl_compare_func now follows). Normalize garbage to
    // D3DCMP_NEVER here, at the single producer of the variant key, so the
    // generated discard matches instead of the codegen default passing
    // everything; the alpha_func rides the WoW64 arg chain, so fixing it at
    // the source keeps both PS paths honest.
    if (alpha_func < D3DCMP_NEVER || alpha_func > D3DCMP_ALWAYS)
      alpha_func = D3DCMP_NEVER;
    bool alpha_test = rs[D3DRS_ALPHATESTENABLE] != FALSE && alpha_func != D3DCMP_ALWAYS;
    // POINTSPRITEENABLE only applies to point-list primitives; non-point
    // draws skip the variant so the cache doesn't explode on toggles.
    // D3DRS_POINTSPRITEENABLE default is FALSE so most apps never hit
    // the variant path at all.
    bool point_sprite = rs[D3DRS_POINTSPRITEENABLE] != FALSE && bd.primitive_type == D3DPT_POINTLIST;
    // TexBem / TexBemL / Bem: the per-stage D3DTSS_BUMPENV* matrix and
    // luminance scale/offset ride the shared PS uniform tail (written into
    // ps_b_blob below, unconditionally from the pod so the buffer stays a
    // pure function of the const-cache key) and the generated PS reads them
    // at runtime. They do not bake into the variant, so an app that
    // animates bump-env keeps one variant per material instead of churning
    // a cold PSO link per frame. DXVK feeds the same constants through its
    // D3D9SharedPS uniform (src/d3d9/d3d9_state.h + dxso_compiler.cpp emitBem).
    // D3D9 fog blend (pre-SM3 contract): the PS epilogue lerps oC0.rgb
    // toward D3DRS_FOGCOLOR by a fog factor, as wined3d's and DXVK's
    // generated pixel shaders do. ps_3_0 computes fog itself per spec;
    // gating the version here keeps SM3 titles that leave FOGENABLE set
    // from forking byte-identical PSOs (wined3d zeroes its fog
    // compile-arg the same way).
    //
    // FOGTABLEMODE != NONE means table (pixel) fog, which takes priority
    // over vertex fog per the D3D9 contract: the factor is computed per
    // fragment from depth in the PS, with FOGSTART/FOGEND/FOGDENSITY
    // threaded through the bool-constant blob below. Otherwise vertex
    // fog uses the VS oFog factor; a VS that writes no oFog (or fixed
    // function with FOGVERTEXMODE none) falls back to the interpolated
    // specular alpha, which test_fog's rows pin.
    int fog_mode = -1;
    if (rs[D3DRS_FOGENABLE] != FALSE && (ffp_ps || ps->metadata().major < 3)) {
      DWORD table_mode = rs[D3DRS_FOGTABLEMODE];
      if (table_mode != D3DFOG_NONE) {
        // D3DFOG_EXP=1, EXP2=2, LINEAR=3; map onto DXSO_PS_FOG_MODE_*.
        switch (table_mode) {
        case D3DFOG_LINEAR:
          fog_mode = DXSO_PS_FOG_MODE_LINEAR;
          break;
        case D3DFOG_EXP:
          fog_mode = DXSO_PS_FOG_MODE_EXP;
          break;
        case D3DFOG_EXP2:
          fog_mode = DXSO_PS_FOG_MODE_EXP2;
          break;
        default:
          break;
        }
      } else if (bd.resolved_position_transformed) {
        // A pre-transformed draw never takes the vertex-fog formula,
        // whatever FOGVERTEXMODE says: the factor is always the
        // specular alpha (test_fog's RHW rows pin it for every mode).
        fog_mode = DXSO_PS_FOG_MODE_SPECULAR_ALPHA;
      } else if (ffp_vs ? ffp_vs_fog_mode != 0 : vs->metadata().writes_fog) {
        fog_mode = DXSO_PS_FOG_MODE_VERTEX;
      } else {
        // No table mode and no fog factor from the vertex stage: the
        // factor is the interpolated specular alpha (a bytecode VS
        // without an oFog write, a pre-transformed draw, or fixed
        // function with FOGVERTEXMODE none); the fog params are ignored
        // on this path per test_fog's contract.
        fog_mode = DXSO_PS_FOG_MODE_SPECULAR_ALPHA;
      }
    }
    // Dual-source blending: only when the active blend factors actually
    // read SRC1 (D3DBLEND_SRCCOLOR2 / INVSRCCOLOR2) does oC1 become the
    // second color index of attachment 0. A draw that writes oC1 as a
    // normal second render target must not take this variant, so the
    // detection is on the bound blend factors, not the shader. Alpha
    // factors only matter under SEPARATEALPHABLENDENABLE. The variant
    // additionally requires the PS to export oC1: a Source1 PSO whose
    // fragment function has no index(1) output fails Metal pipeline
    // creation, so apply_blend_state_to_attachment folds the SRC1
    // factors away instead when the shader can't feed them.
    bool dual_source = false;
    if (!ffp_ps && rs[D3DRS_ALPHABLENDENABLE] != FALSE && ps->metadata().writes_oc1) {
      auto is_src1 = [](DWORD f) { return f == D3DBLEND_SRCCOLOR2 || f == D3DBLEND_INVSRCCOLOR2; };
      dual_source = is_src1(rs[D3DRS_SRCBLEND]) || is_src1(rs[D3DRS_DESTBLEND]);
      if (rs[D3DRS_SEPARATEALPHABLENDENABLE] != FALSE)
        dual_source = dual_source || is_src1(rs[D3DRS_SRCBLENDALPHA]) || is_src1(rs[D3DRS_DESTBLENDALPHA]);
    }
    // Stage-0 texturing mode for the generated PS: a bound texture with
    // the stage-0 color op enabled modulates the diffuse (the default
    // TSS state, MODULATE(texture, diffuse), covers the common draw);
    // everything else passes the diffuse through until the combiner
    // milestone keys the full per-stage table.
    // The generated PS's combiner table, packed per the key contract:
    // ops and args from the frozen texture-stage state, the has-texture
    // and result-is-temp flags; each stage samples its own varying,
    // the per-stage routing living in the vertex key. A stage whose
    // arguments reference TEXTURE with none bound ends the chain, the
    // wined3d contract for incomplete stages.
    // The host-resolved sampler kinds for the combiner's eight stages,
    // packed four bits each; the bytecode variants receive the same
    // resolution through their PSO argument instead.
    uint32_t ffp_sampler_kind_key = 0;
    if (ffp_ps)
      for (uint32_t s = 0; s < 8; ++s)
        ffp_sampler_kind_key |= uint32_t(ps_samp_kinds[s] & 0xFu) << (s * 4);
    uint32_t ffp_stages[8][3] = {};
    if (ffp_ps) {
      for (uint32_t s = 0; s < 8; ++s) {
        const DWORD *tss = pod.texture_stage_states[s];
        uint32_t color_op = tss[D3DTSS_COLOROP] & 0xFF;
        uint32_t alpha_op = tss[D3DTSS_ALPHAOP] & 0xFF;
        if (s > 0 && color_op == D3DTOP_DISABLE) {
          ffp_stages[s][0] = D3DTOP_DISABLE;
          break;
        }
        const bool has_tex = refs.textures[s].ptr() != nullptr;
        auto refs_texture = [&](DWORD arg) { return (arg & D3DTA_SELECTMASK) == D3DTA_TEXTURE; };
        DWORD carg1 = tss[D3DTSS_COLORARG1], carg2 = tss[D3DTSS_COLORARG2], carg0 = tss[D3DTSS_COLORARG0];
        DWORD aarg1 = tss[D3DTSS_ALPHAARG1], aarg2 = tss[D3DTSS_ALPHAARG2], aarg0 = tss[D3DTSS_ALPHAARG0];
        // An op reading TEXTURE with none bound rewrites to
        // SELECTARG1(CURRENT) and the chain continues; the third
        // argument only invalidates the ops that read it (wined3d
        // utils.c is_invalid_op, applied per color and alpha op).
        auto invalid_op = [&](uint32_t op, DWORD a1, DWORD a2, DWORD a0) {
          if (op == D3DTOP_DISABLE || has_tex)
            return false;
          if (refs_texture(a1) && op != D3DTOP_SELECTARG2)
            return true;
          if (refs_texture(a2) && op != D3DTOP_SELECTARG1)
            return true;
          if (refs_texture(a0) && (op == D3DTOP_MULTIPLYADD || op == D3DTOP_LERP))
            return true;
          return false;
        };
        if (invalid_op(color_op, carg1, carg2, carg0)) {
          color_op = D3DTOP_SELECTARG1;
          carg1 = D3DTA_CURRENT;
          carg2 = D3DTA_CURRENT;
          carg0 = D3DTA_CURRENT;
        }
        if (invalid_op(alpha_op, aarg1, aarg2, aarg0)) {
          alpha_op = D3DTOP_SELECTARG1;
          aarg1 = D3DTA_CURRENT;
          aarg2 = D3DTA_CURRENT;
          aarg0 = D3DTA_CURRENT;
        }
        // A dot product on the color op overwrites the alpha operation
        // and replicates the color result into alpha (wined3d utils.c).
        if (color_op == D3DTOP_DOTPRODUCT3) {
          alpha_op = color_op;
          aarg1 = carg1;
          aarg2 = carg2;
          // DOTPRODUCT3 ignores arg0, but wined3d utils.c mirrors carg0 into
          // aarg0 so the identical-op collapse recognises the two ops as equal.
          aarg0 = carg0;
        }
        uint32_t flags = (has_tex ? 1u : 0u) | ((tss[D3DTSS_RESULTARG] & D3DTA_SELECTMASK) == D3DTA_TEMP ? 2u : 0u) |
                         ((tss[D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED) ? 4u : 0u);
        ffp_stages[s][0] = color_op | (alpha_op << 8) | (flags << 16);
        ffp_stages[s][1] = (carg1 & 0xFF) | ((carg2 & 0xFF) << 8) | ((carg0 & 0xFF) << 16);
        ffp_stages[s][2] = (aarg1 & 0xFF) | ((aarg2 & 0xFF) << 8) | ((aarg0 & 0xFF) << 16);
      }
    }
    // Table-fog coordinate: fog against eye-space w (1/position.w) when the
    // projection can produce a non-unit w (pod.ffp_fog_coord_w), else the
    // vertex-output Z. The non-w arm reads the VS-written FOG0.y varying
    // (clip-space Z for a WVP draw, window-space Z for a pre-transformed draw,
    // wined3d ffp_varying_fogcoord), not the fragment [[position]].z, which
    // keeps it off the post-perspective device depth and clear of the
    // rasterizer depth bias. A pre-transformed draw takes the same
    // projection-derived choice: its rhw carries the perspective w a
    // non-orthographic projection would have made.
    const bool fog_coord_w =
        fog_mode >= DXSO_PS_FOG_MODE_LINEAR && fog_mode <= DXSO_PS_FOG_MODE_EXP2 && pod.ffp_fog_coord_w != 0;
    // Per-attachment 8-bit-UNORM snap mask: bit i set when render target i
    // resolves to a LINEAR 8-bit unorm Metal format, so the PS epilogue rounds
    // oC<i> to the nearest k/255 (round-half-to-even) and Metal's unorm write
    // reproduces WARP's byte instead of rounding an exact half the other way.
    // The mask keys the PS variant, so a shader shared between an 8-bit-unorm
    // and a float/HDR RT forks one metallib per mask. An SRGBWRITEENABLE target
    // recalls to an sRGB format IsUnorm8RenderTargetFormat rejects (an sRGB
    // attachment applies its own curve), so sRGB and float/HDR keep full
    // precision. Mirrors the DXBC pipeline's unorm_output_reg_mask.
    uint32_t unorm_snap_mask = 0;
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      MTLD3D9Surface *rt = refs.render_targets[i].ptr();
      if (!rt || IsNullFormat(rt->desc().Format))
        continue;
      WMTPixelFormat fmt = D3DFormatToMetal(rt->desc().Format, D3D9FormatUsage::RenderTarget);
      if (rs[D3DRS_SRGBWRITEENABLE] != 0)
        fmt = Recall_sRGB(fmt);
      if (IsUnorm8RenderTargetFormat(fmt))
        unorm_snap_mask |= 1u << i;
    }
    // ---- PSO descriptor build ----
    MTLD3D9Surface *ds = refs.depth_stencil_surface.ptr();
    // Sample count flows from the realized Metal texture, NOT
    // desc().MultiSampleType: NONMASKABLE (and any path that allocates more
    // samples than the enum encodes) stores an enum that maps to 1 while the
    // texture carries the real count, and Metal hard-errors (hangs AGX) when
    // the pipeline rasterSampleCount differs from an attachment. Mirrors
    // d3d11's OM-bind, which reads the count off the bound view. Resolved
    // before the PS variant so the D3DRS_MULTISAMPLEMASK gate below can key it.
    uint8_t raster_sample_count = 1;
    if (rt0 && !IsNullFormat(rt0->desc().Format) && rt0->dxmtTexture()) {
      raster_sample_count = static_cast<uint8_t>(rt0->dxmtTexture()->sampleCount());
    } else if (ds && ds->dxmtTexture()) {
      raster_sample_count = static_cast<uint8_t>(ds->dxmtTexture()->sampleCount());
    }
    // D3DRS_MULTISAMPLEMASK rides the PS coverage output, not the pipeline key:
    // only a 1-bit enable keys the variant (an animated mask never churns PSOs)
    // while the 32-bit mask word rides the ps_b_blob tail below. The
    // sample-count gate is mandatory: on a single-sample target a cleared mask
    // bit0 would kill every fragment, and an all-ones mask is inert anywhere,
    // so both keep the plain (non-coverage) variant. wined3d/DXVK apply the
    // mask unconditionally; Metal has no encoder/PSO sample mask, only the
    // shader-side [[sample_mask]] output the variant now emits.
    const bool emit_sample_mask = raster_sample_count > 1 && rs[D3DRS_MULTISAMPLEMASK] != 0xffffffffu;
    // The alpha compare FUNC keys the variant; the REF rides the shared PS
    // uniform tail (written into ps_b_blob below), so it never reaches the
    // pipeline key. TexBem bump-env constants ride the same tail.
    D3D9CompiledFunction *ps_fn =
        ffp_ps ? ffpPixelFunction(
                     ffp_stages, rs[D3DRS_SPECULARENABLE] != FALSE, point_sprite, fog_mode, fog_coord_w,
                     alpha_test ? alpha_func : 8, ffp_sampler_kind_key, rs[D3DRS_SHADEMODE] == D3DSHADE_FLAT,
                     emit_sample_mask, unorm_snap_mask
                 )
               : ps->getVariantTask(
                     alpha_test ? alpha_func : D3DCMP_ALWAYS, ps_samp_kinds, point_sprite, fog_mode, fog_coord_w,
                     dual_source, rs[D3DRS_SHADEMODE] == D3DSHADE_FLAT, emit_sample_mask, unorm_snap_mask
                 );
    // Every render-pass attachment + the pipeline must share one sample count.
    // A bound DS whose sample count disagrees with the color target (an app
    // pairing an MSAA depth surface with a single-sample render target, or the
    // reverse) is dropped here, before the PSO bakes a depth format, rather than
    // faulting the GPU. The mismatch is an app error, so warn once instead of
    // once per draw.
    if (ds && ds->dxmtTexture() && rt0 && !IsNullFormat(rt0->desc().Format) &&
        ds->dxmtTexture()->sampleCount() != raster_sample_count) {
      // Encode thread is the sole toucher; a plain static needs no guard.
      static bool warned = false;
      if (!warned) {
        warned = true;
        Logger::warn(
            str::format(
                "d3d9: depth-stencil sample count ", ds->dxmtTexture()->sampleCount(), " != render target ",
                (unsigned)raster_sample_count,
                "; dropping the DS (a render target and depth-stencil must match multisample)"
            )
        );
      }
      ds = nullptr;
      bd.resolved_ds_dxmt = nullptr;
    }
    WMTPixelFormat ds_pixel_format = WMTPixelFormatInvalid;
    bool ds_has_stencil = false;
    if (ds) {
      ds_pixel_format = D3DFormatToMetal(ds->desc().Format, D3D9FormatUsage::DepthStencil);
      ds_has_stencil = HasStencilAspect(ds->desc().Format);
    }
    // Plumb the sample count through to the chunk lambda so its
    // startRenderPass(default_raster_sample_count=N) matches the PSO's
    // raster_sample_count=N. Metal validates this equality at
    // setRenderPipelineState time; a mismatch hard-errors under
    // MTL_DEBUG_LAYER.
    bd.resolved_raster_sample_count = raster_sample_count;

    WMTPrimitiveTopologyClass topology_class = WMTPrimitiveTopologyClassTriangle;
    switch (bd.primitive_type) {
    case D3DPT_POINTLIST:
      topology_class = WMTPrimitiveTopologyClassPoint;
      break;
    case D3DPT_LINELIST:
    case D3DPT_LINESTRIP:
      topology_class = WMTPrimitiveTopologyClassLine;
      break;
    case D3DPT_TRIANGLELIST:
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
      topology_class = WMTPrimitiveTopologyClassTriangle;
      break;
    default:
      break;
    }

    WMTRenderPipelineInfo pso_info;
    WMT::InitializeRenderPipelineInfo(pso_info);
    // The function handles are filled by the PSO task once its function-task
    // dependencies compile off-thread; they do not exist yet, so leave them
    // at the zero InitializeRenderPipelineInfo set.
    pso_info.input_primitive_topology = topology_class;
    pso_info.depth_pixel_format = ds_pixel_format;
    pso_info.stencil_pixel_format = ds_has_stencil ? ds_pixel_format : WMTPixelFormatInvalid;
    pso_info.raster_sample_count = raster_sample_count;

    const bool srgb_write = rs[D3DRS_SRGBWRITEENABLE] != 0;
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      MTLD3D9Surface *rt = refs.render_targets[i].ptr();
      if (!rt || IsNullFormat(rt->desc().Format))
        continue;
      WMTPixelFormat fmt = D3DFormatToMetal(rt->desc().Format, D3D9FormatUsage::RenderTarget);
      if (srgb_write)
        fmt = Recall_sRGB_ForRenderTarget(fmt);
      pso_info.colors[i].pixel_format = fmt;
      const bool alpha_is_one = D3DFormatHasNoAlpha(rt->desc().Format);
      apply_blend_state_to_attachment(pso_info.colors[i], rs, rs[kColorWriteEnableRS[i]], dual_source, alpha_is_one);
    }

    uint64_t pso_key = 0xcbf29ce484222325ull;
    auto mix64 = [&](uint64_t v) {
      pso_key ^= v;
      pso_key *= 0x100000001b3ull;
    };
    // Key on the function-task identities, not the compiled handles (which
    // do not exist until the async compile finishes). The task pointer is a
    // bijection with (module, variant key): get-or-create returns one task
    // per variant, pinned for device lifetime (module tasks by the PSO
    // cache's Com<shader>, FFP tasks by the device caches), so ABA is
    // impossible and two distinct variants never collide.
    mix64(reinterpret_cast<uint64_t>(vs_fn));
    mix64(reinterpret_cast<uint64_t>(ps_fn));
    mix64(static_cast<uint32_t>(pso_info.depth_pixel_format));
    mix64(static_cast<uint32_t>(pso_info.stencil_pixel_format));
    mix64(static_cast<uint32_t>(pso_info.input_primitive_topology));
    mix64(static_cast<uint32_t>(pso_info.raster_sample_count));
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      const auto &b = pso_info.colors[i];
      mix64(static_cast<uint32_t>(b.pixel_format));
      mix64((static_cast<uint64_t>(b.blending_enabled ? 1u : 0u) << 32) | static_cast<uint32_t>(b.write_mask));
      mix64(
          (static_cast<uint64_t>(b.rgb_blend_operation) << 48) |
          (static_cast<uint64_t>(b.alpha_blend_operation) << 32) |
          (static_cast<uint64_t>(b.src_rgb_blend_factor) << 24) |
          (static_cast<uint64_t>(b.dst_rgb_blend_factor) << 16) |
          (static_cast<uint64_t>(b.src_alpha_blend_factor) << 8) | static_cast<uint64_t>(b.dst_alpha_blend_factor)
      );
    }

    D3D9PsoCompileTask *task;
    bool first_time = false;
    // Cluster-miss short-circuit: even when ref_ptr or sampler-state
    // changed (forcing a rebuild here), the PSO inputs (vs/ps function +
    // RT/DS formats + blend state) often haven't moved. The previous
    // draw's pso_key is the cheap gate before the FNV map probe. Both the
    // fast path and the map probe confirm the full key inputs on a hash
    // hit before reusing the task, the same collision guard the bytecode
    // module cache applies (a 64-bit hit alone would pick the wrong
    // pipeline); the verify is a handful of int compares and rejects on
    // the first differing field.
    if (pso_key == resolve_cache.last_pso_key && resolve_cache.last_pso_task &&
        resolve_cache.last_pso_task->matchesKeyInputs(vs_fn, ps_fn, pso_info)) {
      task = resolve_cache.last_pso_task;
    } else if (
        auto it = m_psoCache.find(pso_key);
        it != m_psoCache.end() && it->second->matchesKeyInputs(vs_fn, ps_fn, pso_info)
    ) {
      task = it->second.get();
      resolve_cache.last_pso_key = pso_key;
      resolve_cache.last_pso_task = task;
    } else {
      auto fresh = std::make_unique<D3D9PsoCompileTask>(
          m_metalDevice, Com<MTLD3D9VertexShader, false>{vs}, Com<MTLD3D9PixelShader, false>{ps}, pso_info, vs_fn, ps_fn
      );
      task = fresh.get();
      // A true miss inserts; a verified 64-bit collision (the slot already
      // holds a different PSO's task, which an in-flight chunk may still
      // reference so it can't be evicted) leaves try_emplace's argument
      // un-moved. Pin that loser for device lifetime in m_psoCacheCollisions
      // instead, so the non-owning task pointer handed to the chunk stays
      // valid; a collision is astronomically rare, so a non-cached rebuild is
      // acceptable.
      if (!m_psoCache.try_emplace(pso_key, std::move(fresh)).second)
        m_psoCacheCollisions.push_back(std::move(fresh));
      m_psoScheduler.submit(task);
      first_time = true;
      resolve_cache.last_pso_key = pso_key;
      resolve_cache.last_pso_task = task;
    }
    // Defer the cold-compile wait to the encode thread so the calling
    // thread never blocks on a PSO link; do so ONLY when the compile
    // is still in flight. If the task
    // already completed (cache hit, or rare submit-flushed-fast), do
    // the cheap atomic-load resolve here; that preserves the
    // Resolve-time return-false rejection for known-bad PSOs so a
    // failed front draw can't silently drop the chunk's pending-clear
    // flags. m_psoCache pins the task pointer for the device lifetime.
    if (task->GetDone()) {
      WMT::RenderPipelineState pso = task->state();
      if (pso.handle == 0)
        return false;
      bd.resolved_pso = pso.handle;
    } else {
      bd.resolved_pso_task = task;
      bd.resolved_pso_first_use = first_time;
    }

    // ---- Per-stage textures + samplers ----
    for (uint32_t stage = 0; stage < 16; ++stage) {
      auto *tex = refs.textures[stage].ptr();
      const DWORD *samp_row = samp_states[stage];
      // A bound texture with no Metal backing (a SCRATCH / packed-YUV resource
      // constructed with a null dxmt::Texture, which SetTexture accepts) has no
      // view to resolve; treat it as unbound so the dummy-texture arm below
      // binds a placeholder instead of dereferencing the null backing here on
      // the encode thread. Covers 2D, cube and volume alike.
      if (!tex || !tex->dxmtTexture()) {
        // Unbound sampler post-Reset causes Metal validation error and GPU callback error.
        // Bind 1x1 placeholder + sampler to complete encoder.
        WMTSamplerInfo sinfo = sampler_info_from_d3d9_state(samp_row);
        if (auto sampler = getOrCreateSampler(sinfo))
          bd.resolved_frag_samplers[stage] = sampler->sampler_state.handle;
        // The dummy's type must match the kind the PS variant was compiled
        // with for this slot (set above from the bound texture, or the dcl
        // for an unbound-but-declared slot), or Metal flags a 2D-vs-3D/cube
        // type mismatch and samples undefined.
        WMTTextureType dummy_type = WMTTextureType2D;
        if (ps_samp_kinds[stage] == DXSO_PS_SAMPLER_KIND_TEXTURE_3D)
          dummy_type = WMTTextureType3D;
        else if (ps_samp_kinds[stage] == DXSO_PS_SAMPLER_KIND_TEXTURE_CUBE)
          dummy_type = WMTTextureTypeCube;
        bd.resolved_frag_textures[stage] = dummyFragmentTexture(dummy_type);
        continue;
      }
      // View lives on TextureAllocation (survives wrapper Reset via
      // ref_tracker). derivations chain off fullView.
      const Rc<dxmt::Texture> &rc = tex->dxmtTexture();
      // Per-format channel swizzle + optional sRGB alias + SetLOD mip clamp.
      // Shared with the VTF bind below (deriveSampleView) so a fixup-needing
      // format samples the same shape in a VS as in a PS.
      uint64_t view = deriveSampleView(rc, tex, samp_row);
      // Resolve the view's Metal handle now (encode thread; same
      // allocation as emit since both run inside this chunk). Kept for
      // the cluster cache + the per-encoder bind shadow; the fence-tracked
      // ctx.access(viewId) in EmitCommonRenderSetup_d9 re-fetches the same
      // view object. Fall back to the base view if aliasing failed (an
      // unsupported format pair); matches the old D3D9ViewCache.
      obj_handle_t vh = rc->view(view).texture.handle;
      if (!vh) {
        view = rc->fullView;
        vh = rc->view(view).texture.handle;
      }
      bd.resolved_frag_view[stage] = view;
      bd.resolved_frag_textures[stage] = vh;
      bd.resolved_frag_texture_dxmt[stage] = rc;
      // Hardware-PCF depth textures need a LessEqual compare sampler so
      // sample_compare (emitted by the _DEPTH_COMPARE PS variant for this
      // stage) returns the filtered shadow result. Must match the kind
      // classification above (both gate on IsHardwarePCFDepthFormat).
      WMTSamplerInfo sinfo = sampler_info_from_d3d9_state(
          samp_row, IsHardwarePCFDepthFormat(tex->d3dFormat()), IsMetalNonFilterableFormat(tex->d3dFormat())
      );
      auto sampler = getOrCreateSampler(sinfo);
      if (sampler)
        bd.resolved_frag_samplers[stage] = sampler->sampler_state.handle;
    }

    // ---- DSSO + stencil ref ----
    if (ds) {
      WMTDepthStencilInfo ds_info = depth_stencil_info_from_d3d9_state(rs, /*dsAttached=*/true, ds_has_stencil);
      auto dsso = getOrCreateDSSO(ds_info);
      bd.resolved_dsso = dsso.handle;
      bd.resolved_stencil_ref = static_cast<uint8_t>(rs[D3DRS_STENCILREF] & 0xFF);
    }

    // ---- RT / DS Rc<dxmt::Texture> + TextureViewKey + Metal handles + dims ----
    uint32_t rt_count = 0;
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      auto *rt = refs.render_targets[i].ptr();
      if (!rt || IsNullFormat(rt->desc().Format))
        continue;
      bd.resolved_rt_dxmt[i] = rt->dxmtTexture();
      if (bd.resolved_rt_dxmt[i]) {
        TextureViewKey view = bd.resolved_rt_dxmt[i]->fullView;
        if (srgb_write) {
          // D3DRS_SRGBWRITEENABLE renders through the sRGB-format view; the
          // attachment encodes the fragment output on store.
          WMTPixelFormat base = bd.resolved_rt_dxmt[i]->pixelFormat();
          WMTPixelFormat srgb = Recall_sRGB_ForRenderTarget(base);
          if (srgb != base)
            view = bd.resolved_rt_dxmt[i]->checkViewUseFormat(view, srgb);
        }
        bd.resolved_rt_view[i] = static_cast<uint64_t>(view);
      }
      bd.resolved_rt_handles[i] = rt->metalTexture().handle;
      bd.resolved_rt_level[i] = static_cast<uint16_t>(rt->mipLevel());
      bd.resolved_rt_slice[i] = static_cast<uint16_t>(rt->arraySlice());
      rt_count = i + 1;
      if (i == 0) {
        bd.resolved_rt_width = rt->desc().Width;
        bd.resolved_rt_height = rt->desc().Height;
      }
    }
    bd.resolved_rt_count = static_cast<uint8_t>(rt_count);

    // Self-downsample: a draw renders into mip N while sampling a lower
    // mip of the same texture (e.g. an HDR luminance pyramid). Legal in
    // D3D9, distinct subresources; DXVK skips the hazard for rtMip != 0.
    // Apple GPUs allow attachment + sampler to share an allocation only
    // as distinct, non-overlapping MTLTexture views (MoltenVK mints one
    // per subresource range); the default full-mip attachment view
    // overlaps the sampled mip, so the GPU drops the write and leaves NaN
    // that the tonemap turns black. Bind the sampler to [0,N) and the
    // attachment to a single mip [N,1). A mip-0 RT sampled at 0 is a real
    // feedback loop and is left alone.
    for (unsigned i = 0; i < bd.resolved_rt_count; ++i) {
      auto *rt_tex = bd.resolved_rt_dxmt[i].ptr();
      uint32_t rt_level = bd.resolved_rt_level[i];
      if (!rt_tex || rt_level == 0)
        continue;
      bool self_sampled = false;
      for (uint32_t stage = 0; stage < 16; ++stage) {
        if (bd.resolved_frag_texture_dxmt[stage].ptr() != rt_tex)
          continue;
        self_sampled = true;
        TextureViewKey src_view =
            rt_tex->checkViewUseMipRange(TextureViewKey(bd.resolved_frag_view[stage]), 0, rt_level);
        if (obj_handle_t vh = rt_tex->view(src_view).texture.handle) {
          bd.resolved_frag_view[stage] = static_cast<uint64_t>(src_view);
          bd.resolved_frag_textures[stage] = vh;
        }
      }
      if (self_sampled) {
        TextureViewKey rt_view = rt_tex->checkViewUseMipRange(TextureViewKey(bd.resolved_rt_view[i]), rt_level, 1);
        bd.resolved_rt_view[i] = static_cast<uint64_t>(rt_view);
        bd.resolved_rt_level[i] = 0;
      }
    }
    if (ds) {
      bd.resolved_ds_dxmt = ds->dxmtTexture();
      if (bd.resolved_ds_dxmt)
        bd.resolved_ds_view = static_cast<uint64_t>(bd.resolved_ds_dxmt->fullView);
      bd.resolved_ds_handle = ds->metalTexture().handle;
      bd.resolved_ds_has_stencil = ds_has_stencil;
      bd.resolved_ds_level = static_cast<uint16_t>(ds->mipLevel());
      bd.resolved_ds_slice = static_cast<uint16_t>(ds->arraySlice());
      bd.resolved_depth_bias_scale = DepthBiasScale(ds->desc().Format);
      if (bd.resolved_rt_width == 0) {
        bd.resolved_rt_width = ds->desc().Width;
        bd.resolved_rt_height = ds->desc().Height;
      }
    }

    // ---- Populate cluster cache so the next draw in the cluster can
    // skip the FNV+map-lookup work above. ----
    resolve_cache.pod_ptr = bd.pod_snapshot;
    resolve_cache.ref_gen = m_encodeSideRefsGen;
    resolve_cache.up_vb = up_vb;
    resolve_cache.up_ib = up_ib;
    resolve_cache.up_ib_format = bd.override_ib_format;
    resolve_cache.primitive_type = bd.primitive_type;
    resolve_cache.draw_type = bd.type;
    resolve_cache.resolved_pso = bd.resolved_pso;
    resolve_cache.resolved_pso_task = bd.resolved_pso_task;
    resolve_cache.resolved_dsso = bd.resolved_dsso;
    resolve_cache.resolved_stencil_ref = bd.resolved_stencil_ref;
    resolve_cache.resolved_slot_mask = bd.resolved_slot_mask;
    resolve_cache.resolved_ib_fmt = bd.resolved_ib_fmt;
    resolve_cache.resolved_raster_sample_count = bd.resolved_raster_sample_count;
    resolve_cache.resolved_depth_bias_scale = bd.resolved_depth_bias_scale;
    resolve_cache.resolved_ds_has_stencil = bd.resolved_ds_has_stencil;
    resolve_cache.resolved_rt_count = bd.resolved_rt_count;
    resolve_cache.resolved_rt_width = bd.resolved_rt_width;
    resolve_cache.resolved_rt_height = bd.resolved_rt_height;
    resolve_cache.resolved_ds_handle = bd.resolved_ds_handle;
    resolve_cache.resolved_ds_view = bd.resolved_ds_view;
    resolve_cache.resolved_ds_level = bd.resolved_ds_level;
    resolve_cache.resolved_ds_slice = bd.resolved_ds_slice;
    resolve_cache.resolved_viewport = bd.resolved_viewport;
    resolve_cache.resolved_position_transformed = bd.resolved_position_transformed;
    resolve_cache.resolved_inject_point_size = bd.resolved_inject_point_size;
    std::memcpy(resolve_cache.ffp_texcoord_width, ffp_texcoord_width, sizeof(resolve_cache.ffp_texcoord_width));
    resolve_cache.resolved_scissor = bd.resolved_scissor;
    std::memcpy(resolve_cache.resolved_rt_handles, bd.resolved_rt_handles, sizeof(resolve_cache.resolved_rt_handles));
    std::memcpy(resolve_cache.resolved_rt_view, bd.resolved_rt_view, sizeof(resolve_cache.resolved_rt_view));
    std::memcpy(resolve_cache.resolved_rt_level, bd.resolved_rt_level, sizeof(resolve_cache.resolved_rt_level));
    std::memcpy(resolve_cache.resolved_rt_slice, bd.resolved_rt_slice, sizeof(resolve_cache.resolved_rt_slice));
    std::memcpy(
        resolve_cache.resolved_frag_textures, bd.resolved_frag_textures, sizeof(resolve_cache.resolved_frag_textures)
    );
    std::memcpy(resolve_cache.resolved_frag_view, bd.resolved_frag_view, sizeof(resolve_cache.resolved_frag_view));
    std::memcpy(
        resolve_cache.resolved_frag_samplers, bd.resolved_frag_samplers, sizeof(resolve_cache.resolved_frag_samplers)
    );
    for (uint32_t i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i)
      resolve_cache.resolved_rt_dxmt[i] = bd.resolved_rt_dxmt[i];
    resolve_cache.resolved_ds_dxmt = bd.resolved_ds_dxmt;
    for (uint32_t i = 0; i < 16; ++i)
      resolve_cache.resolved_frag_texture_dxmt[i] = bd.resolved_frag_texture_dxmt[i];
  } // end of !cluster_hit branch

  // ---- Vertex texture fetch (VTF) textures + samplers ----
  // D3DVERTEXTEXTURESAMPLER0-3 live at texture slots 16..19; the VS samples
  // them via s0..s3 -> Metal vertex texture index 0..3. Resolved here (common
  // to both cluster paths, not cached) because VTF draws are rare. Most draws
  // bind nothing here and leave the four slots null.
  //
  // A slot the VS declares (dcl_2d / dcl_volume / dcl_cube s0..s3) but the app
  // left unbound gets an opaque-black dummy of the declared kind, mirroring the
  // fragment path: without it the VS would sample the previous draw's texture.
  // Only declared slots are touched (vs_samp_type stays Unknown otherwise).
  // VTF exists only in vs_3_0, so the scan is skipped for fixed-function and
  // pre-SM3 vertex paths entirely, and a SM3 VS with no sampler dcls finds
  // nothing here: the common no-VTF draw pays zero cost.
  DxsoTextureType vs_samp_type[4] = {};
  if (!ffp_vs && vs->metadata().major >= 3) {
    for (const auto &d : vs->metadata().dcls) {
      if (d.bound_to.type == DxsoRegisterType::Sampler && d.bound_to.num < 4)
        vs_samp_type[d.bound_to.num] = d.dcl.texture_type;
    }
  }
  for (uint32_t vslot = 0; vslot < 4; ++vslot) {
    auto *tex = refs.textures[16 + vslot].ptr();
    // A texture with no Metal backing (a SCRATCH / packed-YUV resource with a
    // null dxmt::Texture) has no view to resolve; treat it as unbound so the
    // dummy/zero arm below runs instead of dereferencing the null backing, the
    // same guard the fragment loop applies.
    if (!tex || !tex->dxmtTexture()) {
      bd.resolved_vert_texture_dxmt[vslot] = {};
      bd.resolved_vert_view[vslot] = 0;
      bd.resolved_vert_samplers[vslot] = 0;
      if (vs_samp_type[vslot] != DxsoTextureType::Unknown) {
        // Declared-but-unbound: type-correct opaque-black dummy + a sampler so
        // the vertex bind is complete and the VS reads (0, 0, 0, 1).
        WMTTextureType dummy_type = WMTTextureType2D;
        if (vs_samp_type[vslot] == DxsoTextureType::Texture3D)
          dummy_type = WMTTextureType3D;
        else if (vs_samp_type[vslot] == DxsoTextureType::TextureCube)
          dummy_type = WMTTextureTypeCube;
        bd.resolved_vert_textures[vslot] = dummyFragmentTexture(dummy_type);
        WMTSamplerInfo sinfo = sampler_info_from_d3d9_state(samp_states[16 + vslot]);
        if (auto sampler = getOrCreateSampler(sinfo))
          bd.resolved_vert_samplers[vslot] = sampler->sampler_state.handle;
      } else {
        bd.resolved_vert_textures[vslot] = 0;
      }
      continue;
    }
    const Rc<dxmt::Texture> &rc = tex->dxmtTexture();
    // Same per-format view derivation as the fragment sample-bind: without it a
    // VTF read of L8/A8L8/L16 (no luminance replicate), X8*/4444 (raw alpha /
    // permuted channels), ATI2 (swapped RG), V8U8/G16R16/2ch-float (.b=0 not 1),
    // R16F/R32F (.g/.b=0) or INTZ (no RRRR) samples the wrong channel shape.
    const DWORD *vsamp_row = samp_states[16 + vslot];
    uint64_t view = deriveSampleView(rc, tex, vsamp_row);
    obj_handle_t vh = rc->view(view).texture.handle;
    if (!vh) {
      view = rc->fullView;
      vh = rc->view(view).texture.handle;
    }
    bd.resolved_vert_view[vslot] = view;
    bd.resolved_vert_textures[vslot] = vh;
    bd.resolved_vert_texture_dxmt[vslot] = rc;
    // Same fp32 non-filterable degrade as the fragment path: an R32F
    // displacement map read with a LINEAR vertex sampler is undefined on AGX.
    // VTF shadow compare is never selected (classic vs_3_0 texldl), so keep
    // shadow_compare false.
    WMTSamplerInfo sinfo =
        sampler_info_from_d3d9_state(vsamp_row, /*shadow_compare=*/false, IsMetalNonFilterableFormat(tex->d3dFormat()));
    if (auto sampler = getOrCreateSampler(sinfo))
      bd.resolved_vert_samplers[vslot] = sampler->sampler_state.handle;
  }

  // ---- Read-only depth-stencil (sampled-while-bound) ----
  // A depth-aware post-process samples the INTZ depth while it is the depth
  // attachment. Apple GPUs leave attachment+sampler aliasing undefined UNLESS
  // there is no write to the attachment; with depth (and stencil) write off the
  // sample reads the stable prior contents. Bind it read-only in that case so
  // the pass is hazard-free, matching DXVK's GetDepthStencilView(false)
  // (UpdateActiveHazardsDS).
  //
  // A stage where the depth-stencil is bound but the shader never samples
  // it is a stale binding left over from an earlier pass, not a hazard.
  // Dropping it removes the aliasing outright, so a draw that writes depth
  // keeps writing it instead of degrading to read-only depth; the D3D9
  // runtime leaves such bindings in place and real content relies on the
  // driver ignoring them. Only a bytecode pixel shader carries a usage
  // mask: the fixed-function path samples per texture-stage state, so its
  // bindings are all treated as live.
  if (auto *dst = bd.resolved_ds_dxmt.ptr()) {
    const uint32_t ps_sampled = ffp_ps ? 0xffffu : ps->metadata().sampler_usage_mask;
    bool sampled = false;
    for (uint32_t st = 0; st < 16; ++st) {
      if (bd.resolved_frag_texture_dxmt[st].ptr() != dst)
        continue;
      if (!(ps_sampled & (1u << st))) {
        // Mirror the unbound-sampler path above: clear the tracked texture
        // so no read access is registered against the attachment, and bind
        // the placeholder rather than leaving the stale handle, which the
        // emit path would otherwise bind untracked. A clear usage bit means
        // the shader was compiled with no texture argument at this index,
        // so unlike that path the placeholder's type is unobservable.
        bd.resolved_frag_texture_dxmt[st] = {};
        bd.resolved_frag_textures[st] = dummyFragmentTexture(WMTTextureType2D);
        continue;
      }
      sampled = true;
    }
    // Vertex texture fetch reaches the same attachment through its own four
    // slots and takes the same treatment. A zero handle makes the emit path
    // skip the slot outright, so no placeholder is needed there.
    const uint32_t vs_sampled = ffp_vs ? 0xfu : vs->metadata().sampler_usage_mask;
    for (uint32_t vslot = 0; vslot < 4; ++vslot) {
      if (bd.resolved_vert_texture_dxmt[vslot].ptr() != dst)
        continue;
      if (!(vs_sampled & (1u << vslot))) {
        bd.resolved_vert_texture_dxmt[vslot] = {};
        bd.resolved_vert_textures[vslot] = 0;
        continue;
      }
      sampled = true;
    }
    bool depth_write = rs[D3DRS_ZENABLE] != D3DZB_FALSE && rs[D3DRS_ZWRITEENABLE];
    bool stencil_write = bd.resolved_ds_has_stencil && rs[D3DRS_STENCILENABLE] && rs[D3DRS_STENCILWRITEMASK] != 0;
    bd.resolved_ds_readonly = sampled && !depth_write && !stencil_write;
  }

  // ---- vbuf table from m_constRingResolve ----
  // Cache slot_mask + per-slot (base_addr, stride, length); ~80% hit
  // rate avoids ~800 allocates/frame.
  struct VbufEntry {
    uint64_t base_addr;
    uint32_t stride;
    uint32_t length;
  };
  uint32_t num_active = static_cast<uint32_t>(__builtin_popcount(bd.resolved_slot_mask));
  VbufEntry stage[D3D9_MAX_VERTEX_STREAMS];
  uint64_t stage_base[D3D9_MAX_VERTEX_STREAMS] = {};
  uint32_t stage_stride[D3D9_MAX_VERTEX_STREAMS] = {};
  uint32_t stage_length[D3D9_MAX_VERTEX_STREAMS] = {};
  uint32_t stage_i = 0;
  for (uint32_t slot = 0; slot < D3D9_MAX_VERTEX_STREAMS; ++slot) {
    if (!(bd.resolved_slot_mask & (1u << slot)))
      continue;
    VbufEntry &entry = stage[stage_i];
    entry = VbufEntry{};
    if (slot == 0 && bd.override_vb_buffer != 0) {
      entry.base_addr = bd.override_vb_addr;
      entry.stride = bd.override_vb_stride;
      entry.length = bd.override_vb_length;
    } else {
      auto *vb = refs.vertex_buffers[slot].ptr();
      if (!vb)
        return false;
      // gpu_address was frozen at BuildDrawCapture time; see the
      // freeze rationale there. Reading vb->gpuAddress() here would
      // pull the LIVE rename cursor and make all queued draws share
      // the latest slot.
      entry.base_addr = cap.vb_slots[slot].gpu_address + cap.vb_slots[slot].offset;
      entry.stride = cap.vb_slots[slot].stride;
      entry.length = vb->size();
    }
    // Per-slot keys for the comparison key (also packs nicely into
    // contiguous arrays for the cache miss path's update).
    stage_base[slot] = entry.base_addr;
    stage_stride[slot] = entry.stride;
    stage_length[slot] = entry.length;
    ++stage_i;
  }
  bool vbuf_cache_hit = resolve_cache.last_vbuf_slot_mask == bd.resolved_slot_mask;
  if (vbuf_cache_hit) {
    for (uint32_t slot = 0; slot < D3D9_MAX_VERTEX_STREAMS; ++slot) {
      if (!(bd.resolved_slot_mask & (1u << slot)))
        continue;
      if (resolve_cache.last_vbuf_base_addr[slot] != stage_base[slot] ||
          resolve_cache.last_vbuf_stride[slot] != stage_stride[slot] ||
          resolve_cache.last_vbuf_length[slot] != stage_length[slot]) {
        vbuf_cache_hit = false;
        break;
      }
    }
  }
  if (vbuf_cache_hit) {
    bd.resolved_vbuf_table_buffer = resolve_cache.last_vbuf_table_buffer;
    bd.resolved_vbuf_table_offset = resolve_cache.last_vbuf_table_offset;
  } else {
    auto vbuf_span =
        tryRingAllocate(m_constRingResolve, chunk_seq, chunk_coherent_id, sizeof(VbufEntry) * num_active, 256);
    if (!vbuf_span)
      return false;
    std::memcpy(vbuf_span.host, stage, sizeof(VbufEntry) * num_active);
    bd.resolved_vbuf_table_buffer = vbuf_span.handle;
    bd.resolved_vbuf_table_offset = vbuf_span.offset;
    resolve_cache.last_vbuf_slot_mask = bd.resolved_slot_mask;
    for (uint32_t slot = 0; slot < D3D9_MAX_VERTEX_STREAMS; ++slot) {
      resolve_cache.last_vbuf_base_addr[slot] = stage_base[slot];
      resolve_cache.last_vbuf_stride[slot] = stage_stride[slot];
      resolve_cache.last_vbuf_length[slot] = stage_length[slot];
    }
    resolve_cache.last_vbuf_table_buffer = bd.resolved_vbuf_table_buffer;
    resolve_cache.last_vbuf_table_offset = bd.resolved_vbuf_table_offset;
  }

  // ---- VS-resident handles per active stream ----
  // PER-DRAW: handle is from cap.vb_slots[slot].buffer (stable across
  // rename moves, but per-draw frozen). Lifetime: pin the VB wrapper
  // into the BatchedDraw so the underlying MTLBuffer survives through
  // Flushcommands; see BatchedDraw::resolved_vb_pins for the trap
  // this prevents. Only active streams (slot_mask bit set) are pinned;
  // unbound slots stay null.
  for (uint32_t slot = 0; slot < D3D9_MAX_VERTEX_STREAMS; ++slot) {
    if (!(bd.resolved_slot_mask & (1u << slot)))
      continue;
    if (slot == 0 && bd.override_vb_buffer != 0) {
      bd.resolved_vs_resident_handles[slot] = bd.override_vb_buffer;
    } else {
      bd.resolved_vs_resident_handles[slot] = cap.vb_slots[slot].buffer;
      // Carry the frozen allocation for this stream from cap (captured on
      // the calling thread from the same immediateName() read as the handle
      // above), so the emit registers a Vertex-stage read against the same
      // allocation the binding froze. Both map modes populate it now; only
      // override (UP) streams are ring-fed and leave it null.
      bd.resolved_vb_dxmt[slot] = cap.vb_slots[slot].alloc;
    }
    bd.resolved_vb_pins[slot] = refs.vertex_buffers[slot];
  }

  // ---- IB handle + offset ----
  // cap.ib_offset must be frozen (currentOffset reflects rename cursor).
  if (indexed) {
    if (bd.override_ib_buffer != 0) {
      bd.resolved_ib_handle = bd.override_ib_buffer;
      bd.resolved_ib_base_offset = bd.override_ib_offset;
    } else {
      bd.resolved_ib_handle = cap.ib_buffer;
      bd.resolved_ib_base_offset = cap.ib_offset;
      // Pin the IB wrapper through chunk lifetime; see resolved_vb_pins
      // for the trap.
      bd.resolved_ib_pin = refs.index_buffer;
      // Frozen allocation for the IB from cap (either map mode); null only
      // for an override (UP) index buffer. Same rationale as the vertex
      // stream above.
      bd.resolved_ib_dxmt = cap.ib_alloc;
    }
  }

  // Relative c# reads (c[a0.x + n]) drive two things. The upload extent:
  // the read can land on any register, so the whole float file uploads
  // (resolve_const_f_extent below), matching DXVK's maxConstIndexF =
  // floatCount rule; a direct-addressing shader stays on the app's sticky
  // reach. And def-stamping: a relative read bypasses the compiler's
  // def-baked literals, so a shader that also carries defs must have its def
  // values present in the uploaded CB. Native seeds the register file with
  // defs; DXVK re-applies them over the app state on every upload
  // (needsConstantCopies), defs winning over Set values. A shader that
  // addresses relatively without any def still needs the widened extent
  // (software skinning Sets the bone matrices rather than def'ing them).
  const bool vs_uses_relative = !ffp_vs && vs->metadata().uses_relative_const;
  const bool ps_uses_relative = ps && ps->metadata().uses_relative_const;
  const bool vs_needs_defs = vs_uses_relative && !vs->metadata().consts.empty();
  const bool ps_needs_defs = ps_uses_relative && !ps->metadata().consts.empty();
  // The FFP axis must key this cache: sub-buffer[0]'s content depends on
  // whether the vertex stage is generated (WVP rows) or bytecode (the c#
  // register file), and a SetVertexShader ref op does not rotate the pod
  // snapshot, so a shader<->fixed-function transition inside one cluster
  // would otherwise reuse the other kind's upload verbatim. A
  // relative-addressing shader also keys here: its upload differs from a
  // plain sticky one (full-file extent, and def-stamping when it has defs),
  // so it must not reuse a direct-addressing shader's narrower bytes for the
  // same pod.
  static const char kFfpConstKey = 0;
  const void *vs_defs_key =
      !ffp_vs ? (vs_uses_relative ? static_cast<const void *>(vs) : nullptr) : static_cast<const void *>(&kFfpConstKey);
  // The pixel side needs the same treatment now that sub-buffer[3]
  // carries the combiner constants for a generated PS.
  const void *ps_defs_key =
      ps ? (ps_uses_relative ? static_cast<const void *>(ps) : nullptr) : static_cast<const void *>(&kFfpConstKey);

  // Reuse prior draw's uploads if pod_snapshot pointer equals (implies byte-equality)
  // and the def-stamping shaders match. VS/PS constants, clip planes
  // live on pod. ~80% hit rate skips 8 KB memcpy+mutex.
  // ffp_texcoord_width (decl-derived, feeds the FFP texture-matrix fold) and the
  // DS-bound bit (gates the POSITIONT z remap) are NOT on the pod snapshot, so a
  // SetVertexDeclaration / SetDepthStencilSurface between two same-pod draws
  // would otherwise reuse stale bytes; key them here. ffp_texcoord_width stays
  // its default for non-FFP draws, so this is free of extra misses there.
  uint32_t ffp_tcw_key = 0;
  for (int i = 0; i < 8; ++i)
    ffp_tcw_key |= (ffp_texcoord_width[i] & 0xFu) << (i * 4);
  const bool ds_bound = bd.resolved_ds_handle != 0;
  if (bd.pod_snapshot == const_cache.pod_ptr && const_cache.pod_ptr != nullptr &&
      const_cache.vs_defs_key == vs_defs_key && const_cache.ps_defs_key == ps_defs_key &&
      const_cache.ffp_texcoord_width_key == ffp_tcw_key && const_cache.ds_bound == ds_bound &&
      const_cache.pos_transformed == bd.resolved_position_transformed) {
    bd.resolved_const_uploads = const_cache.uploads;
  } else {
    // Pack into ONE m_constRingResolve.allocate per draw; the ring is mutex-
    // guarded, so 8 separate calls cost 8 lock/unlock cycles x 10k draws
    // = 80k mutex pairs on the encode thread per frame. Each sub-section
    // is 256-byte-aligned (Metal setBuffer offset alignment requirement
    // on this GPU family, Mac2), so the packed layout is mathematically
    // equivalent to 8 separate allocs with the wasted alignment padding.
    auto pack_bool_bits = [](const BOOL *bv, unsigned count) -> uint32_t {
      uint32_t bits = 0;
      for (unsigned i = 0; i < count; ++i)
        if (bv[i])
          bits |= (1u << i);
      return bits;
    };
    float packed_clip_planes[8][4] = {};
    // D3DRS_CLIPPING gates the whole user-clip-plane mask: when an app
    // disables clipping wholesale (some 2000s engines toggle it off around
    // skybox/HUD passes) any stale enabled planes stop clipping, matching
    // wined3d (args.clip_planes = rs[CLIPPING] ? rs[CLIPPLANEENABLE] : 0,
    // stateblock.c). DXVK ignores CLIPPING; the primary reference gates it.
    // CLIPPING defaults TRUE, so the common path is unchanged.
    uint32_t plane_enable = rs[D3DRS_CLIPPING] != FALSE ? rs[D3DRS_CLIPPLANEENABLE] : 0;
    uint32_t clip_count = 0;
    // A programmable vertex shader clips against the clip-space position, where
    // the raw plane is correct. A non-POSITIONT FFP draw transforms the vertex
    // by world*view*projection, and D3D9 defines its clip planes in world space
    // (MSDN + DXVK), so transform each plane by (View*Projection)^-1 (built at
    // record time) to make the generated shader's clip-space dot equal the
    // world-space dot (see d3d9_matrix.hpp). A POSITIONT (XYZRHW) FFP draw feeds
    // an already-projected position through a window->clip remap, not world*VP,
    // so the transform does not apply there: pack the raw plane. pos_transformed
    // is keyed into the const cache above so the two never share a packing.
    const bool ffp_world_clip = ffp_vs && !bd.resolved_position_transformed;
    D3DMATRIX ffp_vp_inv;
    if (ffp_world_clip)
      std::memcpy(&ffp_vp_inv, pod.ffp_vp_inv, sizeof(ffp_vp_inv));
    for (uint32_t i = 0; i < 8; ++i) {
      if (!(plane_enable & (1u << i)))
        continue;
      if (ffp_world_clip)
        transform_clip_plane(ffp_vp_inv, pod.clip_planes[i], packed_clip_planes[clip_count]);
      else
        std::memcpy(packed_clip_planes[clip_count], pod.clip_planes[i], sizeof(float) * 4);
      ++clip_count;
    }
    uint32_t vs_b_bits = pack_bool_bits(pod.vs_const_B, D3D9_MAX_VS_CONST_B);
    uint32_t ps_b_bits = pack_bool_bits(pod.ps_const_B, D3D9_MAX_PS_CONST_B);
    // Per-draw PS data sharing buffer(2), the shared PS uniform tail (DXVK's
    // D3D9SharedPS equivalent): bool bits at byte 0, D3DRS_FOGCOLOR as float4
    // rgba at byte 16, sampler LOD biases as float[16] at byte 32, table-fog
    // params (FOGSTART/FOGEND/FOGDENSITY) as float[3] at byte 96, the SM1.x
    // projected-texturing mask at uint32 index 27 (byte 108), the normalised
    // alpha ref (D3DRS_ALPHAREF / 255) at index 28 (byte 112), the raw 32-bit
    // D3DRS_MULTISAMPLEMASK coverage mask at index 29 (byte 116), and the
    // per-stage TexBem bump-env block at index 32 (byte 128): six floats per
    // stage {mat00, mat01, mat10, mat11, lscale, loffset}, eight stages
    // (indices 30/31 are free). The PS reads the fog params at uint32 index
    // 24/25/26, the projected mask at 27, the alpha ref at 28, the sample mask
    // at 29 and bump-env at 32 + stage*6; keep this layout in lockstep with
    // dxso_compile.cpp's load_blob_f / alpha-test / sample-mask / texbem and
    // ffp_compile.cpp's alpha-test / sample-mask offsets. Everything here is derived
    // purely from the pod so a const-cache entry shared across draws carries
    // the right values. The 256-byte sub-allocation alignment absorbs the
    // growth.
    uint32_t ps_b_blob[80] = {};
    ps_b_blob[0] = ps_b_bits;
    {
      DWORD fog_c = pod.render_states[D3DRS_FOGCOLOR];
      float *fc = reinterpret_cast<float *>(&ps_b_blob[4]);
      fc[0] = static_cast<float>((fog_c >> 16) & 0xFF) / 255.0f;
      fc[1] = static_cast<float>((fog_c >> 8) & 0xFF) / 255.0f;
      fc[2] = static_cast<float>(fog_c & 0xFF) / 255.0f;
      fc[3] = static_cast<float>((fog_c >> 24) & 0xFF) / 255.0f;
    }
    {
      // D3DSAMP_MIPMAPLODBIAS stores raw float bits; the translated PS
      // applies the value at each sample site because Metal samplers
      // carry no LOD bias (the d3d11 path routes the same value through
      // its argument buffer). The clamp bounds the GPU-side bias against
      // garbage app values; D3D9 defines no range and DXVK only bounds
      // it by its fixed-point sampler-key encoding.
      // This blob carries the 16 pixel-sampler biases only. The vertex
      // samplers (16..19) get no bias: extending it would need a parallel
      // vertex-side constant blob plus a bias add at the VS texldl site in
      // dxso_compile (Metal samplers cannot carry the bias, so the sampler-
      // object route is closed). Not covered by a test: it needs a mipmapped
      // vertex texture sampled by a vs_3_0 shader with a non-zero bias, a
      // rare heightfield-LOD pattern, and DXVK/GL apply the bias to explicit-
      // lod VTF ops, so this is a low-severity gap not worth the uniform-
      // layout churn today.
      float *biases = reinterpret_cast<float *>(&ps_b_blob[8]);
      for (uint32_t i = 0; i < 16; ++i) {
        uint32_t raw = static_cast<uint32_t>(pod.sampler_states[i][D3DSAMP_MIPMAPLODBIAS]);
        float b;
        std::memcpy(&b, &raw, sizeof(b));
        biases[i] = std::isfinite(b) ? std::clamp(b, -15.0f, 15.0f) : 0.0f;
      }
    }
    {
      // Table-fog params; D3DRS_FOGSTART/FOGEND/FOGDENSITY store raw
      // float bits. Only the table-fog PS variants read these (the
      // vertex-fog and no-fog variants ignore the slots), so the values
      // are written unconditionally and cost nothing when unused.
      float *fog_params = reinterpret_cast<float *>(&ps_b_blob[24]);
      const DWORD raw[3] = {
          pod.render_states[D3DRS_FOGSTART],
          pod.render_states[D3DRS_FOGEND],
          pod.render_states[D3DRS_FOGDENSITY],
      };
      std::memcpy(fog_params, raw, sizeof(raw));
    }
    {
      // SM1.0-1.3 projected-texturing mask at uint32 index 27: bit s set
      // when stage s has D3DTSS_TEXTURETRANSFORMFLAGS & D3DTTFF_PROJECTED.
      // The pre-1.4 pixel shader reads this per sampler and divides the
      // texcoord by w at each projected stage; higher shader models never
      // compile that path, so the mask is inert for them. DXVK derives the
      // same per-sampler projected spec constant from this flag.
      uint32_t projected = 0;
      for (uint32_t s = 0; s < dxmt::D9ES_MAX_TEXTURE_STAGES; ++s) {
        if (pod.texture_stage_states[s][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED)
          projected |= 1u << s;
      }
      ps_b_blob[27] = projected;
    }
    {
      // Alpha ref at uint32 index 28, normalised to the fragment alpha range
      // (/255) the way the discard compare expects. Written unconditionally
      // (the shader only reads it when the compare FUNC is active); the
      // normalisation matches the immediate a specialised codegen would bake,
      // so a fixed ref renders identically. wined3d/DXVK normalise the same way.
      float ref = static_cast<float>(pod.render_states[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
      std::memcpy(&ps_b_blob[28], &ref, sizeof(ref));
    }
    {
      // D3DRS_MULTISAMPLEMASK at uint32 index 29: the raw 32-bit coverage
      // bitmask (bit s enables sample s). Written unconditionally (cheap, like
      // the fog params); the coverage-emitting PS variant reads it and ANDs it
      // into hardware coverage, and every other PS never reads the slot. The
      // enable bit gating that variant is resolved from the sample count at
      // draw time, so a mask left at the all-ones default is inert.
      ps_b_blob[29] = pod.render_states[D3DRS_MULTISAMPLEMASK];
    }
    {
      // Per-stage TexBem bump-env: the 2x2 matrix (bm00, bm01, bm10, bm11)
      // then the TexBemL luminance scale + offset, six floats per stage from
      // uint32 index 32. Written unconditionally from the pod (raw float bits
      // in the DWORD TSS slots) so the buffer stays a pure function of the
      // const-cache key; a non-TexBem PS simply never reads it. Same values,
      // lane order and stage indexing the generated combiner already reads
      // from its own buffer(0) block, and that DXVK's D3D9SharedPS carries.
      float *bem = reinterpret_cast<float *>(&ps_b_blob[32]);
      for (uint32_t s = 0; s < 8; ++s) {
        const DWORD *tss = pod.texture_stage_states[s];
        const DWORD raw[6] = {
            tss[D3DTSS_BUMPENVMAT00], tss[D3DTSS_BUMPENVMAT01],  tss[D3DTSS_BUMPENVMAT10],
            tss[D3DTSS_BUMPENVMAT11], tss[D3DTSS_BUMPENVLSCALE], tss[D3DTSS_BUMPENVLOFFSET],
        };
        std::memcpy(bem + s * 6, raw, sizeof(raw));
      }
    }

    constexpr size_t kSubAlign = 256;
    auto align_up = [](size_t v, size_t a) { return (v + a - 1) & ~(a - 1); };
    // Clamp vs/ps const_F to the app's sticky high-water mark (set in
    // Set{Vertex,Pixel}ShaderConstantF). Typical workloads touch ~30 VS /
    // ~20 PS registers against the 256/224 maxes; full-extent uploads are
    // mostly memcpying zeros. DXVK's maxChangedConstF is the literal
    // reference. A relative-addressing shader instead uploads the whole
    // file (resolve_const_f_extent), which also covers its def'd registers
    // for the stamping below. Floor at 16 bytes so a never-Set'd register
    // file still emits a non-zero allocation (some PSO bindings declare a
    // CB even when the shader doesn't read it, and AGX rejects zero-size
    // buffer binds at setVertexBuffer time).
    // On a software / mixed-VP device the file spans past 256 when a bound
    // relative-addressing shader captured the extended constants; the whole
    // file uploads so a c[a0.x] read can reach it (the compiled shader's
    // reladdr clamp matches this size). Otherwise the file is the hardware
    // 256 and the hot path is unchanged.
    const uint32_t vs_file_size =
        pod.vs_const_F_overflow ? (D3D9_MAX_VS_CONST_F + pod.vs_const_F_overflow_count) : D3D9_MAX_VS_CONST_F;
    uint32_t vs_f_regs = resolve_const_f_extent(pod.vs_const_f_max, vs_uses_relative, vs_file_size);
    uint32_t ps_f_regs = resolve_const_f_extent(pod.ps_const_f_max, ps_uses_relative, D3D9_MAX_PS_CONST_F);
    // A generated vertex shader reads the fixed-function uniforms block
    // through this slot; size it for the full layout contract (matrix,
    // fog, point-scale and lighting blocks) rather than the register
    // high-water mark, which is zero for fixed-function draws.
    constexpr size_t kFfpVsUniformBytes = 256 + 8 * 112 + 8 * 64 + 3 * 64 + 3 * 48 + 3 * 16;
    const size_t vs_const_f_bytes =
        ffp_vs ? kFfpVsUniformBytes : (vs_f_regs ? static_cast<size_t>(vs_f_regs) * 16u : 16u);
    // The generated pixel shader's combiner constants (texture factor
    // plus eight stage colors) ride this slot the same way.
    const size_t ps_const_f_bytes = !ps ? 25u * 16u : (ps_f_regs ? static_cast<size_t>(ps_f_regs) * 16u : 16u);
    // Pre-transform viewport remap: two float4 (invExtent, invOffset) packed
    // from the live D3D9 viewport, matching DXVK's HasPositionT constants. The
    // POSITIONT VS variant reads this at loc 5 and applies
    // pos = pos*invExtent + invOffset then the rhw divide. Computed for every
    // draw (it depends only on viewport + ztest, both on the pod the const
    // cache keys on) so a cache entry shared by a non-transformed and a
    // transformed draw still carries the right remap; only POSITIONT draws bind
    // it (the emit gates on resolved_position_transformed).
    float vp_remap[8] = {};
    {
      const float vpW = static_cast<float>(pod.viewport.Width ? pod.viewport.Width : 1u);
      const float vpH = static_cast<float>(pod.viewport.Height ? pod.viewport.Height : 1u);
      const float vpX = static_cast<float>(pod.viewport.X);
      const float vpY = static_cast<float>(pod.viewport.Y);
      // Z passes through to clip space when the depth test is actually live (a
      // DS is bound AND D3DRS_ZENABLE), matching DXVK IsZTestEnabled and dxmt's
      // own depth_stencil_info gate, OR when table (pixel) fog is active: table
      // fog reads the per-fragment device z, so zeroing it here would strip the
      // fog from a pre-transformed draw with the depth test off. Otherwise the
      // Z is forced to 0 so untested UI is never depth-clipped. A pre-transformed
      // z is device depth in [0,1], so passing it through never clips a valid
      // vertex, and with the depth test off nothing writes it back.
      // The same predicate the fog-mode resolve above uses to select table fog.
      const bool table_fog =
          rs[D3DRS_FOGENABLE] != FALSE && (ffp_ps || ps->metadata().major < 3) && rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE;
      const float zt = ((bd.resolved_ds_handle != 0 && rs[D3DRS_ZENABLE] != D3DZB_FALSE) || table_fog) ? 1.0f : 0.0f;
      vp_remap[0] = 2.0f / vpW; // invExtent
      vp_remap[1] = -2.0f / vpH;
      vp_remap[2] = zt;
      vp_remap[3] = 1.0f;
      vp_remap[4] = -2.0f * vpX / vpW - 1.0f; // invOffset = (-X,-Y,0,0)*invExtent + (-1,1,0,0)
      vp_remap[5] = 2.0f * vpY / vpH + 1.0f;
      vp_remap[6] = 0.0f;
      vp_remap[7] = 0.0f;
    }
    // Point-size uniform (float4 = size, min, max) bound to VS buffer 6 for
    // an injecting point draw. Derived purely from the pod render states, so
    // a const-upload cache entry shared by a point and a non-point draw
    // carries the right values; only injecting point draws bind it. The
    // programmable VS clamps the size against these bounds the way DXVK's
    // GetPointSizeInfoVS does, so one variant serves every point size.
    float point_params[4] = {};
    {
      dxmt::D3D9PointSizeParams p =
          dxmt::compute_point_size_params(rs[D3DRS_POINTSIZE], rs[D3DRS_POINTSIZE_MIN], rs[D3DRS_POINTSIZE_MAX]);
      point_params[0] = p.size;
      point_params[1] = p.min;
      point_params[2] = p.max;
    }
    const size_t sz[10] = {
        vs_const_f_bytes,  sizeof(pod.vs_const_I),     sizeof(uint32_t), ps_const_f_bytes, sizeof(pod.ps_const_I),
        sizeof(ps_b_blob), sizeof(packed_clip_planes), sizeof(uint32_t), sizeof(vp_remap), sizeof(point_params),
    };
    size_t sub_off[10];
    size_t total = 0;
    for (uint32_t i = 0; i < 10; ++i) {
      sub_off[i] = total;
      total = align_up(total + sz[i], kSubAlign);
    }
    auto span = tryRingAllocate(m_constRingResolve, chunk_seq, chunk_coherent_id, total, kSubAlign);
    if (!span)
      return false;
    const uint64_t base_off = span.offset;
    char *base = static_cast<char *>(span.host);
    // A fixed-function VS reads the ffp_uniforms block through the same
    // buffer(0) binding the register file uses; the world*view*projection
    // rows land at the sub-buffer head and the rest of the slot is inert.
    static_assert(
        256 + 8 * 112 <= sizeof(pod.vs_const_F), "the ffp uniforms payload must fit the vertex constant sub-buffer"
    );
    if (ffp_vs) {
      // Layout contract with compile_ffp_vs: float4 0..3 = wvp rows,
      // float4 4 = world*view z column, float4 5 = fog start/end/density.
      char *ffp_base = base + sub_off[0];
      std::memcpy(ffp_base, pod.ffp_wvp, sizeof(pod.ffp_wvp));
      std::memcpy(ffp_base + sizeof(pod.ffp_wvp), pod.ffp_wv_z, sizeof(pod.ffp_wv_z));
      const DWORD fog_raw[3] = {
          pod.render_states[D3DRS_FOGSTART],
          pod.render_states[D3DRS_FOGEND],
          pod.render_states[D3DRS_FOGDENSITY],
      };
      std::memcpy(ffp_base + sizeof(pod.ffp_wvp) + sizeof(pod.ffp_wv_z), fog_raw, sizeof(fog_raw));
      // Point-scale block (layout contract float4 6..9): the world*view
      // x and y columns, the scale factors with the viewport height,
      // and the raw size with its clamp bounds.
      std::memcpy(ffp_base + 96, pod.ffp_wv_x, sizeof(pod.ffp_wv_x));
      std::memcpy(ffp_base + 112, pod.ffp_wv_y, sizeof(pod.ffp_wv_y));
      float scale_blk[8] = {};
      std::memcpy(&scale_blk[0], &pod.render_states[D3DRS_POINTSCALE_A], sizeof(float));
      std::memcpy(&scale_blk[1], &pod.render_states[D3DRS_POINTSCALE_B], sizeof(float));
      std::memcpy(&scale_blk[2], &pod.render_states[D3DRS_POINTSCALE_C], sizeof(float));
      scale_blk[3] = static_cast<float>(pod.viewport.Height);
      std::memcpy(&scale_blk[4], &pod.render_states[D3DRS_POINTSIZE], sizeof(float));
      float mn, mx;
      std::memcpy(&mn, &pod.render_states[D3DRS_POINTSIZE_MIN], sizeof(float));
      std::memcpy(&mx, &pod.render_states[D3DRS_POINTSIZE_MAX], sizeof(float));
      // Raw minimum (a negative pulled to 0), matching compute_point_size_params
      // so both vertex paths clamp identically; a size clamped to 0 draws nothing.
      scale_blk[5] = std::isfinite(mn) ? (mn > 0.0f ? mn : 0.0f) : 1.0f;
      scale_blk[6] = std::isfinite(mx) && mx >= 1.0f ? (mx > 511.0f ? 511.0f : mx) : 511.0f;
      std::memcpy(ffp_base + 128, scale_blk, sizeof(scale_blk));
      // Lighting block (layout contract float4 10..15 + 7 per light):
      // material colors, power and the packed light count, the global
      // ambient, then the host-packed enabled lights.
      std::memcpy(ffp_base + 160, pod.ffp_material, 64);
      float misc[4] = {pod.ffp_material[16], static_cast<float>(pod.ffp_light_count), 0.f, 0.f};
      std::memcpy(ffp_base + 224, misc, sizeof(misc));
      float amb[4];
      {
        DWORD a = pod.render_states[D3DRS_AMBIENT];
        amb[0] = static_cast<float>((a >> 16) & 0xFF) / 255.0f;
        amb[1] = static_cast<float>((a >> 8) & 0xFF) / 255.0f;
        amb[2] = static_cast<float>(a & 0xFF) / 255.0f;
        amb[3] = static_cast<float>((a >> 24) & 0xFF) / 255.0f;
      }
      std::memcpy(ffp_base + 240, amb, sizeof(amb));
      for (uint32_t li = 0; li < pod.ffp_light_count; ++li) {
        const D3DLIGHT9 *lp = reinterpret_cast<const D3DLIGHT9 *>(pod.ffp_lights[li]);
        float blk[28] = {};
        std::memcpy(&blk[0], &lp->Diffuse, 16);
        std::memcpy(&blk[4], &lp->Specular, 16);
        std::memcpy(&blk[8], &lp->Ambient, 16);
        blk[12] = lp->Position.x;
        blk[13] = lp->Position.y;
        blk[14] = lp->Position.z;
        blk[15] = lp->Range;
        blk[16] = lp->Direction.x;
        blk[17] = lp->Direction.y;
        blk[18] = lp->Direction.z;
        blk[19] = static_cast<float>(lp->Type);
        blk[20] = lp->Attenuation0;
        blk[21] = lp->Attenuation1;
        blk[22] = lp->Attenuation2;
        blk[23] = lp->Falloff;
        blk[24] = std::cos(lp->Theta * 0.5f);
        blk[25] = std::cos(lp->Phi * 0.5f);
        std::memcpy(ffp_base + 256 + li * 112, blk, sizeof(blk));
      }
      // Texture matrices (layout contract float4 72 + 4 per stage),
      // preprocessed the way wined3d's compute_texture_matrix does: the
      // attribute-width column copy (a fetched coordinate pads with
      // 0, 0, 1, so the coefficients expecting a 1 move to the fourth
      // column), zeroing past the count, and the projected divisor
      // copied into w for the fragment-side divide.
      for (uint32_t s = 0; s < 8; ++s) {
        float m[16];
        std::memcpy(m, pod.ffp_tex_mats[s], sizeof(m));
        DWORD ttf = pod.texture_stage_states[s][D3DTSS_TEXTURETRANSFORMFLAGS];
        // Count is the flags with only the PROJECTED bit removed (wined3d
        // compute_texture_matrix), so any high-bit garbage falls to the
        // identity arm rather than aliasing a 2..4 count.
        uint32_t count = ttf & ~static_cast<DWORD>(D3DTTFF_PROJECTED);
        // Generated coordinates carry three components into the matrix
        // (wined3d get_texture_matrix passes attrib_count 3 for them). The
        // coordinate index clamps to the last texcoord set (wined3d
        // min(index, WINED3D_MAX_FFP_TEXTURES - 1)) rather than wrapping.
        uint32_t coord_idx = pod.texture_stage_states[s][D3DTSS_TEXCOORDINDEX] & 0xFFFFu;
        uint32_t aw = ((pod.texture_stage_states[s][D3DTSS_TEXCOORDINDEX] >> 16) & 0xFFFFu)
                          ? 3u
                          : ffp_texcoord_width[coord_idx > 7u ? 7u : coord_idx];
        auto row = [&](uint32_t r) { return m + r * 4; };
        if (count < 2 || count > 4) {
          std::memset(m, 0, sizeof(m));
          m[0] = m[5] = m[10] = 1.0f;
          m[15] = aw < 4 ? 0.0f : 1.0f;
          if (ttf & D3DTTFF_PROJECTED) {
            if (aw >= 1 && aw <= 3)
              row(aw - 1)[3] = 1.0f;
          }
        } else {
          if (aw == 1 || aw == 2)
            std::memcpy(row(3), row(aw), 4 * sizeof(float));
          if (count < 4)
            for (uint32_t r = 0; r < 4; ++r)
              row(r)[3] = 0.0f;
          if (count < 3)
            for (uint32_t r = 0; r < 4; ++r)
              row(r)[2] = 0.0f;
          if (ttf & D3DTTFF_PROJECTED) {
            if (count == 2)
              for (uint32_t r = 0; r < 4; ++r)
                row(r)[3] = row(r)[1];
            else if (count == 3)
              for (uint32_t r = 0; r < 4; ++r)
                row(r)[3] = row(r)[2];
          }
        }
        std::memcpy(ffp_base + 1152 + s * 64, m, sizeof(m));
      }
      // Vertex-blend companions (layout contract float4 104 + 4 per
      // extra matrix): world matrices 1..3 folded with view*projection.
      std::memcpy(ffp_base + 1664, pod.ffp_wvp_blend, sizeof(pod.ffp_wvp_blend));
      // Vertex-blend eye-space columns (layout contract float4 116 + 3 per
      // extra matrix): world matrices 1..3 folded with view only, so the
      // shader blends the eye position and normal across the same matrices
      // the clip position uses.
      std::memcpy(ffp_base + 1856, pod.ffp_wv_blend, sizeof(pod.ffp_wv_blend));
      // Inverse-transpose normal matrix (layout contract float4 125..127):
      // the x/y/z rows of inverse(matrix-0 world*view) for the eye normal.
      std::memcpy(ffp_base + 2000, pod.ffp_normal, sizeof(pod.ffp_normal));
    } else {
      // Hot registers (< 256) from the POD shadow; extended registers (>= 256)
      // from the captured overflow snapshot. sz[0] is the sub-buffer size the
      // extent already computed: when it stays within the 256 shadow (every
      // hardware-VP draw) this is the same single memcpy as before.
      const size_t hot_bytes = std::min<size_t>(sz[0], sizeof(pod.vs_const_F));
      std::memcpy(base + sub_off[0], pod.vs_const_F, hot_bytes);
      if (sz[0] > sizeof(pod.vs_const_F)) {
        char *ext_dst = base + sub_off[0] + sizeof(pod.vs_const_F);
        const size_t ext_bytes = sz[0] - sizeof(pod.vs_const_F);
        const size_t have_bytes =
            pod.vs_const_F_overflow ? static_cast<size_t>(pod.vs_const_F_overflow_count) * 16u : 0u;
        const size_t copy = std::min(ext_bytes, have_bytes);
        if (copy)
          std::memcpy(ext_dst, pod.vs_const_F_overflow, copy);
        if (copy < ext_bytes)
          std::memset(ext_dst + copy, 0, ext_bytes - copy);
      }
    }
    std::memcpy(base + sub_off[1], pod.vs_const_I, sz[1]);
    std::memcpy(base + sub_off[2], &vs_b_bits, sz[2]);
    if (!ps) {
      // Layout contract with the generated PS's combiner: float4 0 =
      // D3DRS_TEXTUREFACTOR, float4 1..8 = the per-stage D3DTSS_CONSTANT
      // colors, both unpacked from their D3DCOLOR words.
      float ps_consts[100] = {};
      auto unpack = [](DWORD c, float *dst) {
        dst[0] = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
        dst[1] = static_cast<float>((c >> 8) & 0xFF) / 255.0f;
        dst[2] = static_cast<float>(c & 0xFF) / 255.0f;
        dst[3] = static_cast<float>((c >> 24) & 0xFF) / 255.0f;
      };
      unpack(pod.render_states[D3DRS_TEXTUREFACTOR], &ps_consts[0]);
      for (uint32_t s = 0; s < 8; ++s)
        unpack(pod.texture_stage_states[s][D3DTSS_CONSTANT], &ps_consts[4 + s * 4]);
      // Bump-env constants (float4 9..16 the 2x2 matrices, 17..24 the
      // luminance scale and offset pairs); the stage states store raw
      // float bits in their DWORD slots.
      for (uint32_t s = 0; s < 8; ++s) {
        std::memcpy(&ps_consts[36 + s * 4], &pod.texture_stage_states[s][D3DTSS_BUMPENVMAT00], 2 * sizeof(float));
        std::memcpy(&ps_consts[38 + s * 4], &pod.texture_stage_states[s][D3DTSS_BUMPENVMAT10], 2 * sizeof(float));
        std::memcpy(&ps_consts[68 + s * 4], &pod.texture_stage_states[s][D3DTSS_BUMPENVLSCALE], sizeof(float));
        std::memcpy(&ps_consts[69 + s * 4], &pod.texture_stage_states[s][D3DTSS_BUMPENVLOFFSET], sizeof(float));
      }
      static_assert(sizeof(ps_consts) <= sizeof(pod.ps_const_F), "");
      std::memcpy(base + sub_off[3], ps_consts, sizeof(ps_consts));
    } else
      std::memcpy(base + sub_off[3], pod.ps_const_F, sz[3]);
    std::memcpy(base + sub_off[4], pod.ps_const_I, sz[4]);
    std::memcpy(base + sub_off[5], ps_b_blob, sz[5]);
    std::memcpy(base + sub_off[6], packed_clip_planes, sz[6]);
    std::memcpy(base + sub_off[7], &clip_count, sz[7]);
    std::memcpy(base + sub_off[8], vp_remap, sz[8]);
    std::memcpy(base + sub_off[9], point_params, sz[9]);
    // Stamp def'd float constants over the app state (see vs_needs_defs
    // above for the relative-addressing contract).
    auto apply_defs = [](char *dst, const DxsoShaderMetadata &md, uint32_t reg_count) {
      for (const auto &c : md.consts) {
        if (c.def.kind != DxsoDefKind::Float32 || c.bound_to.type != DxsoRegisterType::Const)
          continue;
        if (c.bound_to.num >= reg_count)
          continue;
        std::memcpy(dst + static_cast<size_t>(c.bound_to.num) * 16u, c.def.payload.f32, 16u);
      }
    };
    if (vs_needs_defs)
      apply_defs(base + sub_off[0], vs->metadata(), vs_f_regs);
    if (ps_needs_defs)
      apply_defs(base + sub_off[3], ps->metadata(), ps_f_regs);
    const obj_handle_t buf = span.handle;
    for (uint32_t i = 0; i < 10; ++i) {
      bd.resolved_const_uploads[i].buffer = buf;
      bd.resolved_const_uploads[i].offset = base_off + sub_off[i];
    }
    const_cache.pod_ptr = bd.pod_snapshot;
    const_cache.vs_defs_key = vs_defs_key;
    const_cache.ps_defs_key = ps_defs_key;
    const_cache.ffp_texcoord_width_key = ffp_tcw_key;
    const_cache.ds_bound = ds_bound;
    const_cache.pos_transformed = bd.resolved_position_transformed;
    const_cache.uploads = bd.resolved_const_uploads;
  }

  return true;
}

HRESULT
MTLD3D9Device::FlushDrawBatch() {
  if (m_pendingOps.empty())
    return D3D_OK;

  // Post pending-clear + AUTOGENMIPMAP BEFORE op-stream emit (EMIT-order within chunk).
  // Without this, pending clear drops silently when all draws fail Resolve.
  flushOpenWork();

  // Snapshot seq + coherent_id at chunk-push time so the encode-side
  // m_constRingResolve.allocate inside Resolve uses THIS chunk's seq (we
  // ++m_currentCmdSeq below, before the encode thread necessarily
  // begins draining this chunk; reading this->m_currentCmdSeq from
  // the encode lambda would observe the bumped-up NEXT-chunk value).
  uint64_t chunk_seq = m_currentCmdSeq;
  uint64_t chunk_coherent_id = m_cachedSignaled.load(std::memory_order_acquire);

  // Pre-reserve next frame capacity. std::move leaves capacity 0,
  // causing geometric growth (~5-10 MB churn/frame without this).
  size_t prev_ops_size = m_pendingOps.size();
  size_t prev_draws_size = m_pendingDraws.size();
  size_t prev_blits_size = m_pendingBlits.size();
  size_t prev_refops_size = m_pendingRefOps.size();
  auto *chunk = m_dxmtQueue->CurrentChunk();
  chunk->emitcc([this, ops = std::move(m_pendingOps), draws = std::move(m_pendingDraws),
                 blits = std::move(m_pendingBlits), ref_ops = std::move(m_pendingRefOps), chunk_seq,
                 chunk_coherent_id](ArgumentEncodingContext &ctx) mutable {
    // Single chunk-level autorelease pool. Wine's encode worker has
    // no outer NSAutoreleasePool; one pool here covers every autoreleased
    // Metal temporary produced by
    // Resolve + the op-stream walk for this chunk.
    auto pool = WMT::MakeAutoreleasePool();
    ConstUploadCache const_cache;
    ResolveCache resolve_cache;

    // Op-stream walker. Iterates m_pendingOps in arrival order;
    // dispatches each Draw through Resolve + EmitCommonRenderSetup +
    // EmitDrawCommand, and each Blit through EmitBlitOp. Kind
    // transitions close the prior encoder (endPass) before opening
    // the next one (startRenderPass / startBlitPass). Same shape as
    // d3d11_context_impl.cpp's EmitOP queue + wine cs.c's
    // WINED3D_CS_OP_* dispatcher.
    if (ops.empty())
      return;

    D9PassKind pass = D9PassKind::None;
    ChunkEmitState s{};
    const BatchedDraw *prev_draw = nullptr;
    auto end_current_pass = [&]() {
      if (pass != D9PassKind::None) {
        ctx.endPass();
        pass = D9PassKind::None;
        prev_draw = nullptr;
      }
    };

    for (auto &ref : ops) {
      if (ref.kind == PendingOpRef::SetRef) {
        // Apply the ref-state mutation onto the persistent encode-side
        // mirror BEFORE any subsequent Draw reads it. The walker is the
        // sole writer of m_encodeSideRefs; mutating mid-encoder is safe
        // because a SetRef carries no GPU work (no encoder access).
        this->ApplyRefOp_d9(ref_ops[ref.index]);
        ++this->m_encodeSideRefsGen;
        continue;
      }
      if (ref.kind == PendingOpRef::Draw) {
        auto &bd = draws[ref.index];
        // Per-draw encode cost: ResolveBatchedDrawForChunk + PSO readiness + emit.
        // Cluster/const caches persist across Blit (snapshot unchanged by Blit).
        // The chunk carries its own frame slot, so this sink stays correct even
        // once the calling thread has moved on to the next frame.
        auto *perf_stats = dxmt::perf::frameStatisticsForContext(ctx);
        {
          dxmt::perf::ScopedFrameDurationCounted _resolve_timer(
              perf_stats, &dxmt::FrameStatistics::frame_draw_resolve_interval,
              &dxmt::FrameStatistics::frame_draw_resolve_count
          );
          if (!this->ResolveBatchedDrawForChunk(bd, chunk_seq, chunk_coherent_id, const_cache, resolve_cache))
            continue;
        }
        // Encode-thread PSO readiness. The cold compile runs on m_psoScheduler;
        // if the link is not hot yet, WAIT for it here and then render. Every
        // reference compiles a cold pipeline synchronously at the draw site
        // (wined3d on its CS thread, DXVK's default and non-GPL-fallback paths,
        // and the d3d11 sibling's blocking GetPipeline). Skipping the draw
        // instead, the way an async fork does, presents a frame whose content
        // the app believes it already replaced, which surfaces as a stale flash
        // at a scene cut where every pipeline is cold at once. The wait is
        // timed so DXMT_PERF_STATS shows the one-time cut cost.
        // Null state() means newRenderPipelineState failed; skip permanently.
        if (bd.resolved_pso_task) {
          bool pso_ready;
          {
            dxmt::perf::ScopedFrameDuration _pso_timer(
                perf_stats, &dxmt::FrameStatistics::frame_pso_compile_wait_interval
            );
            pso_ready = bd.resolved_pso_task->GetDone();
          }
          if (!pso_ready) {
            dxmt::perf::ScopedFrameDuration _pso_wait(
                perf_stats, &dxmt::FrameStatistics::frame_pso_compile_wait_interval
            );
            dxmt::perf::addFrameCounter(perf_stats, &dxmt::FrameStatistics::frame_pso_compile_wait_count);
            bd.resolved_pso_task->Wait();
          }
          bd.resolved_pso = bd.resolved_pso_task->state().handle;
          if (bd.resolved_pso == 0)
            continue; // link failed: skip
        }
        bool need_new_pass = pass != D9PassKind::Render || !prev_draw || !RtDsAttachmentsMatch(*prev_draw, bd);
        if (need_new_pass) {
          end_current_pass();
          StartRenderPassForBatch_d9(ctx, bd);
          pass = D9PassKind::Render;
          s = ChunkEmitState{}; // fresh encoder, fresh shadow
        }
        {
          dxmt::perf::ScopedFrameDuration _emit_timer(perf_stats, &dxmt::FrameStatistics::frame_draw_emit_interval);
          EmitCommonRenderSetup_d9(ctx, bd, s);
          // Advance the visibility-result offset for this draw and emit
          // SetVisibilityMode (Counting while an occlusion query is open,
          // Disabled otherwise), as every d3d11 draw does. Without this the
          // offset never moves, so an occlusion query only ever sees the empty
          // initial range and resolves to 0 no matter what it covers.
          ctx.bumpVisibilityResultOffset();
          EmitDrawCommand_d9(ctx, bd);
        }
        prev_draw = &bd;
      } else { // Blit
        auto &op = blits[ref.index];
        switch (op.kind) {
        case MTLD3D9Device::PendingBlitOp::Kind::Stretch:
          // Stretch / Resolve paths open their own render-pass
          // encoder via ctx.stretchBlit / ctx.resolveTexture; end any
          // open Blit/Render pass first so the new encoder doesn't
          // try to nest. fence_locality tracking is handled inside
          // each call via access(src, Read)+(dst, Write), matching
          // the Copy path's discipline.
          end_current_pass();
          // The scale path samples the source in a render pass; re-tile it for
          // GPU access first so a source last written by a blit/fill is not read
          // through a stale GPU-compressed layout (Metal3 backend only; the
          // StretchBlit encoder is compiled out under Metal4).
          ctx.optimizeTextureForGPUAccess(op.src_tex, op.src_mip, op.src_slice);
          EmitStretchBlitOp_d9(ctx.stretch_blit_cmd, op);
          break;
        case MTLD3D9Device::PendingBlitOp::Kind::Resolve:
        case MTLD3D9Device::PendingBlitOp::Kind::DepthResolve:
          end_current_pass();
          EmitResolveBlitOp_d9(ctx.resolve_texture_cmd, op);
          break;
        case MTLD3D9Device::PendingBlitOp::Kind::Copy:
          if (pass != D9PassKind::Blit) {
            end_current_pass();
            ctx.startBlitPass();
            pass = D9PassKind::Blit;
          }
          EmitBlitOp_d9(ctx, op);
          break;
        case MTLD3D9Device::PendingBlitOp::Kind::BufferToTexture:
          if (pass != D9PassKind::Blit) {
            end_current_pass();
            ctx.startBlitPass();
            pass = D9PassKind::Blit;
          }
          EmitBufferToTextureOp_d9(ctx, op);
          break;
        case MTLD3D9Device::PendingBlitOp::Kind::GenerateMipmaps:
          if (pass != D9PassKind::Blit) {
            end_current_pass();
            ctx.startBlitPass();
            pass = D9PassKind::Blit;
          }
          EmitGenerateMipmapsOp_d9(ctx, op);
          break;
        }
      }
    }
    end_current_pass();
    // No per-chunk signalEvent; Present chunk tails one that advances m_completionEvent.
    // Per-chunk signals cause SYNCHRONIZE instead of SWAP in dxmt_context coalescing.
  });
  // Recycle the const/upload rings off real GPU completion rather than the
  // Present-only m_completionEvent tail signal. This chunk runs its
  // completion callback on the finish thread once the GPU retires it,
  // even across a long run of draws with no Present (and even if the
  // cmdbuf faulted), advancing the rings' coherent watermark so their
  // placed host blocks can be reused. Without it the rings only recycle
  // at Present and grow without bound in the 32-bit address space during a
  // no-Present burst. The watermark only ever moves forward: targets are
  // folded with a max, so a later completion can never lower it. The device
  // pointer stays valid across teardown: ~CommandQueue joins the finish
  // thread and runs any residual completion targets before m_dxmtQueue (an
  // earlier-declared member) is destroyed, so none can outlive the device. The
  // target also only touches m_cachedSignaled, a trivially destructible atomic
  // whose storage lives until operator delete.
  m_dxmtQueue->CurrentChunk()->addCompletionTarget(
      std::make_shared<D9BatchWatermark>(m_cachedSignaled, m_currentCmdSeq)
  );
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();

  // Restore capacity for the next batch. Single upfront alloc instead
  // of log2(size) geometric grows. See snapshot comment at the
  // chunk->emitcc site above for the heap-churn math. The high-water rather
  // than the last size: batches are flushed per render-target pass, so their
  // sizes alternate between a handful of draws and several hundred, and
  // restoring the last one makes every large batch after a small one grow
  // geometrically from almost nothing.
  m_pendingOpsPeak = std::max(m_pendingOpsPeak, prev_ops_size);
  m_pendingDrawsPeak = std::max(m_pendingDrawsPeak, prev_draws_size);
  m_pendingBlitsPeak = std::max(m_pendingBlitsPeak, prev_blits_size);
  m_pendingRefOpsPeak = std::max(m_pendingRefOpsPeak, prev_refops_size);
  m_pendingOps.reserve(m_pendingOpsPeak);
  m_pendingDraws.reserve(m_pendingDrawsPeak);
  m_pendingBlits.reserve(m_pendingBlitsPeak);
  m_pendingRefOps.reserve(m_pendingRefOpsPeak);
  return D3D_OK;
}

void
MTLD3D9Device::emitCmdbufTailSignal() {
  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;
  auto *chunk = m_dxmtQueue->CurrentChunk();
  chunk->emitcc([event_handle, signal_seq](ArgumentEncodingContext &ctx) mutable {
    ctx.signalEventByHandle(event_handle, signal_seq);
  });
  ++m_currentCmdSeq;
}

void
MTLD3D9Device::forceFlushAndCommit() {
  FlushDrawBatch();
  flushOpenWork();
  emitCmdbufTailSignal();
  commitCurrentChunkTimed();
}

void
MTLD3D9Device::refreshSignaledAndTrimRings() {
  // Post-3.8 old finalize-tail path is dead; without explicit signaledValue refresh,
  // ring reuse predicate never holds, burning fresh placed-buffer blocks per sub-allocate.
  constexpr uint64_t kRingRefreshGap = 8;
  if (m_currentCmdSeq - m_lastRingRefreshSeq < kRingRefreshGap)
    return;
  m_lastRingRefreshSeq = m_currentCmdSeq;
  // m_cachedSignaled is advanced by both the Present-time tail signal
  // (m_completionEvent) and the per-chunk completion callbacks registered
  // in FlushDrawBatch (which cover no-Present bursts). Fold the event
  // value in without regressing the callbacks' progress, then trim
  // against the resulting max.
  uint64_t event_signalled = m_completionEvent.signaledValue();
  uint64_t prev = m_cachedSignaled.load(std::memory_order_relaxed);
  while (prev < event_signalled &&
         !m_cachedSignaled.compare_exchange_weak(prev, event_signalled, std::memory_order_relaxed))
    ;
  uint64_t signalled = m_cachedSignaled.load(std::memory_order_acquire);
  m_constRing.free_blocks(signalled);
  m_uploadRing.free_blocks(signalled);
  m_constRingResolve.free_blocks(signalled);
}

// Shared fan-list buffer [0,1,2, 0,2,3, ...] up to kFanListPrimCap.
// Static-fan draws avoid per-call m_constRing allocate.
obj_handle_t
MTLD3D9Device::fanListIBForPrimCount(uint32_t prim_count) {
  constexpr uint32_t kFanListPrimCap = 4096;
  if (prim_count == 0 || prim_count > kFanListPrimCap)
    return 0;
  if (m_fanListIB == nullptr) {
    const size_t bytes = static_cast<size_t>(kFanListPrimCap) * 3 * sizeof(uint32_t);
    void *backing = wsi::aligned_malloc(bytes, DXMT_PAGE_SIZE);
    if (!backing)
      return 0;
    auto *idx = static_cast<uint32_t *>(backing);
    for (uint32_t k = 0; k < kFanListPrimCap; ++k) {
      idx[k * 3 + 0] = 0;
      idx[k * 3 + 1] = k + 1;
      idx[k * 3 + 2] = k + 2;
    }
    WMTBufferInfo info{};
    info.length = bytes;
    info.options = (WMTResourceOptions)(WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared);
    info.memory.set(backing);
    m_fanListIB = m_metalDevice.newBuffer(info);
    if (m_fanListIB == nullptr) {
      wsi::aligned_free(backing);
      return 0;
    }
    m_fanListIBBacking = backing;
  }
  return m_fanListIB.handle;
}

// Shared fan-emulation IB resolver: dedupes the four Draw* fan
// branches. Synthesised cases (src=null) hit the pre-baked cache when
// PrimitiveCount fits; remapped cases (DIP / DIP_UP) and cache-miss
// cases allocate from m_constRing and call fill_fan_to_list_indices
// directly. The returned offset is bytes into the buffer's
// mapped_address; the caller stores it in BatchedDraw.override_ib_offset.
std::pair<obj_handle_t, uint32_t>
MTLD3D9Device::BuildFanIndexBuffer(uint32_t prim_count, const void *src, uint32_t src_idx_size) {
  if (src == nullptr) {
    obj_handle_t cached = fanListIBForPrimCount(prim_count);
    if (cached != 0)
      return {cached, 0};
  }
  size_t ib_bytes = static_cast<size_t>(prim_count) * 3 * sizeof(uint32_t);
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto ib_span = tryRingAllocate(m_constRing, m_currentCmdSeq, coherent_id, ib_bytes, 4);
  if (!ib_span)
    return {0, 0};
  fill_fan_to_list_indices(reinterpret_cast<uint32_t *>(ib_span.host), prim_count, src, src_idx_size);
  return {ib_span.handle, static_cast<uint32_t>(ib_span.offset)};
}

std::pair<obj_handle_t, uint32_t>
MTLD3D9Device::BuildWidenedIndexBuffer(const uint16_t *src, uint32_t index_count) {
  size_t ib_bytes = static_cast<size_t>(index_count) * sizeof(uint32_t);
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto ib_span = tryRingAllocate(m_constRing, m_currentCmdSeq, coherent_id, ib_bytes, 4);
  if (!ib_span)
    return {0, 0};
  uint32_t *dst = reinterpret_cast<uint32_t *>(ib_span.host);
  for (uint32_t i = 0; i < index_count; ++i)
    dst[i] = src[i];
  return {ib_span.handle, static_cast<uint32_t>(ib_span.offset)};
}

// DrawPrimitiveUP: inline vertex data via transient Shared MTLBuffer.
// Buffer lifetime pinned by encoder; cmdbuf retains through completion.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride
) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::addFrameCounter(frameStats(), &dxmt::FrameStatistics::frame_draw_issue_count);
  // wined3d device.c gates on vertex_declaration only; no
  // BeginScene gate. UP-draws on loading screens / OSD overlays
  // frequently fire outside any BeginScene/EndScene bracket.
  //
  // Validation order: both INVALIDCALL gates run before the
  // PrimitiveCount==0 D3D_OK early-out. DXVK D3D9DeviceEx::DrawPrimitiveUP
  // checks stride==0 (INVALIDCALL), then vertex declaration (INVALIDCALL),
  // then PrimitiveCount==0 (D3D_OK); wined3d d3d9_device_DrawPrimitiveUP
  // checks stride==0 (INVALIDCALL) then the declaration too. A zero-stride
  // call must surface INVALIDCALL even when PrimitiveCount is also 0.
  // wined3d does not null-check pVertexStreamZeroData, so neither do we.
  if (VertexStreamZeroStride == 0)
    return D3DERR_INVALIDCALL;
  // DrawPrimitiveUP binds the UP data to stream 0 (offset 0, the call's stride),
  // draws, then restores the stream. The internal UP buffer is not app-visible,
  // so GetStreamSource(0) reads NULL for the buffer either way. On success the
  // stream is reset to (NULL, 0, 0); on the decl-failure INVALIDCALL path the
  // reset does not run, so the bound stride lingers (the app reads NULL, 0,
  // stride). Only the stride-0 gate above returns before any UP binding. The
  // success clear runs after QueueBatchedDraw so the draw's snapshot still
  // captures the stream 0 binding at the point of the draw.
  auto clear_up_stream0 = [this](UINT residual_stride) {
    if (m_vertexBuffers[0].ptr())
      QueueRefOp(PendingRefOp::VertexBuffer0, nullptr);
    m_vertexBuffers[0] = nullptr;
    m_streamOffsets[0] = 0;
    m_streamStrides[0] = residual_stride;
  };
  // Vertex declaration must be set before any draw. wined3d device.c
  // enforces this for both UP variants (and the non-UP
  // paths defer the same gate to the wined3d core). Without it the
  // encode-side PSO build hits a null decl and produces a cryptic
  // Metal validation error instead of the spec-correct D3DERR_INVALIDCALL.
  if (!m_vertexDeclaration) {
    clear_up_stream0(VertexStreamZeroStride);
    return D3DERR_INVALIDCALL;
  }
  if (PrimitiveCount == 0)
    return D3D_OK;
  // Software-VP-only shader bound in hardware VP: reject the first draw
  // (nothing queued, so the implicit stream-0 unbind below is skipped too).
  if (swvpDrawGateRejects())
    return D3DERR_INVALIDCALL;

  UINT vertex_count = prim_to_vertex_count(PrimitiveType, PrimitiveCount);
  uint64_t total_bytes = static_cast<uint64_t>(vertex_count) * VertexStreamZeroStride;

  // No autorelease pool; see DrawPrimitive for the rationale.
  // m_constRing.allocate and fanListIBForPrimCount only ever fire +1
  // retained newBuffer when they grow; the UP path otherwise just
  // memcpys into existing ring blocks and pushes a BatchedDraw.

  // Route the inline VB (and the synthesised fan IB, if any) through
  // the queue's staging_allocator instead of allocating a fresh
  // MTLBuffer per call.
  // wined3d uses the same primitive (wined3d_streaming_buffer_upload).
  // Per-call newBuffer crosses WoW64 every time and contends Metal's
  // allocator; UI / loading screens that hammer DrawPrimitiveUP fall
  // off a cliff without a ring. 16-byte alignment is the conservative
  // floor for Metal vertex-buffer offsets across all stride shapes.
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto vb_span = tryRingAllocate(m_constRing, m_currentCmdSeq, coherent_id, static_cast<size_t>(total_bytes), 16);
  // Skip the draw rather than failing the call: a D3D9 application does not
  // check a draw's return, and the reference layers log and carry on.
  if (!vb_span)
    return D3D_OK;
  std::memcpy(vb_span.host, pVertexStreamZeroData, total_bytes);
  uint64_t vb_gpu_address = vb_span.gpu_address;

  // Fan emulation: synth a TRIANGLELIST IB and route through the
  // indexed common path. Same ring-allocator shape as the VB above.
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto [ib_handle, ib_offset_u32] = BuildFanIndexBuffer(PrimitiveCount, nullptr, 0);
    // See DrawPrimitive: a zero handle reads as "no override" downstream.
    if (ib_handle == 0)
      return D3D_OK;
    BatchedDraw draw{};
    draw.cap = BuildDrawCapture();
    draw.type = BatchedDraw::kIndexed;
    draw.primitive_type = D3DPT_TRIANGLELIST;
    draw.vertex_or_index_count = PrimitiveCount * 3;
    draw.override_vb_buffer = vb_span.handle;
    draw.override_vb_addr = vb_gpu_address;
    draw.override_vb_length = static_cast<uint32_t>(total_bytes);
    draw.override_vb_stride = VertexStreamZeroStride;
    draw.override_ib_buffer = ib_handle;
    draw.override_ib_offset = ib_offset_u32;
    draw.override_ib_format = D3DFMT_INDEX32;
    QueueBatchedDraw(std::move(draw));
    // Accumulate. State-change handlers + Present drain m_pendingDraws.
    clear_up_stream0(0);
    return D3D_OK;
  }

  BatchedDraw draw{};
  draw.cap = BuildDrawCapture();
  draw.type = BatchedDraw::kNonIndexed;
  draw.primitive_type = PrimitiveType;
  draw.vertex_or_index_count = vertex_count;
  draw.override_vb_buffer = vb_span.handle;
  draw.override_vb_addr = vb_gpu_address;
  draw.override_vb_length = static_cast<uint32_t>(total_bytes);
  draw.override_vb_stride = VertexStreamZeroStride;
  QueueBatchedDraw(std::move(draw));
  // Accumulate. State-change handlers + Present drain m_pendingDraws.
  clear_up_stream0(0);
  return D3D_OK;
}
// DrawIndexedPrimitiveUP: inline vertex + index via transient buffers.
// Vertex buffer sized to (MinVertexIndex + NumVertices) * stride.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData,
    D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride
) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::addFrameCounter(frameStats(), &dxmt::FrameStatistics::frame_draw_issue_count);
  // wined3d device.c gates on vertex_declaration only; no
  // BeginScene gate. Same rationale as DrawPrimitiveUP above.
  //
  // Validation order matches DXVK D3D9DeviceEx::DrawIndexedPrimitiveUP:
  // stride==0 (INVALIDCALL), then vertex declaration (INVALIDCALL), then
  // the (!PrimitiveCount || !NumVertices) D3D_OK early-out. wined3d
  // d3d9_device_DrawIndexedPrimitiveUP checks stride==0 first the same
  // way; a zero-stride call must surface INVALIDCALL even when the count
  // is also 0. wined3d/DXVK do not null-check pIndexData or
  // pVertexStreamZeroData, so neither do we.
  if (VertexStreamZeroStride == 0)
    return D3DERR_INVALIDCALL;
  // Per D3D9 spec, indexed UP draws unbind stream 0 AND the bound index buffer
  // on return (wined3d device.c): the UP data is bound, drawn, then the stream
  // and index buffer are restored, so GetStreamSource(0) / GetIndices read NULL
  // afterward. On success the stream resets fully to (NULL, 0, 0); the
  // decl-failure INVALIDCALL path leaves the UP bind's stride (NULL vb, offset
  // 0, stride), same as DrawPrimitiveUP; only the stride-0 gate above returns
  // first. This affects post-call state observability, not the queued UP draw
  // itself (which carries its own override_vb_* / override_ib_* fields).
  auto clear_up_state = [this](UINT residual_stride) {
    if (m_vertexBuffers[0].ptr())
      QueueRefOp(PendingRefOp::VertexBuffer0, nullptr);
    m_vertexBuffers[0] = nullptr;
    m_streamOffsets[0] = 0;
    m_streamStrides[0] = residual_stride;
    if (m_indexBuffer.ptr())
      QueueRefOp(PendingRefOp::IndexBuffer, nullptr);
    m_indexBuffer = nullptr;
  };
  // Vertex declaration must be set before any draw; wined3d device.c:
  // 3401-3406. Same rationale as DrawPrimitiveUP above.
  if (!m_vertexDeclaration) {
    clear_up_state(VertexStreamZeroStride);
    return D3DERR_INVALIDCALL;
  }
  // NumVertices==0 is spec-legal (degenerate draw -> no-op), same as the
  // non-UP DrawIndexedPrimitive sibling. DXVK returns D3D_OK on
  // (!PrimitiveCount || !NumVertices); wined3d doesn't check at the
  // d3d9 layer. Apps passing 0 (rare but possible from procedural mesh
  // generators that may emit empty batches) see a spurious failure if
  // we reject. Treat as a no-op.
  if (PrimitiveCount == 0 || NumVertices == 0)
    return D3D_OK;
  // Software-VP-only shader bound in hardware VP: reject the first draw.
  if (swvpDrawGateRejects())
    return D3DERR_INVALIDCALL;

  UINT index_count = prim_to_vertex_count(PrimitiveType, PrimitiveCount);
  // DXVK d3d9_device.cpp treats any format that is not INDEX16 as
  // 32-bit rather than rejecting it; mirror that so an unusual format
  // never trips a spurious INVALIDCALL.
  uint32_t index_size = (IndexDataFormat == D3DFMT_INDEX16) ? 2u : 4u;
  uint64_t vb_total_bytes = static_cast<uint64_t>(MinVertexIndex + NumVertices) * VertexStreamZeroStride;

  // No autorelease pool; see DrawPrimitive for the rationale.

  // Both VB and IB go through the queue's staging_allocator (same
  // shape as DrawPrimitiveUP above); a fresh newBuffer per call
  // would dominate UI/loading hot paths.
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto vb_span = tryRingAllocate(m_constRing, m_currentCmdSeq, coherent_id, static_cast<size_t>(vb_total_bytes), 16);
  // See DrawPrimitiveUP: skip rather than fail the call.
  if (!vb_span)
    return D3D_OK;
  std::memcpy(vb_span.host, pVertexStreamZeroData, vb_total_bytes);
  uint64_t vb_gpu_address = vb_span.gpu_address;

  // Fan emulation: caller-supplied indices are at pIndexData[0..N-1];
  // remap them into a u32 TRIANGLELIST and route through the indexed
  // common path. The fan IB always lives in u32 (one allocation
  // covers index_size 16 / 32 inputs uniformly).
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto [ib_handle, ib_offset_u32] = BuildFanIndexBuffer(PrimitiveCount, pIndexData, index_size);
    // See DrawPrimitive: a zero handle reads as "no override" downstream.
    if (ib_handle == 0)
      return D3D_OK;
    BatchedDraw draw{};
    draw.cap = BuildDrawCapture();
    draw.type = BatchedDraw::kIndexed;
    draw.primitive_type = D3DPT_TRIANGLELIST;
    draw.vertex_or_index_count = PrimitiveCount * 3;
    draw.override_vb_buffer = vb_span.handle;
    draw.override_vb_addr = vb_gpu_address;
    draw.override_vb_length = static_cast<uint32_t>(vb_total_bytes);
    draw.override_vb_stride = VertexStreamZeroStride;
    draw.override_ib_buffer = ib_handle;
    draw.override_ib_offset = ib_offset_u32;
    draw.override_ib_format = D3DFMT_INDEX32;
    QueueBatchedDraw(std::move(draw));
    // Accumulate. State-change handlers + Present drain m_pendingDraws.
    clear_up_state(0);
    return D3D_OK;
  }

  // Metal restarts strips at the 0xffff sentinel; widen a 16-bit strip UP draw
  // that contains it to 32-bit so the strip is not cut (see the bound-IB
  // DrawIndexedPrimitive for the full rationale). Gated to 16-bit strips.
  bool widen_strip = false;
  if (index_size == 2 && (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_LINESTRIP)) {
    const uint16_t *s16 = reinterpret_cast<const uint16_t *>(pIndexData);
    for (UINT i = 0; i < index_count; ++i)
      if (s16[i] == 0xffffu) {
        widen_strip = true;
        break;
      }
  }
  const uint32_t eff_index_size = widen_strip ? 4u : index_size;
  size_t ib_bytes = static_cast<size_t>(index_count) * eff_index_size;
  auto ib_span = tryRingAllocate(m_constRing, m_currentCmdSeq, coherent_id, ib_bytes, eff_index_size);
  if (!ib_span)
    return D3D_OK;
  if (widen_strip) {
    const uint16_t *s16 = reinterpret_cast<const uint16_t *>(pIndexData);
    uint32_t *dst = reinterpret_cast<uint32_t *>(ib_span.host);
    for (UINT i = 0; i < index_count; ++i)
      dst[i] = s16[i];
  } else {
    std::memcpy(ib_span.host, pIndexData, ib_bytes);
  }

  BatchedDraw draw{};
  draw.cap = BuildDrawCapture();
  draw.type = BatchedDraw::kIndexed;
  draw.primitive_type = PrimitiveType;
  draw.vertex_or_index_count = index_count;
  draw.override_vb_buffer = vb_span.handle;
  draw.override_vb_addr = vb_gpu_address;
  draw.override_vb_length = static_cast<uint32_t>(vb_total_bytes);
  draw.override_vb_stride = VertexStreamZeroStride;
  draw.override_ib_buffer = ib_span.handle;
  draw.override_ib_offset = static_cast<uint32_t>(ib_span.offset);
  // Canonicalise to INDEX16/INDEX32 so the encode-side index-type
  // mapping (resolved_ib_fmt) agrees with index_size above; a non-INDEX16
  // format (or a widened strip) was uploaded as 32-bit indices.
  draw.override_ib_format = (IndexDataFormat == D3DFMT_INDEX16 && !widen_strip) ? D3DFMT_INDEX16 : D3DFMT_INDEX32;
  QueueBatchedDraw(std::move(draw));
  // Accumulate. State-change handlers + Present drain m_pendingDraws.
  clear_up_state(0);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ProcessVertices(
    UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer,
    IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags
) {
  D9DeviceLock lock = LockDevice();
  // CPU vertex processing, ported from wined3d process_vertices_strided
  // (dlls/wined3d/device.c): the source vertices' object-space position is
  // folded through world*view*projection, perspective divided, and written to
  // the destination buffer as viewport-mapped screen-space XYZRHW. wine's
  // visual test test_process_vertices (dlls/d3d9/tests/visual.c) pins this
  // byte-exact on a plain HARDWARE_VERTEXPROCESSING device, so it is a
  // native-universal entry, not an SWVP-only corner. wined3d always runs the
  // fixed-function transform here: it nulls the bound VS before building the
  // stream layout and FIXMEs the programmable output-declaration path, so a
  // bound programmable vertex shader is never executed. dxmt follows that
  // model. DXVK instead emulates the whole call on the GPU through a stream-out
  // geometry shader, a shape Metal has no equivalent for. The rarer lit path
  // (per-vertex diffuse / specular from FFP lighting) is deferred and warns
  // once; the position is still transformed.

  // Front gates (DXVK ProcessVertices): reject a null destination, honour the
  // MSDN rule that a vertex shader 3.0 or above requires an explicit output
  // declaration, and no-op an empty batch.
  if (!pDestBuffer)
    return D3DERR_INVALIDCALL;
  if (MTLD3D9VertexShader *vs = m_vertexShader.ptr()) {
    if (pVertexDecl == nullptr) {
      // DWORD (unsigned long) != uint32_t under mingw; cast the bytecode
      // pointer at the boundary the way Create{Vertex,Pixel}Shader does.
      const auto *words = reinterpret_cast<const uint32_t *>(vs->bytecode());
      auto hdr = parse_dxso_header(words, vs->bytecodeByteLength() / sizeof(uint32_t));
      if (hdr && hdr->major >= 3)
        return D3DERR_INVALIDCALL;
    }
  }
  if (!VertexCount)
    return D3D_OK;

  auto *dstBuffer = static_cast<MTLD3D9VertexBuffer *>(pDestBuffer);

  // Source layout is the currently-bound declaration (a SetVertexDeclaration
  // decl, or the FVF-synthesised one SetFVF binds); wined3d builds its
  // stream_info from the same state. Locate the POSITION element (usage index
  // 0), the only element the transform reads.
  MTLD3D9VertexDeclaration *srcDecl = m_vertexDeclaration.ptr();
  if (!srcDecl)
    return D3DERR_INVALIDCALL;
  const D3DVERTEXELEMENT9 *srcElems = srcDecl->elements();
  const UINT srcElemCount = srcDecl->elementCount();
  const D3DVERTEXELEMENT9 *posElem = nullptr;
  for (UINT i = 0; i < srcElemCount; ++i) {
    if (srcElems[i].Stream == 0xFF) // D3DDECL_END terminator
      break;
    if (srcElems[i].Usage == D3DDECLUSAGE_POSITION && srcElems[i].UsageIndex == 0) {
      posElem = &srcElems[i];
      break;
    }
  }
  if (!posElem || posElem->Stream >= D3D9_MAX_VERTEX_STREAMS || !m_vertexBuffers[posElem->Stream])
    return D3DERR_INVALIDCALL;

  // Destination layout: the app's output declaration when supplied, else the
  // destination buffer's FVF (DXVK builds the same decl-from-FVF). Both are
  // walked as stream-0 element arrays; the stride is the largest element end.
  std::vector<D3DVERTEXELEMENT9> fvfElems;
  const D3DVERTEXELEMENT9 *dstElems;
  UINT dstElemCount;
  if (pVertexDecl) {
    auto *dd = static_cast<MTLD3D9VertexDeclaration *>(pVertexDecl);
    dstElems = dd->elements();
    dstElemCount = dd->elementCount();
  } else {
    build_fvf_decl_elements(dstBuffer->fvf(), fvfElems);
    fvfElems.push_back(D3DDECL_END());
    dstElems = fvfElems.data();
    dstElemCount = static_cast<UINT>(fvfElems.size());
  }
  UINT dstStride = 0;
  bool dstHasColour = false;
  for (UINT i = 0; i < dstElemCount; ++i) {
    if (dstElems[i].Stream == 0xFF)
      break;
    if (dstElems[i].Stream != 0)
      continue;
    UINT end = dstElems[i].Offset + decl_element_byte_size(dstElems[i].Type);
    if (end > dstStride)
      dstStride = end;
    if (dstElems[i].Usage == D3DDECLUSAGE_COLOR)
      dstHasColour = true;
  }
  if (!dstStride)
    return D3DERR_INVALIDCALL;

  // Host-safety bounds: dxmt reads the source and writes the destination on the
  // CPU, so it must not walk past either allocation (the references issue the
  // equivalent reads GPU-side, where an over-large count yields garbage rather
  // than a fault). The source stream needs SrcStartIndex + VertexCount vertices
  // at its stride, the destination DestIndex + VertexCount at dstStride.
  const UINT srcStride = m_streamStrides[posElem->Stream];
  const uint64_t lastVtx = static_cast<uint64_t>(SrcStartIndex) + VertexCount - 1;
  const uint64_t lastPos = static_cast<uint64_t>(m_streamOffsets[posElem->Stream]) + posElem->Offset +
                           lastVtx * srcStride + 3u * sizeof(float);
  if (lastPos > m_vertexBuffers[posElem->Stream]->size())
    return D3DERR_INVALIDCALL;
  if ((static_cast<uint64_t>(DestIndex) + VertexCount) * dstStride > dstBuffer->size())
    return D3DERR_INVALIDCALL;

  // Fold world*view*projection exactly as the fixed-function snapshot does
  // (m_transforms[10] = WORLDMATRIX(0), [0] = VIEW, [1] = PROJECTION).
  const D3DMATRIX wvp = mat4_multiply(mat4_multiply(m_transforms[10], m_transforms[0]), m_transforms[1]);

  // D3DPV_DONOTCOPYDATA skips the non-position copy-through, leaving those dest
  // bytes untouched (MSDN). wined3d ignores the flags word and always copies;
  // dxmt honours the documented flag.
  const bool copyData = !(Flags & D3DPV_DONOTCOPYDATA);

  // The lit path (FFP lighting feeding per-vertex diffuse / specular) is
  // consciously unimplemented: warn once, still transform the position and pass
  // the source colours through. wined3d's lighting arm engages only when the
  // destination carries a colour and D3DRS_LIGHTING is on, so gate the warn the
  // same way. The wine conformance test exercises neither (it disables lighting
  // and writes an XYZRHW-only destination).
  if (copyData && dstHasColour && m_renderStates[D3DRS_LIGHTING] != FALSE) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      Logger::warn(
          "d3d9: ProcessVertices with lighting enabled: lit diffuse/specular unimplemented "
          "(position transformed, source colours passed through)"
      );
  }

  // Resolve each destination stream-0 element to a fill action. Every source
  // stream a fill reads is locked read-only up front. A copy-through element
  // whose source semantic is absent or out of bounds is zero-filled rather than
  // dropped, so the destination is always defined (wined3d writes a value into
  // every slot; leaving stale allocation bytes there would be nondeterministic).
  struct DestFill {
    UINT dstOffset;
    uint32_t srcStream;
    UINT srcOffset;
    bool position;
    bool writeRhw;
    bool zero;
    uint32_t copyBytes;
  };
  std::vector<DestFill> fills;

  void *srcBase[D3D9_MAX_VERTEX_STREAMS] = {};
  bool srcLocked[D3D9_MAX_VERTEX_STREAMS] = {};
  auto ensureStreamLocked = [&](uint32_t s) -> bool {
    if (s >= D3D9_MAX_VERTEX_STREAMS || !m_vertexBuffers[s])
      return false;
    if (srcLocked[s])
      return true;
    void *p = nullptr;
    if (FAILED(m_vertexBuffers[s]->Lock(0, 0, &p, D3DLOCK_READONLY)) || !p)
      return false;
    srcBase[s] = p;
    srcLocked[s] = true;
    return true;
  };

  auto unlockStreams = [&]() {
    for (uint32_t s = 0; s < D3D9_MAX_VERTEX_STREAMS; ++s)
      if (srcLocked[s])
        m_vertexBuffers[s]->Unlock();
  };

  if (!ensureStreamLocked(posElem->Stream))
    return D3DERR_INVALIDCALL;

  for (UINT i = 0; i < dstElemCount; ++i) {
    const D3DVERTEXELEMENT9 &de = dstElems[i];
    if (de.Stream == 0xFF)
      break;
    if (de.Stream != 0)
      continue;
    const bool isPosition =
        (de.Usage == D3DDECLUSAGE_POSITION || de.Usage == D3DDECLUSAGE_POSITIONT) && de.UsageIndex == 0;
    if (isPosition) {
      fills.push_back({de.Offset, posElem->Stream, posElem->Offset, true, de.Type == D3DDECLTYPE_FLOAT4, false, 0});
      continue;
    }
    if (!copyData)
      continue;
    const uint32_t dstSize = decl_element_byte_size(de.Type);
    if (!dstSize)
      continue;
    // Match the destination semantic to a source element of the same
    // (usage, usage index) and copy it through unchanged; a slot with no usable
    // source (missing semantic, unbound stream, out-of-bounds read) is zeroed.
    const D3DVERTEXELEMENT9 *srcMatch = nullptr;
    for (UINT j = 0; j < srcElemCount; ++j) {
      if (srcElems[j].Stream == 0xFF)
        break;
      if (srcElems[j].Usage == de.Usage && srcElems[j].UsageIndex == de.UsageIndex) {
        srcMatch = &srcElems[j];
        break;
      }
    }
    uint32_t copyBytes = 0;
    if (srcMatch && srcMatch->Stream < D3D9_MAX_VERTEX_STREAMS && m_vertexBuffers[srcMatch->Stream]) {
      copyBytes = std::min(decl_element_byte_size(srcMatch->Type), dstSize);
      const UINT ms = m_streamStrides[srcMatch->Stream];
      const uint64_t lastRead =
          static_cast<uint64_t>(m_streamOffsets[srcMatch->Stream]) + srcMatch->Offset + lastVtx * ms + copyBytes;
      if (copyBytes && lastRead <= m_vertexBuffers[srcMatch->Stream]->size() && ensureStreamLocked(srcMatch->Stream)) {
        fills.push_back({de.Offset, srcMatch->Stream, srcMatch->Offset, false, false, false, copyBytes});
        continue;
      }
    }
    fills.push_back({de.Offset, 0, 0, false, false, true, dstSize});
  }

  // Lock the destination for the written span and run the transform / copy.
  void *dstBase = nullptr;
  if (HRESULT hr = dstBuffer->Lock(DestIndex * dstStride, VertexCount * dstStride, &dstBase, 0); FAILED(hr)) {
    unlockStreams();
    return hr;
  }

  for (UINT i = 0; i < VertexCount; ++i) {
    char *dv = static_cast<char *>(dstBase) + static_cast<size_t>(i) * dstStride;
    for (const DestFill &f : fills) {
      if (f.zero) {
        std::memset(dv + f.dstOffset, 0, f.copyBytes);
        continue;
      }
      const char *sv = static_cast<const char *>(srcBase[f.srcStream]) + m_streamOffsets[f.srcStream] +
                       static_cast<size_t>(SrcStartIndex + i) * m_streamStrides[f.srcStream] + f.srcOffset;
      if (f.position) {
        float p[3];
        std::memcpy(p, sv, sizeof(p));
        float screen[4];
        process_vertex_to_screen(wvp, p, m_viewport, screen);
        std::memcpy(dv + f.dstOffset, screen, f.writeRhw ? 4u * sizeof(float) : 3u * sizeof(float));
      } else {
        std::memcpy(dv + f.dstOffset, sv, f.copyBytes);
      }
    }
  }

  dstBuffer->Unlock();
  unlockStreams();
  return D3D_OK;
}
// CreateVertexDeclaration: wined3d device.c. The element array
// includes a D3DDECL_END() terminator; MTLD3D9VertexDeclaration's
// ctor scans for it and stores the inclusive range so GetDeclaration's
// returned count matches wined3d.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) {
  D9DeviceLock lock = LockDevice();
  if (!ppDecl)
    return D3DERR_INVALIDCALL;
  // InitReturnPtr; DXVK d3d9_device.cpp zeroes the out-pointer
  // before any other validation so failure paths leave the app's
  // local at NULL rather than a stale value.
  *ppDecl = nullptr;
  if (!pVertexElements)
    return D3DERR_INVALIDCALL;
  // Reject any non-terminator element whose Type is past the documented
  // D3DDECLTYPE range (D3DDECLTYPE_UNUSED is legal only as the
  // terminator). Pre-this check dxmt stored any byte verbatim and
  // silently emitted MTLAttributeFormatInvalid in to_mtl_attr_format,
  // producing a broken PSO at draw time.
  if (HRESULT hr = validate_vertex_elements(pVertexElements); FAILED(hr))
    return hr;
  *ppDecl = ::dxmt::ref<IDirect3DVertexDeclaration9>(new MTLD3D9VertexDeclaration(this, pVertexElements));
  return D3D_OK;
}

// SetVertexDeclaration / GetVertexDeclaration: same priv-pin shape
// as SetTexture / SetRenderTarget; cross-device check via deviceRaw().
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
  D9DeviceLock lock = LockDevice();
  auto *decl = static_cast<MTLD3D9VertexDeclaration *>(pDecl);
  if (decl && decl->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapVertexDeclaration = decl;
    m_recordingBlock->m_changes.vertex_declaration = true;
    // Record the derived FVF alongside the decl (the live arm keeps m_fvf in
    // lockstep below) so an applied block leaves GetFVF consistent.
    m_recordingBlock->m_snapFvf = decl ? decl->fvf() : 0;
    return D3D_OK;
  }
  // GetFVF reports the bound declaration's FVF: the source FVF for a
  // SetFVF-synthesized decl, 0 for an app-created one. Keeping the device
  // field in lockstep here means a later SetFVF / GetFVF / StateBlock
  // capture all observe the decl that is actually bound.
  m_fvf = decl ? decl->fvf() : 0;
  if (m_vertexDeclaration.ptr() == decl)
    return D3D_OK;
  m_vertexDeclaration = decl;
  // Op-stream mirror: two independent refs (calling-thread shadow +
  // encode-side mirror) stay in lockstep during dual-tracking.
  if (decl)
    decl->AddRefPrivate();
  QueueRefOp(PendingRefOp::VertexDeclaration, decl);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) {
  D9DeviceLock lock = LockDevice();
  if (!ppDecl)
    return D3DERR_INVALIDCALL;
  *ppDecl = nullptr;
  MTLD3D9VertexDeclaration *bound = m_vertexDeclaration.ptr();
  if (bound)
    *ppDecl = ::dxmt::ref<IDirect3DVertexDeclaration9>(bound);
  return D3D_OK;
}
// FVF -> synthesized vertex declaration, cached per FVF dword. Shared
// by SetFVF's live and recording arms so the recording path can pin
// the decl a recorded SetFVF implies without touching the live slot.
MTLD3D9VertexDeclaration *
MTLD3D9Device::getOrCreateFvfDecl(DWORD FVF) {
  auto it = m_fvfDeclCache.find(FVF);
  if (it == m_fvfDeclCache.end()) {
    std::vector<D3DVERTEXELEMENT9> elements;
    build_fvf_decl_elements(FVF, elements);
    // CreateVertexDeclaration requires a D3DDECL_END terminator at
    // the back. build_fvf_decl_elements emits the body without it so
    // the helper is reusable for tools that want raw element arrays.
    D3DVERTEXELEMENT9 terminator{};
    terminator.Stream = 0xFF;
    terminator.Type = D3DDECLTYPE_UNUSED;
    elements.push_back(terminator);
    auto *raw = new MTLD3D9VertexDeclaration(this, elements.data(), /*selfPin=*/false);
    // Record the source FVF so GetFVF reports it while this decl is bound.
    raw->setFvf(FVF);
    auto [ins, _] = m_fvfDeclCache.emplace(FVF, Com<MTLD3D9VertexDeclaration, false>{});
    ins->second = raw;
    it = ins;
  }
  return it->second.ptr();
}

// SetFVF / GetFVF: synthesise vertex decl from FVF dword.
// SetFVF and SetVertexDeclaration alias same slot; last call wins.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetFVF(DWORD FVF) {
  D9DeviceLock lock = LockDevice();
  // FVF=0 is not a valid FVF: wined3d (device.c) and DXVK (d3d9_device.cpp)
  // both return D3D_OK without touching any state. Leaving m_fvf and the bound
  // declaration alone keeps GetFVF reporting the decl that is actually bound.
  // Checked before the recording branch so a recorded SetFVF(0) is a true
  // no-op as well.
  if (FVF == 0)
    return D3D_OK;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapFvf = FVF;
    m_recordingBlock->m_changes.fvf = true;
    // A recorded SetFVF also pins the synthesized decl, the same dual-slot
    // effect the live arm has (DXVK's SetFVF routes through
    // SetVertexDeclaration, which records the decl).
    m_recordingBlock->m_snapVertexDeclaration = getOrCreateFvfDecl(FVF);
    m_recordingBlock->m_changes.vertex_declaration = true;
    return D3D_OK;
  }
  m_fvf = FVF;
  auto *new_decl = getOrCreateFvfDecl(FVF);
  if (m_vertexDeclaration.ptr() == new_decl)
    return D3D_OK;
  m_vertexDeclaration = new_decl;
  // Op-stream mirror; same shape as SetVertexDeclaration; this site
  // bypasses that setter so we push the SetRef inline.
  if (new_decl)
    new_decl->AddRefPrivate();
  QueueRefOp(PendingRefOp::VertexDeclaration, new_decl);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetFVF(DWORD *pFVF) {
  D9DeviceLock lock = LockDevice();
  if (!pFVF)
    return D3DERR_INVALIDCALL;
  *pFVF = m_fvf;
  return D3D_OK;
}
// CreateVertexShader: freeze bytecode; AIR translation at draw time (lazy).
// Length via shader_bytecode_dword_count helper (not full decoder; swappable later).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::ScopedFrameDurationCounted _create_timer(
      frameStats(), &dxmt::FrameStatistics::frame_create_resource_interval,
      &dxmt::FrameStatistics::frame_create_resource_count
  );
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  if (!pFunction)
    return D3DERR_INVALIDCALL;
  size_t dwords = shader_bytecode_dword_count(pFunction);
  if (dwords == 0)
    return D3DERR_INVALIDCALL;
  // pFunction is `const DWORD *` per the D3D9 COM signature; the DXSO
  // walker works on `const uint32_t *` (matching the storage type the
  // compiler keeps the bytecode in). DWORD aliases differently across
  // toolchains; uint32_t under our native macOS shim, unsigned long
  // under mingw; so the bytecode pointer needs a one-line cast at
  // the boundary rather than at every walker call site.
  const auto *words = reinterpret_cast<const uint32_t *>(pFunction);
  // Reject non-VS bytecode (PS blob bound as VS, or malformed
  // version) up front. DXVK d3d9_device.cpp does the same kind
  // mismatch check; we validate the version DWORD itself so future
  // AIR-emit doesn't have to re-walk it.
  auto header = parse_dxso_header(words, dwords);
  if (!header || header->kind != DxsoShaderKind::Vertex)
    return D3DERR_INVALIDCALL;
  // Pass this device's float register count so a def past c255 validates on a
  // software/mixed VP device (8192) and still rejects on a hardware-VP one (256).
  auto metadata = walk_dxso_shader(words, static_cast<uint32_t>(dwords), *header, m_vsConstFCount);
  if (!metadata)
    return D3DERR_INVALIDCALL;
  log_shader_dump("CreateVertexShader", *header, *metadata, pFunction, dwords);
  // Dedup the compiled module by bytecode hash so re-creates of the same
  // shader share one variant cache + MTLFunctions (DXVK
  // D3D9ShaderModuleSet). The wrapper is thin and per-call; the module is
  // device-lifetime.
  auto module = getOrCreateVertexShaderModule(pFunction, dwords, std::move(*metadata));
  *ppShader = ::dxmt::ref<IDirect3DVertexShader9>(new MTLD3D9VertexShader(this, std::move(module)));
  return D3D_OK;
}

// SetVertexShader / GetVertexShader: same priv-pin shape as the
// other slot bindings. NULL is allowed (apps unbind to switch to FFP
// vertex processing).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShader(IDirect3DVertexShader9 *pShader) {
  D9DeviceLock lock = LockDevice();
  auto *shader = static_cast<MTLD3D9VertexShader *>(pShader);
  if (shader && shader->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapVertexShader = shader;
    m_recordingBlock->m_changes.vertex_shader = true;
    return D3D_OK;
  }
  if (m_vertexShader.ptr() == shader)
    return D3D_OK;
  m_vertexShader = shader;
  // A newly bound shader re-arms the one-shot software-VP draw gate.
  m_swvpDrawRejected = false;
  // On a software / mixed-VP device the extended-constant snapshot depends on
  // the bound shader (whether it reaches c256..), so force the next draw to
  // re-capture it. A hardware-VP device never captures the overflow, so this
  // stays inert there.
  if (m_vsConstFCount > D3D9_MAX_VS_CONST_F)
    m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_F;
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (shader)
    shader->AddRefPrivate();
  QueueRefOp(PendingRefOp::VertexShader, shader);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShader(IDirect3DVertexShader9 **ppShader) {
  D9DeviceLock lock = LockDevice();
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  MTLD3D9VertexShader *bound = m_vertexShader.ptr();
  if (bound)
    *ppShader = ::dxmt::ref<IDirect3DVertexShader9>(bound);
  return D3D_OK;
}
// VS constant Set/Get: DXVK SetShaderConstants (d3d9_device.cpp).
// HWVP-only path: DXVK's software/hardware reg-count split collapses to
// a single bound. Get keeps an explicit overflow guard that DXVK omits.
// without it, a wrap on StartRegister+Count slips past the bound check
// and we'd memcpy out-of-range. Bool storage is a flat BOOL[] so Set
// normalises to TRUE/FALSE on store and Get is a pass-through.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4fCount)
    return D3DERR_INVALIDCALL;
  // The accepted register count is the device's file size: 256 on a
  // hardware-VP device, 8192 on a software / mixed-VP device. Native and DXVK
  // accept the extended range on a MIXED device regardless of the current
  // SetSoftwareVertexProcessing mode; m_vsConstFCount is the immutable size.
  if (StartRegister + Vector4fCount > m_vsConstFCount)
    return D3DERR_INVALIDCALL;
  if (Vector4fCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  const UINT end = StartRegister + Vector4fCount;
  if (m_inStateBlockRecord) {
    // The per-block snapshot mirrors the hardware 256 file only (growing it to
    // 8192 would bloat every state block 128 KB); a record that reaches into
    // the extended range captures just the hardware-visible portion.
    if (StartRegister < D3D9_MAX_VS_CONST_F) {
      const UINT hot_end = std::min<UINT>(end, D3D9_MAX_VS_CONST_F);
      std::memcpy(
          &m_recordingBlock->m_snapVsConstantsF[StartRegister][0], pConstantData,
          static_cast<size_t>(hot_end - StartRegister) * sizeof(float) * 4
      );
      // Grow the recorded F range (Apply restores only this span; gaps
      // inside it hold the Begin-time seed) and the reach mark Apply
      // raises the device's upload clamp from.
      auto &changes = m_recordingBlock->m_changes;
      const uint16_t rec_lo = static_cast<uint16_t>(StartRegister);
      const uint16_t rec_hi = static_cast<uint16_t>(hot_end);
      if (changes.vs_const_f_hi <= changes.vs_const_f_lo) {
        changes.vs_const_f_lo = rec_lo;
        changes.vs_const_f_hi = rec_hi;
      } else {
        changes.vs_const_f_lo = std::min(changes.vs_const_f_lo, rec_lo);
        changes.vs_const_f_hi = std::max(changes.vs_const_f_hi, rec_hi);
      }
      if (rec_hi > m_recordingBlock->m_snapVsConstFMax)
        m_recordingBlock->m_snapVsConstFMax = rec_hi;
    }
    return D3D_OK;
  }
  // Hot registers (< 256) ride the shadow array + the sticky high-water mark
  // the encode side clamps the per-draw upload to. Bump reach before the
  // memcmp short-circuit so a no-op Set still advances coverage.
  if (StartRegister < D3D9_MAX_VS_CONST_F) {
    const UINT hot_end = std::min<UINT>(end, D3D9_MAX_VS_CONST_F);
    const uint16_t reach = static_cast<uint16_t>(hot_end);
    if (reach > m_vsConstFMax) {
      m_vsConstFMax = reach;
      m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_F_MAX;
    }
    const size_t bytes = static_cast<size_t>(hot_end - StartRegister) * sizeof(float) * 4;
    // Unchanged-value short-circuit. D3DX effect frameworks push the same
    // constant table after every technique pass; skip the dirty mark when the
    // data did not actually change.
    if (std::memcmp(&m_vsConstantsF[StartRegister][0], pConstantData, bytes) != 0) {
      std::memcpy(&m_vsConstantsF[StartRegister][0], pConstantData, bytes);
      m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_F;
    }
  }
  // Extended registers (>= 256) ride the device overflow store; the draw path
  // uploads them for a relative / over-256 shader on this device. Mark the
  // constant axis dirty so the next draw re-captures the overflow snapshot.
  if (end > D3D9_MAX_VS_CONST_F && m_vsConstantsFOverflow) {
    const UINT ov_lo = std::max<UINT>(StartRegister, D3D9_MAX_VS_CONST_F);
    const float *src = pConstantData + static_cast<size_t>(ov_lo - StartRegister) * 4u;
    float *dst = m_vsConstantsFOverflow.get() + static_cast<size_t>(ov_lo - D3D9_MAX_VS_CONST_F) * 4u;
    const size_t bytes = static_cast<size_t>(end - ov_lo) * sizeof(float) * 4;
    if (std::memcmp(dst, src, bytes) != 0) {
      std::memcpy(dst, src, bytes);
      m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_F;
    }
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4fCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4fCount > m_vsConstFCount)
    return D3DERR_INVALIDCALL;
  if (Vector4fCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  // Read-back mirrors the Set split: hot registers (< 256) from the shadow
  // array, extended registers (>= 256) from the device overflow store.
  const UINT end = StartRegister + Vector4fCount;
  if (StartRegister < D3D9_MAX_VS_CONST_F) {
    const UINT hot_end = std::min<UINT>(end, D3D9_MAX_VS_CONST_F);
    std::memcpy(
        pConstantData, &m_vsConstantsF[StartRegister][0],
        static_cast<size_t>(hot_end - StartRegister) * sizeof(float) * 4
    );
  }
  if (end > D3D9_MAX_VS_CONST_F && m_vsConstantsFOverflow) {
    const UINT ov_lo = std::max<UINT>(StartRegister, D3D9_MAX_VS_CONST_F);
    float *dst = pConstantData + static_cast<size_t>(ov_lo - StartRegister) * 4u;
    const float *src = m_vsConstantsFOverflow.get() + static_cast<size_t>(ov_lo - D3D9_MAX_VS_CONST_F) * 4u;
    std::memcpy(dst, src, static_cast<size_t>(end - ov_lo) * sizeof(float) * 4);
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4iCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4iCount > D3D9_MAX_VS_CONST_I)
    return D3DERR_INVALIDCALL;
  if (Vector4iCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  const size_t bytes = static_cast<size_t>(Vector4iCount) * sizeof(int) * 4;
  if (m_inStateBlockRecord) {
    std::memcpy(&m_recordingBlock->m_snapVsConstantsI[StartRegister][0], pConstantData, bytes);
    m_recordingBlock->m_changes.vs_constants = true;
    return D3D_OK;
  }
  if (std::memcmp(&m_vsConstantsI[StartRegister][0], pConstantData, bytes) == 0)
    return D3D_OK;
  std::memcpy(&m_vsConstantsI[StartRegister][0], pConstantData, bytes);
  m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_I;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4iCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4iCount > D3D9_MAX_VS_CONST_I)
    return D3DERR_INVALIDCALL;
  if (Vector4iCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  std::memcpy(pConstantData, &m_vsConstantsI[StartRegister][0], Vector4iCount * sizeof(int) * 4);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - BoolCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + BoolCount > D3D9_MAX_VS_CONST_B)
    return D3DERR_INVALIDCALL;
  if (BoolCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    for (UINT i = 0; i < BoolCount; ++i)
      m_recordingBlock->m_snapVsConstantsB[StartRegister + i] = pConstantData[i] ? TRUE : FALSE;
    m_recordingBlock->m_changes.vs_constants = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit: normalise on the stack so we can
  // compare against the stored values; only commit + bump the shadow
  // generation when at least one bit actually changed.
  bool any_change = false;
  for (UINT i = 0; i < BoolCount; ++i) {
    BOOL norm = pConstantData[i] ? TRUE : FALSE;
    if (m_vsConstantsB[StartRegister + i] != norm) {
      any_change = true;
      m_vsConstantsB[StartRegister + i] = norm;
    }
  }
  if (!any_change)
    return D3D_OK;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_B;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - BoolCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + BoolCount > D3D9_MAX_VS_CONST_B)
    return D3DERR_INVALIDCALL;
  if (BoolCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < BoolCount; ++i)
    pConstantData[i] = m_vsConstantsB[StartRegister + i];
  return D3D_OK;
}

// SetStreamSource / GetStreamSource: hot path with strict out-of-range validation
// (bad index corrupts fetch at draw). Unbind with NULL preserves offset/stride.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride
) {
  D9DeviceLock lock = LockDevice();
  if (StreamNumber >= D3D9_MAX_VERTEX_STREAMS)
    return D3DERR_INVALIDCALL;
  auto *buffer = static_cast<MTLD3D9VertexBuffer *>(pStreamData);
  if (buffer && buffer->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_changes.stream_source |= static_cast<uint16_t>(1u << StreamNumber);
    m_recordingBlock->m_snapVertexBuffers[StreamNumber] = buffer;
    if (buffer) {
      m_recordingBlock->m_snapStreamOffsets[StreamNumber] = OffsetInBytes;
      m_recordingBlock->m_snapStreamStrides[StreamNumber] = Stride;
    } else {
      // NULL buffer preserves the live offset/stride, mirroring the live arm's
      // wined3d shape (device.c) rather than recording zeros.
      m_recordingBlock->m_snapStreamOffsets[StreamNumber] = m_streamOffsets[StreamNumber];
      m_recordingBlock->m_snapStreamStrides[StreamNumber] = m_streamStrides[StreamNumber];
    }
    return D3D_OK;
  }
  // No-op rebind: same buffer + same offset/stride (or both-NULL, which
  // preserves offset/stride per wined3d device.c). Offsets and
  // strides feed BuildDrawCapture directly (not via the POD snapshot),
  // so a stride-only change still needs the gen bump to propagate.
  bool buffer_changed = m_vertexBuffers[StreamNumber].ptr() != buffer;
  if (!buffer_changed) {
    if (buffer == nullptr)
      return D3D_OK;
    if (m_streamOffsets[StreamNumber] == OffsetInBytes && m_streamStrides[StreamNumber] == Stride)
      return D3D_OK;
  }
  m_vertexBuffers[StreamNumber] = buffer;
  if (buffer) {
    m_streamOffsets[StreamNumber] = OffsetInBytes;
    m_streamStrides[StreamNumber] = Stride;
  }
  // Buffer == NULL: preserve previous offset/stride (wined3d behaviour).
  // Op-stream mirror; only push a SetRef when the BUFFER changes (the
  // ref-counted slot). Offset/stride-only changes flow through
  // BuildDrawCapture's per-stream snapshot, not D9EncodingRefs, so the
  // op stream doesn't need to record them. See SetVertexDeclaration for
  // the dual-tracking shape.
  if (buffer_changed) {
    if (buffer)
      buffer->AddRefPrivate();
    QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::VertexBuffer0 + StreamNumber), buffer);
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride
) {
  D9DeviceLock lock = LockDevice();
  // wined3d device.c; buffer out-pointer must be non-null;
  // offset is optional, stride is required. Match that.
  if (!ppStreamData || !pStride)
    return D3DERR_INVALIDCALL;
  *ppStreamData = nullptr;
  if (pOffsetInBytes)
    *pOffsetInBytes = 0;
  *pStride = 0;
  if (StreamNumber >= D3D9_MAX_VERTEX_STREAMS)
    return D3DERR_INVALIDCALL;
  MTLD3D9VertexBuffer *bound = m_vertexBuffers[StreamNumber].ptr();
  if (bound)
    *ppStreamData = ::dxmt::ref<IDirect3DVertexBuffer9>(bound);
  if (pOffsetInBytes)
    *pOffsetInBytes = m_streamOffsets[StreamNumber];
  *pStride = m_streamStrides[StreamNumber];
  return D3D_OK;
}

// SetStreamSourceFreq: wined3d device.c d3d9_device_SetStreamSourceFreq.
// Validation rules match DXVK d3d9_device.cpp: stream index in
// range, INSTANCEDATA on stream 0 is INVALIDCALL, INSTANCEDATA + INDEXED
// together is INVALIDCALL, and Setting==0 is INVALIDCALL (apps must
// either pass a divisor / count or one of the two flags). Setting==1
// (the spec default) reverts the stream to per-vertex stepping.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
  D9DeviceLock lock = LockDevice();
  if (StreamNumber >= D3D9_MAX_VERTEX_STREAMS)
    return D3DERR_INVALIDCALL;
  // Native D3D9 (Wine visual.c stream_test) accepts a plain frequency
  // other than 1 and INSTANCEDATA with a zero divider; both round-trip
  // through Get; the draw path clamps the GPU step rate to >= 1. Only a
  // zero setting, both flags at once, and INSTANCEDATA on stream 0 are
  // rejected.
  if (HRESULT hr = validate_stream_source_freq(StreamNumber, Setting); FAILED(hr))
    return hr;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_changes.stream_freq |= static_cast<uint16_t>(1u << StreamNumber);
    m_recordingBlock->m_snapStreamFreq[StreamNumber] = Setting;
    return D3D_OK;
  }
  // Unchanged-value short-circuit. stream_freq is in pod_snapshot;
  // a no-op rewrite would force a fresh COW snapshot rebuild on the
  // next QueueBatchedDraw.
  if (m_streamFreq[StreamNumber] == Setting)
    return D3D_OK;
  m_streamFreq[StreamNumber] = Setting;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_STREAM_FREQ;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) {
  D9DeviceLock lock = LockDevice();
  if (StreamNumber >= D3D9_MAX_VERTEX_STREAMS || !pSetting)
    return D3DERR_INVALIDCALL;
  *pSetting = m_streamFreq[StreamNumber];
  return D3D_OK;
}

// SetIndices / GetIndices: wined3d device.c. Single slot,
// no stream-index validation. NULL is allowed (apps unbind before
// switching to a different draw-call shape).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
  D9DeviceLock lock = LockDevice();
  auto *buffer = static_cast<MTLD3D9IndexBuffer *>(pIndexData);
  if (buffer && buffer->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapIndexBuffer = buffer;
    m_recordingBlock->m_changes.index_buffer = true;
    return D3D_OK;
  }
  if (m_indexBuffer.ptr() == buffer)
    return D3D_OK;
  m_indexBuffer = buffer;
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (buffer)
    buffer->AddRefPrivate();
  QueueRefOp(PendingRefOp::IndexBuffer, buffer);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) {
  D9DeviceLock lock = LockDevice();
  if (!ppIndexData)
    return D3DERR_INVALIDCALL;
  *ppIndexData = nullptr;
  MTLD3D9IndexBuffer *bound = m_indexBuffer.ptr();
  if (bound)
    *ppIndexData = ::dxmt::ref<IDirect3DIndexBuffer9>(bound);
  return D3D_OK;
}
// Mirror image of CreateVertexShader. Same bytecode-length helper,
// same InitReturnPtr discipline, same lifetime shape, same kind-
// mismatch reject (DXVK d3d9_device.cpp).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
  D9DeviceLock lock = LockDevice();
  dxmt::perf::ScopedFrameDurationCounted _create_timer(
      frameStats(), &dxmt::FrameStatistics::frame_create_resource_interval,
      &dxmt::FrameStatistics::frame_create_resource_count
  );
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  if (!pFunction)
    return D3DERR_INVALIDCALL;
  size_t dwords = shader_bytecode_dword_count(pFunction);
  if (dwords == 0)
    return D3DERR_INVALIDCALL;
  // See CreateVertexShader: DWORD aliases differently across toolchains;
  // walker takes uint32_t * to match the storage type.
  const auto *words = reinterpret_cast<const uint32_t *>(pFunction);
  auto header = parse_dxso_header(words, dwords);
  if (!header || header->kind != DxsoShaderKind::Pixel)
    return D3DERR_INVALIDCALL;
  auto metadata = walk_dxso_shader(words, static_cast<uint32_t>(dwords), *header);
  if (!metadata)
    return D3DERR_INVALIDCALL;
  log_shader_dump("CreatePixelShader", *header, *metadata, pFunction, dwords);
  // See CreateVertexShader: dedup the module by bytecode hash.
  auto module = getOrCreatePixelShaderModule(pFunction, dwords, std::move(*metadata));
  *ppShader = ::dxmt::ref<IDirect3DPixelShader9>(new MTLD3D9PixelShader(this, std::move(module)));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShader(IDirect3DPixelShader9 *pShader) {
  D9DeviceLock lock = LockDevice();
  auto *shader = static_cast<MTLD3D9PixelShader *>(pShader);
  if (shader && shader->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapPixelShader = shader;
    m_recordingBlock->m_changes.pixel_shader = true;
    return D3D_OK;
  }
  if (m_pixelShader.ptr() == shader)
    return D3D_OK;
  m_pixelShader = shader;
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (shader)
    shader->AddRefPrivate();
  QueueRefOp(PendingRefOp::PixelShader, shader);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShader(IDirect3DPixelShader9 **ppShader) {
  D9DeviceLock lock = LockDevice();
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  MTLD3D9PixelShader *bound = m_pixelShader.ptr();
  if (bound)
    *ppShader = ::dxmt::ref<IDirect3DPixelShader9>(bound);
  return D3D_OK;
}
// PS constant Set/Get: same shape as the VS path above; bound is
// SM3's 224 floats / 16 int / 16 bool. SM2 apps only ever address
// [0..31] of F but the API surface uses the SM3 limit.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4fCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4fCount > D3D9_MAX_PS_CONST_F)
    return D3DERR_INVALIDCALL;
  if (Vector4fCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    std::memcpy(
        &m_recordingBlock->m_snapPsConstantsF[StartRegister][0], pConstantData,
        static_cast<size_t>(Vector4fCount) * sizeof(float) * 4
    );
    // See SetVertexShaderConstantF for the range + reach tracking.
    auto &changes = m_recordingBlock->m_changes;
    const uint16_t rec_lo = static_cast<uint16_t>(StartRegister);
    const uint16_t rec_hi = static_cast<uint16_t>(StartRegister + Vector4fCount);
    if (changes.ps_const_f_hi <= changes.ps_const_f_lo) {
      changes.ps_const_f_lo = rec_lo;
      changes.ps_const_f_hi = rec_hi;
    } else {
      changes.ps_const_f_lo = std::min(changes.ps_const_f_lo, rec_lo);
      changes.ps_const_f_hi = std::max(changes.ps_const_f_hi, rec_hi);
    }
    if (rec_hi > m_recordingBlock->m_snapPsConstFMax)
      m_recordingBlock->m_snapPsConstFMax = rec_hi;
    return D3D_OK;
  }
  // Sticky high-water mark; see SetVertexShaderConstantF for the
  // rationale. Bump before the memcmp short-circuit.
  const uint16_t reach = static_cast<uint16_t>(StartRegister + Vector4fCount);
  if (reach > m_psConstFMax) {
    m_psConstFMax = reach;
    m_encShadowDirty |= dxmt::D9ES_DIRTY_PS_CONST_F_MAX;
  }
  const size_t bytes = static_cast<size_t>(Vector4fCount) * sizeof(float) * 4;
  if (std::memcmp(&m_psConstantsF[StartRegister][0], pConstantData, bytes) == 0)
    return D3D_OK;
  std::memcpy(&m_psConstantsF[StartRegister][0], pConstantData, bytes);
  m_encShadowDirty |= dxmt::D9ES_DIRTY_PS_CONST_F;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4fCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4fCount > D3D9_MAX_PS_CONST_F)
    return D3DERR_INVALIDCALL;
  if (Vector4fCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  std::memcpy(pConstantData, &m_psConstantsF[StartRegister][0], Vector4fCount * sizeof(float) * 4);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4iCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4iCount > D3D9_MAX_PS_CONST_I)
    return D3DERR_INVALIDCALL;
  if (Vector4iCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  const size_t bytes = static_cast<size_t>(Vector4iCount) * sizeof(int) * 4;
  if (m_inStateBlockRecord) {
    std::memcpy(&m_recordingBlock->m_snapPsConstantsI[StartRegister][0], pConstantData, bytes);
    m_recordingBlock->m_changes.ps_constants = true;
    return D3D_OK;
  }
  if (std::memcmp(&m_psConstantsI[StartRegister][0], pConstantData, bytes) == 0)
    return D3D_OK;
  std::memcpy(&m_psConstantsI[StartRegister][0], pConstantData, bytes);
  m_encShadowDirty |= dxmt::D9ES_DIRTY_PS_CONST_I;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4iCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4iCount > D3D9_MAX_PS_CONST_I)
    return D3DERR_INVALIDCALL;
  if (Vector4iCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  std::memcpy(pConstantData, &m_psConstantsI[StartRegister][0], Vector4iCount * sizeof(int) * 4);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - BoolCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + BoolCount > D3D9_MAX_PS_CONST_B)
    return D3DERR_INVALIDCALL;
  if (BoolCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    for (UINT i = 0; i < BoolCount; ++i)
      m_recordingBlock->m_snapPsConstantsB[StartRegister + i] = pConstantData[i] ? TRUE : FALSE;
    m_recordingBlock->m_changes.ps_constants = true;
    return D3D_OK;
  }
  bool any_change = false;
  for (UINT i = 0; i < BoolCount; ++i) {
    BOOL norm = pConstantData[i] ? TRUE : FALSE;
    if (m_psConstantsB[StartRegister + i] != norm) {
      any_change = true;
      m_psConstantsB[StartRegister + i] = norm;
    }
  }
  if (!any_change)
    return D3D_OK;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_PS_CONST_B;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  D9DeviceLock lock = LockDevice();
  if (StartRegister > std::numeric_limits<UINT>::max() - BoolCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + BoolCount > D3D9_MAX_PS_CONST_B)
    return D3DERR_INVALIDCALL;
  if (BoolCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < BoolCount; ++i)
    pConstantData[i] = m_psConstantsB[StartRegister + i];
  return D3D_OK;
}
// Higher-Order Surface (N-patch / rect-patch) draws. Deprecated in
// D3D10+; almost no modern app uses these. DXVK's stub returns D3D_OK
// for the Draw* pair (warns once, silently skips the draw) so apps that
// speculatively issue them don't bail on hr-check. DeletePatch returns
// INVALIDCALL because deleting an unknown handle is per-spec illegal.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawRectPatch(UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo) {
  D9DeviceLock lock = LockDevice();
  (void)Handle;
  (void)pNumSegs;
  (void)pRectPatchInfo;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
    Logger::warn("d3d9: DrawRectPatch is a stub (HOS deprecated); silently skipping");
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo) {
  D9DeviceLock lock = LockDevice();
  (void)Handle;
  (void)pNumSegs;
  (void)pTriPatchInfo;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
    Logger::warn("d3d9: DrawTriPatch is a stub (HOS deprecated); silently skipping");
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DeletePatch(UINT Handle) {
  D9DeviceLock lock = LockDevice();
  (void)Handle;
  // No patch storage today, so any Handle is "unknown"; D3DERR_INVALIDCALL
  // matches the per-spec answer DXVK returns. Warn once for symmetry with the
  // Draw* patch stubs so an app that reaches the HOS surface self-diagnoses.
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
    Logger::warn("d3d9: DeletePatch is a stub (HOS deprecated); returning D3DERR_INVALIDCALL");
  return D3DERR_INVALIDCALL;
}
// CreateQuery: wined3d device.c d3d9_device_CreateQuery (~3940). The
// Type-only call (ppQuery == NULL) is the "is this query type
// supported?" probe; D3D_OK means yes. With ppQuery, allocate a real
// IDirect3DQuery9.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) {
  D9DeviceLock lock = LockDevice();
  // ppQuery=NULL is a support probe. The out pointer is written only on
  // success: an unsupported type returns NOTAVAILABLE and leaves the caller's
  // pointer untouched (it is not a sentinel), matching d3d9_device_CreateQuery.
  if (!d3d9_query_supported(Type))
    return D3DERR_NOTAVAILABLE;
  if (!ppQuery)
    return D3D_OK;

  auto *q = new MTLD3D9Query(this, Type);
  q->AddRef();
  *ppQuery = q;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetConvolutionMonoKernel(UINT, UINT, float *, float *) {
  D9DeviceLock lock = LockDevice();
  // DXVK d3d9_device.cpp: this is exposed via a CAPS bit
  // (D3DPMISCCAPS_TSSARGTEMP family) which neither DXVK nor dxmt
  // advertise, so the per-spec answer is INVALIDCALL. STUB_HR's
  // E_NOTIMPL was breaking hr-strict app init paths.
  return D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ComposeRects(
    IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst, IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects,
    IDirect3DVertexBuffer9 *pDstRectDescs, D3DCOMPOSERECTSOP Operation, INT Xoffset, INT Yoffset
) {
  D9DeviceLock lock = LockDevice();
  // MSDN: any of the four surface/buffer pointers null is INVALIDCALL.
  // DXVK enforces. Without this gate an app passing nulls; even a
  // smoke-test that expects the failure HRESULT to fall back; saw a
  // silent OK from the stub.
  if (!pSrc || !pDst || !pSrcRectDescs || !pDstRectDescs)
    return D3DERR_INVALIDCALL;
  (void)NumRects;
  (void)Operation;
  (void)Xoffset;
  (void)Yoffset;
  // DXVK d3d9_device.cpp: warn once + silent D3D_OK so the
  // few apps that probe this niche D3D9Ex blit-compose helper at init
  // don't bail on E_NOTIMPL. The compose itself is dropped; apps that
  // depend on it will visibly miss the blit, but ComposeRects is rare
  // (used for video overlay multi-rect composition).
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
    Logger::warn("d3d9: ComposeRects is a stub; silently skipping");
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::PresentEx(
    const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags
) {
  D9DeviceLock lock = LockDevice();
  // An Ex present on an unfocused fullscreen chain reports occlusion
  // without presenting (wine d3d9 device.c returns it off the device
  // state ahead of the swapchain present).
  if (m_implicitSwapChain && !m_implicitSwapChain->windowed() && !fullscreenOwnsDisplay())
    return S_PRESENT_OCCLUDED;
  return m_implicitSwapChain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}
// Device9Ex bookkeeping returns. Pre-Reset / pre-frame-pacing dxmt
// has nothing meaningful to back any of these with; Metal doesn't
// expose GPU-thread-priority or vblank waits, residency is implicit,
// and "device state" is always OK until Reset/Lost lands. Returning
// E_NOTIMPL here pushes engines into device-lost recovery loops on
// the per-frame callers (CheckDeviceState in particular). Match
// DXVK's contract: D3D_OK with a one-shot warn and round-trip storage
// where the API has a getter.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetGPUThreadPriority(INT *pPriority) {
  D9DeviceLock lock = LockDevice();
  if (!pPriority)
    return D3DERR_INVALIDCALL;
  *pPriority = 0;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetGPUThreadPriority(INT Priority) {
  D9DeviceLock lock = LockDevice();
  // MSDN: Priority must be in [-7, 7]; out-of-range is INVALIDCALL. Metal has
  // no GPU-thread-priority control, so validate per MSDN and no-op. wined3d
  // returns E_NOTIMPL (which trips hr-strict apps that init at the default 0);
  // DXVK silent-OKs without the range check. The MSDN-faithful
  // validate-then-no-op is the correct permissive shape.
  if (Priority < -7 || Priority > 7)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::WaitForVBlank(UINT iSwapChain) {
  D9DeviceLock lock = LockDevice();
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CheckResourceResidency(IDirect3DResource9 **pResourceArray, UINT32 NumResources) {
  D9DeviceLock lock = LockDevice();
  // Per MSDN: D3DERR_INVALIDCALL if pResourceArray is NULL while
  // NumResources is non-zero. DXVK enforces the same gate; the prior
  // silent-OK shape let app-side null-pointer bugs slip past.
  // On UMA every resource is "always resident", so the only reason to
  // walk the array would be a per-resource sanity check; leave the
  // body as the always-OK stance for now.
  if (NumResources > 0 && !pResourceArray)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetMaximumFrameLatency(UINT MaxLatency) {
  D9DeviceLock lock = LockDevice();
  if (MaxLatency > 30)
    return D3DERR_INVALIDCALL;
  m_frameLatency = (MaxLatency == 0) ? 3u : MaxLatency;
  // No queue push here; the swapchain's Present re-pushes
  // min(m_frameLatency, BackBufferCount + 1u) per frame, mirroring DXVK
  // d3d9_swapchain.cpp GetActualFrameLatency. Pushing the raw
  // m_frameLatency here would briefly let the queue race ahead of the
  // BackBufferCount-implied limit between this setter and the next
  // Present; the per-Present clamp closes that window.
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetMaximumFrameLatency(UINT *pMaxLatency) {
  D9DeviceLock lock = LockDevice();
  if (!pMaxLatency)
    return D3DERR_INVALIDCALL;
  *pMaxLatency = m_frameLatency;
  return D3D_OK;
}
// The non-Ex focus-loss machine, wined3d's WM_ACTIVATEAPP transitions
// reconstructed at the poll points (TestCooperativeLevel and Present):
// a fullscreen device that loses the foreground goes Ok to Lost, and
// regaining it moves Lost to NotReset so the app's recovery loop sees
// DEVICENOTRESET and runs Reset. Windowed devices never lose this way,
// and the Ex machine reports occlusion instead.
void
MTLD3D9Device::updateNonExLostState() {
  if (m_isEx || !m_implicitSwapChain || m_implicitSwapChain->windowed())
    return;
  const bool owns = fullscreenOwnsDisplay();
  const auto state = m_deviceState.load(std::memory_order_relaxed);
  if (state == DeviceState::Ok && !owns)
    m_deviceState.store(DeviceState::Lost, std::memory_order_relaxed);
  else if (state == DeviceState::Lost && owns)
    m_deviceState.store(DeviceState::NotReset, std::memory_order_relaxed);
}

bool
MTLD3D9Device::fullscreenOwnsDisplay() {
  // Edge-triggered reconstruction of wine's WM_ACTIVATEAPP device state
  // from polls: the latch starts owning (wine initialises the state OK at
  // create and Reset) and flips only when the observed foreground window
  // CHANGES, to whether the new foreground is the focus window. A level
  // poll would misread a stale creation-time foreground as occlusion;
  // the latch shares wine's blind spot for a device that is never
  // activated at all, which is the message model's own behavior.
  HWND focus = m_creationParams.hFocusWindow;
  if (!focus && m_implicitSwapChain)
    focus = m_implicitSwapChain->hWindow();
  // WM_ACTIVATEAPP is the authority whenever the focus proc is installed, so
  // the poll only runs when it is not. Left running alongside it, the poll
  // invents transitions the message model does not have: moving the foreground
  // to another window of the same process raises no WM_ACTIVATEAPP, but reads
  // here as a loss of the focus window.
  if (m_focusProcHooked)
    return !m_fullscreenOccluded.load(std::memory_order_relaxed);

  HWND foreground = wsi::foregroundWindow();
  HWND sampled = m_lastForegroundSample.load(std::memory_order_relaxed);
  if (foreground != sampled) {
    // Only a transition that involves the focus window is an activation
    // event; foreground moving between two foreign windows never raises
    // WM_ACTIVATEAPP and must not disturb the state.
    const bool was_focus = sampled == focus;
    const bool is_focus = focus != nullptr && foreground == focus;
    m_lastForegroundSample.store(foreground, std::memory_order_relaxed);
    if (was_focus != is_focus)
      m_fullscreenOccluded.store(!is_focus, std::memory_order_relaxed);
  }
  return !m_fullscreenOccluded.load(std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CheckDeviceState(HWND hDestinationWindow) {
  D9DeviceLock lock = LockDevice();
  // Fullscreen: display ownership keys the answer, polled off the
  // foreground window (wine d3d9 device.c CheckDeviceState keys the same
  // branches off its focus-message device state; its FIXME notes the
  // cross-device case is unhandled there too). A window other than the
  // device window is occluded exactly while the fullscreen chain owns
  // the display; the device window itself is occluded once it loses it.
  // The caller's null stays null here: wine compares it raw, and the
  // tests rely on null meaning "some other window".
  if (m_implicitSwapChain && !m_implicitSwapChain->windowed()) {
    const bool owns_display = fullscreenOwnsDisplay();
    if (hDestinationWindow != m_implicitSwapChain->hWindow())
      return owns_display ? S_PRESENT_OCCLUDED : D3D_OK;
    return owns_display ? D3D_OK : S_PRESENT_OCCLUDED;
  }
  // Windowed: DXVK's shape, the minimized probe, so Ex apps that poll
  // CheckDeviceState instead of letting Present surface the status stop
  // rendering full-speed while minimized. Falls back to the swapchain's
  // own window when the caller passes null (DXVK does the same).
  HWND hWindow = hDestinationWindow;
  if (!hWindow && m_implicitSwapChain)
    hWindow = m_implicitSwapChain->hWindow();
  if (hWindow && wsi::isMinimized(hWindow))
    return S_PRESENT_OCCLUDED;
  return D3D_OK;
}
// validateCreateExUsage (shared Usage-bit gate for the three Create*Ex
// methods) lives in d3d9_create_validation.hpp so the host test pins it.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateRenderTargetEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage
) {
  D9DeviceLock lock = LockDevice();
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  // D3D9 leaves the out-param NULL on every failure path; validateCreateExUsage
  // can reject below before the forward that would clear it.
  *ppSurface = nullptr;
  if (HRESULT hr = validateCreateExUsage(Usage, pSharedHandle); hr != D3D_OK)
    return hr;
  // The RESTRICT_* / RESTRICTED_CONTENT bits are not enforced (no
  // cross-process sharing or content protection yet), but D3D9Ex requires
  // GetDesc to report the requested Usage alongside the implicit
  // RENDERTARGET bit, so thread it onto the surface after the base create.
  HRESULT hr =
      CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
  if (SUCCEEDED(hr) && Usage)
    static_cast<MTLD3D9Surface *>(*ppSurface)->addDescUsage(Usage);
  return hr;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateOffscreenPlainSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle,
    DWORD Usage
) {
  D9DeviceLock lock = LockDevice();
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  // D3D9 leaves the out-param NULL on every failure path; validateCreateExUsage
  // can reject below before the forward that would clear it.
  *ppSurface = nullptr;
  if (HRESULT hr = validateCreateExUsage(Usage, pSharedHandle); hr != D3D_OK)
    return hr;
  // The Ex entry's pSharedHandle is the same SYSTEMMEM user-memory overload the
  // non-Ex method accepts: native aliases the app pointer in place. Forward to
  // the non-Ex path, which handles that pool and rejects the handle on every
  // other pool the same way.
  return CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateDepthStencilSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage
) {
  D9DeviceLock lock = LockDevice();
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  // The usage check precedes the out-param clear: an invalid usage returns
  // INVALIDCALL and leaves the caller's pointer untouched, while a format
  // failure nulls it in the base create below. wined3d orders it the same way
  // (d3d9 device.c CreateDepthStencilSurfaceEx); its render-target Ex entry
  // clears first, so a usage reject nulls there (native nulls offscreen-plain likewise).
  if (HRESULT hr = validateCreateExUsage(Usage, pSharedHandle); hr != D3D_OK)
    return hr;
  // GetDesc must report the requested Usage alongside the implicit
  // DEPTHSTENCIL bit (the RESTRICTED_CONTENT restriction is informational,
  // not enforced); thread it onto the surface after the base create.
  HRESULT hr = CreateDepthStencilSurface(
      Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle
  );
  if (SUCCEEDED(hr) && Usage)
    static_cast<MTLD3D9Surface *>(*ppSurface)->addDescUsage(Usage);
  return hr;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode) {
  D9DeviceLock lock = LockDevice();
  if (!m_isEx)
    return D3DERR_INVALIDCALL;
  if (!pPresentationParameters)
    return D3DERR_INVALIDCALL;
  // The mode is passed if and only if the present is fullscreen, and its
  // extent must match the backbuffer (wine d3d9 device.c ResetEx; note
  // CreateDeviceEx deliberately tolerates a stale windowed mode instead,
  // see d3d9_interface.cpp). Beyond the validation the mode itself is not
  // consumed: there is no WSI display-mode switch on this backend, the
  // extent change happens via the swapchain rebuild in Reset.
  if (!pPresentationParameters->Windowed == !pFullscreenDisplayMode)
    return D3DERR_INVALIDCALL;
  if (pFullscreenDisplayMode && (pFullscreenDisplayMode->Width != pPresentationParameters->BackBufferWidth ||
                                 pFullscreenDisplayMode->Height != pPresentationParameters->BackBufferHeight))
    return D3DERR_INVALIDCALL;
  return Reset(pPresentationParameters);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  D9DeviceLock lock = LockDevice();
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  if (!m_isEx)
    return D3DERR_INVALIDCALL;
  return m_parent->GetAdapterDisplayModeEx(m_creationParams.AdapterOrdinal, pMode, pRotation);
}

} // namespace dxmt
