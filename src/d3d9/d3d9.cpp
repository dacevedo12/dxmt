#include "d3d9.h"
#include "d3d9_interface.hpp"
#include "log/log.hpp"

#include <atomic>

namespace dxmt {
Logger Logger::s_instance("d3d9.log");
}

#ifdef _WIN32
extern "C" BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(instance);
  return TRUE;
}
#endif

extern "C" IDirect3D9 *WINAPI
Direct3DCreate9(UINT SDKVersion) {
  auto *iface = new dxmt::MTLD3D9Interface(SDKVersion, /*isEx=*/false);
  iface->AddRef();
  return static_cast<IDirect3D9 *>(iface);
}

extern "C" HRESULT WINAPI
Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D) {
  if (!ppD3D)
    return D3DERR_INVALIDCALL;
  auto *iface = new dxmt::MTLD3D9Interface(SDKVersion, /*isEx=*/true);
  iface->AddRef();
  *ppD3D = static_cast<IDirect3D9Ex *>(iface);
  return D3D_OK;
}

// PIX event nesting counter. Mirrors wined3d d3d9_main.c.
// BeginEvent returns the depth the new event sits at (the prior nesting
// level, returned before incrementing); EndEvent returns the depth left
// after popping (the level after decrementing). Apps inspect the level to validate
// nesting; returning 0 unconditionally would silently break that
// contract. Atomic because PIX events are documented as callable
// from any thread holding the device.
static std::atomic<int> s_d3dperfEventLevel{0};

extern "C" int WINAPI
D3DPERF_BeginEvent(D3DCOLOR, const WCHAR *) {
  return s_d3dperfEventLevel.fetch_add(1, std::memory_order_relaxed);
}
extern "C" int WINAPI
D3DPERF_EndEvent(void) {
  return s_d3dperfEventLevel.fetch_sub(1, std::memory_order_relaxed) - 1;
}
extern "C" DWORD WINAPI
D3DPERF_GetStatus(void) {
  return 0;
}
extern "C" BOOL WINAPI
D3DPERF_QueryRepeatFrame(void) {
  return FALSE;
}
extern "C" void WINAPI
D3DPERF_SetMarker(D3DCOLOR, const WCHAR *) {}
extern "C" void WINAPI
D3DPERF_SetOptions(DWORD) {}
extern "C" void WINAPI
D3DPERF_SetRegion(D3DCOLOR, const WCHAR *) {}

extern "C" void WINAPI
DebugSetLevel(DWORD) {}
extern "C" BOOL WINAPI
DebugSetMute(void) {
  return TRUE;
}

// Direct3DShaderValidatorCreate9: undocumented MS export (fxc.exe drives it to
// validate a shader token stream out of process). It is a small COM-style state
// machine: Begin, then one Instruction per token group (version token, then the
// body, then the 0x0000ffff end token), then End. A malformed stream fires the
// caller's message callback. The exact contract (message ids, the two messages a
// bad version token reports, the register errors that fire a message yet return
// S_OK and only make End fail, and the stuck Error state) is pinned against
// native by the wine d3d9 test. DXVK's d3d9_shader_validator is a partial
// reference: it stops after the first header error where native reports both.
namespace {

typedef HRESULT(WINAPI *shader_validator_cb)(
    const char *file, int line, DWORD_PTR arg3, DWORD_PTR message_id, const char *message, void *context
);

enum shader_validator_state {
  SV_BEGIN = 0,   // awaiting Begin
  SV_HEADER,      // Begin done, awaiting the version token
  SV_BODY,        // version accepted, validating instructions
  SV_END_TOKEN,   // end token seen, awaiting End
  SV_ERROR,       // a fatal error latched; only a fresh validator recovers
};

// Message ids match the values the wine d3d9 test pins against native for the
// cases it exercises (0xeb, 0xef, 0xf0, 0x12c, 0x167); the remaining ids follow
// the same numbering DXVK uses and only surface on malformed API call order fxc
// never produces, so they are consistent but not independently native-verified.
enum shader_validator_message {
  SVM_BEGIN_OUT_OF_ORDER = 0xeb,
  SVM_INSTRUCTION_OUT_OF_ORDER = 0xec,
  SVM_INSTRUCTION_AFTER_END = 0xed,
  SVM_INSTRUCTION_NULL_ARGS = 0xee,
  SVM_BAD_VERSION_LENGTH = 0xef,
  SVM_BAD_VERSION_TYPE = 0xf0,
  SVM_BAD_END_TOKEN = 0xf1,
  SVM_END_OUT_OF_ORDER = 0xf2,
  SVM_MISSING_END_TOKEN = 0xf3,
  SVM_BAD_INPUT_REGISTER_DECL = 0x12c,
  SVM_BAD_INPUT_REGISTER = 0x167,
};

struct IDirect3DShaderValidator9;

struct IDirect3DShaderValidator9Vtbl {
  HRESULT(WINAPI *QueryInterface)(IDirect3DShaderValidator9 *iface, REFIID iid, void **out);
  ULONG(WINAPI *AddRef)(IDirect3DShaderValidator9 *iface);
  ULONG(WINAPI *Release)(IDirect3DShaderValidator9 *iface);
  HRESULT(WINAPI *Begin)(IDirect3DShaderValidator9 *iface, shader_validator_cb callback, void *context, DWORD_PTR arg3);
  HRESULT(WINAPI *Instruction)(
      IDirect3DShaderValidator9 *iface, const char *file, int line, const DWORD *tokens, unsigned int token_count
  );
  HRESULT(WINAPI *End)(IDirect3DShaderValidator9 *iface);
};

struct IDirect3DShaderValidator9 {
  const struct IDirect3DShaderValidator9Vtbl *vtbl;
  std::atomic<ULONG> refcount;
  int state;
  shader_validator_cb callback;
  void *context;
  bool is_pixel_shader;
  bool error_seen;
};

void
shader_validator_emit(
    IDirect3DShaderValidator9 *v, const char *file, int line, DWORD_PTR message_id, const char *message
) {
  v->error_seen = true;
  if (v->callback)
    v->callback(file, line, 0, message_id, message, v->context);
}

HRESULT WINAPI
shader_validator_QueryInterface(IDirect3DShaderValidator9 *, REFIID, void **out) {
  if (out)
    *out = nullptr;
  return E_NOINTERFACE;
}
ULONG WINAPI
shader_validator_AddRef(IDirect3DShaderValidator9 *v) {
  return v->refcount.fetch_add(1, std::memory_order_relaxed) + 1;
}
ULONG WINAPI
shader_validator_Release(IDirect3DShaderValidator9 *v) {
  ULONG rc = v->refcount.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (rc == 0)
    delete v;
  return rc;
}
HRESULT WINAPI
shader_validator_Begin(IDirect3DShaderValidator9 *v, shader_validator_cb callback, void *context, DWORD_PTR) {
  v->callback = callback;
  v->context = context;
  // Begin is legal only on a fresh validator or once ::End has reset the state
  // to SV_BEGIN. A second Begin mid-stream, or one after a latched error, is out
  // of order: report it and latch, so only ::End or a fresh validator recovers.
  if (v->state != SV_BEGIN) {
    shader_validator_emit(v, nullptr, 0, SVM_BEGIN_OUT_OF_ORDER,
                          "IDirect3DShaderValidator9::Begin called out of order. ::End must be called first.");
    v->state = SV_ERROR;
    return E_FAIL;
  }
  v->state = SV_HEADER;
  v->error_seen = false;
  return S_OK;
}
HRESULT WINAPI
shader_validator_Instruction(
    IDirect3DShaderValidator9 *v, const char *file, int line, const DWORD *tokens, unsigned int token_count
) {
  if (!tokens || !token_count) {
    shader_validator_emit(v, file, line, SVM_INSTRUCTION_NULL_ARGS,
                          "IDirect3DShaderValidator9::Instruction called with NULL tokens or a zero token count.");
    return E_FAIL;
  }
  if (v->state == SV_ERROR)
    return E_FAIL;
  if (v->state == SV_BEGIN) {
    shader_validator_emit(v, file, line, SVM_INSTRUCTION_OUT_OF_ORDER,
                          "IDirect3DShaderValidator9::Instruction called out of order. ::Begin must be called first.");
    return E_FAIL;
  }
  if (v->state == SV_END_TOKEN) {
    shader_validator_emit(v, file, line, SVM_INSTRUCTION_AFTER_END,
                          "IDirect3DShaderValidator9::Instruction called after the end token. Call ::End next.");
    return E_FAIL;
  }
  if (v->state == SV_HEADER) {
    // The version token is a single DWORD whose high word selects VS (0xfffe) or
    // PS (0xffff). A malformed one reports up to two messages, both with a null
    // file: the length arm carries line -1, the type arm line 0.
    bool bad = false;
    if (token_count != 1) {
      shader_validator_emit(v, nullptr, -1, SVM_BAD_VERSION_LENGTH,
                            "IDirect3DShaderValidator9::Instruction: bad version token, expected a single DWORD.");
      bad = true;
    }
    const DWORD kind = tokens[0] & 0xffff0000u;
    if (kind != 0xffff0000u && kind != 0xfffe0000u) {
      shader_validator_emit(v, nullptr, 0, SVM_BAD_VERSION_TYPE,
                            "IDirect3DShaderValidator9::Instruction: bad version token, neither a pixel nor a vertex shader.");
      bad = true;
    }
    if (bad) {
      v->state = SV_ERROR;
      return E_FAIL;
    }
    v->is_pixel_shader = kind == 0xffff0000u;
    v->state = SV_BODY;
    return S_OK;
  }
  // SV_BODY. The end token closes the stream.
  if (tokens[0] == 0x0000ffffu) {
    if (token_count != 1) {
      shader_validator_emit(v, file, line, SVM_BAD_END_TOKEN,
                            "IDirect3DShaderValidator9::Instruction: bad end token, expected a single DWORD.");
      v->state = SV_ERROR;
      return E_FAIL;
    }
    v->state = SV_END_TOKEN;
    return S_OK;
  }
  // PS 3.0 exposes v0..v9 only; a source or dcl referencing an input register
  // >= 10 is reported (dcl gets its own message id) but is not fatal: the call
  // still succeeds and only ::End then fails. Register tokens have bit 31 set;
  // def / defb / defi / comment carry immediate data, not registers, so skip them.
  const DWORD opcode = tokens[0] & 0x0000ffffu;
  if (v->is_pixel_shader && opcode != 0xfffeu /* comment */ && opcode != 0x51u /* def */ &&
      opcode != 0x52u /* defb */ && opcode != 0x53u /* defi */) {
    for (unsigned int i = 1; i < token_count && (tokens[i] >> 31); ++i) {
      const DWORD reg_type = ((tokens[i] & 0x70000000u) >> 28) | ((tokens[i] & 0x00001800u) >> 8);
      const DWORD reg_index = tokens[i] & 0x000007ffu;
      if (reg_type == 1u /* D3DSPR_INPUT */ && reg_index >= 10u) {
        shader_validator_emit(v, file, line,
                              opcode == 0x1fu /* dcl */ ? SVM_BAD_INPUT_REGISTER_DECL : SVM_BAD_INPUT_REGISTER,
                              "IDirect3DShaderValidator9::Instruction: pixel shader input register index out of range.");
        break;
      }
    }
  }
  return S_OK;
}
HRESULT WINAPI
shader_validator_End(IDirect3DShaderValidator9 *v) {
  if (v->state == SV_ERROR)
    return E_FAIL;
  if (v->state == SV_BEGIN) {
    shader_validator_emit(v, nullptr, 0, SVM_END_OUT_OF_ORDER,
                          "IDirect3DShaderValidator9::End called out of order. ::Begin must be called first.");
    return E_FAIL;
  }
  if (v->state != SV_END_TOKEN) {
    shader_validator_emit(v, nullptr, 0, SVM_MISSING_END_TOKEN,
                          "IDirect3DShaderValidator9::End: the shader is missing its end token.");
    v->state = SV_BEGIN;
    return E_FAIL;
  }
  const bool had_error = v->error_seen;
  v->state = SV_BEGIN;
  v->error_seen = false;
  return had_error ? E_FAIL : S_OK;
}

const IDirect3DShaderValidator9Vtbl shader_validator_vtbl = {
    shader_validator_QueryInterface, shader_validator_AddRef,      shader_validator_Release,
    shader_validator_Begin,          shader_validator_Instruction, shader_validator_End,
};

} // namespace

extern "C" IDirect3DShaderValidator9 *WINAPI
Direct3DShaderValidatorCreate9(void) {
  auto *v = new IDirect3DShaderValidator9{};
  v->vtbl = &shader_validator_vtbl;
  v->refcount.store(1, std::memory_order_relaxed);
  v->state = SV_BEGIN;
  v->callback = nullptr;
  v->context = nullptr;
  v->is_pixel_shader = false;
  v->error_seen = false;
  return v;
}
