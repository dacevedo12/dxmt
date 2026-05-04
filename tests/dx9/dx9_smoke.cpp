/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Single-binary harness for the d3d9 smoke suite. Each test_*() lives
 * in its own translation unit under smokes/; we dispatch them by name
 * here. Running them all in one wine process amortises wine boot,
 * Metal init, and dxmt setup across the suite — wined3d's
 * tests/visual.c uses the same shape, and for the same reason.
 */

#include "dx9_smoke.h"

#include "d3d9_diag.hpp"

#include <stdlib.h>

int g_failures = 0;

void
check_hr_(int line, const char *expr, HRESULT hr) {
  if (SUCCEEDED(hr)) {
    printf("ok %d - %s hr=%#lx\n", line, expr, (unsigned long)hr);
    return;
  }
  printf("not ok %d - %s hr=%#lx\n", line, expr, (unsigned long)hr);
  ++g_failures;
}

void
check_hr_eq_(int line, const char *expr, HRESULT hr, HRESULT expected) {
  if (hr == expected) {
    printf("ok %d - %s hr=%#lx\n", line, expr, (unsigned long)hr);
    return;
  }
  printf("not ok %d - %s hr=%#lx expected=%#lx\n", line, expr, (unsigned long)hr, (unsigned long)expected);
  ++g_failures;
}

void
check_true_(int line, const char *expr, bool value) {
  if (value) {
    printf("ok %d - %s\n", line, expr);
    return;
  }
  printf("not ok %d - %s\n", line, expr);
  ++g_failures;
}

void
check_eq_u32_(int line, const char *expr, uint32_t got, uint32_t expected) {
  if (got == expected) {
    printf("ok %d - %s value=%#x\n", line, expr, got);
    return;
  }
  printf("not ok %d - %s value=%#x expected=%#x\n", line, expr, got, expected);
  ++g_failures;
}

void
check_eq_ptr_(int line, const char *expr, const void *got, const void *expected) {
  if (got == expected) {
    printf("ok %d - %s\n", line, expr);
    return;
  }
  printf("not ok %d - %s got=%p expected=%p\n", line, expr, got, expected);
  ++g_failures;
}

void
check_skip_(int line, const char *expr, const char *reason) {
  printf("ok %d - %s # SKIP %s\n", line, expr, reason);
}

void
check_rgba_eq_(int line, const char *expr, uint32_t got, uint32_t expected) {
  if (got == expected) {
    printf("ok %d - %s rgba=%#010x\n", line, expr, got);
    return;
  }
  printf("not ok %d - %s rgba=%#010x expected=%#010x\n", line, expr, got, expected);
  ++g_failures;
}

void
check_rgba_close_(int line, const char *expr, uint32_t got, uint32_t expected, uint32_t eps) {
  // Per-channel absolute difference. ARGB unpacked as four uint8_t so we
  // don't underflow on subtraction.
  uint8_t got_b[4] = {
      static_cast<uint8_t>((got >> 24) & 0xFF), static_cast<uint8_t>((got >> 16) & 0xFF),
      static_cast<uint8_t>((got >> 8) & 0xFF), static_cast<uint8_t>(got & 0xFF)
  };
  uint8_t exp_b[4] = {
      static_cast<uint8_t>((expected >> 24) & 0xFF), static_cast<uint8_t>((expected >> 16) & 0xFF),
      static_cast<uint8_t>((expected >> 8) & 0xFF), static_cast<uint8_t>(expected & 0xFF)
  };
  uint32_t max_delta = 0;
  for (int i = 0; i < 4; ++i) {
    uint32_t d = got_b[i] > exp_b[i] ? got_b[i] - exp_b[i] : exp_b[i] - got_b[i];
    if (d > max_delta)
      max_delta = d;
  }
  if (max_delta <= eps) {
    printf("ok %d - %s rgba=%#010x expected=%#010x maxDelta=%u eps=%u\n", line, expr, got, expected, max_delta, eps);
    return;
  }
  printf("not ok %d - %s rgba=%#010x expected=%#010x maxDelta=%u eps=%u\n", line, expr, got, expected, max_delta, eps);
  ++g_failures;
}

void
check_float_eq_eps_(int line, const char *expr, float got, float expected, float eps) {
  float diff = got - expected;
  if (diff < 0.0f)
    diff = -diff;
  if (diff <= eps) {
    printf("ok %d - %s got=%g expected=%g diff=%g eps=%g\n", line, expr, got, expected, diff, eps);
    return;
  }
  printf("not ok %d - %s got=%g expected=%g diff=%g eps=%g\n", line, expr, got, expected, diff, eps);
  ++g_failures;
}

void
check_render_state_dict_(int line, IDirect3DDevice9 *dev, std::initializer_list<DxRSExpect> expects) {
  if (!dev) {
    printf("not ok %d - check_render_state_dict (dev=null)\n", line);
    ++g_failures;
    return;
  }
  for (const auto &e : expects) {
    DWORD got = 0;
    HRESULT hr = dev->GetRenderState(e.state, &got);
    if (FAILED(hr)) {
      printf("not ok %d - GetRenderState(%u) hr=%#lx\n", line, (unsigned)e.state, (unsigned long)hr);
      ++g_failures;
      continue;
    }
    if (got == e.expected) {
      printf("ok %d - RenderState[%u]=%#lx\n", line, (unsigned)e.state, (unsigned long)got);
    } else {
      printf(
          "not ok %d - RenderState[%u]=%#lx expected=%#lx\n", line, (unsigned)e.state, (unsigned long)got,
          (unsigned long)e.expected
      );
      ++g_failures;
    }
  }
}

void
check_sampler_state_dict_(int line, IDirect3DDevice9 *dev, DWORD stage, std::initializer_list<DxSampExpect> expects) {
  if (!dev) {
    printf("not ok %d - check_sampler_state_dict (dev=null)\n", line);
    ++g_failures;
    return;
  }
  for (const auto &e : expects) {
    DWORD got = 0;
    HRESULT hr = dev->GetSamplerState(stage, e.state, &got);
    if (FAILED(hr)) {
      printf(
          "not ok %d - GetSamplerState(%lu,%u) hr=%#lx\n", line, (unsigned long)stage, (unsigned)e.state,
          (unsigned long)hr
      );
      ++g_failures;
      continue;
    }
    if (got == e.expected) {
      printf("ok %d - SamplerState[%lu][%u]=%#lx\n", line, (unsigned long)stage, (unsigned)e.state, (unsigned long)got);
    } else {
      printf(
          "not ok %d - SamplerState[%lu][%u]=%#lx expected=%#lx\n", line, (unsigned long)stage, (unsigned)e.state,
          (unsigned long)got, (unsigned long)e.expected
      );
      ++g_failures;
    }
  }
}

void
check_texture_stage_state_dict_(
    int line, IDirect3DDevice9 *dev, DWORD stage, std::initializer_list<DxTSSExpect> expects
) {
  if (!dev) {
    printf("not ok %d - check_texture_stage_state_dict (dev=null)\n", line);
    ++g_failures;
    return;
  }
  for (const auto &e : expects) {
    DWORD got = 0;
    HRESULT hr = dev->GetTextureStageState(stage, e.state, &got);
    if (FAILED(hr)) {
      printf(
          "not ok %d - GetTextureStageState(%lu,%u) hr=%#lx\n", line, (unsigned long)stage, (unsigned)e.state,
          (unsigned long)hr
      );
      ++g_failures;
      continue;
    }
    if (got == e.expected) {
      printf(
          "ok %d - TextureStageState[%lu][%u]=%#lx\n", line, (unsigned long)stage, (unsigned)e.state, (unsigned long)got
      );
    } else {
      printf(
          "not ok %d - TextureStageState[%lu][%u]=%#lx expected=%#lx\n", line, (unsigned long)stage, (unsigned)e.state,
          (unsigned long)got, (unsigned long)e.expected
      );
      ++g_failures;
    }
  }
}

void
check_zero_losable_count_(int line, IDirect3DDevice9 *dev) {
  // Audit G1 — QI for the private dxmt diag interface and read the
  // outstanding-DEFAULT-pool-resource counter. The diag pointer is
  // borrowed (see d3d9_diag.hpp) — no Release needed; the caller's
  // IDirect3DDevice9 ref keeps the device alive across the read.
  // SKIP path stays in place so smokes against a non-dxmt or pre-G1
  // d3d9.dll (passthrough A/B builds, older artifact downloads) still
  // pass the call rather than fail-closed.
  if (!dev) {
    printf("ok %d - check_zero_losable_count # SKIP dev=NULL\n", line);
    return;
  }
  dxmt::IDxmtDiag9 *diag = nullptr;
  HRESULT hr = dev->QueryInterface(dxmt::IID_IDxmtDiag9, reinterpret_cast<void **>(&diag));
  if (FAILED(hr) || !diag) {
    printf("ok %d - check_zero_losable_count # SKIP IDxmtDiag9 unavailable\n", line);
    return;
  }
  UINT count = diag->GetLosableResourceCount();
  if (count == 0) {
    printf("ok %d - check_zero_losable_count count=0\n", line);
  } else {
    printf("not ok %d - check_zero_losable_count count=%u (expected 0)\n", line, count);
    ++g_failures;
  }
}

extern void test_begin_end_scene(void);
extern void test_begin_end_scene_gates(void);
extern void test_clear(void);
extern void test_clear_gates(void);
extern void test_clip_plane_gates(void);
extern void test_colorfill(void);
extern void test_colorfill_gates(void);
extern void test_create_buffer(void);
extern void test_create_buffer_gates(void);
extern void test_create_cubetexture(void);
extern void test_create_device(void);
extern void test_create_dxt(void);
extern void test_create_intz(void);
extern void test_create_rendertarget(void);
extern void test_create_texture(void);
extern void test_create_texture_gates(void);
extern void test_depth_test(void);
extern void test_device_probe_gates(void);
extern void test_device_readback_gates(void);
extern void test_draw_gates(void);
extern void test_draw_indexed(void);
extern void test_draw_textured(void);
extern void test_draw_triangle(void);
extern void test_draw_trianglefan(void);
extern void test_drawindexed_up(void);
extern void test_drawprimitive_up(void);
extern void test_dxso_header(void);
extern void test_dynamic_buffer_rename(void);
extern void test_gamma_ramp_gates(void);
extern void test_get_device_caps_gates(void);
extern void test_get_backbuffer(void);
extern void test_getfrontbufferdata(void);
extern void test_getrendertargetdata(void);
extern void test_intz_dsv_srv(void);
extern void test_lockrect(void);
extern void test_lockrect_gates(void);
extern void test_managed_lockrect(void);
extern void test_present(void);
extern void test_query(void);
extern void test_reset_gates(void);
extern void test_scissor_rect_gates(void);
extern void test_set_pixelshader(void);
extern void test_set_pixelshaderconstant(void);
extern void test_set_cursor_properties_gates(void);
extern void test_set_renderstate(void);
extern void test_set_rendertarget(void);
extern void test_set_samplerstate(void);
extern void test_set_shader_gates(void);
extern void test_set_streamsource(void);
extern void test_set_texture(void);
extern void test_set_texturestagestate(void);
extern void test_set_vertexdeclaration(void);
extern void test_set_vertexshader(void);
extern void test_set_vertexshaderconstant(void);
extern void test_softimpl_gates(void);
extern void test_set_viewport(void);
extern void test_setfvf(void);
extern void test_setmaterial_setlight(void);
extern void test_settransform(void);
extern void test_state_block(void);
extern void test_stretchrect(void);
extern void test_stretchrect_gates(void);
extern void test_update_texture_gates(void);
extern void test_updatesurface(void);

struct TestEntry {
  const char *name;
  void (*fn)(void);
};

static const TestEntry kTests[] = {
    {"begin-end-scene", test_begin_end_scene},
    {"begin-end-scene-gates", test_begin_end_scene_gates},
    {"clear", test_clear},
    {"clear-gates", test_clear_gates},
    {"clip-plane-gates", test_clip_plane_gates},
    {"colorfill", test_colorfill},
    {"colorfill-gates", test_colorfill_gates},
    {"create-buffer", test_create_buffer},
    {"create-buffer-gates", test_create_buffer_gates},
    {"create-cubetexture", test_create_cubetexture},
    {"create-device", test_create_device},
    {"create-dxt", test_create_dxt},
    {"create-intz", test_create_intz},
    {"create-rendertarget", test_create_rendertarget},
    {"create-texture", test_create_texture},
    {"create-texture-gates", test_create_texture_gates},
    {"depth-test", test_depth_test},
    {"device-probe-gates", test_device_probe_gates},
    {"device-readback-gates", test_device_readback_gates},
    {"draw-gates", test_draw_gates},
    {"draw-indexed", test_draw_indexed},
    {"draw-textured", test_draw_textured},
    {"draw-triangle", test_draw_triangle},
    {"draw-trianglefan", test_draw_trianglefan},
    {"drawindexed-up", test_drawindexed_up},
    {"drawprimitive-up", test_drawprimitive_up},
    {"dxso-header", test_dxso_header},
    {"dynamic-buffer-rename", test_dynamic_buffer_rename},
    {"gamma-ramp-gates", test_gamma_ramp_gates},
    {"get-device-caps-gates", test_get_device_caps_gates},
    {"get-backbuffer", test_get_backbuffer},
    {"getfrontbufferdata", test_getfrontbufferdata},
    {"getrendertargetdata", test_getrendertargetdata},
    {"intz-dsv-srv", test_intz_dsv_srv},
    {"lockrect", test_lockrect},
    {"lockrect-gates", test_lockrect_gates},
    {"managed-lockrect", test_managed_lockrect},
    {"present", test_present},
    {"query", test_query},
    {"reset-gates", test_reset_gates},
    {"scissor-rect-gates", test_scissor_rect_gates},
    {"set-pixelshader", test_set_pixelshader},
    {"set-pixelshaderconstant", test_set_pixelshaderconstant},
    {"set-cursor-properties-gates", test_set_cursor_properties_gates},
    {"set-renderstate", test_set_renderstate},
    {"set-rendertarget", test_set_rendertarget},
    {"set-samplerstate", test_set_samplerstate},
    {"set-shader-gates", test_set_shader_gates},
    {"set-streamsource", test_set_streamsource},
    {"set-texture", test_set_texture},
    {"set-texturestagestate", test_set_texturestagestate},
    {"set-vertexdeclaration", test_set_vertexdeclaration},
    {"set-vertexshader", test_set_vertexshader},
    {"set-vertexshaderconstant", test_set_vertexshaderconstant},
    {"softimpl-gates", test_softimpl_gates},
    {"set-viewport", test_set_viewport},
    {"setfvf", test_setfvf},
    {"setmaterial-setlight", test_setmaterial_setlight},
    {"settransform", test_settransform},
    {"state-block", test_state_block},
    {"stretchrect", test_stretchrect},
    {"stretchrect-gates", test_stretchrect_gates},
    {"update-texture-gates", test_update_texture_gates},
    {"updatesurface", test_updatesurface},
};

static const size_t kNumTests = sizeof(kTests) / sizeof(kTests[0]);

static LONG WINAPI
dxmt_smoke_unhandled_filter(EXCEPTION_POINTERS *info) {
  // Print exception code, faulting IP, access kind, registers, and
  // a window of stack qwords, then ExitProcess. Runs before wine's
  // AeDebug path so we don't hand the crash off to winedbg --auto,
  // which would hang the headless runner waiting on stdin.
  EXCEPTION_RECORD *rec = info->ExceptionRecord;
  CONTEXT *ctx = info->ContextRecord;
  fflush(stdout);
  fprintf(
      stderr,
      "\ndxmt-smoke FATAL exception code=0x%08lx ip=0x%016llx "
      "flags=0x%lx num_params=%lu\n",
      (unsigned long)rec->ExceptionCode, (unsigned long long)(uintptr_t)rec->ExceptionAddress,
      (unsigned long)rec->ExceptionFlags, (unsigned long)rec->NumberParameters
  );
  if (rec->NumberParameters >= 2 && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    fprintf(
        stderr, "  access=%llu (0=read,1=write,8=exec) fault_addr=0x%016llx\n",
        (unsigned long long)rec->ExceptionInformation[0], (unsigned long long)rec->ExceptionInformation[1]
    );
  }
  fprintf(
      stderr,
      "  RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n"
      "  RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n"
      "  R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n"
      "  R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n"
      "  RIP=%016llx\n",
      (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx, (unsigned long long)ctx->Rcx,
      (unsigned long long)ctx->Rdx, (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
      (unsigned long long)ctx->Rbp, (unsigned long long)ctx->Rsp, (unsigned long long)ctx->R8,
      (unsigned long long)ctx->R9, (unsigned long long)ctx->R10, (unsigned long long)ctx->R11,
      (unsigned long long)ctx->R12, (unsigned long long)ctx->R13, (unsigned long long)ctx->R14,
      (unsigned long long)ctx->R15, (unsigned long long)ctx->Rip
  );
  fprintf(stderr, "  stack (RSP, first 24 qwords):\n");
  unsigned long long *sp = (unsigned long long *)(uintptr_t)ctx->Rsp;
  for (int i = 0; i < 24 && sp; ++i) {
    fprintf(stderr, "    [%02d] %016llx\n", i, sp[i]);
  }
  fflush(stderr);
  ExitProcess(rec->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

int
main(int argc, char **argv) {
  SetUnhandledExceptionFilter(dxmt_smoke_unhandled_filter);
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
  const char *filter = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--list")) {
      for (size_t j = 0; j < kNumTests; ++j)
        puts(kTests[j].name);
      return 0;
    }
    if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
      filter = argv[++i];
    }
  }

  for (size_t j = 0; j < kNumTests; ++j) {
    if (filter && strstr(kTests[j].name, filter) == nullptr)
      continue;
    printf("# %s\n", kTests[j].name);
    fflush(stdout);
    kTests[j].fn();
    fflush(stdout);
  }

  printf("dx9_smoke: %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
  return g_failures ? 1 : 0;
}
