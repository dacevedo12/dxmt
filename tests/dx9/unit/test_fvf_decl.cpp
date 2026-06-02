/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

// Unit test: D3DFVF -> D3DVERTEXELEMENT9 lowering (no device/GPU).
// Vectors from Wine's d3d9 conformance suite (reference for D3D9).

#include "d3d9_fvf.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

struct FvfCase {
  const char *name;
  DWORD fvf;
  // Expected declaration INCLUDING the D3DDECL_END terminator, exactly
  // as Wine spells it.
  D3DVERTEXELEMENT9 expected[8];
};

// Verbatim from Wine dlls/d3d9/tests/device.c fvf_to_decl_tests[].
// Element field order: { Stream, Offset, Type, Method(=0 DEFAULT), Usage,
// UsageIndex }.
const FvfCase g_cases[] = {
    {"XYZ", D3DFVF_XYZ, {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()}},
    {"XYZW", D3DFVF_XYZW, {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()}},
    {"XYZRHW", D3DFVF_XYZRHW, {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITIONT, 0}, D3DDECL_END()}},
    {"XYZB5",
     D3DFVF_XYZB5,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 28, D3DDECLTYPE_FLOAT1, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB5|LASTBETA_UBYTE4",
     D3DFVF_XYZB5 | D3DFVF_LASTBETA_UBYTE4,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 28, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB5|LASTBETA_D3DCOLOR",
     D3DFVF_XYZB5 | D3DFVF_LASTBETA_D3DCOLOR,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 28, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB1",
     D3DFVF_XYZB1,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT1, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      D3DDECL_END()}},
    {"XYZB1|LASTBETA_UBYTE4",
     D3DFVF_XYZB1 | D3DFVF_LASTBETA_UBYTE4,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB1|LASTBETA_D3DCOLOR",
     D3DFVF_XYZB1 | D3DFVF_LASTBETA_D3DCOLOR,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB2",
     D3DFVF_XYZB2,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      D3DDECL_END()}},
    {"XYZB2|LASTBETA_UBYTE4",
     D3DFVF_XYZB2 | D3DFVF_LASTBETA_UBYTE4,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT1, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 16, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB2|LASTBETA_D3DCOLOR",
     D3DFVF_XYZB2 | D3DFVF_LASTBETA_D3DCOLOR,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 16, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB3",
     D3DFVF_XYZB3,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      D3DDECL_END()}},
    {"XYZB3|LASTBETA_UBYTE4",
     D3DFVF_XYZB3 | D3DFVF_LASTBETA_UBYTE4,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 20, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB3|LASTBETA_D3DCOLOR",
     D3DFVF_XYZB3 | D3DFVF_LASTBETA_D3DCOLOR,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 20, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB4",
     D3DFVF_XYZB4,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      D3DDECL_END()}},
    {"XYZB4|LASTBETA_UBYTE4",
     D3DFVF_XYZB4 | D3DFVF_LASTBETA_UBYTE4,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 24, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"XYZB4|LASTBETA_D3DCOLOR",
     D3DFVF_XYZB4 | D3DFVF_LASTBETA_D3DCOLOR,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 24, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BLENDINDICES, 0},
      D3DDECL_END()}},
    {"NORMAL", D3DFVF_NORMAL, {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL, 0}, D3DDECL_END()}},
    {"PSIZE", D3DFVF_PSIZE, {{0, 0, D3DDECLTYPE_FLOAT1, 0, D3DDECLUSAGE_PSIZE, 0}, D3DDECL_END()}},
    {"DIFFUSE", D3DFVF_DIFFUSE, {{0, 0, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()}},
    {"SPECULAR", D3DFVF_SPECULAR, {{0, 0, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 1}, D3DDECL_END()}},
    // Texture coordinate sizes 1..4 -> FLOAT1..FLOAT4.
    {"TEX1.size1",
     D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEX1,
     {{0, 0, D3DDECLTYPE_FLOAT1, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()}},
    {"TEX1.size2",
     D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEX1,
     {{0, 0, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()}},
    {"TEX1.size3",
     D3DFVF_TEXCOORDSIZE3(0) | D3DFVF_TEX1,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()}},
    {"TEX1.size4",
     D3DFVF_TEXCOORDSIZE4(0) | D3DFVF_TEX1,
     {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()}},
    // Several textures, mixed sizes: exercises TEXCOORD index ordering
    // and packed offsets (1,3,2,4 floats -> offsets 0,4,16,24).
    {"TEX4.mixed",
     D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEXCOORDSIZE3(1) | D3DFVF_TEXCOORDSIZE2(2) | D3DFVF_TEXCOORDSIZE4(3) |
         D3DFVF_TEX4,
     {{0, 0, D3DDECLTYPE_FLOAT1, 0, D3DDECLUSAGE_TEXCOORD, 0},
      {0, 4, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TEXCOORD, 1},
      {0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 2},
      {0, 24, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 3},
      D3DDECL_END()}},
    // Full combination: the single most exercising vector.
    {"XYZB4|DIFFUSE|SPECULAR|TEX2",
     D3DFVF_XYZB4 | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE3(1) | D3DFVF_TEX2,
     {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
      {0, 28, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
      {0, 32, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 1},
      {0, 36, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
      {0, 44, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TEXCOORD, 1},
      D3DDECL_END()}},
};

size_t
expected_count_with_terminator(const D3DVERTEXELEMENT9 *elements) {
  size_t n = 0;
  // The terminator is the first element with Stream == 0xFF; count
  // includes it, matching GetDeclaration's contract.
  while (elements[n].Stream != 0xFF)
    ++n;
  return n + 1;
}

void
dump(const char *tag, const D3DVERTEXELEMENT9 &e) {
  printf(
      "    %s stream %u, offset %u, type %#x, method %#x, usage %#x, usage_index %u\n", tag, e.Stream, e.Offset, e.Type,
      e.Method, e.Usage, e.UsageIndex
  );
}

void
run(const FvfCase &c) {
  std::vector<D3DVERTEXELEMENT9> got;
  dxmt::build_fvf_decl_elements(c.fvf, got);
  // build_fvf_decl_elements emits the body without the terminator;
  // append it so the comparison matches Wine's full expected array.
  const D3DVERTEXELEMENT9 terminator = D3DDECL_END();
  got.push_back(terminator);

  const size_t want = expected_count_with_terminator(c.expected);

  bool equal = got.size() == want;
  for (size_t i = 0; equal && i < want; ++i)
    equal = std::memcmp(&got[i], &c.expected[i], sizeof(D3DVERTEXELEMENT9)) == 0;

  if (equal) {
    printf("ok - fvf %-26s (%#010x)\n", c.name, (unsigned)c.fvf);
    return;
  }

  ++g_failures;
  printf("not ok - fvf %-22s (%#010x): declaration mismatch\n", c.name, (unsigned)c.fvf);
  printf("  expected %zu element(s):\n", want);
  for (size_t i = 0; i < want; ++i)
    dump("expected", c.expected[i]);
  printf("  got %zu element(s):\n", got.size());
  for (size_t i = 0; i < got.size(); ++i)
    dump("got     ", got[i]);
}

} // namespace

int
main() {
  const size_t n = sizeof(g_cases) / sizeof(g_cases[0]);
  for (size_t i = 0; i < n; ++i)
    run(g_cases[i]);

  printf("\n%zu case(s), %d failure(s)\n", n, g_failures);
  return g_failures ? 1 : 0;
}
