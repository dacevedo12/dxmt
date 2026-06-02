#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d9_shader_scan.hpp"
#include "dxso_decoder.hpp"

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

// IDirect3DVertexShader9: frozen copy of the app's bytecode blob.
// No Metal handle yet; the AIR translation happens at draw time when
// the (vs, ps, decl) trio resolves into a pipeline.
//
// References: wined3d dlls/d3d9/shader.c d3d9_vertexshader_* (vtable
// shape, GetFunction byte-count semantics). DXVK src/d3d9/d3d9_shader.h
// D3D9VertexShader (the freeze-the-bytecode-and-defer-compile
// pattern). MGL has nothing analogous.
//
// Lifetime mirrors the standalone-resource pattern: self-pinned via
// the same m_self_pinned exactly-once-drop guard surface/texture/
// buffer/declaration use.
class MTLD3D9VertexShader final : public ComObject<IDirect3DVertexShader9> {
public:
  MTLD3D9VertexShader(MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata);
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
  // MTLFunction baked from the metallib at Create time. Null when
  // either DXSO→AIR compile or MTLLibrary load failed; the pipeline
  // builder treats null as the same hard error as empty bytecode.
  // The function retains its parent MTLLibrary, so we don't carry the
  // library separately.
  WMT::Function
  function() const {
    return m_function;
  }

  // Compile a per-IA-layout VS variant from cached bytecode; manual
  // buffer fetch instead of [[stage_in]]. Cached by layout fingerprint.
  // Sync compile: layout.elements must outlive this call.
  WMT::Function compileVariant(const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout, float point_size_override = 0.0f);

private:
  MTLD3D9Device *m_device;
  std::vector<DWORD> m_bytecode;
  DxsoShaderMetadata m_metadata;
  WMT::Reference<WMT::Function> m_function;
  // Variant cache: layout fingerprint → MTLFunction. Owns the
  // functions; the per-draw caller borrows a non-owning WMT::Function
  // handle (the cache outlives the draw). Cleared when the shader
  // dies.
  std::unordered_map<uint64_t, WMT::Reference<WMT::Function>> m_variantCache;
  // Auto-injection variant cache, keyed by (layout fingerprint,
  // D3DRS_POINTSIZE bit pattern). Separate map keeps the hot path
  // (override == 0.0f, default for all non-POINTLIST and oPts-writing
  // VSes) byte-identical to its pre-injection lookup shape. Apps that
  // use POINTLIST + non-oPts VS + D3DRS_POINTSIZE != 1.0 are rare and
  // typically settle on one or two distinct sizes; the cache stays
  // small even when active.
  std::unordered_map<uint64_t, WMT::Reference<WMT::Function>> m_pointSizeVariantCache;
  bool m_self_pinned = true;
};

// IDirect3DPixelShader9: same shape as MTLD3D9VertexShader. The
// bytecode-length helper, the GetFunction contract, and the lifetime
// shape are shared verbatim; only the COM vtable interface differs.
// We don't fold these into a CRTP base because the duplication is
// small and an explicit two-subclass pair reads more naturally than
// template wiring.
class MTLD3D9PixelShader final : public ComObject<IDirect3DPixelShader9> {
public:
  MTLD3D9PixelShader(MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata);
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
  WMT::Function
  function() const {
    return m_function;
  }

  // Compile per-(alpha-test, sampler-layout) PS variant. Sampler kind
  // resolution: UNKNOWN → infer from dcl (SM 1.4+/2.0+) or default-to-
  // Texture2D (SM 1.0..1.3).
  WMT::Function compileVariant(
      uint32_t alpha_test_func, uint32_t alpha_test_ref, const uint8_t samp_kinds[16], bool point_sprite,
      const ::DXSO_SHADER_PS_BUMP_ENV_DATA *bump_env = nullptr, bool fog_blend = false
  );

  // Back-compat shim: alpha-only variant. Forwards to compileVariant
  // with an all-UNKNOWN sampler layout (no host override) and
  // point_sprite=false. Existing callers that don't yet plumb the
  // layout keep working unchanged.
  WMT::Function compileAlphaVariant(uint32_t alpha_test_func, uint32_t alpha_test_ref);

private:
  MTLD3D9Device *m_device;
  std::vector<DWORD> m_bytecode;
  DxsoShaderMetadata m_metadata;
  WMT::Reference<WMT::Function> m_function;
  // Keyed by FNV-1a hash of (alpha_test_func, alpha_test_ref, kinds[0..15]).
  std::unordered_map<uint64_t, WMT::Reference<WMT::Function>> m_variantCache;
  bool m_self_pinned = true;
};

} // namespace dxmt
