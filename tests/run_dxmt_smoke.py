#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# ///
#
# Copyright (c) 2026 GameSir Labs and contributors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.

from __future__ import annotations

import argparse
import os
import pathlib
import shlex
import subprocess
import sys


def quote_command(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def run_command(command: list[str], cwd: pathlib.Path, env: dict[str, str]) -> tuple[int, list[str]]:
    print("+", quote_command(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, env=env, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(completed.stdout, end="")
    failures = [
        line for line in completed.stdout.splitlines()
        if line.startswith("not ok") or "Test failed:" in line
    ]
    if completed.returncode:
        failures.append(f"process exited with status {completed.returncode}")
    for marker in ("Unhandled exception:", "Unhandled page fault", "wine: Unhandled"):
        if marker in completed.stdout:
            failures.append(marker)
    if failures:
        print("dxmt-smoke failed assertions:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
    return completed.returncode, failures


def resolve_repo_dir() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[1]


def resolve_build_dir(repo_dir: pathlib.Path, build_dir: str) -> pathlib.Path:
    path = pathlib.Path(build_dir)
    if path.is_absolute():
        return path
    return (repo_dir / path).resolve()


class TestTarget:
    def __init__(self, path: pathlib.Path, kind: str):
        self.path = path
        self.kind = kind


def list_suite_tests(runner: pathlib.Path, target: TestTarget, env: dict[str, str]) -> list[str]:
    command = [str(runner), str(target.path), "--list-tests"]
    print("+", quote_command(command), flush=True)
    completed = subprocess.run(command, cwd=target.path.parent, env=env, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(completed.stdout, end="")
    if completed.returncode:
        raise RuntimeError(f"failed to list {target.path.name}: status {completed.returncode}")

    tests: list[str] = []
    for line in completed.stdout.splitlines():
        name = line.strip()
        if name.startswith("test_") and " " not in name and name not in tests:
            tests.append(name)
    return tests


def smoke_targets(build_dir: pathlib.Path, only: str) -> list[TestTarget]:
    tests: list[TestTarget] = []
    if only in ("all", "dx11"):
        tests.append(TestTarget(build_dir / "tests" / "dx11" / "dx11_smoke.exe", "smoke"))
    if only in ("all", "dx12"):
        tests.append(TestTarget(build_dir / "tests" / "dx12" / "dx12_smoke.exe", "smoke"))
    if only in ("all", "d3d12-core"):
        tests.append(TestTarget(build_dir / "tests" / "vkd3d-proton" / "d3d12-suite.exe", "suite"))
        tests.append(TestTarget(build_dir / "tests" / "vkd3d-proton" / "d3d12-invalid-usage.exe", "suite"))
        tests.append(TestTarget(build_dir / "tests" / "vkd3d-proton" / "d3d12-api-d3d.exe", "suite"))
        tests.append(TestTarget(build_dir / "tests" / "vkd3d-proton" / "d3d12-common-test.exe", "suite"))
    if only in ("all", "d3d12-perf"):
        tests.append(TestTarget(build_dir / "tests" / "vkd3d-proton" / "d3d12-descriptor-performance.exe", "plain"))
        tests.append(TestTarget(build_dir / "tests" / "vkd3d-proton" / "d3d12-pso-library-bloat.exe", "plain"))
    return tests


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run DXMT DX11/DX12 smoke tests and the vendored D3D12 test groups through wine-test."
    )
    parser.add_argument(
        "--build-dir",
        default=os.environ.get("DXMT_BUILD_DIR", "build-codex-win64-ourwine-nightly"),
        help="Meson build directory that contains the smoke executables.",
    )
    parser.add_argument(
        "--dxmt-root",
        default=os.environ.get(
            "DXMT_ROOT",
            "/Users/shiyu/Library/Caches/dxmt-runtime-smoke/ourwine-nightly-install",
        ),
        help="Installed DXMT root used by wine-test/run-dxmt.sh.",
    )
    parser.add_argument(
        "--runner",
        default=os.environ.get(
            "DXMT_WINE_RUNNER",
            "/Users/shiyu/Documents/Project/wine-test/run-dxmt.sh",
        ),
        help="wine-test launcher.",
    )
    parser.add_argument(
        "--only",
        choices=("all", "dx11", "dx12", "d3d12-core", "d3d12-perf"),
        default="all",
        help="Subset to run.",
    )
    parser.add_argument(
        "--filter",
        help="Pass a smoke filter, or set VKD3D_TEST_FILTER for the D3D12 suite tests.",
    )
    parser.add_argument(
        "--match",
        help="Set VKD3D_TEST_MATCH for the D3D12 suite tests.",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List tests instead of running them where the executable supports it.",
    )
    parser.add_argument(
        "--allow-failures",
        action="store_true",
        help="Report failed assertions but return success. Useful while strict D3D12 tests are still being ported.",
    )
    parser.add_argument(
        "--d3d12-single-process",
        action="store_true",
        help="Run list-capable D3D12 suites in one process instead of isolating each named test.",
    )
    parser.add_argument(
        "--include-stress",
        action="store_true",
        help="Include tests whose names contain 'stress' when isolating named tests.",
    )
    args = parser.parse_args(argv)

    repo_dir = resolve_repo_dir()
    build_dir = resolve_build_dir(repo_dir, args.build_dir)
    runner = pathlib.Path(args.runner)
    dxmt_root = pathlib.Path(args.dxmt_root)

    tests = smoke_targets(build_dir, args.only)
    missing = [target.path for target in tests if not target.path.exists()]
    if missing:
        for path in missing:
            print(f"missing smoke executable: {path}", file=sys.stderr)
        print(f"build them with: meson compile -C {build_dir}", file=sys.stderr)
        return 2

    if not runner.exists():
        print(f"missing wine runner: {runner}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env.setdefault("DXMT_SHADER_CACHE", "0")
    env["DXMT_ROOT"] = str(dxmt_root)

    failed = 0
    failed_assertions: list[str] = []
    for target in tests:
        test_exe = target.path
        test_env = env.copy()
        command = [str(runner), str(test_exe)]
        if target.kind == "suite" and not args.d3d12_single_process:
            if args.list:
                command.append("--list-tests")
                returncode, assertions = run_command(command, test_exe.parent, test_env)
                failed |= returncode
                failed_assertions.extend(assertions)
                continue

            try:
                names = list_suite_tests(runner, target, test_env)
            except RuntimeError as e:
                print(str(e), file=sys.stderr)
                failed = 1
                failed_assertions.append(str(e))
                continue

            if args.match:
                names = [name for name in names if name == args.match]
            if args.filter:
                names = [name for name in names if args.filter in name]
            if not args.include_stress:
                names = [name for name in names if "stress" not in name]

            if not names:
                print(f"no D3D12 tests selected for {test_exe.name}; skipping")
                continue

            print(f"dxmt-smoke: running {len(names)} isolated D3D12 tests from {test_exe.name}")
            for name in names:
                per_test_env = test_env.copy()
                per_test_env["VKD3D_TEST_MATCH"] = name
                returncode, assertions = run_command(command, test_exe.parent, per_test_env)
                failed |= returncode
                failed_assertions.extend(assertions)
        elif target.kind == "suite":
            if args.list:
                command.append("--list-tests")
            if args.filter:
                test_env["VKD3D_TEST_FILTER"] = args.filter
            if args.match:
                test_env["VKD3D_TEST_MATCH"] = args.match
            returncode, assertions = run_command(command, test_exe.parent, test_env)
            failed |= returncode
            failed_assertions.extend(assertions)
        elif args.list:
            continue
        elif args.filter:
            command.extend(["--filter", args.filter])
            returncode, assertions = run_command(command, test_exe.parent, test_env)
            failed |= returncode
            failed_assertions.extend(assertions)
        else:
            returncode, assertions = run_command(command, test_exe.parent, test_env)
            failed |= returncode
            failed_assertions.extend(assertions)

    if args.allow_failures and failed:
        print(f"dxmt-smoke: allowed {len(failed_assertions)} failed assertions")
        return 0
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
