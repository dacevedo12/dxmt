#!/usr/bin/env python3
#
# Copyright (c) 2026 GameSir Labs and contributors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# T2 shader-translation goldens. Drives the NATIVE airconv CLI (built
# for the build machine, no wine, no Metal device) over a corpus of
# known D3D9 shader bytecode and asserts that DXSO -> AIR translation
# produces:
#   1. valid AIR LLVM IR  (-S): the air64 target triple, a `shader_main`
#      entry point, and the air.{vertex,fragment} stage metadata, and
#   2. a loadable metallib (-A): the `MTLB` container magic.
#
# This is the wine-free, deterministic counterpart to the smoke draws:
# it exercises the airconv translator (where the hardest D3D9 bring-up
# bugs lived) without standing up a device. The bytecode below is the
# same documented blobs the draw smokes submit, embedded as DWORD token
# streams (not opaque binaries) so a reviewer can read the shaders.

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# argv[1] is the native airconv binary (meson passes the built target).
AIRCONV = sys.argv[1]

# Each entry: a D3D9 token stream + the AIR stage it must lower to.
SHADERS = [
    {
        "name": "vs_2_0 passthrough (mov oPos, v0)",
        "stage": "vertex",
        "tokens": [
            0xFFFE0200,                          # vs_2_0
            0x0200001F, 0x80000000, 0x900F0000,  # dcl_position v0
            0x02000001, 0xC00F0000, 0x90E40000,  # mov oPos, v0
            0x0000FFFF,                          # end
        ],
    },
    {
        "name": "vs_2_0 position + passthrough texcoord",
        "stage": "vertex",
        "tokens": [
            0xFFFE0200,                          # vs_2_0
            0x0200001F, 0x80000000, 0x900F0000,  # dcl_position v0
            0x0200001F, 0x80000005, 0x900F0001,  # dcl_texcoord0 v1
            0x02000001, 0xC00F0000, 0x90E40000,  # mov oPos, v0
            0x02000001, 0xE00F0000, 0x90E40001,  # mov oT0, v1
            0x0000FFFF,                          # end
        ],
    },
    {
        "name": "ps_2_0 solid color (mov oC0, c0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0200,                          # ps_2_0
            0x02000001, 0x800F0800, 0xA0E40000,  # mov oC0, c0
            0x0000FFFF,                          # end
        ],
    },
    {
        "name": "ps_2_0 textured (texld r0, t0, s0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0200,                                      # ps_2_0
            0x0200001F, 0x90000000, 0xA00F0800,              # dcl_2d s0
            0x0200001F, 0x80000005, 0xB00F0000,              # dcl_texcoord0 t0
            0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,  # texld r0, t0, s0
            0x02000001, 0x800F0800, 0x80E40000,              # mov oC0, r0
            0x0000FFFF,                                      # end
        ],
    },
    # vkd3d-tests d3dbc corpus (tests/d3dbc/*.shader_test, the hand-
    # assembled d3dbc-hex blocks that need no HLSL compiler), transliterated
    # DWORD-for-DWORD. Widens coverage to dp2add, the mNxN matrix family,
    # nrm, rep flow control, and the SM1.x texreg2* family, with vkd3d's
    # per-instruction comments preserved.
    {
        "name": "vkd3d d3dbc dp2add #0 (ps_2_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0200,                                               # ps_2_0
            0x05000051, 0xA00F0000, 0x3F000000, 0x3F000000, 0x3F800000, 0x00000000,# def c0, 0.5, 0.5, 1.0, 0.0
            0x02000001, 0x800F0000, 0xA0E40000,                       # mov r0, c0
            0x0400005A, 0x80070000, 0x80000000, 0x80000000, 0x80FF0000,# dp2add r0.xyz, r0, r0, r0.w
            0x02000001, 0x80080000, 0xA0AA0000,                       # mov r0.w, c0.z
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc dp2add #1 (ps_2_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0200,                                               # ps_2_0
            0x05000051, 0xA00F0000, 0xBF000000, 0xBF000000, 0x3F800000, 0x40000000,# def c0, -0.5, -0.5, 1.0, 2.0
            0x02000001, 0x800F0000, 0xA0E40000,                       # mov r0, c0
            0x0400005A, 0x80170000, 0x80000000, 0x80000000, 0x80FF0000,# dp2add_sat r0.xyz, r0, r0, r0.w
            0x03000002, 0x80070000, 0x80E40000, 0xA0000000,           # add r0.xyz, r0, c0.x
            0x02000001, 0x80080000, 0xA0AA0000,                       # mov r0.w, c0.z
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc mnxn #0 (vs_3_0)",
        "stage": "vertex",
        "tokens": [
            0xFFFE0300,                                               # vs_3_0
            0x0200001F, 0x80000000, 0x900F0000,                       # dcl_position v0
            0x0200001F, 0x80000000, 0xE00F0000,                       # dcl_position o0
            0x0200001F, 0x80000005, 0xE00F0001,                       # dcl_texcoord o1
            0x02000001, 0x800F0001, 0xA0E40004,                       # mov r1, c4
            0x03000014, 0x800F0000, 0x80E40001, 0xA0E40000,           # m4x4 r0, r1, c0
            0x02000001, 0xE00F0001, 0x80E40000,                       # mov o1, r0
            0x02000001, 0xE00F0000, 0x90E40000,                       # mov o0, v0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc mnxn #1 (vs_3_0)",
        "stage": "vertex",
        "tokens": [
            0xFFFE0300,                                               # vs_3_0
            0x0200001F, 0x80000000, 0x900F0000,                       # dcl_position v0
            0x0200001F, 0x80000000, 0xE00F0000,                       # dcl_position o0
            0x0200001F, 0x80000005, 0xE00F0001,                       # dcl_texcoord o1
            0x02000001, 0x800F0001, 0xA0E40004,                       # mov r1, c4
            0x03000016, 0x800F0000, 0x81E10001, 0xA0E40000,           # m3x4 r0, -r1.yxzw, c0
            0x02000001, 0xE00F0001, 0x80E40000,                       # mov o1, r0
            0x02000001, 0xE00F0000, 0x90E40000,                       # mov o0, v0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc mnxn #2 (vs_3_0)",
        "stage": "vertex",
        "tokens": [
            0xFFFE0300,                                               # vs_3_0
            0x05000051, 0xA00F0004, 0xBF800000, 0xBF800000, 0xBF800000, 0xBF800000,# def c4, -1, -1, -1, -1
            0x0200001F, 0x80000000, 0x900F0000,                       # dcl_position v0
            0x0200001F, 0x80000000, 0xE00F0000,                       # dcl_position o0
            0x0200001F, 0x80000005, 0xE00F0001,                       # dcl_texcoord o1
            0x02000001, 0x800F0001, 0xA0E40003,                       # mov r1, c3
            0x02000001, 0x800F0000, 0xA0E40004,                       # mov r0, c4
            0x03000015, 0x80070000, 0x80E40001, 0xA0E40000,           # m4x3 r0.xyz, r1, c0
            0x02000001, 0xE00F0001, 0x80E40000,                       # mov o1, r0
            0x02000001, 0xE00F0000, 0x90E40000,                       # mov o0, v0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc mnxn #3 (vs_3_0)",
        "stage": "vertex",
        "tokens": [
            0xFFFE0300,                                               # vs_3_0
            0x05000051, 0xA00F0004, 0xBF800000, 0xBF800000, 0xBF800000, 0xBF800000,# def c4, -1, -1, -1, -1
            0x0200001F, 0x80000000, 0x900F0000,                       # dcl_position v0
            0x0200001F, 0x80000000, 0xE00F0000,                       # dcl_position o0
            0x0200001F, 0x80000005, 0xE00F0001,                       # dcl_texcoord o1
            0x02000001, 0x800F0001, 0xA0E40003,                       # mov r1, c3
            0x02000001, 0x800F0000, 0xA0E40004,                       # mov r0, c4
            0x03000017, 0x80070000, 0x801B0001, 0xA0E40000,           # m3x3 r0.xyz, r1.wzyx, c0
            0x02000001, 0xE00F0001, 0x80E40000,                       # mov o1, r0
            0x02000001, 0xE00F0000, 0x90E40000,                       # mov o0, v0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc mnxn #4 (vs_3_0)",
        "stage": "vertex",
        "tokens": [
            0xFFFE0300,                                               # vs_3_0
            0x05000051, 0xA00F0003, 0xBF800000, 0xC0000000, 0xC0400000, 0xC0800000,# def c3, -1, -2, -3, -4
            0x0200001F, 0x80000000, 0x900F0000,                       # dcl_position v0
            0x0200001F, 0x80000000, 0xE00F0000,                       # dcl_position o0
            0x0200001F, 0x80000005, 0xE00F0001,                       # dcl_texcoord o1
            0x02000001, 0x800F0001, 0xA0E40000,                       # mov r1, c0
            0x02000001, 0x800F0000, 0xA0E40003,                       # mov r0, c3
            0x03000018, 0x80030000, 0x80080001, 0xA0E40001,           # m3x2 r0.xy, r1.xzxx, c1
            0x02000001, 0xE00F0001, 0x80E40000,                       # mov o1, r0
            0x02000001, 0xE00F0000, 0x90E40000,                       # mov o0, v0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #0 (vs_3_0)",
        "stage": "vertex",
        "tokens": [
            0xFFFE0300,                                               # vs_3_0
            0x05000051, 0xA00F0000, 0x00000000, 0x3F800000, 0x3E99999A, 0x3F000000,# def c0, 0.0, 1.0, 0.3, 0.5
            0x0200001F, 0x80000000, 0x900F0000,                       # dcl_position v0
            0x0200001F, 0x80000000, 0xE00F0000,                       # dcl_position o0
            0x0200001F, 0x80000005, 0xE00F0001,                       # dcl_texcoord0 o1
            0x0200001F, 0x80010005, 0xE00F0002,                       # dcl_texcoord1 o2
            0x0200001F, 0x80020005, 0xE00F0003,                       # dcl_texcoord2 o3
            0x02000001, 0xE00F0000, 0x90E40000,                       # mov o0, v0
            0x02000001, 0xE00F0001, 0xA0450000,                       # mov o1, c0.yyxy
            0x02000001, 0xE00F0002, 0xA0E40000,                       # mov o2, c0
            0x02000024, 0x800F0000, 0xA0E40000,                       # nrm r0, c0
            0x02000001, 0xE00F0003, 0x80E40000,                       # mov o3, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #1 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x0200001F, 0x80000005, 0x90070001,                       # dcl_texcoord0 v0.xyz
            0x02000024, 0x800F0000, 0x90E40001,                       # nrm r0, v0
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #2 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x0200001F, 0x80000005, 0x90070000,                       # dcl_texcoord0 v0.xyz
            0x02000001, 0x800F0000, 0xA0E40000,                       # mov r0, c0
            0x02000024, 0x80070000, 0x90E40000,                       # nrm r0.xyz, v0
            0x02000001, 0x80040000, 0x80FF0000,                       # mov r0.z, r0.w
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #3 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x0200001F, 0x80010005, 0x900F0000,                       # dcl_texcoord1 v0.xyzw
            0x02000024, 0x800F0000, 0x90E40000,                       # nrm r0, v0
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #4 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x0200001F, 0x80020005, 0x900F0000,                       # dcl_texcoord2 v0.xyzw
            0x02000001, 0x800F0000, 0x90E40000,                       # mov r0, v0
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #5 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x0200001F, 0x80010005, 0x900F0000,                       # dcl_texcoord1 v0.xyzw
            0x02000001, 0x800F0000, 0xA0E40000,                       # mov r0, c0
            0x02000024, 0x80070000, 0x90E40000,                       # nrm r0.xyz, v0
            0x02000001, 0x80040000, 0x80FF0000,                       # mov r0.z, r0.w
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #6 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x0200001F, 0x80010005, 0x900F0000,                       # dcl_texcoord1 v0.xyzw
            0x02000001, 0x800F0000, 0xA0E40000,                       # mov r0, c0
            0x02000024, 0x800F0000, 0x90540000,                       # nrm r0, v0.xy
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #7 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x0200001F, 0x80010005, 0x900F0000,                       # dcl_texcoord1 v0.xyzw
            0x02000001, 0x800F0000, 0xA0E40000,                       # mov r0, c0
            0x02000024, 0x80070000, 0x90FD0000,                       # nrm r0.xyz, v0.yw
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #8 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x0200001F, 0x80010005, 0x90030000,                       # dcl_texcoord1 v0.xy
            0x02000001, 0x800F0000, 0xA0E40000,                       # mov r0, c0
            0x02000024, 0x800F0000, 0x90540000,                       # nrm r0, v0
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #9 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x02000024, 0x800F0000, 0xA0E40000,                       # nrm r0, c0
            0x02000001, 0x80040000, 0x80FF0000,                       # mov r0.z, r0.w
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc nrm #10 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x02000024, 0x800F0000, 0xA0390000,                       # nrm r0, c0.yzwx
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc rep #0 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x05000051, 0xA00F0001, 0x00000000, 0x3F800000, 0x00000000, 0x00000000,# def c1, 0, 1, 0, 0
            0x02000001, 0x80010000, 0xA0000001,                       # mov r0.x, c1.x
            0x01000026, 0xF0E40000,                                   # rep i0
            0x03000002, 0x80010000, 0x80000000, 0xA0550001,           # add r0.x, r0.x, c1.y
            0x00000027,                                               # endrep
            0x02000001, 0x80010800, 0x80000000,                       # mov oC0.x, r0.x
            0x02000001, 0x80030000, 0xA0000001,                       # mov r0.xy, c1.x
            0x01000026, 0xF0E40001,                                   # rep i1
            0x03000002, 0x80010000, 0x80550000, 0x80000000,           # add r0.x, r0.y, r0.x
            0x03000002, 0x80020000, 0x80550000, 0xA0550001,           # add r0.y, r0.y, c1.y
            0x00000027,                                               # endrep
            0x02000001, 0x80020800, 0x80000000,                       # mov oC0.y, r0.x
            0x02000001, 0x80040800, 0xA0000001,                       # mov oC0.z, c1.x
            0x02000001, 0x80080800, 0xA0000000,                       # mov oC0.w, c0.x
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc rep #1 (ps_3_0)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0300,                                               # ps_3_0
            0x05000051, 0xA00F0000, 0x00000000, 0x41200000, 0x3F800000, 0x41980000,# def c0, 0, 10, 1, 19
            0x02000001, 0x800F0000, 0xA0000000,                       # mov r0, c0.x
            0x02000001, 0x80010001, 0xA0000000,                       # mov r1.x, c0.x
            0x01000026, 0xF0E40000,                                   # rep i0
            0x03000002, 0x800F0002, 0x80E40000, 0xA0550000,           # add r2, r0, c0.y
            0x03000002, 0x80020001, 0x80000001, 0xA0AA0000,           # add r1.y, r1.x, c0.z
            0x02030029, 0x80000001, 0xA0FF0000,                       # if_ge r1.x, c0.w
            0x02000001, 0x800F0000, 0x80E40002,                       # mov r0, r2
            0x0205002D, 0xA0AA0000, 0xA1AA0000,                       # break_ne c0.z, -c0.z
            0x0000002B,                                               # endif
            0x02000001, 0x800F0000, 0x80E40002,                       # mov r0, r2
            0x02000001, 0x80010001, 0x80550001,                       # mov r1.x, r1.y
            0x00000027,                                               # endrep
            0x02000001, 0x800F0800, 0x80E40000,                       # mov oC0, r0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc texreg #0 (vs_1_1)",
        "stage": "vertex",
        "tokens": [
            0xFFFE0101,                                               # vs_1_1
            0x0000001F, 0x80000000, 0x900F0000,                       # dcl_position v0
            0x00000001, 0xE00F0000, 0x90E40000,                       # mov oT0, v0
            0x00000001, 0xC00F0000, 0x90E40000,                       # oPos, v0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc texreg #1 (ps_1_2)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0102,                                               # ps_1_2
            0x00000051, 0xA00F0000, 0x00000000, 0x00000000, 0x00000000, 0x3F800000,# def c0, 0, 0, 0, 1
            0x00000040, 0xB00F0000,                                   # texcoord t0
            0x00000001, 0x80070000, 0xB0E40000,                       # mov r0.xyz, t0
            0x40000001, 0x80080000, 0xA0FF0000,                       # + mov r0.w, c0.w
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc texreg #2 (ps_1_2)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0102,                                               # ps_1_2
            0x00000042, 0xB00F0000,                                   # tex t0
            0x00000001, 0x800F0000, 0xB0E40000,                       # mov r0, t0
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc texreg #3 (ps_1_2)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0102,                                               # ps_1_2
            0x00000042, 0xB00F0000,                                   # tex t0
            0x00000045, 0xB00F0001, 0xB0E40000,                       # texreg2ar t1, t0
            0x00000001, 0x800F0000, 0xB0E40001,                       # mov r0, t1
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc texreg #4 (ps_1_2)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0102,                                               # ps_1_2
            0x00000042, 0xB00F0000,                                   # tex t0
            0x00000046, 0xB00F0001, 0xB0E40000,                       # texreg2gb t1, t0
            0x00000001, 0x800F0000, 0xB0E40001,                       # mov r0, t1
            0x0000FFFF,                                               # end
        ],
    },
    {
        "name": "vkd3d d3dbc texreg #5 (ps_1_2)",
        "stage": "fragment",
        "tokens": [
            0xFFFF0102,                                               # ps_1_2
            0x00000042, 0xB00F0000,                                   # tex t0
            0x00000052, 0xB00F0001, 0xB0E40000,                       # texreg2rgb t1, t0
            0x00000001, 0x800F0000, 0xB0E40001,                       # mov r0, t1
            0x0000FFFF,                                               # end
        ],
    },
]

STAGE_METADATA = {"vertex": "!air.vertex", "fragment": "!air.fragment"}

failures = 0


def fail(name: str, why: str) -> None:
    global failures
    failures += 1
    print(f"not ok - {name}: {why}")


def run(args: list[str]) -> tuple[int, bytes]:
    proc = subprocess.run([AIRCONV, *args], capture_output=True)
    return proc.returncode, proc.stdout + proc.stderr


def check(shader: dict, tmp: Path) -> None:
    name = shader["name"]
    blob = tmp / "shader.dxso"
    blob.write_bytes(b"".join(struct.pack("<I", w) for w in shader["tokens"]))

    # 1. AIR LLVM IR.
    ll = tmp / "shader.ll"
    rc, out = run(["--dxso", "-S", str(blob), "-o", str(ll)])
    if rc != 0:
        return fail(name, f"airconv -S exited {rc}: {out.decode(errors='replace').strip()}")
    ir = ll.read_text(errors="replace")
    if 'target triple = "air64' not in ir:
        return fail(name, "AIR output missing air64 target triple")
    if "@shader_main" not in ir:
        return fail(name, "AIR output missing shader_main entry point")
    want = STAGE_METADATA[shader["stage"]]
    if want not in ir:
        return fail(name, f"AIR output missing {want} stage metadata")

    # 2. Metal library.
    lib = tmp / "shader.metallib"
    rc, out = run(["--dxso", "-A", str(blob), "-o", str(lib)])
    if rc != 0:
        return fail(name, f"airconv -A exited {rc}: {out.decode(errors='replace').strip()}")
    magic = lib.read_bytes()[:4]
    if magic != b"MTLB":
        return fail(name, f"metallib magic is {magic!r}, expected b'MTLB'")

    print(f"ok - {name}")


def main() -> int:
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        for shader in SHADERS:
            check(shader, tmp)
    print(f"{len(SHADERS)} shader(s), {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
