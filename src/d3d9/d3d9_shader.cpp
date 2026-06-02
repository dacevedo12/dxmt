#include "d3d9_shader.hpp"

#include "airconv_public.h"
#include "d3d9_device.hpp"
#include "log/log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#ifdef _WIN32
#include <direct.h>
#define DXMT_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define DXMT_MKDIR(p) ::mkdir((p), 0755)
#endif

namespace dxmt {

namespace {
// One-shot env-var read. DXMT_DUMP_D9_SHADER=1 enables a per-create
// summary line via Logger::warn (so it shows up in d3d9.log alongside
// the first-call traces). Off by default to avoid unconditional spam
// from apps that create hundreds of shaders, but available without a
// rebuild for real-shader coverage poking.
bool
shader_dump_enabled() {
  static const bool v = []() {
    const char *e = std::getenv("DXMT_DUMP_D9_SHADER");
    return e && e[0] && e[0] != '0';
  }();
  return v;
}

// Optional bytecode-to-disk dump. Empty → disabled. Set to a writable
// directory path; we write one file per unique bytecode hash, so the
// session ends with a directory containing every shader the app
// created.
const std::string &
shader_dump_dir() {
  static const std::string v = []() -> std::string {
    const char *e = std::getenv("DXMT_DUMP_PATH");
    return (e && e[0]) ? std::string(e) : std::string();
  }();
  return v;
}

uint64_t
fnv1a_dwords(const DWORD *byte_code, size_t dwordCount) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < dwordCount; ++i) {
    h ^= static_cast<uint64_t>(byte_code[i]);
    h *= 0x100000001b3ull;
  }
  return h;
}

// Write the bytecode once per (hash, kind) pair. The set is module-
// global so re-creates of the same shader don't re-write the file.
// Mutex-protected because shader creation is cross-thread for some
// apps. The first hit ensures the target directory exists (mkdir is
// silently a no-op if already present), so the env var alone is
// enough: no preflight `mkdir -p` required from the user.
void
maybe_write_bytecode(const char *kind, const DxsoHeader &header, const DWORD *byte_code, size_t dwordCount) {
  const std::string &dir = shader_dump_dir();
  if (dir.empty() || !byte_code || dwordCount == 0)
    return;
  static std::mutex s_mutex;
  static std::unordered_set<uint64_t> s_written;
  static bool s_dir_announced = false;
  uint64_t hash = fnv1a_dwords(byte_code, dwordCount);
  bool first_call = false;
  {
    std::lock_guard<std::mutex> lk(s_mutex);
    if (!s_dir_announced) {
      s_dir_announced = true;
      first_call = true;
    }
    if (!s_written.insert(hash).second)
      return;
  }
  if (first_call) {
    // mkdir 0755; ignore EEXIST. Single-level only: caller is
    // expected to provide a path whose parent already exists.
    int mkrc = DXMT_MKDIR(dir.c_str());
    int err = (mkrc == 0) ? 0 : errno;
    Logger::warn(
        std::string("DXSO dump: directory=") + dir + " mkdir-rc=" + std::to_string(mkrc) +
        " (errno=" + std::to_string(err) + ")"
    );
  }
  char path[1024];
  const char *tag = (header.kind == DxsoShaderKind::Vertex) ? "vs" : "ps";
  std::snprintf(
      path, sizeof(path), "%s/%s_%u_%u_%016llx.bin", dir.c_str(), tag, static_cast<unsigned>(header.major),
      static_cast<unsigned>(header.minor), static_cast<unsigned long long>(hash)
  );
  std::FILE *f = std::fopen(path, "wb");
  if (!f) {
    Logger::warn(std::string("DXSO dump: fopen failed (errno=") + std::to_string(errno) + ") for " + path);
    return;
  }
  std::fwrite(byte_code, sizeof(DWORD), dwordCount, f);
  std::fclose(f);
  Logger::warn(
      std::string("DXSO dump: wrote ") + path + " (" + std::to_string(dwordCount * sizeof(DWORD)) + " bytes, " + kind +
      ")"
  );
}

const char *
usage_short(DxsoUsage u) {
  switch (u) {
  case DxsoUsage::Position:
    return "Position";
  case DxsoUsage::BlendWeight:
    return "BlendWeight";
  case DxsoUsage::BlendIndices:
    return "BlendIndices";
  case DxsoUsage::Normal:
    return "Normal";
  case DxsoUsage::PointSize:
    return "PointSize";
  case DxsoUsage::Texcoord:
    return "Texcoord";
  case DxsoUsage::Tangent:
    return "Tangent";
  case DxsoUsage::Binormal:
    return "Binormal";
  case DxsoUsage::TessFactor:
    return "TessFactor";
  case DxsoUsage::PositionT:
    return "PositionT";
  case DxsoUsage::Color:
    return "Color";
  case DxsoUsage::Fog:
    return "Fog";
  case DxsoUsage::Depth:
    return "Depth";
  case DxsoUsage::Sample:
    return "Sample";
  }
  return "?";
}

} // namespace

void
log_shader_dump(
    const char *kind, const DxsoHeader &header, const DxsoShaderMetadata &md, const DWORD *byte_code, size_t dwordCount
) {
  // Bytecode-to-disk works independently of the per-create summary
  // line: set DXMT_DUMP_PATH alone if the summary is too
  // noisy.
  maybe_write_bytecode(kind, header, byte_code, dwordCount);
  if (!shader_dump_enabled())
    return;
  std::string msg = std::string(kind) + " " + (header.kind == DxsoShaderKind::Vertex ? "vs" : "ps") + "_" +
                    std::to_string(header.major) + "_" + std::to_string(header.minor) +
                    " ins=" + std::to_string(md.instruction_count);
  if (!md.dcls.empty()) {
    msg += " dcls=[";
    for (size_t i = 0; i < md.dcls.size(); ++i) {
      if (i)
        msg += ",";
      const auto &d = md.dcls[i];
      if (d.dcl.texture_type != DxsoTextureType::Unknown) {
        msg += "tex" + std::to_string((unsigned)d.dcl.texture_type) + "@type" +
               std::to_string((unsigned)d.bound_to.type) + std::to_string((unsigned)d.bound_to.num);
      } else {
        msg += usage_short(d.dcl.usage);
        if (d.dcl.usage_index)
          msg += std::to_string((unsigned)d.dcl.usage_index);
        msg += "@type" + std::to_string((unsigned)d.bound_to.type) + std::to_string((unsigned)d.bound_to.num);
      }
    }
    msg += "]";
  }
  if (!md.consts.empty())
    msg += " consts=" + std::to_string(md.consts.size());
  if (md.uses_kill)
    msg += " kill";
  if (md.uses_derivatives)
    msg += " deriv";
  Logger::warn(msg);
}

// Compile a DXSO blob to an AIR metallib via airconv's cross-DLL
// thunks. The shader objects own the resulting bytes; the draw path
// turns them into MTLLibrary + MTLFunction when the (vs, ps, layout,
// rt formats) tuple resolves into a pipeline state. An empty result
// means compilation failed: caller treats it as a fatal mismatch
// rather than emitting a half-built shader.

// Compile DXSO bytecode to a Metal MTLFunction. Null function = hard error.
// WoW64: pointer from DXSOGetCompiledBitcode (unix-side 64-bit address)
// is fed to newLibraryFromNativeBuffer to avoid 32-bit dereference.
static WMT::Reference<WMT::Function>
compile_dxso_to_function(
    MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DXSO_SHADER_COMPILATION_ARGUMENT_DATA *args,
    const char *kind
) {
  WMT::Reference<WMT::Function> function;
  uint64_t library_handle_for_log = 0;
  dxso_shader_t handle = nullptr;
  if (DXSOInitialize(byte_code, dwordCount * sizeof(DWORD), &handle) != 0 || !handle) {
    // Surface the failure rather than silently returning a null
    // function: silent failures behind this early-return have hidden
    // ABI / dispatcher-table drift between the d3d9 caller and the
    // unix-side thunk for entire commit ranges.
    Logger::err(std::string("DXSO ") + kind + ": Initialize rejected bytecode");
    return function;
  }
  dxso_bitcode_t bc = nullptr;
  // LLVM internals on the airconv side can throw bad_alloc and similar;
  // the C-API thunks don't currently re-catch, so guard cleanup here so
  // a partial compile can't leak the dxso_shader_t handle.
  try {
    if (DXSOCompile(handle, args, "shader_main", &bc) == 0 && bc) {
      SM50_COMPILED_BITCODE blob{};
      DXSOGetCompiledBitcode(bc, &blob);
      // sm50_ptr64_t::operator uint64_t(): fully native uint64 carries
      // the high bits across the WoW64 boundary intact.
      uint64_t native_ptr = (uint64_t)blob.Data;
      uint64_t size = (uint64_t)blob.Size;
      if (native_ptr && size) {
        // Wine's main thread has no outer NSAutoreleasePool:
        // newLibrary autoreleases internally so the pool here keeps
        // Create-time leaks bounded.
        auto pool = WMT::MakeAutoreleasePool();
        WMT::Reference<WMT::Error> error;
        auto library = device->metalDevice().newLibraryFromNativeBuffer(native_ptr, size, error);
        if (!library) {
          std::string detail = error ? error.description().getUTF8String() : std::string("(no NSError)");
          Logger::err(std::string("DXSO ") + kind + ": MTLLibrary load failed: " + detail);
        } else {
          library_handle_for_log = library.handle;
          function = library.newFunction("shader_main");
          if (!function)
            Logger::err(std::string("DXSO ") + kind + ": MTLFunction shader_main not found");
        }
      }
    }
  } catch (...) {
    if (bc)
      DXSODestroyBitcode(bc);
    DXSODestroy(handle);
    throw;
  }
  if (bc)
    DXSODestroyBitcode(bc);
  DXSODestroy(handle);
  if (!function)
    Logger::err(std::string("DXSO ") + kind + ": compile to AIR failed");
  // Log function/library handles to bridge counter-keyed AIR dumps (kind_n.metallib) with hash-keyed bytecode files
  // (vs|ps_maj_min_hex-hash.bin). Without this, picking which dumped metallib corresponds to a failing PSO requires
  // counter ordering guesses.
  if (function && (std::getenv("DXMT_DUMP_PATH") || std::getenv("DXMT_AIRCONV_DUMP"))) {
    uint64_t hash = fnv1a_dwords(byte_code, dwordCount);
    char line[200];
    std::snprintf(
        line, sizeof(line), "DXSO %s: function=0x%llx library=0x%llx bytecode_hash=%016llx", kind,
        (unsigned long long)function.handle, (unsigned long long)library_handle_for_log, (unsigned long long)hash
    );
    Logger::warn(line);
  }
  return function;
}

MTLD3D9VertexShader::MTLD3D9VertexShader(
    MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata
) :
    m_device(device),
    m_metadata(std::move(metadata)) {
  m_bytecode.assign(byte_code, byte_code + dwordCount);
  m_function = compile_dxso_to_function(
      device, byte_code, dwordCount,
      /*args=*/nullptr, "vs"
  );
  AddRefPrivate();
}

MTLD3D9VertexShader::~MTLD3D9VertexShader() = default;

// FNV-1a fingerprint of layout: slot_mask, num_elements, per-element {reg, slot, offset, format, step_function,
// step_rate}. step_function and step_rate must be keyed: same vertex layout with different instancing settings collides
// silently, causing per-vertex-compiled MSL to read wrong vertices.
static uint64_t
layout_fingerprint(const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout) {
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&](uint32_t v) {
    h ^= static_cast<uint64_t>(v);
    h *= 0x100000001b3ull;
  };
  mix(layout.slot_mask);
  mix(layout.num_elements);
  mix(static_cast<uint32_t>(layout.index_buffer_format));
  for (uint32_t i = 0; i < layout.num_elements; ++i) {
    const auto &e = layout.elements[i];
    mix(e.reg);
    mix(e.slot);
    mix(e.aligned_byte_offset);
    mix(e.format);
    // step_function is a 1-bit field, step_rate is 31 bits: pack so
    // the full instancing state contributes to the hash.
    mix(e.step_function | (e.step_rate << 1));
  }
  return h;
}

WMT::Function
MTLD3D9VertexShader::compileVariant(const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout, float point_size_override) {
  // Cache hit on layout fingerprint. The MTLFunction lives on the
  // shader; the caller borrows the handle for the duration of the
  // PSO build, which never outlives the shader.
  const bool inject_point_size = point_size_override > 0.0f;
  uint64_t layout_key = layout_fingerprint(layout);
  if (!inject_point_size) {
    if (auto it = m_variantCache.find(layout_key); it != m_variantCache.end())
      return it->second;
  } else {
    // Key the point-size cache on (layout_fp XOR float-bits). Floats
    // are quantized at the call site (rs[D3DRS_POINTSIZE] read from a
    // DWORD slot, so the bit pattern is whatever the app stored)
    // which keeps the cache bounded: apps that genuinely sweep
    // point size pay the compile cost they asked for.
    uint32_t bits;
    std::memcpy(&bits, &point_size_override, sizeof(bits));
    uint64_t ps_key = layout_key ^ (static_cast<uint64_t>(bits) << 32);
    if (auto it = m_pointSizeVariantCache.find(ps_key); it != m_pointSizeVariantCache.end())
      return it->second;
  }

  // Triage: dump unique layouts under DXMT_DUMP_D9_LAYOUT=1 so we can
  // see what D3DDECLTYPE → MTLAttributeFormat the host is feeding the
  // manual-fetch path. Useful when a class of draws renders wrong
  // (champions invisible, particles miscoloured) and the suspect is
  // a format mismatch inside the layout we built from the vertex
  // declaration. One-shot per (shader, fingerprint).
  if (const char *en = std::getenv("DXMT_DUMP_D9_LAYOUT"); en && en[0] == '1') {
    std::string line = "[d3d9 layout] vs slot_mask=" + std::to_string(layout.slot_mask) +
                       " ib_fmt=" + std::to_string(static_cast<unsigned>(layout.index_buffer_format)) +
                       " elements=" + std::to_string(layout.num_elements);
    for (uint32_t i = 0; i < layout.num_elements; ++i) {
      const auto &e = layout.elements[i];
      line += " {v" + std::to_string(e.reg) + ",slot" + std::to_string(e.slot) + ",off" +
              std::to_string(e.aligned_byte_offset) + ",fmt" + std::to_string(e.format) + "}";
    }
    Logger::warn(line);
  }

  // Build a one- or two-element chain head pointing at a local copy of
  // the layout: the chain walker in DXSOCompile reads `type` and `next`
  // off the head and dispatches by type. Layouts are small (~16
  // elements typical), so the by-value copy is fine. The caller
  // owns the elements pointer it passed in. Append the optional
  // D3DRS_POINTSIZE arg when inject_point_size is true.
  DXSO_SHADER_IA_INPUT_LAYOUT_DATA layout_arg = layout;
  layout_arg.next = nullptr;
  layout_arg.type = DXSO_SHADER_IA_INPUT_LAYOUT;
  DXSO_SHADER_VS_POINT_SIZE_DATA point_size_arg{};
  if (inject_point_size) {
    point_size_arg.next = nullptr;
    point_size_arg.type = DXSO_SHADER_VS_POINT_SIZE;
    point_size_arg.value = point_size_override;
    layout_arg.next = &point_size_arg;
  }
  auto *args = reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(&layout_arg);

  auto fn = compile_dxso_to_function(m_device, m_bytecode.data(), m_bytecode.size(), args, "vs-variant");
  // Insert even on null: caches the failure too. A subsequent draw
  // with the same layout will short-circuit instead of re-burning the
  // compile path. The pipeline builder already treats null as a hard
  // mismatch.
  if (!inject_point_size) {
    auto [ins, _] = m_variantCache.emplace(layout_key, std::move(fn));
    return ins->second;
  }
  uint32_t bits;
  std::memcpy(&bits, &point_size_override, sizeof(bits));
  uint64_t ps_key = layout_key ^ (static_cast<uint64_t>(bits) << 32);
  auto [ins, _] = m_pointSizeVariantCache.emplace(ps_key, std::move(fn));
  return ins->second;
}

ULONG STDMETHODCALLTYPE
MTLD3D9VertexShader::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9VertexShader::Release() {
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    m_device->Release();
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexShader::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexShader9)) {
    *ppv = static_cast<IDirect3DVertexShader9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexShader::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexShader::GetFunction(void *pData, UINT *pSizeOfData) {
  // wined3d/dxvk shape: pSizeOfData is required (in/out). pData is
  // optional: when null, the call returns the required size only,
  // letting the app size its destination before re-calling. See
  // wined3d_shader_get_byte_code in dlls/wined3d/shader.c.
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT need = static_cast<UINT>(bytecodeByteLength());
  if (!pData) {
    *pSizeOfData = need;
    return D3D_OK;
  }
  if (*pSizeOfData < need)
    return D3DERR_INVALIDCALL;
  std::memcpy(pData, m_bytecode.data(), need);
  // Leave *pSizeOfData untouched on success-with-buffer: wined3d
  // (dlls/wined3d/shader.c) and DXVK
  // (src/d3d9/d3d9_shader.h) both omit the write here. Real apps
  // pass the buffer size in and don't expect it to come back changed.
  return D3D_OK;
}

// ---- MTLD3D9PixelShader: see header for shape rationale. The body
// is a near-copy of the vertex-shader path; the shared helper
// shader_bytecode_dword_count makes the bytecode walk identical, and
// the only differences are the QI vtable IID and the COM interface
// type the GetFunction contract is hung on.

MTLD3D9PixelShader::MTLD3D9PixelShader(
    MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata
) :
    m_device(device),
    m_metadata(std::move(metadata)) {
  m_bytecode.assign(byte_code, byte_code + dwordCount);
  m_function = compile_dxso_to_function(
      device, byte_code, dwordCount,
      /*args=*/nullptr, "ps"
  );
  AddRefPrivate();
}

MTLD3D9PixelShader::~MTLD3D9PixelShader() = default;

WMT::Function
MTLD3D9PixelShader::compileVariant(
    uint32_t alpha_test_func, uint32_t alpha_test_ref, const uint8_t samp_kinds[16], bool point_sprite,
    const ::DXSO_SHADER_PS_BUMP_ENV_DATA *bump_env, bool fog_blend
) {
  // FNV-1a over the (alpha-test, sampler-kinds, point-sprite, bump-env,
  // fog-blend) tuple. Alpha args use 8 bits each; kinds are also 8 bits each;
  // point_sprite is one bit folded into a byte. Bump-env constants
  // (16 stages × 6 floats) feed in byte-wise when bump_env != nullptr:
  // host only passes the arg when the shader actually uses TexBem
  // family (metadata.bem_stage_mask != 0), so non-bem shaders see the
  // same key as before this change.
  uint64_t key = 0xcbf29ce484222325ull;
  auto mix = [&](uint64_t v) {
    key ^= v;
    key *= 0x100000001b3ull;
  };
  mix(alpha_test_func & 0xFFu);
  mix(alpha_test_ref & 0xFFu);
  for (uint32_t i = 0; i < 16; ++i)
    mix(samp_kinds[i] & 0xFFu);
  mix(point_sprite ? 1u : 0u);
  // Fog folds in one bit only: the colour is a runtime constant read
  // from the bool-buffer tail, never part of the key.
  mix(fog_blend ? 1u : 0u);
  if (bump_env) {
    auto *bytes = reinterpret_cast<const uint8_t *>(bump_env->mat);
    for (size_t i = 0; i < sizeof(bump_env->mat); ++i)
      mix(bytes[i]);
    auto *ls_bytes = reinterpret_cast<const uint8_t *>(bump_env->lscale);
    for (size_t i = 0; i < sizeof(bump_env->lscale); ++i)
      mix(ls_bytes[i]);
    auto *lo_bytes = reinterpret_cast<const uint8_t *>(bump_env->loffset);
    for (size_t i = 0; i < sizeof(bump_env->loffset); ++i)
      mix(lo_bytes[i]);
  }
  if (auto it = m_variantCache.find(key); it != m_variantCache.end())
    return it->second;

  // Build arg chain: PSO_PIXEL_SHADER (if alpha test); PS_SAMPLER_LAYOUT (always, allows SM 1.x override per-slot
  // kinds); PS_POINT_SPRITE (if active, zero-data marker for [[point_coord]] routing); PS_BUMP_ENV (if present, carries
  // D3DTSS_BUMPENV* for TexBem); PS_FOG (if active, zero-data marker for the fixed fog blend).
  DXSO_SHADER_PSO_PIXEL_SHADER_DATA alpha_arg{};
  DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA samp_arg{};
  DXSO_SHADER_PS_POINT_SPRITE_DATA point_sprite_arg{};
  samp_arg.next = nullptr;
  samp_arg.type = DXSO_SHADER_PS_SAMPLER_LAYOUT;
  std::memcpy(samp_arg.kinds, samp_kinds, sizeof(samp_arg.kinds));
  void **tail_next = &samp_arg.next;
  if (point_sprite) {
    point_sprite_arg.next = nullptr;
    point_sprite_arg.type = DXSO_SHADER_PS_POINT_SPRITE;
    *tail_next = &point_sprite_arg;
    tail_next = &point_sprite_arg.next;
  }
  DXSO_SHADER_PS_BUMP_ENV_DATA bump_env_arg{};
  if (bump_env) {
    bump_env_arg = *bump_env;
    bump_env_arg.next = nullptr;
    bump_env_arg.type = DXSO_SHADER_PS_BUMP_ENV;
    *tail_next = &bump_env_arg;
    tail_next = &bump_env_arg.next;
  }
  DXSO_SHADER_PS_FOG_DATA fog_arg{};
  if (fog_blend) {
    fog_arg.next = nullptr;
    fog_arg.type = DXSO_SHADER_PS_FOG;
    *tail_next = &fog_arg;
  }

  DXSO_SHADER_COMPILATION_ARGUMENT_DATA *head;
  bool emit_alpha = alpha_test_func != D3DCMP_ALWAYS;
  if (emit_alpha) {
    alpha_arg.next = &samp_arg;
    alpha_arg.type = DXSO_SHADER_PSO_PIXEL_SHADER;
    alpha_arg.alpha_test_func = alpha_test_func;
    alpha_arg.alpha_test_ref = alpha_test_ref;
    head = reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(&alpha_arg);
  } else {
    head = reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(&samp_arg);
  }

  auto fn = compile_dxso_to_function(m_device, m_bytecode.data(), m_bytecode.size(), head, "ps-variant");
  // Insert even on null: caches the failure so subsequent draws with
  // the same tuple short-circuit instead of re-burning the compile
  // path. The pipeline builder treats null as a hard mismatch.
  auto [ins, _] = m_variantCache.emplace(key, std::move(fn));
  return ins->second;
}

WMT::Function
MTLD3D9PixelShader::compileAlphaVariant(uint32_t alpha_test_func, uint32_t alpha_test_ref) {
  // No host-provided sampler-kind layout: leave every slot UNKNOWN so
  // the shader's own dcl + the SM 1.x default-to-Texture2D arm in
  // dxso_compile decide the kind. point_sprite=false for back-compat
  // callers (POINTSPRITEENABLE is plumbed via the main compileVariant
  // entry from the draw-resolve site only).
  uint8_t kinds[16] = {};
  return compileVariant(alpha_test_func, alpha_test_ref, kinds, false);
}

ULONG STDMETHODCALLTYPE
MTLD3D9PixelShader::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9PixelShader::Release() {
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    m_device->Release();
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9PixelShader::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DPixelShader9)) {
    *ppv = static_cast<IDirect3DPixelShader9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9PixelShader::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9PixelShader::GetFunction(void *pData, UINT *pSizeOfData) {
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT need = static_cast<UINT>(bytecodeByteLength());
  if (!pData) {
    *pSizeOfData = need;
    return D3D_OK;
  }
  if (*pSizeOfData < need)
    return D3DERR_INVALIDCALL;
  std::memcpy(pData, m_bytecode.data(), need);
  return D3D_OK;
}

} // namespace dxmt
