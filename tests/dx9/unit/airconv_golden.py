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
