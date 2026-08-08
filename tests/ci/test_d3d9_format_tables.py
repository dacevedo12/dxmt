#!/usr/bin/env python3
"""Cross-consistency of the D3D9 format tables.

d3d9_format.cpp answers a dozen questions about a D3DFORMAT from separate
switches. They are siblings: a format one of them accepts has to be known to
the others that its callers reach, and nothing in the compiler enforces that.
When one switch fell behind, a supported format took the `default:` arm and
reported zero bytes per pixel, which computed a zero LockRect pitch and, worse,
made every unknown format compare EQUAL to every other under the
`bpp(src) == bpp(dst)` tests that pick a copy path.

This runs on the source rather than the binary because the answer is a property
of the tables themselves, so it needs no device, no Wine and no Windows headers.
"""

import pathlib
import re
import sys

FORMAT_CPP = pathlib.Path(__file__).resolve().parents[2] / "src" / "d3d9" / "d3d9_format.cpp"

# A format whose Metal mapping is only ever Invalid is declared unsupported, so
# the siblings are not required to know it.
UNSUPPORTED_RETURN = "WMTPixelFormatInvalid"


def function_bodies(text):
    """Split the file into {function name: body}, one entry per definition.

    The file's definitions all start at column 0 with the name, the previous
    line carrying the return type, and end at the first column-0 '}'.
    """
    bodies = {}
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\(D3DFORMAT format", line)
        if not m:
            continue
        end = i + 1
        while end < len(lines) and lines[end] != "}":
            end += 1
        bodies[m.group(1)] = "\n".join(lines[i:end])
    return bodies


def cases(body):
    """Every D3DFMT_ named in a case label of this body."""
    return {m.group(1) for m in re.finditer(r"case\s+(D3DFMT_[A-Za-z0-9_]+)\s*:", body)}


def supported_formats(body):
    """Formats D3DFormatToMetal maps to a real Metal format.

    A case whose every return is Invalid is an explicit rejection, not support.
    Case labels stack, so a run of labels shares the returns that follow it.
    """
    supported = set()
    pending = []
    for line in body.splitlines():
        label = re.match(r"\s*case\s+(D3DFMT_[A-Za-z0-9_]+)\s*:", line)
        if label:
            pending.append(label.group(1))
            continue
        if "return" in line and pending:
            if UNSUPPORTED_RETURN not in line or line.count("WMTPixelFormat") > 1:
                supported.update(pending)
            pending = []
    return supported


def main():
    text = FORMAT_CPP.read_text()
    bodies = function_bodies(text)

    missing = []
    for required in ("D3DFormatToMetal", "D3DFormatBytesPerPixel", "IsCompressedFormat"):
        if required not in bodies:
            missing.append(required)
    if missing:
        print(f"FAIL: {FORMAT_CPP.name} no longer defines {', '.join(missing)}; update this test")
        return 1

    supported = supported_formats(bodies["D3DFormatToMetal"])
    sized = cases(bodies["D3DFormatBytesPerPixel"])
    compressed = cases(bodies["IsCompressedFormat"])

    failures = []

    # Every supported format needs a byte size. Block-compressed formats are
    # sized through the block geometry helpers instead, so they are exempt.
    unsized = sorted(supported - sized - compressed)
    if unsized:
        failures.append(
            "D3DFormatToMetal maps these to a Metal format but D3DFormatBytesPerPixel "
            "does not size them, so they take its default arm and report 0:\n    "
            + "\n    ".join(unsized)
        )

    # A compressed format must be sized by the block helpers, not by bpp alone.
    # A helper covers them either by naming them or by asking IsCompressedFormat,
    # which is how both currently do it.
    for helper in ("D3DFormatBlockWidth", "D3DFormatBlockHeight"):
        if helper not in bodies:
            failures.append(f"{helper} is gone; block-compressed sizing is unverified")
            continue
        body = bodies[helper]
        if "IsCompressedFormat(" in body:
            continue
        unblocked = sorted(compressed - cases(body))
        if unblocked:
            failures.append(
                f"IsCompressedFormat accepts these but {helper} neither names them nor "
                f"defers to IsCompressedFormat:\n    " + "\n    ".join(unblocked)
            )

    # A stencil aspect only makes sense on a depth-stencil format.
    if "HasStencilAspect" in bodies and "IsDepthStencilFormat" in bodies:
        stray = sorted(cases(bodies["HasStencilAspect"]) - cases(bodies["IsDepthStencilFormat"]))
        if stray:
            failures.append(
                "HasStencilAspect claims a stencil aspect for formats IsDepthStencilFormat "
                "does not accept:\n    " + "\n    ".join(stray)
            )

    if failures:
        print(f"FAIL: {FORMAT_CPP.name} format tables disagree\n")
        for f in failures:
            print(f"  {f}\n")
        return 1

    print(
        f"ok: {len(supported)} supported formats, all sized; "
        f"{len(compressed)} compressed, all blocked"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
