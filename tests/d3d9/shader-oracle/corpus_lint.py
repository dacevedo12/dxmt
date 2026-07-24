#!/usr/bin/env python3
# Wine-free, GPU-free integrity check for the vendored d3d9 shader-oracle corpus.
#
# The oracle proper (runner/shader_runner*.c) needs a live Metal device under
# wine to draw each quad and probe the render target, so it can only run on a
# device-capable runner. This lint is the part CI can run on the wine-free host
# gate: it parses every .shader_test the way vkd3d's shader_runner would and
# fails if a file stopped being well-formed, so a bad corpus edit is caught in
# the required suite instead of surfacing only on the device.
#
# It deliberately does not evaluate the shaders (that is the device run's job);
# it checks structure: a [require] shader model, at least one shader block, that
# every hand-assembled d3dbc-hex block is valid bytecode (8-hex-digit words, a
# ps_/vs_ version token first, the 0x0000ffff end token last), and that there is
# at least one [test] block carrying a probe.
import pathlib
import re
import sys

CORPUS = pathlib.Path(__file__).resolve().parent / "corpus"

# Section headers look like "[pixel shader d3dbc-hex]", "[vertex shader]",
# "[require]", "[test]". Capture the bracketed name.
SECTION_RE = re.compile(r"^\s*\[([^\]]+)\]\s*$")
HEX_WORD_RE = re.compile(r"^[0-9a-fA-F]{8}$")


def strip_comment(line):
    # A '%' starts a comment for the rest of the line (the disassembly gloss).
    return line.split("%", 1)[0]


def check_d3dbc_hex(name, header, body_lines, errors):
    words = []
    for raw in body_lines:
        for tok in strip_comment(raw).split():
            if not HEX_WORD_RE.match(tok):
                errors.append(f"{name}: [{header}] has non-hex token {tok!r}")
                return
            words.append(tok.lower())
    if not words:
        errors.append(f"{name}: [{header}] is empty")
        return
    # Version token: 0xffff.... for a pixel shader, 0xfffe.... for a vertex one.
    if words[0][:4] not in ("ffff", "fffe"):
        errors.append(f"{name}: [{header}] first word {words[0]!r} is not a ps/vs version token")
    # End token.
    if words[-1] != "0000ffff":
        errors.append(f"{name}: [{header}] last word {words[-1]!r} is not the 0x0000ffff end token")


def check_file(path, errors):
    name = path.name
    sections = []  # (header, [lines])
    current = None
    for raw in path.read_text().splitlines():
        m = SECTION_RE.match(raw)
        if m:
            current = (m.group(1).strip(), [])
            sections.append(current)
        elif current is not None:
            current[1].append(raw)

    headers = [h for h, _ in sections]
    if not any(h == "require" for h in headers):
        errors.append(f"{name}: missing [require] section")
    require_body = next((b for h, b in sections if h == "require"), [])
    if not any("shader model" in line for line in require_body):
        errors.append(f"{name}: [require] does not pin a shader model")

    shader_blocks = [(h, b) for h, b in sections if h.endswith("shader") or " shader " in h or h.endswith("d3dbc-hex")]
    if not shader_blocks:
        errors.append(f"{name}: no shader blocks")

    for header, body in shader_blocks:
        if header.endswith("d3dbc-hex"):
            check_d3dbc_hex(name, header, body, errors)

    test_blocks = [b for h, b in sections if h == "test"]
    if not test_blocks:
        errors.append(f"{name}: no [test] section")
    elif not any("probe" in line for b in test_blocks for line in b):
        errors.append(f"{name}: no probe directive in any [test] section")


def main():
    files = sorted(CORPUS.glob("*.shader_test"))
    if len(files) < 9:
        print(f"FAIL: expected at least 9 corpus files, found {len(files)}", file=sys.stderr)
        return 1

    errors = []
    for path in files:
        check_file(path, errors)

    if errors:
        print("shader-oracle corpus lint FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1

    print(f"shader-oracle corpus lint OK: {len(files)} files well-formed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
