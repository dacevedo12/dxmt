#!/usr/bin/env python3
"""Wrap each test function call in a Wine conformance module so it can be run
or skipped individually.

Wine runs every test function of a module from one START_TEST in a single
process, so a crash or a hang takes the rest of the module with it and the
functions after it are never measured. Wrapping each call lets the suite re-run
a module while skipping a function that did not survive, which is what turns a
module that crashes into a module that reports results.

The wrapping happens here, at build time, rather than in the corpus: those
sources are fetched from Wine unmodified and must stay that way. Wrapping in
place also preserves each module's own preamble, which registers a window class
and gates the whole module on the display mode, and which a generated wrapper
would have to reproduce exactly.

One check fails the build: every bare call inside START_TEST must end up
wrapped, because one that is missed runs unconditionally in every re-run and
quietly breaks the isolation the re-run depends on. The number of calls found is
also compared against a recorded count and reported when it differs, which says
the corpus moved.

The count is a report rather than a build failure because the corpus is not
pinned: the managed cache resets its Wine checkout to the branch tip, so the
count changes whenever Wine does and failing on it would break the build for
reasons unrelated to the change being built. Coverage is instead enforced where
it can be enforced honestly, at run time, by accounting for every function in
the manifest this emits from the source actually used.
"""

import argparse
import re
import sys

# Wine's test functions are called with no arguments from START_TEST. Every
# other statement there takes arguments, so this cannot match one of them.
CALL = re.compile(r"^(\s+)([A-Za-z_][A-Za-z_0-9]*)\(\);\s*$")

START_TEST = re.compile(r"^START_TEST\s*\(\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)")


def find_start_test(lines):
    """Return the half-open line range of the START_TEST body."""
    for index, line in enumerate(lines):
        if not START_TEST.match(line):
            continue
        # The body opens on this line or the next one, then runs to the brace
        # that closes it.
        depth = 0
        seen = False
        for end in range(index, len(lines)):
            depth += lines[end].count("{") - lines[end].count("}")
            if lines[end].count("{"):
                seen = True
            if seen and depth == 0:
                return index, end
        raise SystemExit("unterminated START_TEST body")
    raise SystemExit("no START_TEST found")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("--output", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--expect-count", type=int, required=True)
    parser.add_argument("--module", required=True)
    arguments = parser.parse_args()

    with open(arguments.source, "r", encoding="utf-8", errors="surrogateescape") as handle:
        lines = handle.read().splitlines()

    begin, end = find_start_test(lines)

    functions = []
    for index in range(begin, end + 1):
        match = CALL.match(lines[index])
        if not match:
            continue
        indent, name = match.group(1), match.group(2)
        functions.append(name)
        lines[index] = f"{indent}DXMT_RUN({name});"

    # A call left unwrapped would run in every re-run regardless of the skip
    # list, so treat it as a build failure rather than a coverage surprise.
    leftover = [
        lines[i] for i in range(begin, end + 1)
        if CALL.match(lines[i]) is not None
    ]
    if leftover:
        sys.exit(
            f"{arguments.module}: {len(leftover)} call(s) inside START_TEST were "
            f"not wrapped: {leftover[:3]}"
        )

    if len(functions) != arguments.expect_count:
        print(
            f"{arguments.module}: found {len(functions)} test functions, "
            f"recorded {arguments.expect_count}; the Wine corpus has moved",
            file=sys.stderr,
        )

    with open(arguments.output, "w", encoding="utf-8", errors="surrogateescape") as handle:
        handle.write("\n".join(lines))
        handle.write("\n")

    with open(arguments.manifest, "w", encoding="utf-8") as handle:
        for name in functions:
            handle.write(name + "\n")


if __name__ == "__main__":
    main()
