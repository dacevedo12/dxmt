#!/usr/bin/env python3
"""Gate src/d3d9 on its own .clang-format.

The formatter is not otherwise wired into anything, so nothing stops a file from
drifting. This gates only src/d3d9, whose files are all new in this work: the
modules DXMT already shipped are left alone, because reformatting a maintained
file would bury a real change under a diff nobody asked for.

Version sensitivity is the reason this reports the binary it used. clang-format's
output changes between releases, so a gate that runs "whatever is on PATH" fails
on the machine that did not format the tree rather than on the change that broke
it. If this fails with no formatting change in the diff, compare versions first.
"""

import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
TARGET = REPO / "src" / "d3d9"


def sources():
    for path in sorted(TARGET.iterdir()):
        # Sync duplicates ("name 2.cpp") are not tracked and are not ours to gate.
        if path.suffix in (".cpp", ".hpp") and " 2." not in path.name and " 3." not in path.name:
            yield path


def main():
    binary = shutil.which("clang-format")
    if binary is None:
        print("FAIL: clang-format is not on PATH, so formatting is unverified.")
        print("      Install it, or drop this test rather than letting it pass silently.")
        return 1

    version = subprocess.run([binary, "--version"], capture_output=True, text=True).stdout.strip()
    offenders = []
    for path in sources():
        result = subprocess.run(
            [binary, "--dry-run", "--Werror", str(path)], capture_output=True, text=True
        )
        if result.returncode != 0:
            offenders.append(path.name)

    if offenders:
        print(f"FAIL: {len(offenders)} file(s) in src/d3d9 do not match src/d3d9/.clang-format\n")
        for name in offenders:
            print(f"    {name}")
        print(f"\n  fix: clang-format -i src/d3d9/*.cpp src/d3d9/*.hpp")
        print(f"  formatter: {version}")
        return 1

    count = sum(1 for _ in sources())
    print(f"ok: {count} files in src/d3d9 match .clang-format ({version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
