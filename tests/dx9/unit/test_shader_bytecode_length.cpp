/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

// Unit test: shader_bytecode_dword_count (no device/GPU). The length is
// the version token through the End token inclusive. The regression of
// record: an operand or immediate DWORD whose low 16 bits alias the
// Comment opcode (0xFFFE) or the End token (0xFFFF) must not be read as a
// token, because the instruction-length walk skips it as part of an
// instruction body. The earlier every-DWORD scan misread such operands
// and either skipped past End (crash) or stopped early (wrong length).

#include "d3d9_shader_scan.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

// Version token: bits 31..16 = 0xFFFE (vertex) / 0xFFFF (pixel),
// 15..8 = major, 7..0 = minor.
constexpr DWORD VS_2_0 = 0xFFFE0200u;
constexpr DWORD VS_1_1 = 0xFFFE0101u;
constexpr DWORD PS_3_0 = 0xFFFF0300u;
constexpr DWORD PS_1_4 = 0xFFFF0104u;
constexpr DWORD END = 0x0000FFFFu;
// SM2.0+ opcode token: bits 24..27 = body length, low 16 = opcode.
// mov = 1, so a mov with two body DWORDs is 0x02000001.
constexpr DWORD SM2_MOV2 = 0x02000001u;
// SM1.x mov opcode token (length comes from the default table = 2).
constexpr DWORD SM1_MOV = 0x00000001u;
// SM1.x tex opcode token (opcode 66; default length 1, +1 under SM1.4).
constexpr DWORD SM1_TEX = 0x00000042u;
// Comment token with a two-DWORD body: bits 16..30 = length, low 16 = 0xFFFE.
constexpr DWORD COMMENT2 = 0x0002FFFEu;

struct Case {
  const char *name;
  DWORD code[8];
  size_t expected;
};

const Case g_cases[] = {
    // Minimal shader: version + End.
    {"empty", {VS_2_0, END}, 2},
    // SM2 mov with ordinary operands.
    {"sm2_mov", {VS_2_0, SM2_MOV2, 0x80000000u, 0x90000000u, END}, 5},
    // Operand whose low 16 bits alias Comment AND carry a large body
    // field (0x7FFF): the every-DWORD scan would skip ~32K DWORDs past
    // End. The length walk skips it as part of the mov body.
    {"sm2_operand_aliases_comment", {VS_2_0, SM2_MOV2, 0x7FFFFFFEu, 0x00000000u, END}, 5},
    // Operand that aliases the End token: the every-DWORD scan would stop
    // here and report a short length.
    {"sm2_operand_aliases_end", {VS_2_0, SM2_MOV2, 0x0000FFFFu, 0x11111111u, END}, 5},
    // SM1.x: body length comes from the per-opcode default table, not the
    // token. mov = 2 body DWORDs.
    {"sm1_mov", {VS_1_1, SM1_MOV, 0x00000000u, 0x00000000u, END}, 5},
    // A real comment block (e.g. CTAB) is skipped by its length field.
    {"sm3_real_comment", {PS_3_0, COMMENT2, 0xDEADBEEFu, 0xCAFEBABEu, END}, 5},
    // SM1.4 packs the texcoord source into the stream, so tex carries one
    // body DWORD beyond the default-table length.
    {"sm1_4_tex", {PS_1_4, SM1_TEX, 0x00000000u, 0x00000000u, END}, 5},
    // Invalid version token (major 0): rejected before any walk.
    {"bad_version", {0x00000000u, END}, 0},
};

void
run(const Case &c) {
  size_t got = dxmt::shader_bytecode_dword_count(c.code);
  if (got == c.expected) {
    printf("ok - %-30s -> %zu\n", c.name, got);
    return;
  }
  ++g_failures;
  printf("not ok - %-26s expected %zu, got %zu\n", c.name, c.expected, got);
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
