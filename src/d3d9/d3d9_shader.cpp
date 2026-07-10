#include "d3d9_shader.hpp"

#include "airconv_public.h"
#include "d3d9_device.hpp"
#include "d3d9_point_size.hpp"
#include "d3d9_ps_variant_key.hpp"
#include "dxmt_shader_cache.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <variant>

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

} // namespace

uint64_t
bytecode_hash(const DWORD *byte_code, size_t dwordCount) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < dwordCount; ++i) {
    h ^= static_cast<uint64_t>(byte_code[i]);
    h *= 0x100000001b3ull;
  }
  return h;
}

namespace {

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
  uint64_t hash = bytecode_hash(byte_code, dwordCount);
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
// Disk shader cache (the same DXMT_SHADER_CACHE store the d3d11 side
// uses): key = (shader digest, variant digest), value = the compiled
// metallib bytes. The variant digest walks the compile-argument chain
// hashing each node's payload with pointers excluded; an unknown node
// type disables caching for the call instead of risking a stale hit.
// DXSO and the generated pair emit one AIR version regardless of the
// device, so the store instance is pinned rather than device-derived.
// The epoch joins the shader digest: bump it whenever the DXSO or
// fixed-function codegen changes the emitted AIR for an unchanged key,
// or stale entries outlive the change (the db-level version constant
// only guards the shared store format).
static constexpr uint32_t kD9ShaderCacheEpoch = 10;

static bool
hash_dxso_args(Sha1HashState &h, const DXSO_SHADER_COMPILATION_ARGUMENT_DATA *args) {
  for (const void *node = args; node;) {
    auto *head = static_cast<const DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(node);
    h.update(static_cast<uint32_t>(head->type));
    switch (head->type) {
    case DXSO_SHADER_IA_INPUT_LAYOUT: {
      auto *d = static_cast<const DXSO_SHADER_IA_INPUT_LAYOUT_DATA *>(node);
      // index_buffer_format is deliberately not hashed: the manual-fetch
      // prologue reads [[vertex_id]] (already index-resolved), so the emitted
      // function is byte-identical for NONE/UINT16/UINT32. Keying it would fan
      // the variant and PSO link out up to 3x per layout for no codegen change.
      h.update(d->slot_mask);
      h.update(d->num_elements);
      h.update(d->position_transformed);
      // The float-const count changes the direct/relative constant ceiling
      // the codegen bakes in, so a hardware-VP (256) and a software-VP (8192)
      // compile of the same bytecode must not alias in the disk cache.
      h.update(d->vs_float_const_count);
      if (d->num_elements)
        h.update(d->elements, sizeof(DXSO_IA_INPUT_ELEMENT) * d->num_elements);
      node = d->next;
      break;
    }
    case DXSO_SHADER_PSO_PIXEL_SHADER: {
      auto *d = static_cast<const DXSO_SHADER_PSO_PIXEL_SHADER_DATA *>(node);
      // alpha_test_ref is deliberately absent: the ref rides the bool-buffer
      // tail, so shaders that differ only in D3DRS_ALPHAREF share one cache
      // entry (the compare FUNC still keys the metallib).
      h.update(d->alpha_test_func);
      h.update(d->dual_source_blending);
      // flat_shading flips COLOR-input interpolation, so it changes the emitted
      // function and must key the digest as it keys the in-memory variant. Only
      // present in the digest when the node is emitted at all (alpha test /
      // dual-source / flat / sample-mask / unorm snap), matching the in-memory key.
      h.update(d->flat_shading);
      // emit_sample_mask adds the [[sample_mask]] coverage output, so it keys
      // the metallib exactly as it keys the in-memory variant.
      h.update(d->emit_sample_mask);
      // The 8-bit-unorm snap mask changes the color store (rint round-to-even),
      // so it keys the disk digest exactly as it keys the in-memory variant.
      h.update(d->unorm_output_reg_mask);
      node = d->next;
      break;
    }
    case DXSO_SHADER_PS_SAMPLER_LAYOUT: {
      auto *d = static_cast<const DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *>(node);
      h.update(d->kinds, sizeof(d->kinds));
      node = d->next;
      break;
    }
    case DXSO_SHADER_PS_POINT_SPRITE: {
      auto *d = static_cast<const DXSO_SHADER_PS_POINT_SPRITE_DATA *>(node);
      node = d->next;
      break;
    }
    case DXSO_SHADER_VS_POINT_SIZE: {
      auto *d = static_cast<const DXSO_SHADER_VS_POINT_SIZE_DATA *>(node);
      // The marker carries no payload (the size rides a uniform); its mere
      // presence distinguishes the injecting variant from the base, so hash
      // a stable token to key the cache on that bit.
      h.update(static_cast<uint32_t>(DXSO_SHADER_VS_POINT_SIZE));
      node = d->next;
      break;
    }
    case DXSO_SHADER_PS_FOG: {
      auto *d = static_cast<const DXSO_SHADER_PS_FOG_DATA *>(node);
      h.update(static_cast<uint32_t>(d->mode));
      h.update(d->coord_is_w);
      node = d->next;
      break;
    }
    case DXSO_SHADER_FFP_KEY: {
      auto *d = static_cast<const DXSO_SHADER_FFP_KEY_DATA *>(node);
      // Flat uint32 payload; hash everything after the chain header.
      h.update(
          reinterpret_cast<const uint8_t *>(d) + offsetof(DXSO_SHADER_FFP_KEY_DATA, kind),
          sizeof(*d) - offsetof(DXSO_SHADER_FFP_KEY_DATA, kind)
      );
      node = d->next;
      break;
    }
    default:
      return false;
    }
  }
  return true;
}

static ShaderCache &
d9_shader_cache() {
  return ShaderCache::getInstance(WMTMetal310);
}

static WMT::Reference<WMT::Function>
d9_cache_lookup(MTLD3D9Device *device, const std::pair<Sha1Digest, Sha1Digest> &key) {
  WMT::Reference<WMT::Function> function;
  auto reader = d9_shader_cache().getReader();
  if (!reader)
    return function;
  auto data = reader->get(key);
  if (data == nullptr)
    return function;
  auto pool = WMT::MakeAutoreleasePool();
  WMT::Reference<WMT::Error> error;
  auto library = device->metalDevice().newLibrary(data, error);
  if (!library)
    return function;
  function = library.newFunction("shader_main");
  return function;
}

static void
d9_cache_store(const std::pair<Sha1Digest, Sha1Digest> &key, uint64_t native_ptr, uint64_t size) {
  auto writer = d9_shader_cache().getWriter();
  if (!writer)
    return;
  auto data = WMT::MakeDispatchData(native_ptr, size);
  if (data != nullptr)
    writer->set(key, data);
}

static WMT::Reference<WMT::Function>
compile_dxso_to_function(
    MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DXSO_SHADER_COMPILATION_ARGUMENT_DATA *args,
    const char *kind
) {
  WMT::Reference<WMT::Function> function;
  uint64_t library_handle_for_log = 0;
  // Disk-cache probe before touching the compiler: a hit loads the
  // stored metallib and skips the whole translate + AIR link cost.
  std::pair<Sha1Digest, Sha1Digest> cache_key{};
  bool cacheable = false;
  {
    Sha1HashState hs;
    hs.update(static_cast<uint32_t>(0x64397378u)); /* domain tag "d9sx" */
    hs.update(kD9ShaderCacheEpoch);
    hs.update(byte_code, dwordCount * sizeof(DWORD));
    cache_key.first = hs.final();
    Sha1HashState hv;
    cacheable = hash_dxso_args(hv, args);
    cache_key.second = hv.final();
  }
  if (cacheable) {
    function = d9_cache_lookup(device, cache_key);
    if (function)
      return function;
  }
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
        if (cacheable)
          d9_cache_store(cache_key, native_ptr, size);
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
    uint64_t hash = bytecode_hash(byte_code, dwordCount);
    char line[200];
    std::snprintf(
        line, sizeof(line), "DXSO %s: function=0x%llx library=0x%llx bytecode_hash=%016llx", kind,
        (unsigned long long)function.handle, (unsigned long long)library_handle_for_log, (unsigned long long)hash
    );
    Logger::warn(line);
  }
  return function;
}

uint64_t layout_fingerprint(const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout);

// Field-wise equality over exactly what layout_fingerprint folds. Used by the
// variant caches on a hash hit to rule out a fingerprint / key collision
// before reusing a compiled function. The stored side keeps its elements in a
// separate vector (the layout's own elements pointer is left dangling once the
// per-resolve scratch it pointed at is gone), so it is passed explicitly
// rather than read back through stored_layout.elements.
bool layout_matches(
    const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &stored_layout, const std::vector<DXSO_IA_INPUT_ELEMENT> &stored_elements,
    const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &incoming
);

// Fixed-function sibling of compile_dxso_to_function: no bytecode and
// no shader handle; the key + layout ride the same argument chain into
// DXSOCompile's generated-shader mode, and the bitcode -> MTLLibrary ->
// MTLFunction tail is identical.
static WMT::Reference<WMT::Function>
compile_ffp_to_function(
    MTLD3D9Device *device, uint32_t stage_kind, const DXSO_SHADER_IA_INPUT_LAYOUT_DATA *layout, bool has_diffuse,
    bool has_texcoord0, bool has_specular, uint32_t tex0_mode, const uint32_t (*stages)[3], uint32_t lighting_key,
    uint32_t texcoord_mask, uint32_t texcoord_transform_key, uint32_t fog_vertex_mode, bool range_fog, bool point_size,
    bool point_sprite, bool point_scale, uint32_t vertex_blend, uint32_t texgen_key, uint32_t texcoord_index_key,
    uint32_t sampler_kind_key, bool flat_shading, bool point_size_per_vertex, bool decl_has_diffuse, int ps_fog_mode,
    bool ps_fog_coord_w, uint32_t alpha_func, bool emit_sample_mask, uint32_t unorm_snap_mask, const char *kind
) {
  WMT::Reference<WMT::Function> function;
  DXSO_SHADER_FFP_KEY_DATA key{};
  key.next = nullptr;
  key.type = DXSO_SHADER_FFP_KEY;
  key.kind = stage_kind;
  key.has_diffuse = has_diffuse ? 1u : 0u;
  key.decl_has_diffuse = decl_has_diffuse ? 1u : 0u;
  key.has_texcoord0 = has_texcoord0 ? 1u : 0u;
  key.has_specular = has_specular ? 1u : 0u;
  key.tex0_mode = tex0_mode;
  if (stages)
    std::memcpy(key.stages, stages, sizeof(key.stages));
  key.point_size = point_size ? 1u : 0u;
  key.point_sprite = point_sprite ? 1u : 0u;
  key.point_scale = point_scale ? 1u : 0u;
  key.lighting_key = lighting_key;
  key.texcoord_mask = texcoord_mask;
  key.texcoord_transform_key = texcoord_transform_key;
  key.fog_vertex_mode = fog_vertex_mode;
  key.range_fog = range_fog ? 1u : 0u;
  key.vertex_blend = vertex_blend;
  key.texgen_key = texgen_key;
  key.texcoord_index_key = texcoord_index_key;
  key.sampler_kind_key = sampler_kind_key;
  key.flat_shading = flat_shading ? 1u : 0u;
  key.emit_sample_mask = emit_sample_mask ? 1u : 0u;
  key.point_size_per_vertex = point_size_per_vertex ? 1u : 0u;
  // The fog and alpha-test axes ride the same chain arguments the
  // bytecode variants use rather than growing the key struct.
  DXSO_SHADER_PS_FOG_DATA fog_arg{};
  DXSO_SHADER_PSO_PIXEL_SHADER_DATA ps_arg{};
  void *chain_head = &key;
  if (stage_kind == 1) {
    void *next = nullptr;
    // Emit the PSO_PIXEL_SHADER node when the alpha compare or the unorm snap is
    // active. The compare FUNC keys the metallib (the ref rides the bool-buffer
    // tail, read at runtime); the snap mask keys the 8-bit-unorm color store.
    if (alpha_func != 8 /* D3DCMP_ALWAYS */ || unorm_snap_mask != 0) {
      ps_arg.next = next;
      ps_arg.type = DXSO_SHADER_PSO_PIXEL_SHADER;
      ps_arg.alpha_test_func = alpha_func;
      ps_arg.unorm_output_reg_mask = unorm_snap_mask;
      next = &ps_arg;
    }
    if (ps_fog_mode >= 0) {
      fog_arg.next = next;
      fog_arg.type = DXSO_SHADER_PS_FOG;
      fog_arg.mode = (DXSO_PS_FOG_MODE)ps_fog_mode;
      fog_arg.coord_is_w = ps_fog_coord_w ? 1u : 0u;
      next = &fog_arg;
    }
    key.next = next;
  }
  DXSO_SHADER_IA_INPUT_LAYOUT_DATA layout_arg{};
  DXSO_SHADER_COMPILATION_ARGUMENT_DATA *args;
  if (layout) {
    layout_arg = *layout;
    layout_arg.type = DXSO_SHADER_IA_INPUT_LAYOUT;
    layout_arg.next = chain_head;
    args = reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(&layout_arg);
  } else {
    args = reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(chain_head);
  }
  // Disk-cache probe: the generated pair has no bytecode, so a fixed
  // tag stands in for the shader digest and the argument chain alone
  // is the variant.
  std::pair<Sha1Digest, Sha1Digest> cache_key{};
  bool cacheable = false;
  {
    Sha1HashState hs;
    hs.update(static_cast<uint32_t>(0x64396670u)); /* domain tag "d9fp" */
    hs.update(kD9ShaderCacheEpoch);
    cache_key.first = hs.final();
    Sha1HashState hv;
    cacheable = hash_dxso_args(hv, reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(args));
    cache_key.second = hv.final();
  }
  if (cacheable) {
    function = d9_cache_lookup(device, cache_key);
    if (function)
      return function;
  }
  dxso_bitcode_t bc = nullptr;
  try {
    if (DXSOCompile(nullptr, args, "shader_main", &bc) == 0 && bc) {
      SM50_COMPILED_BITCODE blob{};
      DXSOGetCompiledBitcode(bc, &blob);
      uint64_t native_ptr = (uint64_t)blob.Data;
      uint64_t size = (uint64_t)blob.Size;
      if (native_ptr && size) {
        if (cacheable)
          d9_cache_store(cache_key, native_ptr, size);
        auto pool = WMT::MakeAutoreleasePool();
        WMT::Reference<WMT::Error> error;
        auto library = device->metalDevice().newLibraryFromNativeBuffer(native_ptr, size, error);
        if (!library) {
          std::string detail = error ? error.description().getUTF8String() : std::string("(no NSError)");
          Logger::err(std::string("FFP ") + kind + ": MTLLibrary load failed: " + detail);
        } else {
          function = library.newFunction("shader_main");
          if (!function)
            Logger::err(std::string("FFP ") + kind + ": MTLFunction shader_main not found");
        }
      }
    }
  } catch (...) {
    if (bc)
      DXSODestroyBitcode(bc);
    throw;
  }
  if (bc)
    DXSODestroyBitcode(bc);
  if (!function)
    Logger::err(std::string("FFP ") + kind + ": compile to AIR failed");
  return function;
}

// Per-variant compile inputs, one struct per compile shape. Each is fully
// self-contained so the pool thread needs nothing from the transient
// encode-side resolve state: layout.elements points into per-resolve scratch
// that does not outlive the resolve, so the element array is deep-copied here
// (compile() re-points the layout at that copy). Mirrors d3d11's ShaderVariant
// std::variant of input structs (d3d11_shader.hpp / d3d11_pipeline_cache.cpp).
struct D3D9VsVariant {
  MTLD3D9VertexShaderModule *module; // pins the bytecode (device-lifetime)
  DXSO_SHADER_IA_INPUT_LAYOUT_DATA layout;
  std::vector<DXSO_IA_INPUT_ELEMENT> elements;
  bool inject_point_size;
};
struct D3D9PsVariant {
  MTLD3D9PixelShaderModule *module;
  uint32_t alpha_test_func;
  uint8_t samp_kinds[16];
  bool point_sprite;
  int fog_mode;
  bool fog_coord_w;
  bool dual_source;
  bool flat_shading;
  bool emit_sample_mask;
  uint32_t unorm_snap_mask;
};
struct D3D9FfpVsVariant {
  MTLD3D9Device *device;
  DXSO_SHADER_IA_INPUT_LAYOUT_DATA layout;
  std::vector<DXSO_IA_INPUT_ELEMENT> elements;
  bool has_diffuse;
  bool has_texcoord0;
  bool has_specular;
  uint32_t fog_vertex_mode;
  bool range_fog;
  bool point_size;
  bool point_scale;
  uint32_t lighting_key;
  uint32_t texcoord_mask;
  uint32_t texcoord_transform_key;
  uint32_t vertex_blend;
  uint32_t texgen_key;
  uint32_t texcoord_index_key;
  bool point_size_per_vertex;
  bool decl_has_diffuse;
};
struct D3D9FfpPsVariant {
  MTLD3D9Device *device;
  uint32_t stages[8][3];
  bool specular_enable;
  bool point_sprite;
  int fog_mode;
  bool fog_coord_w;
  uint32_t alpha_func;
  uint32_t sampler_kind_key;
  bool flat_shading;
  bool emit_sample_mask;
  uint32_t unorm_snap_mask;
};

// The async function-compile task: runs the LLVM AIR emit off the calling
// thread and latches the resulting MTLFunction. A leaf task with no
// dependencies, so RunTask always self-completes and the scheduler always
// sets its done bit, even on failure (a null function is cached; the emit is
// deterministic on its input, so re-running cannot succeed). The bodies below
// are the former inline compileVariant / ffp*Function bodies minus their cache
// lookups.
class D3D9FunctionCompileTask final : public D3D9CompiledFunction {
public:
  using Input = std::variant<D3D9VsVariant, D3D9PsVariant, D3D9FfpVsVariant, D3D9FfpPsVariant>;

  explicit D3D9FunctionCompileTask(Input input) : m_input(std::move(input)) {}

  D3D9AsyncTask *
  RunTask() override {
    m_function = std::visit([](auto &v) { return compile(v); }, m_input);
    return this;
  }

  bool
  GetDone() const noexcept override {
    return m_ready.load(std::memory_order_acquire);
  }
  void
  SetDone(bool s) noexcept override {
    m_ready.store(s, std::memory_order_release);
    m_ready.notify_all();
  }

  WMT::Function
  function() override {
    return m_function ? WMT::Function{m_function.handle} : WMT::Function{};
  }

  // The variant inputs this task compiled from. The device FFP caches and the
  // per-module variant caches key on a 64-bit fold of these; on a hash hit
  // they compare the stored inputs against the incoming ones to rule out a key
  // collision before reuse, the same full-key verify the bytecode module cache
  // applies (see bytecode_equal). std::get_if against the known alternative
  // keeps it exception-free.
  const Input &
  input() const noexcept {
    return m_input;
  }

private:
  static WMT::Reference<WMT::Function> compile(D3D9VsVariant &v);
  static WMT::Reference<WMT::Function> compile(D3D9PsVariant &v);
  static WMT::Reference<WMT::Function> compile(D3D9FfpVsVariant &v);
  static WMT::Reference<WMT::Function> compile(D3D9FfpPsVariant &v);

  Input m_input;
  WMT::Reference<WMT::Function> m_function;
  mutable std::atomic<bool> m_ready{false};
};

WMT::Reference<WMT::Function>
D3D9FunctionCompileTask::compile(D3D9VsVariant &v) {
  DXSO_SHADER_IA_INPUT_LAYOUT_DATA layout = v.layout;
  layout.elements = v.elements.data();
  // The float constant file size is a device property (256 hardware-VP, up to
  // 8192 software / mixed-VP); thread it so the codegen sizes its direct-read
  // ceiling and reladdr clamp to the file the device actually binds.
  layout.vs_float_const_count = v.module->device()->vertexShaderFloatConstantCount();

  // Triage: dump unique layouts under DXMT_DUMP_D9_LAYOUT=1 so we can
  // see what D3DDECLTYPE → MTLAttributeFormat the host is feeding the
  // manual-fetch path. Useful when a class of draws renders wrong and the
  // suspect is a format mismatch inside the layout built from the vertex
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

  // Build a one- or two-element chain head pointing at the local layout copy:
  // the chain walker in DXSOCompile reads `type` and `next` and dispatches by
  // type. Append the point-size marker when injecting (the size rides a
  // uniform, not the key).
  layout.next = nullptr;
  layout.type = DXSO_SHADER_IA_INPUT_LAYOUT;
  DXSO_SHADER_VS_POINT_SIZE_DATA point_size_arg{};
  if (v.inject_point_size) {
    point_size_arg.next = nullptr;
    point_size_arg.type = DXSO_SHADER_VS_POINT_SIZE;
    layout.next = &point_size_arg;
  }
  auto *args = reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(&layout);
  return compile_dxso_to_function(
      v.module->device(), v.module->bytecode(), v.module->bytecodeByteLength() / sizeof(DWORD), args, "vs-variant"
  );
}

WMT::Reference<WMT::Function>
D3D9FunctionCompileTask::compile(D3D9PsVariant &v) {
  // Build arg chain: PSO_PIXEL_SHADER (if alpha test / dual-source / flat);
  // PS_SAMPLER_LAYOUT (always, allows SM 1.x override per-slot kinds);
  // PS_POINT_SPRITE (if active); PS_FOG (if active). TexBem/TexBemL/Bem read
  // the bump-env matrix + luminance from the shared PS uniform tail at
  // runtime, so no arg carries them.
  DXSO_SHADER_PSO_PIXEL_SHADER_DATA alpha_arg{};
  DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA samp_arg{};
  DXSO_SHADER_PS_POINT_SPRITE_DATA point_sprite_arg{};
  samp_arg.next = nullptr;
  samp_arg.type = DXSO_SHADER_PS_SAMPLER_LAYOUT;
  std::memcpy(samp_arg.kinds, v.samp_kinds, sizeof(samp_arg.kinds));
  void **tail_next = &samp_arg.next;
  if (v.point_sprite) {
    point_sprite_arg.next = nullptr;
    point_sprite_arg.type = DXSO_SHADER_PS_POINT_SPRITE;
    *tail_next = &point_sprite_arg;
    tail_next = &point_sprite_arg.next;
  }
  DXSO_SHADER_PS_FOG_DATA fog_arg{};
  if (v.fog_mode >= 0) {
    fog_arg.next = nullptr;
    fog_arg.type = DXSO_SHADER_PS_FOG;
    fog_arg.mode = (uint32_t)v.fog_mode;
    fog_arg.coord_is_w = v.fog_coord_w ? 1u : 0u;
    *tail_next = &fog_arg;
  }

  // The PSO_PIXEL_SHADER arg carries the alpha-test tuple, the dual-source
  // flag, the flat-shading gate, the sample-mask gate and the 8-bit-unorm snap
  // mask, so emit it when any is active; leaving it out keeps the unspecialised
  // PS shape for the common case.
  DXSO_SHADER_COMPILATION_ARGUMENT_DATA *head;
  bool emit_alpha = v.alpha_test_func != D3DCMP_ALWAYS;
  if (emit_alpha || v.dual_source || v.flat_shading || v.emit_sample_mask || v.unorm_snap_mask) {
    alpha_arg.next = &samp_arg;
    alpha_arg.type = DXSO_SHADER_PSO_PIXEL_SHADER;
    alpha_arg.alpha_test_func = v.alpha_test_func;
    alpha_arg.dual_source_blending = v.dual_source ? 1u : 0u;
    alpha_arg.flat_shading = v.flat_shading ? 1u : 0u;
    alpha_arg.emit_sample_mask = v.emit_sample_mask ? 1u : 0u;
    alpha_arg.unorm_output_reg_mask = v.unorm_snap_mask;
    head = reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(&alpha_arg);
  } else {
    head = reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA *>(&samp_arg);
  }

  return compile_dxso_to_function(
      v.module->device(), v.module->bytecode(), v.module->bytecodeByteLength() / sizeof(DWORD), head, "ps-variant"
  );
}

WMT::Reference<WMT::Function>
D3D9FunctionCompileTask::compile(D3D9FfpVsVariant &v) {
  DXSO_SHADER_IA_INPUT_LAYOUT_DATA layout = v.layout;
  layout.elements = v.elements.data();
  return compile_ffp_to_function(
      v.device, 0, &layout, v.has_diffuse, v.has_texcoord0, v.has_specular, 0, nullptr, v.lighting_key,
      v.texcoord_mask, v.texcoord_transform_key, v.fog_vertex_mode, v.range_fog, v.point_size, false, v.point_scale,
      v.vertex_blend, v.texgen_key, v.texcoord_index_key, 0, false, v.point_size_per_vertex, v.decl_has_diffuse, -1,
      false, 8, false /*emit_sample_mask: VS never emits coverage*/, 0 /*unorm_snap_mask: VS writes no color*/, "vs"
  );
}

WMT::Reference<WMT::Function>
D3D9FunctionCompileTask::compile(D3D9FfpPsVariant &v) {
  return compile_ffp_to_function(
      v.device, 1, nullptr, false, false, false, 2, v.stages, v.specular_enable ? 4u : 0u, 0, 0, 0 /*fog_vertex_mode*/,
      false /*range_fog*/, 0, v.point_sprite, false, 0, 0, 0, v.sampler_kind_key, v.flat_shading, false, false,
      v.fog_mode, v.fog_coord_w, v.alpha_func, v.emit_sample_mask, v.unorm_snap_mask, "ps"
  );
}

// Device-level caches for the generated pair. Failures cache as a task whose
// function() stays null, the same contract the bytecode variant caches keep;
// the pipeline builder treats a null function as a hard mismatch. The encode
// thread is the sole toucher of these maps (get-or-create stays here); the
// pool threads only write inside their own task, published by SetDone.
D3D9CompiledFunction *
MTLD3D9Device::ffpVertexFunction(
    const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout, bool has_diffuse, bool has_texcoord0, bool has_specular,
    uint32_t fog_vertex_mode, bool range_fog, bool point_size, bool point_scale, uint32_t lighting_key,
    uint32_t texcoord_mask, uint32_t texcoord_transform_key, uint32_t vertex_blend, uint32_t texgen_key,
    uint32_t texcoord_index_key, bool point_size_per_vertex, bool decl_has_diffuse
) {
  // Every term folds in through its own distinct 64-bit constant, all pairwise
  // distinct (the XOR constants, the multipliers here, and the point-size
  // sentinels in ffp_point_size_variant_key). Distinctness is required for
  // correctness: two terms that are not decorrelated by the layout fingerprint
  // can otherwise collide, e.g. POINTSCALEENABLE (a render state) and
  // TEXCOORDINDEX (a texture-stage state) both fold in and neither moves the
  // fingerprint, so a shared constant would hash {point_scale, tci=0} and
  // {no point_scale, tci=1} to one key (silent wrong shader). Keeping all
  // constants distinct removes any reliance on which pairs the fingerprint saves.
  uint64_t base = layout_fingerprint(layout) ^ (has_diffuse ? 0x9e3779b97f4a7c15ull : 0ull) ^
                  (has_texcoord0 ? 0xc2b2ae3d27d4eb4full : 0ull) ^ (has_specular ? 0x165667b19e3779f9ull : 0ull) ^
                  (uint64_t(fog_vertex_mode) << 57) ^ (uint64_t(lighting_key) * 0xff51afd7ed558ccdull) ^
                  (uint64_t(texcoord_mask) << 33) ^ (uint64_t(texcoord_transform_key) * 0xc4ceb9fe1a85ec53ull) ^
                  (uint64_t(vertex_blend) << 51) ^ (uint64_t(texgen_key) * 0xbf58476d1ce4e5b9ull) ^
                  (uint64_t(texcoord_index_key) * 0x27d4eb2f165667c5ull) ^
                  (decl_has_diffuse ? 0x2545f4914f6cdd1dull : 0ull) ^ (range_fog ? 0xa0761d6478bd642full : 0ull);
  // The point size and its clamp bounds ride the uniform, so only the
  // point-vs-nonpoint / POINTSCALEENABLE / per-vertex gates key the
  // variant; distinct sizes share one generated function.
  uint64_t key = ffp_point_size_variant_key(base, point_size, point_scale, point_size_per_vertex, 0, 0, 0, 0, 0, 0);
  if (auto it = m_ffpVSCache.find(key); it != m_ffpVSCache.end()) {
    // Full-key verify (same discipline as the bytecode module cache): confirm
    // every folded input matches before reusing the generated function. The
    // key XORs many render-state terms together, and XOR-combination
    // uniqueness needs linear independence of the constant set, not just the
    // pairwise distinctness the fold's comment claims, so this compare is what
    // actually rules out a collision returning the wrong shader.
    const auto *v = std::get_if<D3D9FfpVsVariant>(&static_cast<D3D9FunctionCompileTask *>(it->second.get())->input());
    // point_scale / point_size_per_vertex enter the key only on a point-list
    // draw (ffp_point_size_variant_key gates them on point_size), so compare
    // them only then: matching exactly what the fold keyed on keeps a
    // non-point draw from false-missing its cached function.
    if (v && v->has_diffuse == has_diffuse && v->has_texcoord0 == has_texcoord0 && v->has_specular == has_specular &&
        v->fog_vertex_mode == fog_vertex_mode && v->range_fog == range_fog && v->point_size == point_size &&
        v->lighting_key == lighting_key && v->texcoord_mask == texcoord_mask &&
        v->texcoord_transform_key == texcoord_transform_key && v->vertex_blend == vertex_blend &&
        v->texgen_key == texgen_key && v->texcoord_index_key == texcoord_index_key &&
        v->decl_has_diffuse == decl_has_diffuse &&
        (!point_size || (v->point_scale == point_scale && v->point_size_per_vertex == point_size_per_vertex)) &&
        layout_matches(v->layout, v->elements, layout))
      return it->second.get();
    // Verified 64-bit collision: fall through and build a fresh function; the
    // loser is pinned but not cached (see the try_emplace below).
  }
  // Deep-copy the transient element array (layout.elements points into
  // per-resolve scratch), then submit on create so the PSO task that waits
  // on this can never park on an unsubmitted dependency.
  D3D9FfpVsVariant input{};
  input.device = this;
  input.layout = layout;
  input.elements.assign(layout.elements, layout.elements + layout.num_elements);
  input.has_diffuse = has_diffuse;
  input.has_texcoord0 = has_texcoord0;
  input.has_specular = has_specular;
  input.fog_vertex_mode = fog_vertex_mode;
  input.range_fog = range_fog;
  input.point_size = point_size;
  input.point_scale = point_scale;
  input.lighting_key = lighting_key;
  input.texcoord_mask = texcoord_mask;
  input.texcoord_transform_key = texcoord_transform_key;
  input.vertex_blend = vertex_blend;
  input.texgen_key = texgen_key;
  input.texcoord_index_key = texcoord_index_key;
  input.point_size_per_vertex = point_size_per_vertex;
  input.decl_has_diffuse = decl_has_diffuse;
  auto task = std::make_unique<D3D9FunctionCompileTask>(std::move(input));
  auto *raw = task.get();
  // A true miss inserts; a verified collision leaves try_emplace's argument
  // un-moved, so pin the loser for device lifetime in m_functionKeyCollisions
  // instead of dropping it: the PSO task keys on and holds a non-owning
  // pointer to this task. A collision is astronomically rare, so the loser
  // never being deduped again is acceptable.
  if (!m_ffpVSCache.try_emplace(key, std::move(task)).second)
    m_functionKeyCollisions.push_back(std::move(task));
  m_psoScheduler.submit(raw);
  return raw;
}

D3D9CompiledFunction *
MTLD3D9Device::ffpPixelFunction(
    const uint32_t (*stages)[3], bool specular_enable, bool point_sprite, int fog_mode, bool fog_coord_w,
    uint32_t alpha_func, uint32_t sampler_kind_key, bool flat_shading, bool emit_sample_mask,
    uint32_t unorm_snap_mask
) {
  // FNV over the combiner table plus the packed bounded axes. The alpha
  // ref rides the bool-buffer tail (ffp_ps_variant_key drops it), so
  // distinct D3DRS_ALPHAREF values share one generated combiner; the
  // D3DRS_MULTISAMPLEMASK word rides the same tail, so only its enable keys.
  // The unorm snap mask DOES key: it changes the rt0 color store, so an
  // 8-bit-unorm and a float target must not share one combiner.
  uint64_t key = ffp_ps_variant_key(
      stages, specular_enable, point_sprite, fog_mode, fog_coord_w, alpha_func, sampler_kind_key, flat_shading,
      emit_sample_mask, unorm_snap_mask
  );
  if (auto it = m_ffpPSCache.find(key); it != m_ffpPSCache.end()) {
    // Full-key verify (same discipline as the bytecode module cache): confirm
    // every folded input matches before reuse, so a 64-bit key collision
    // compiles a fresh combiner instead of returning the wrong one. The stages
    // table is a contiguous uint32 array with no padding, so memcmp is safe.
    const auto *v = std::get_if<D3D9FfpPsVariant>(&static_cast<D3D9FunctionCompileTask *>(it->second.get())->input());
    if (v && std::memcmp(v->stages, stages, sizeof(v->stages)) == 0 && v->specular_enable == specular_enable &&
        v->point_sprite == point_sprite && v->fog_mode == fog_mode && v->fog_coord_w == fog_coord_w &&
        v->alpha_func == alpha_func && v->sampler_kind_key == sampler_kind_key && v->flat_shading == flat_shading &&
        v->emit_sample_mask == emit_sample_mask && v->unorm_snap_mask == unorm_snap_mask)
      return it->second.get();
    // Verified 64-bit collision: fall through to a fresh (uncached) compile.
  }
  D3D9FfpPsVariant input{};
  input.device = this;
  std::memcpy(input.stages, stages, sizeof(input.stages));
  input.specular_enable = specular_enable;
  input.point_sprite = point_sprite;
  input.fog_mode = fog_mode;
  input.fog_coord_w = fog_coord_w;
  input.alpha_func = alpha_func;
  input.sampler_kind_key = sampler_kind_key;
  input.flat_shading = flat_shading;
  input.emit_sample_mask = emit_sample_mask;
  input.unorm_snap_mask = unorm_snap_mask;
  auto task = std::make_unique<D3D9FunctionCompileTask>(std::move(input));
  auto *raw = task.get();
  // See ffpVertexFunction: pin a collision loser for device lifetime rather
  // than drop the task the PSO cache holds a non-owning pointer to.
  if (!m_ffpPSCache.try_emplace(key, std::move(task)).second)
    m_functionKeyCollisions.push_back(std::move(task));
  m_psoScheduler.submit(raw);
  return raw;
}

MTLD3D9VertexShaderModule::MTLD3D9VertexShaderModule(
    MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata
) :
    m_device(device),
    m_metadata(std::move(metadata)) {
  m_bytecode.assign(byte_code, byte_code + dwordCount);
}

void
MTLD3D9VertexShaderModule::incRef() {
  m_refcount.fetch_add(1u, std::memory_order_acquire);
}

void
MTLD3D9VertexShaderModule::decRef() {
  if (m_refcount.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
}

// FNV-1a fingerprint of layout: slot_mask, num_elements, per-element {reg, slot, offset, format, step_function,
// step_rate}. step_function and step_rate must be keyed: same vertex layout with different instancing settings collides
// silently, causing per-vertex-compiled MSL to read wrong vertices.
uint64_t
layout_fingerprint(const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout) {
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&](uint32_t v) {
    h ^= static_cast<uint64_t>(v);
    h *= 0x100000001b3ull;
  };
  mix(layout.slot_mask);
  mix(layout.num_elements);
  // index_buffer_format is deliberately not mixed: the manual-fetch prologue
  // reads [[vertex_id]] (already index-resolved), so the same layout drawn
  // indexed vs non-indexed (or 16- vs 32-bit) compiles one identical function.
  // Pre-transformed draws inject the screen->clip remap into the VS
  // epilogue, so they are a distinct variant from the same layout drawn
  // untransformed.
  mix(layout.position_transformed);
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

bool
layout_matches(
    const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &stored_layout, const std::vector<DXSO_IA_INPUT_ELEMENT> &stored_elements,
    const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &incoming
) {
  if (stored_layout.slot_mask != incoming.slot_mask || stored_layout.num_elements != incoming.num_elements ||
      stored_layout.position_transformed != incoming.position_transformed)
    return false;
  if (stored_elements.size() != incoming.num_elements)
    return false;
  for (uint32_t i = 0; i < incoming.num_elements; ++i) {
    const auto &a = stored_elements[i];
    const auto &b = incoming.elements[i];
    if (a.reg != b.reg || a.slot != b.slot || a.aligned_byte_offset != b.aligned_byte_offset || a.format != b.format ||
        a.step_function != b.step_function || a.step_rate != b.step_rate)
      return false;
  }
  return true;
}

D3D9CompiledFunction *
MTLD3D9VertexShaderModule::getVariantTask(const DXSO_SHADER_IA_INPUT_LAYOUT_DATA &layout, bool inject_point_size) {
  // Cache hit on layout fingerprint. The task lives on the module; the
  // caller borrows a non-owning pointer for the PSO build, which never
  // outlives the module. The point-size render state (size, min, max) rides
  // a per-draw uniform at VS buffer 6, so the variant key gains only the
  // single injection bit: distinct point sizes share one variant instead of
  // minting a cold PSO link per value the way the old float-keyed cache did.
  // DXVK feeds the size the same way (d3d9_fixed_function.cpp
  // GetPointSizeInfoVS). The task caches a failure as a null function() too,
  // so a subsequent draw with the same layout short-circuits here instead of
  // re-burning the compile path; the pipeline builder treats null as a hard
  // mismatch.
  uint64_t key = point_size_variant_key(layout_fingerprint(layout), inject_point_size);
  if (auto it = m_variantCache.find(key); it != m_variantCache.end()) {
    // Full-key verify (same discipline as the bytecode module cache): the key
    // is a lossy fold of the layout fingerprint plus the injection bit, so a
    // hash hit is confirmed against the actual layout + bit before reuse,
    // ruling out a collision compiling draws against the wrong variant.
    const auto *v = std::get_if<D3D9VsVariant>(&static_cast<D3D9FunctionCompileTask *>(it->second.get())->input());
    if (v && v->inject_point_size == inject_point_size && layout_matches(v->layout, v->elements, layout))
      return it->second.get();
    // Verified 64-bit collision: fall through to a fresh (uncached) compile.
  }

  // Deep-copy the transient element array (layout.elements points into
  // per-resolve scratch that does not outlive this call, but the compile
  // runs later on a pool thread). Submit on create so the PSO task that
  // waits on this variant can never park on an unsubmitted dependency.
  D3D9VsVariant input{};
  input.module = this;
  input.layout = layout;
  input.elements.assign(layout.elements, layout.elements + layout.num_elements);
  input.inject_point_size = inject_point_size;
  auto task = std::make_unique<D3D9FunctionCompileTask>(std::move(input));
  auto *raw = task.get();
  // A true miss inserts; a verified collision leaves try_emplace's argument
  // un-moved, so pin the loser for module lifetime in m_variantCacheCollisions
  // rather than drop the task the PSO cache holds a non-owning pointer to.
  if (!m_variantCache.try_emplace(key, std::move(task)).second)
    m_variantCacheCollisions.push_back(std::move(task));
  m_device->submitAsyncTask(raw);
  return raw;
}

// The module Rc arrives already retained by the device map; the wrapper
// just holds a reference to it. No public AddRef on the wrapper here: the
// COM refcount starts at 0 and is driven by ::dxmt::ref at the Create
// site, independent of the module's Rc lifetime. AddRefPrivate is the
// self-pin (dropped exactly once in Release).
MTLD3D9VertexShader::MTLD3D9VertexShader(MTLD3D9Device *device, Rc<MTLD3D9VertexShaderModule> module) :
    m_device(device),
    m_module(std::move(module)) {
  AddRefPrivate();
}

MTLD3D9VertexShader::~MTLD3D9VertexShader() = default;

ULONG STDMETHODCALLTYPE
MTLD3D9VertexShader::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9VertexShader::Release() {
  // D3D9 Release-at-0 clamp: handed out at public 0 while self-pinned / bound
  // (DXVK clamps every device child); guard the underflow before the decrement.
  if (m_refCount.load() == 0)
    return 0;
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
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexShader::GetFunction(void *pData, UINT *pSizeOfData) {
  D9DeviceLock lock = m_device->LockDevice();
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
  std::memcpy(pData, m_module->bytecode(), need);
  // Leave *pSizeOfData untouched on success-with-buffer: wined3d
  // (dlls/wined3d/shader.c) and DXVK
  // (src/d3d9/d3d9_shader.h) both omit the write here. Real apps
  // pass the buffer size in and don't expect it to come back changed.
  return D3D_OK;
}

// ---- MTLD3D9PixelShaderModule / MTLD3D9PixelShader: see header for
// shape rationale. The module body is a near-copy of the vertex-shader
// module path; the only differences are the variant key axes and the QI
// vtable IID / COM interface type the wrapper hangs the GetFunction
// contract on.

MTLD3D9PixelShaderModule::MTLD3D9PixelShaderModule(
    MTLD3D9Device *device, const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata
) :
    m_device(device),
    m_metadata(std::move(metadata)) {
  m_bytecode.assign(byte_code, byte_code + dwordCount);
}

void
MTLD3D9PixelShaderModule::incRef() {
  m_refcount.fetch_add(1u, std::memory_order_acquire);
}

void
MTLD3D9PixelShaderModule::decRef() {
  if (m_refcount.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
}

D3D9CompiledFunction *
MTLD3D9PixelShaderModule::getVariantTask(
    uint32_t alpha_test_func, const uint8_t samp_kinds[16], bool point_sprite, int fog_mode, bool fog_coord_w,
    bool dual_source, bool flat_shading, bool emit_sample_mask, uint32_t unorm_snap_mask
) {
  // FNV-1a over the (alpha FUNC, sampler-kinds, point-sprite, fog-mode,
  // dual-source, flat) bounded tuple. The alpha REF and the bump-env
  // matrix / luminance scale + offset ride the shared PS uniform tail and
  // are read at runtime, so distinct refs / bump-env values collapse to one
  // variant per module instead of minting a cold PSO link per value. Fog
  // folds in the mode only (none/vertex/linear/exp/exp2, four real
  // variants): the colour and the table params are runtime constants too.
  // A failure caches as a null function() so a subsequent same-tuple draw
  // short-circuits here; the pipeline builder treats null as a hard mismatch.
  uint64_t key = programmable_ps_variant_key(
      alpha_test_func, samp_kinds, point_sprite, fog_mode, fog_coord_w, dual_source, flat_shading, emit_sample_mask,
      unorm_snap_mask
  );
  if (auto it = m_variantCache.find(key); it != m_variantCache.end()) {
    // Full-key verify (same discipline as the bytecode module cache): confirm
    // every folded axis matches before reuse, so a 64-bit key collision
    // compiles a fresh variant instead of returning the wrong one. samp_kinds
    // is a fixed 16-byte array with no padding, so memcmp is safe.
    const auto *v = std::get_if<D3D9PsVariant>(&static_cast<D3D9FunctionCompileTask *>(it->second.get())->input());
    if (v && v->alpha_test_func == alpha_test_func && std::memcmp(v->samp_kinds, samp_kinds, sizeof(v->samp_kinds)) == 0 &&
        v->point_sprite == point_sprite && v->fog_mode == fog_mode && v->fog_coord_w == fog_coord_w &&
        v->dual_source == dual_source && v->flat_shading == flat_shading &&
        v->emit_sample_mask == emit_sample_mask && v->unorm_snap_mask == unorm_snap_mask)
      return it->second.get();
    // Verified 64-bit collision: fall through to a fresh (uncached) compile.
  }

  // samp_kinds is a fixed 16-byte array copied by value; nothing here points
  // into transient scratch. Submit on create so a PSO task can never park on
  // an unsubmitted dependency. The arg chain is rebuilt on the pool thread in
  // D3D9FunctionCompileTask::compile (its links point at task-thread locals).
  D3D9PsVariant input{};
  input.module = this;
  input.alpha_test_func = alpha_test_func;
  std::memcpy(input.samp_kinds, samp_kinds, sizeof(input.samp_kinds));
  input.point_sprite = point_sprite;
  input.fog_mode = fog_mode;
  input.fog_coord_w = fog_coord_w;
  input.dual_source = dual_source;
  input.flat_shading = flat_shading;
  input.emit_sample_mask = emit_sample_mask;
  input.unorm_snap_mask = unorm_snap_mask;
  auto task = std::make_unique<D3D9FunctionCompileTask>(std::move(input));
  auto *raw = task.get();
  // See the VS module path: pin a collision loser for module lifetime rather
  // than drop the task the PSO cache holds a non-owning pointer to.
  if (!m_variantCache.try_emplace(key, std::move(task)).second)
    m_variantCacheCollisions.push_back(std::move(task));
  m_device->submitAsyncTask(raw);
  return raw;
}

// See MTLD3D9VertexShader's ctor: the module Rc is already retained by
// the device map, the wrapper just shares it; no public AddRef on the
// wrapper here, only the self-pin.
MTLD3D9PixelShader::MTLD3D9PixelShader(MTLD3D9Device *device, Rc<MTLD3D9PixelShaderModule> module) :
    m_device(device),
    m_module(std::move(module)) {
  AddRefPrivate();
}

MTLD3D9PixelShader::~MTLD3D9PixelShader() = default;

ULONG STDMETHODCALLTYPE
MTLD3D9PixelShader::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9PixelShader::Release() {
  // D3D9 Release-at-0 clamp (see MTLD3D9VertexShader::Release).
  if (m_refCount.load() == 0)
    return 0;
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
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9PixelShader::GetFunction(void *pData, UINT *pSizeOfData) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT need = static_cast<UINT>(bytecodeByteLength());
  if (!pData) {
    *pSizeOfData = need;
    return D3D_OK;
  }
  if (*pSizeOfData < need)
    return D3DERR_INVALIDCALL;
  std::memcpy(pData, m_module->bytecode(), need);
  return D3D_OK;
}

namespace {
// True when two bytecode blobs are byte-for-byte identical. The module
// map keys on a 64-bit hash, which (unlike DXVK's Sha1 ShaderKey) is not
// collision-proof, so a hash hit is confirmed with this before reusing a
// module. Cheap: identical shaders match on the length check + a memcmp
// of a few hundred DWORDs; the only cost is on a genuine hit, which is
// exactly the path we want to be correct.
bool
bytecode_equal(const DWORD *a, size_t a_dwords, const DWORD *b, size_t b_dwords) {
  return a_dwords == b_dwords && std::memcmp(a, b, a_dwords * sizeof(DWORD)) == 0;
}
} // namespace

// Device-level vertex-shader module dedup. Mirrors DXVK's
// D3D9ShaderModuleSet::GetShaderModule (src/d3d9/d3d9_shader.cpp): lock,
// probe by key, compile-on-miss outside the lock, then re-lock and insert
// with a double-check that discards the redundant module if another
// thread won the race. The map pins every module for device lifetime (so
// a recreation of the same bytecode reuses the compiled artifact and its
// variant cache), matching getOrCreateDSSO / getOrCreateSampler.
Rc<MTLD3D9VertexShaderModule>
MTLD3D9Device::getOrCreateVertexShaderModule(
    const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata
) {
  uint64_t hash = bytecode_hash(byte_code, dwordCount);
  {
    std::unique_lock<dxmt::mutex> lock(m_shaderModuleMutex);
    if (auto it = m_vsShaderModules.find(hash); it != m_vsShaderModules.end()) {
      // Hash hit: confirm the bytecode actually matches before aliasing
      // (a 64-bit collision would otherwise compile draws against the
      // wrong shader). On a true collision, fall through and build a
      // fresh module; it just won't be deduped against this slot.
      if (bytecode_equal(byte_code, dwordCount, it->second->bytecode(), it->second->bytecodeByteLength() / sizeof(DWORD))) {
        return it->second;
      }
    }
  }

  Rc<MTLD3D9VertexShaderModule> module = new MTLD3D9VertexShaderModule(this, byte_code, dwordCount, std::move(metadata));

  std::unique_lock<dxmt::mutex> lock(m_shaderModuleMutex);
  auto [it, inserted] = m_vsShaderModules.emplace(hash, module);
  // Lost the race (or a benign collision already occupies the slot): keep
  // the existing module only if its bytecode matches, otherwise the
  // caller gets its own fresh module that simply isn't cached.
  if (!inserted &&
      bytecode_equal(byte_code, dwordCount, it->second->bytecode(), it->second->bytecodeByteLength() / sizeof(DWORD)))
    module = it->second;
  return module;
}

// Device-level pixel-shader module dedup. Same shape as the vertex path
// above; see DXVK D3D9ShaderModuleSet::GetShaderModule.
Rc<MTLD3D9PixelShaderModule>
MTLD3D9Device::getOrCreatePixelShaderModule(
    const DWORD *byte_code, size_t dwordCount, DxsoShaderMetadata metadata
) {
  uint64_t hash = bytecode_hash(byte_code, dwordCount);
  {
    std::unique_lock<dxmt::mutex> lock(m_shaderModuleMutex);
    if (auto it = m_psShaderModules.find(hash); it != m_psShaderModules.end()) {
      if (bytecode_equal(byte_code, dwordCount, it->second->bytecode(), it->second->bytecodeByteLength() / sizeof(DWORD))) {
        return it->second;
      }
    }
  }

  Rc<MTLD3D9PixelShaderModule> module = new MTLD3D9PixelShaderModule(this, byte_code, dwordCount, std::move(metadata));

  std::unique_lock<dxmt::mutex> lock(m_shaderModuleMutex);
  auto [it, inserted] = m_psShaderModules.emplace(hash, module);
  if (!inserted &&
      bytecode_equal(byte_code, dwordCount, it->second->bytecode(), it->second->bytecodeByteLength() / sizeof(DWORD)))
    module = it->second;
  return module;
}

} // namespace dxmt
