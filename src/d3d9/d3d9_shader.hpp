#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d9_shader_scan.hpp"
#include "dxso_decoder.hpp"
#include "rc/util_rc_ptr.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA;

namespace dxmt {

class MTLD3D9Device;

// Async work item run off the calling thread by the device's task
// scheduler. Both the PSO link (D3D9PsoCompileTask, d3d9_device.cpp) and
// the per-variant function compile (D3D9FunctionCompileTask, d3d9_shader.cpp)
// flow through the one scheduler as this base, so a PSO task can name a
// not-yet-compiled function task as a dependency and the scheduler parks it
// until that task completes: the encode thread never runs LLVM. Mirrors
// d3d11's ThreadpoolWork (d3d11_shader.hpp); RunTask returns the task itself
// when finished, or a different task to wait on first.
class D3D9AsyncTask {
public:
  virtual ~D3D9AsyncTask() {}
  virtual D3D9AsyncTask *RunTask() = 0;
  virtual bool GetDone() const noexcept = 0;
  virtual void SetDone(bool state) noexcept = 0;
};

// A function-compile task seen by its consumer (the PSO task): the compiled
// MTLFunction, valid once GetDone() latches. Mirrors d3d11's CompiledShader
// (d3d11_shader.hpp), which layers GetShader over the same base. function()
// is null while pending and stays null permanently on a compile failure: the
// AIR emit is deterministic on its input, so the negative result is correct
// to cache.
class D3D9CompiledFunction : public D3D9AsyncTask {
public:
  virtual WMT::Function function() = 0;
};

// The concrete function-compile task is defined in d3d9_shader.cpp; the
// variant caches below only own and hand out the abstract view.
class D3D9FunctionCompileTask;

// DxsoBoundDcl, DxsoBoundConst, DxsoShaderMetadata, walk_dxso_shader
// live in dxso_decoder.hpp so airconv can build a DXSO compiler
// without linking d3d9.dll. The types are re-exported here through
// the dxso_decoder.hpp include above.

// Optional shader dump at CreateShader time (DXMT_DUMP_D9_SHADER=1).
// With DXMT_DUMP_PATH=/path, unique bytecodes are written to
// `<path>/<vs|ps>_<hex-hash>.bin` for offline disassembly.
void log_shader_dump(
    const char *kind, const DxsoHeader &header, const DxsoShaderMetadata &md, const DWORD *byte_code, size_t dwordCount
);

// FNV-1a over the DWORD bytecode blob. Key for the device-level module
// dedup (CreateVertexShader / CreatePixelShader compute it, the
// getOrCreate*ShaderModule accessors key the maps on it). 64-bit, so
// not collision-proof; the device caches memcmp the bytecode on a hash
// hit before reusing a module. Also the value the DXSO bytecode-dump
// diagnostic tags files with, so the two stay in lockstep.
uint64_t bytecode_hash(const DWORD *byte_code, size_t dwordCount);

// Compiled-shader module: the shared, bytecode-derived artifact owning
// the frozen bytecode blob, its metadata, and the per-variant MTLFunction
// caches. dxmt's analogue of DXVK's D3D9CommonShader
// (src/d3d9/d3d9_shader.h): the thing the device dedups by bytecode and
// pins for its lifetime, separate from the COM wrapper the app holds.
// Intrusive Rc<> (incRef/decRef + atomic refcount), the same shape as
// dxmt::Sampler, because it is an internal shared resource whose lifetime
// is independent of any COM refcount. Identical bytecode yields identical
// metadata, computed once here; the wrapper delegates.
class MTLD3D9VertexShaderModule {
public:
  MTLD3D9VertexShaderModule(
      MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata
  );

  void incRef();
  void decRef();

  const DWORD *
  bytecode() const {
    return m_bytecode.data();
  }
  size_t
  bytecodeByteLength() const {
    return m_bytecode.size() * sizeof(DWORD);
  }
  const DxsoShaderMetadata &
  metadata() const {
    return m_metadata;
  }
  MTLD3D9Device *
  device() const {
    return m_device;
  }
  // Find-or-create the async compile task for this (layout, point-size)
  // variant, submitting it on first request; the encode thread never runs
  // the LLVM emit itself. Returns the shared task (the PSO task waits on it).
  D3D9CompiledFunction *getVariantTask(const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout, bool inject_point_size = false);

private:
  MTLD3D9Device *m_device;
  std::vector<DWORD> m_bytecode;
  DxsoShaderMetadata m_metadata;
  // Variant cache: (layout fingerprint, point-size injection bit) → compile
  // task. Owns the tasks; the per-draw caller borrows a non-owning pointer
  // (the cache outlives the draw). Cleared when the module dies. Point-size
  // auto-injection folds a single sentinel into the key
  // (point_size_variant_key), so an injecting point draw and its
  // non-injecting sibling are the only two entries per layout; the size
  // itself rides a uniform, not the key.
  std::unordered_map<uint64_t, std::unique_ptr<D3D9CompiledFunction>> m_variantCache;
  // Tasks that lost a 64-bit key collision in m_variantCache. The map holds
  // one entry per key, so on the astronomically rare verified collision the
  // fresh task is pinned here for module lifetime (a PSO task holds a
  // non-owning pointer to it) instead of being cached.
  std::vector<std::unique_ptr<D3D9CompiledFunction>> m_variantCacheCollisions;
  std::atomic<uint32_t> m_refcount = {0u};
};

// Pixel-shader module: same role as MTLD3D9VertexShaderModule for the
// fragment stage; only the variant key axes differ (alpha-test, sampler
// kinds, point-sprite, bump-env, fog, dual-source instead of the VS
// layout fingerprint).
class MTLD3D9PixelShaderModule {
public:
  MTLD3D9PixelShaderModule(
      MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata
  );

  void incRef();
  void decRef();

  const DWORD *
  bytecode() const {
    return m_bytecode.data();
  }
  size_t
  bytecodeByteLength() const {
    return m_bytecode.size() * sizeof(DWORD);
  }
  const DxsoShaderMetadata &
  metadata() const {
    return m_metadata;
  }
  MTLD3D9Device *
  device() const {
    return m_device;
  }

  // Find-or-create the async compile task for the per-(alpha FUNC,
  // sampler-layout) PS variant, submitting it on first request. Sampler kind
  // resolution: UNKNOWN → infer from dcl (SM 1.4+/2.0+) or default-to-
  // Texture2D (SM 1.0..1.3). The alpha REF and the TexBem bump-env
  // constants ride the shared PS uniform tail, so they are not variant
  // axes here.
  D3D9CompiledFunction *getVariantTask(
      uint32_t alpha_test_func, const uint8_t samp_kinds[16], bool point_sprite, int fog_mode = -1,
      bool fog_coord_w = false, bool dual_source = false, bool flat_shading = false, bool emit_sample_mask = false,
      uint32_t unorm_snap_mask = 0
  );

private:
  MTLD3D9Device *m_device;
  std::vector<DWORD> m_bytecode;
  DxsoShaderMetadata m_metadata;
  // Keyed by FNV-1a hash of (alpha_test_func, kinds[0..15], and the other
  // bounded axes); the alpha ref + bump-env ride the uniform, not the key.
  std::unordered_map<uint64_t, std::unique_ptr<D3D9CompiledFunction>> m_variantCache;
  // Tasks that lost a 64-bit key collision in m_variantCache; see the vertex
  // module for the same full-key-verify + module-lifetime-pin discipline.
  std::vector<std::unique_ptr<D3D9CompiledFunction>> m_variantCacheCollisions;
  std::atomic<uint32_t> m_refcount = {0u};
};

// IDirect3DVertexShader9: thin COM wrapper around an Rc<module>. The app
// can recreate the same bytecode any number of times; each Create call
// mints a fresh wrapper but they all share the one device-deduped module,
// so identical draws resolve to identical MTLFunction handles and the PSO
// cache stops growing without bound.
//
// References: wined3d dlls/d3d9/shader.c d3d9_vertexshader_* (vtable
// shape, GetFunction byte-count semantics). DXVK src/d3d9/d3d9_shader.h
// D3D9VertexShader / D3D9CommonShader (the wrapper-over-shared-module
// split). MGL has nothing analogous.
//
// Two independent lifetimes: m_module (Rc, kept alive by the device map
// for device lifetime) and the COM refcount (governs bound-shader
// keep-alive via the same m_self_pinned exactly-once-drop guard that
// surface/texture/buffer/declaration use). The ctor takes no public
// AddRef on the wrapper; the module Rc is handed in already retained.
class MTLD3D9VertexShader final : public ComObject<IDirect3DVertexShader9> {
public:
  MTLD3D9VertexShader(MTLD3D9Device *device, Rc<MTLD3D9VertexShaderModule> module);
  ~MTLD3D9VertexShader();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
  HRESULT STDMETHODCALLTYPE GetFunction(void *pData, UINT *pSizeOfData) override;

  MTLD3D9Device *
  deviceRaw() const {
    return m_device;
  }
  const DWORD *
  bytecode() const {
    return m_module->bytecode();
  }
  size_t
  bytecodeByteLength() const {
    return m_module->bytecodeByteLength();
  }
  const DxsoShaderMetadata &
  metadata() const {
    return m_module->metadata();
  }
  D3D9CompiledFunction *
  getVariantTask(const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout, bool inject_point_size = false) {
    return m_module->getVariantTask(layout, inject_point_size);
  }

private:
  MTLD3D9Device *m_device;
  Rc<MTLD3D9VertexShaderModule> m_module;
  bool m_self_pinned = true;
};

// IDirect3DPixelShader9: same shape as MTLD3D9VertexShader. The
// bytecode-length helper, the GetFunction contract, and the lifetime
// shape are shared verbatim; only the COM vtable interface and the
// module type differ. We don't fold these into a CRTP base because the
// duplication is small and an explicit two-subclass pair reads more
// naturally than template wiring.
class MTLD3D9PixelShader final : public ComObject<IDirect3DPixelShader9> {
public:
  MTLD3D9PixelShader(MTLD3D9Device *device, Rc<MTLD3D9PixelShaderModule> module);
  ~MTLD3D9PixelShader();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
  HRESULT STDMETHODCALLTYPE GetFunction(void *pData, UINT *pSizeOfData) override;

  MTLD3D9Device *
  deviceRaw() const {
    return m_device;
  }
  const DWORD *
  bytecode() const {
    return m_module->bytecode();
  }
  size_t
  bytecodeByteLength() const {
    return m_module->bytecodeByteLength();
  }
  const DxsoShaderMetadata &
  metadata() const {
    return m_module->metadata();
  }
  D3D9CompiledFunction *
  getVariantTask(
      uint32_t alpha_test_func, const uint8_t samp_kinds[16], bool point_sprite, int fog_mode = -1,
      bool fog_coord_w = false, bool dual_source = false, bool flat_shading = false, bool emit_sample_mask = false,
      uint32_t unorm_snap_mask = 0
  ) {
    return m_module->getVariantTask(
        alpha_test_func, samp_kinds, point_sprite, fog_mode, fog_coord_w, dual_source, flat_shading, emit_sample_mask,
        unorm_snap_mask
    );
  }

private:
  MTLD3D9Device *m_device;
  Rc<MTLD3D9PixelShaderModule> m_module;
  bool m_self_pinned = true;
};

} // namespace dxmt
