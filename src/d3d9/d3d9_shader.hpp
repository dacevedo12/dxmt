#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d9_shader_scan.hpp"
#include "dxso_decoder.hpp"
#include "rc/util_rc_ptr.hpp"

#include <atomic>
#include <optional>
#include <unordered_map>
#include <vector>

struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA;
struct DXSO_SHADER_PS_BUMP_ENV_DATA;

namespace dxmt {

class MTLD3D9Device;

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
  WMT::Function compileVariant(const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout, float point_size_override = 0.0f);

private:
  MTLD3D9Device *m_device;
  std::vector<DWORD> m_bytecode;
  DxsoShaderMetadata m_metadata;
  // Variant cache: layout fingerprint → MTLFunction. Owns the
  // functions; the per-draw caller borrows a non-owning WMT::Function
  // handle (the cache outlives the draw). Cleared when the module dies.
  std::unordered_map<uint64_t, WMT::Reference<WMT::Function>> m_variantCache;
  // Auto-injection variant cache, keyed by (layout fingerprint,
  // D3DRS_POINTSIZE bit pattern). Separate map keeps the hot path
  // (override == 0.0f, default for all non-POINTLIST and oPts-writing
  // VSes) byte-identical to its pre-injection lookup shape. Apps that
  // use POINTLIST + non-oPts VS + D3DRS_POINTSIZE != 1.0 are rare and
  // typically settle on one or two distinct sizes; the cache stays
  // small even when active.
  std::unordered_map<uint64_t, WMT::Reference<WMT::Function>> m_pointSizeVariantCache;
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

  // Compile per-(alpha-test, sampler-layout) PS variant. Sampler kind
  // resolution: UNKNOWN → infer from dcl (SM 1.4+/2.0+) or default-to-
  // Texture2D (SM 1.0..1.3).
  WMT::Function compileVariant(
      uint32_t alpha_test_func, uint32_t alpha_test_ref, const uint8_t samp_kinds[16], bool point_sprite,
      const ::DXSO_SHADER_PS_BUMP_ENV_DATA *bump_env = nullptr, int fog_mode = -1, bool dual_source = false
  );

private:
  MTLD3D9Device *m_device;
  std::vector<DWORD> m_bytecode;
  DxsoShaderMetadata m_metadata;
  // Keyed by FNV-1a hash of (alpha_test_func, alpha_test_ref, kinds[0..15]).
  std::unordered_map<uint64_t, WMT::Reference<WMT::Function>> m_variantCache;
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
  WMT::Function
  compileVariant(const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout, float point_size_override = 0.0f) {
    return m_module->compileVariant(layout, point_size_override);
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
  WMT::Function
  compileVariant(
      uint32_t alpha_test_func, uint32_t alpha_test_ref, const uint8_t samp_kinds[16], bool point_sprite,
      const ::DXSO_SHADER_PS_BUMP_ENV_DATA *bump_env = nullptr, int fog_mode = -1, bool dual_source = false
  ) {
    return m_module->compileVariant(
        alpha_test_func, alpha_test_ref, samp_kinds, point_sprite, bump_env, fog_mode, dual_source
    );
  }

private:
  MTLD3D9Device *m_device;
  Rc<MTLD3D9PixelShaderModule> m_module;
  bool m_self_pinned = true;
};

} // namespace dxmt
