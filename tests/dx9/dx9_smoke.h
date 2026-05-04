/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int g_failures;

#define check_hr(expr) check_hr_(__LINE__, #expr, (expr))
#define check_hr_eq(expr, expected) check_hr_eq_(__LINE__, #expr, (expr), (expected))
#define check_true(expr) check_true_(__LINE__, #expr, !!(expr))
#define check_eq_u32(expr, expected) check_eq_u32_(__LINE__, #expr, (uint32_t)(expr), (uint32_t)(expected))
#define check_eq_ptr(expr, expected) check_eq_ptr_(__LINE__, #expr, (const void *)(expr), (const void *)(expected))
#define check_skip(expr, reason) check_skip_(__LINE__, #expr, (reason))

// Pixel value checks. ARGB layout (D3DFMT_A8R8G8B8 / X8R8G8B8) is the only
// shape used in the smoke suite today; if a non-ARGB format ever needs a
// pixel check, add a sibling macro rather than overloading these.
#define check_rgba_eq(expr, expected) check_rgba_eq_(__LINE__, #expr, (uint32_t)(expr), (uint32_t)(expected))
#define check_rgba_close(expr, expected, eps)                                                                          \
  check_rgba_close_(__LINE__, #expr, (uint32_t)(expr), (uint32_t)(expected), (eps))

// Float compare with an epsilon. Used for FVF/transform/viewport floats
// where Metal-side rounding can produce a different bit pattern from
// what the source value started as.
#define check_float_eq_eps(expr, expected, eps) check_float_eq_eps_(__LINE__, #expr, (expr), (expected), (eps))

// State read-back compares. Each macro bulk-issues the matching Get* across
// the supplied table and reports per-entry pass/fail. Separate macros per
// Get* signature (GetRenderState takes 1 key, GetSamplerState / GetTextureStageState
// take stage+key) rather than one polymorphic helper.
struct DxRSExpect {
  D3DRENDERSTATETYPE state;
  DWORD expected;
};
struct DxSampExpect {
  D3DSAMPLERSTATETYPE state;
  DWORD expected;
};
struct DxTSSExpect {
  D3DTEXTURESTAGESTATETYPE state;
  DWORD expected;
};

#define check_render_state_dict(dev, ...) check_render_state_dict_(__LINE__, (dev), {__VA_ARGS__})
#define check_sampler_state_dict(dev, stage, ...) check_sampler_state_dict_(__LINE__, (dev), (stage), {__VA_ARGS__})
#define check_texture_stage_state_dict(dev, stage, ...)                                                                \
  check_texture_stage_state_dict_(__LINE__, (dev), (stage), {__VA_ARGS__})

// Teardown probe: asserts the device's losable D3DPOOL_DEFAULT resource
// count is zero, i.e. the smoke released everything it created. Becomes
// real once audit G1 (m_losableResourceCount counter on MTLD3D9Device) is
// committed and a diag export is added to d3d9.def; until then this is a
// SKIP no-op so smokes can already adopt the call site.
#define check_zero_losable_count(dev) check_zero_losable_count_(__LINE__, (dev))

void check_hr_(int line, const char *expr, HRESULT hr);
void check_hr_eq_(int line, const char *expr, HRESULT hr, HRESULT expected);
void check_true_(int line, const char *expr, bool value);
void check_eq_u32_(int line, const char *expr, uint32_t got, uint32_t expected);
void check_eq_ptr_(int line, const char *expr, const void *got, const void *expected);
void check_skip_(int line, const char *expr, const char *reason);
void check_rgba_eq_(int line, const char *expr, uint32_t got, uint32_t expected);
void check_rgba_close_(int line, const char *expr, uint32_t got, uint32_t expected, uint32_t eps);
void check_float_eq_eps_(int line, const char *expr, float got, float expected, float eps);
void check_render_state_dict_(int line, IDirect3DDevice9 *dev, std::initializer_list<DxRSExpect> expects);
void
check_sampler_state_dict_(int line, IDirect3DDevice9 *dev, DWORD stage, std::initializer_list<DxSampExpect> expects);
void check_texture_stage_state_dict_(
    int line, IDirect3DDevice9 *dev, DWORD stage, std::initializer_list<DxTSSExpect> expects
);
void check_zero_losable_count_(int line, IDirect3DDevice9 *dev);

template <typename T>
static inline void
release_object(T **object) {
  if (*object) {
    (*object)->Release();
    *object = nullptr;
  }
}

// Most smokes need a default-shaped Direct3D9 + Device9 pair. Centralising
// the boilerplate keeps the per-test files focused on the API surface they
// exercise.
struct Dx9Fixture {
  IDirect3D9 *d3d = nullptr;
  IDirect3DDevice9 *dev = nullptr;

  ~Dx9Fixture() {
    release_object(&dev);
    release_object(&d3d);
  }

  bool
  create(UINT w = 320, UINT h = 240, D3DFORMAT fmt = D3DFMT_X8R8G8B8) {
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    check_true(d3d != nullptr);
    if (!d3d)
      return false;

    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferFormat = fmt;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    check_hr(hr);
    return SUCCEEDED(hr) && dev != nullptr;
  }
};
