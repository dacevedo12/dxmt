# D3D12 Tests

This directory vendors the upstream D3D12 test suite from:

```
https://github.com/HansKristian-Work/vkd3d-proton
commit 3b10bd7a7ec6a7347e616cf8bea59333afec2255
```

The tree intentionally keeps the upstream test sources, generated shader
headers, IDL files, and the small shared support library needed by the test
framework. `meson.build` is the local DXMT wrapper that generates WIDL headers
and links the tests against this repository's `d3d12.dll` and `dxgi.dll`.

The main upstream D3D12 suite is built as:

```
build-codex-win64-ourwine-nightly/tests/vkd3d-proton/d3d12-suite.exe
```

The upstream `d3d12_tests.h` list currently exposes 544 named D3D12 test cases.
Additional standalone targets are built for invalid usage, common helper tests,
the D3D12 API black-box coverage, and the perf-oriented cases. The local
runner enumerates list-capable targets and runs each named test in a separate
Wine process so one crash does not hide later failures. Tests whose names
contain `stress` follow upstream's default and are skipped unless
`--include-stress` is passed.

The upstream `tests/vkd3d_api.c` target depends on the upstream Vulkan/internal
API surface rather than the D3D DLL ABI exposed by DXMT. DXMT keeps that source
for reference and builds `tests/d3d12_api_d3d.c` instead, which ports the same
broad coverage areas to D3D12/DXGI black-box checks for device creation,
feature support, adapter LUIDs, COM parent relationships, queues/fences,
private data, root signatures, resources, heap allocation, and format support.

Useful commands:

```
meson compile -C build-codex-win64-ourwine-nightly
uv run --script tests/run_dxmt_smoke.py --only d3d12-core --list
uv run --script tests/run_dxmt_smoke.py --only d3d12-core --match test_create_device --allow-failures
uv run --script tests/run_dxmt_smoke.py --only d3d12-core --include-stress --allow-failures
uv run --script tests/run_dxmt_smoke.py --only d3d12-core --d3d12-single-process --allow-failures
uv run --script tests/run_dxmt_smoke.py --only d3d12-perf --allow-failures
```

During the D3D12 bring-up period, strict failures and runtime crashes are
expected to remain visible. Do not weaken upstream assertions unless the
difference is caused by the local test environment rather than DXMT behavior.
