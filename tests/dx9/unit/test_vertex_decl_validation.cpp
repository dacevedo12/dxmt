/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

// Host-native unit test (no wine, no Metal, no device): the
// CreateVertexDeclaration element-array validation contract. Exercises
// dxmt's validate_vertex_elements() directly.
//
// The rejection cases are transcribed verbatim from Wine's d3d9
// conformance suite (dlls/d3d9/tests/device.c, test_unused_declaration_type):
// every array there puts a D3DDECLTYPE_UNUSED element before the
// terminator and expects E_FAIL. The acceptance cases are well-formed
// declarations that must pass (D3D_OK).

#include "d3d9_validation.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

struct Case {
  const char *name;
  D3DVERTEXELEMENT9 elements[4];
  HRESULT expected;
};

const Case g_cases[] = {
    // Wine test_unused_declaration_type: UNUSED as a non-terminator
    // element is rejected regardless of the usage it claims.
    {"UNUSED as COLOR0",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_UNUSED, 0, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()},
     E_FAIL},
    {"UNUSED as TEXCOORD0",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_UNUSED, 0, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()},
     E_FAIL},
    {"UNUSED as TEXCOORD1",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_UNUSED, 0, D3DDECLUSAGE_TEXCOORD, 1},
      D3DDECL_END()},
     E_FAIL},
    {"UNUSED as TEXCOORD12",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_UNUSED, 0, D3DDECLUSAGE_TEXCOORD, 12},
      D3DDECL_END()},
     E_FAIL},
    {"UNUSED as TEXCOORD12 stream1",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {1, 16, D3DDECLTYPE_UNUSED, 0, D3DDECLUSAGE_TEXCOORD, 12},
      D3DDECL_END()},
     E_FAIL},
    {"UNUSED as NORMAL",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_UNUSED, 0, D3DDECLUSAGE_NORMAL, 0},
      D3DDECL_END()},
     E_FAIL},
    {"UNUSED as NORMAL stream1",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {1, 16, D3DDECLTYPE_UNUSED, 0, D3DDECLUSAGE_NORMAL, 0},
      D3DDECL_END()},
     E_FAIL},
    // Well-formed declarations must pass.
    {"position only", {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()}, D3D_OK},
    {"position + normal + texcoord",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL, 0},
      {0, 24, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()},
     D3D_OK},
    // Wine test_vertex_declaration_alignment: an element offset must be
    // 4-byte aligned (every D3DDECLTYPE is at least 4 bytes, so the required
    // alignment is always 4). Offsets 16 and 20 pass; 17/18/19 are rejected.
    {"D3DCOLOR at offset 16",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()},
     D3D_OK},
    {"D3DCOLOR at offset 17",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 17, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()},
     E_FAIL},
    {"D3DCOLOR at offset 18",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 18, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()},
     E_FAIL},
    {"D3DCOLOR at offset 19",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 19, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()},
     E_FAIL},
    {"D3DCOLOR at offset 20",
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 20, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()},
     D3D_OK},
};

} // namespace

int
main() {
  const size_t n = sizeof(g_cases) / sizeof(g_cases[0]);
  for (size_t i = 0; i < n; ++i) {
    const Case &c = g_cases[i];
    HRESULT got = dxmt::validate_vertex_elements(c.elements);
    if (got == c.expected)
      continue;
    ++g_failures;
    printf("not ok - %-30s expected %#010lx, got %#010lx\n", c.name, (unsigned long)c.expected, (unsigned long)got);
  }

  printf("%zu case(s), %d failure(s)\n", n, g_failures);
  return g_failures ? 1 : 0;
}
