# Build DXMT

[Our CI](/.github/workflows/ci.yml) is the best reference.

## Prerequisites:
- MacOS Sonoma and later. <sub>It doesn't make a lot of sense to build a project targeting Apple platforms from Windows, Linux or other OSs.</sub>
- Meson 1.3+, it's the build system used by this project
- LLVM 15 (exact major version), with headers and static libs
    - CMake 3.27+, when building LLVM from the source
- Xcode 16+, with Metal toolchain
- Wine 8+, with headers and tools, _if you want to use DXMT with Wine_.
    - A cross-compiler (mingw-w64 or llvm-mingw) for Windows is also required
    - When building DXMT with Wine, it is a **Cross Build**.

The following documentation is NOT for beginners. You are assumed to be familiar with C/C++ compilers and build systems.

## Managed builder and project-local state

The supported entry point is the standalone C++20 builder. Its own Meson
project lives in `tools/dxmt-builder` and is bootstrapped by the thin launcher:

```sh
scripts/dxmt-builder bootstrap host wine-x64 llvm-darwin-x64
scripts/dxmt-builder build --profile gcc-x64-release-full runtime
scripts/dxmt-builder test --profile gcc-x64-release-full all
```

`bootstrap wine-x64` keeps a git worktree of the remote Wine branch under
`<cache_root>/sources/`, always fetches the branch tip, and builds with
**incremental `make`/`make install`** when an out-of-tree `build/` already
exists. Set `DXMT_WINE_CLEAN=1` to wipe `build/`/`install/` and force a full
configure. Published install trees live under `<cache_root>/deps/wine-x86_64-*`
for later DXMT builds to consume without recompiling Wine.

Without local configuration, all builder state lives under `.cache/managed`:
stable Meson/Ninja profile directories, ccache, Metal and apitrace CAS entries,
Wine and LLVM dependencies, staged artifacts, locks, and telemetry. Existing
directories outside the configured root are legacy state; the builder neither
reads, prunes, nor deletes them.

For machine-local configuration, copy the tracked template and edit the local
file as needed:

```sh
cp .dxmt-builder/config.json.example .dxmt-builder/config.json
```

The local `config.json` is ignored by Git. Without an explicit configuration
path, the launcher and builder read only `.dxmt-builder/config.json` from the
active checkout root. They never search parent directories, so sibling Git
worktrees cannot inherit one another's machine-local configuration. An
explicit `--config <path>` remains available for CI or exceptional commands.
Relative `cache_root` values are resolved from the active checkout root.

```json
{
  "cache_root": "../cache",
  "profile_namespace": "git"
}
```

`profile_namespace: "git"` uses the attached branch name, a matching remote
branch, or the commit SHA for a detached checkout. Only mutable profile state
(`build`, `install`, `stage`, `prefix`, and `meta`) and ccache are namespaced.
This keeps compiler-cache capacity and statistics independent between branches.
Dependencies, downloads, CAS entries, artifacts, and telemetry remain globally
shared under `cache_root`. Use `"none"` for an unnamespaced profile layout. CI
uses `.github/dxmt-builder-config.json` explicitly.

Use `scripts/dxmt-builder cache status`, `cache verify`, and `cache prune
--dry-run` to inspect managed state. There is no automatic capacity eviction in
the initial implementation.

## Local DX12/Metal4 audit

The repository has a developer-only audit entry point. It is intentionally not
wired into CI while the rules and baseline are being hardened:

```sh
# Fast path: prerequisite DX12/Metal4 build, architecture policy, thread
# contracts, and selected clang-tidy checks.
scripts/dxmt-builder audit --scope dx12-metal4

# Fast architecture/ownership rules without compiling or running clang-tidy.
scripts/dxmt-builder audit --scope dx12-metal4 --policy-only

# Verify that every required checker is present and that the deliberately
# invalid ownership, bounds, allocator, and thread-safety fixtures are found.
scripts/dxmt-builder audit --scope dx12-metal4 --checker-fixtures-only

# Slower Clang Static Analyzer pass over the ownership-sensitive resource,
# fence, allocation, and Metal4 unix translation units.
scripts/dxmt-builder audit --scope dx12-metal4 --deep --jobs 2

# Build and run the platform-independent submission model under ASan+UBSan
# and TSan in separate Builder-owned native artifacts.
scripts/dxmt-builder audit --scope dx12-metal4 --sanitizers

# Run the native replay/lifetime state-machine tests or Builder self-tests.
scripts/dxmt-builder test unit --suite d3d12-model
scripts/dxmt-builder test unit --suite builder
```

The audit is implemented directly in the C++ Builder and does not add a Python
runtime dependency. It uses the managed LLVM/clang-tidy dependency and defaults
to the `llvm-mingw-x64-debugoptimized` profile so the PE and native compilation
database is Clang-compatible.

Builds default to `llvm-mingw-x64-release`. Clang is not merely a preference
here: GCC ignores `-Wthread-safety` entirely, so every `DXMT_CAPABILITY`,
`DXMT_GUARDED_BY`, `DXMT_REQUIRES` and lock-ordering annotation in the tree is
unchecked under GCC and only verified during an audit. The project build now
passes `-Wthread-safety -Wthread-safety-beta -Werror=thread-safety-analysis`
(dropped automatically by `get_supported_arguments()` on GCC), so a lock
contract violation fails the build. `-beta` is required for *member* lock
ordering; without it `DXMT_ACQUIRED_BEFORE`/`_AFTER` silently degrade into
comments, which is why `audit --checker-fixtures-only` asserts on the exact
"must be acquired before" diagnostic.

Analyzer results are cached under `<cache-root>/audit-cache`, keyed by the
clang-tidy identity, its config, the exact command line, and the content of
every in-repo source the unit can reach (a conservative transitive include
scan -- over-approximating only costs a needless miss, under-approximating
would serve a stale verdict). Keys are content-based, so switching branches or
rebuilding identical files still hits. A no-op deep re-run drops from minutes
to seconds; editing a shared header correctly invalidates every dependent unit.
Pass `--no-cache` to force a full re-analysis. The DX12/Metal4 compilation database is filtered
to select the Metal4 variant of shared sources. Every selected translation unit
is compiled with `-Wthread-safety`; capability-analysis violations are promoted
to errors so clang-tidy cannot reduce them to an ignored warning summary.

Repository-specific policy checks enforce explicit `MTLResidencySet`
ownership domains without introducing a universal per-set lock registry. The
DX12 device owns and serializes its external lifetime set. Winemetal owns its
command and sparse sets and mutates them only through helpers annotated with
`DXMT_REQUIRES(owner_lock)`. One-shot sets that have not been published can use
the direct backend helpers. The four public residency thunks are deliberately
thin direct Metal calls used only by the submission owner; acquiring another
thunk-side lock would create a second lock domain.

The policy rejects a residency lock registry, any lock acquisition in a direct
backend helper or external thunk, raw `MTL4ArgumentTable` handle casts, and ABI
layout drift. It also prevents the remaining raw `NSLock` message count from
increasing. New native owner locks should use `dxmt_nslock_scope`; the legacy
ceiling is migration-only and should only move downward.

Residency ownership is also structural: raw Metal objects cannot be passed
directly to a residency API. Callers must construct one of the non-convertible
`LifetimeResidencyAllocation`, `ReplayAllocatorResidencyAllocation`, or
`ReplayTemporaryResidencyAllocation` tokens. All three domains register into
the single device-owned Metal 4 residency set; the non-convertible token types
only express lifetime policy. Builder restricts allocator tokens to queue-owned
ring allocator blocks and temporary tokens to explicit temporary-buffer paths.
Ordinary D3D/DXMT resources must use lifetime residency, while temporary
registrations must remain retained through their GPU completion sequence.

The external Metal 4 residency set follows D3DMetal's long-lived-set model with
one Wine-specific adaptation. Resource lifetime events journal individual
desired membership transitions under `DeviceResidencyMutex`; they never call
Metal from producer threads. `CommandQueue::CommitChunkInternal` is the sole
backend writer: it snapshots the journal, issues one synchronous
`addAllocation:` or `removeAllocation:` Unix call per transition, and calls
`commit` once immediately before the Metal command-buffer commit. Bulk
`addAllocations:count:`/`removeAllocations:count:` calls and cross-boundary
allocation arrays are forbidden.

`DeviceResidencyLock` is a Clang scoped capability; logical membership and the
journal are `DXMT_GUARDED_BY`. The submission-owner capability is also a real
device-wide mutex because Direct, Copy, and Compute queues have independent
submission workers but consume the same residency set. Its scope covers only
the ordered journal snapshot, its native mutations, the residency-set commit,
and the immediately following Metal command-buffer commit. The backend owner
is always acquired first; the PE journal mutex is held inside it only long
enough to move the snapshot and is released before any Metal call. No path
acquires those locks in reverse order, and no thunk-side lock is introduced.
Allocations removed from the backend set are transferred to the submitted
command chunk and released only after that command buffer completes. This
provides a concrete feedback-completion retirement boundary without a
residency mailbox or thunk-side lock.

Builder restricts individual add/remove/commit/request operations to
`DeviceResidency::ApplyForSubmission`, rejects calls outside the submission
owner, rejects bulk residency selectors and delta ABIs, and requires retired
allocation ownership to remain attached to the command chunk.

### Queue decomposition and module isolation

`CommandQueueImpl` is split across `src/d3d12/d3d12_command_queue_*.inc`
domain fragments that are `#include`d inside the class body. They are not
translation units and are not listed in `meson.build`; Builder rejects a
fragment that no longer appears in an `#include` directive of
`d3d12_command_queue.cpp`, so a fragment cannot silently fall out of the
build. Architecture rules that need to see the whole class concatenate the
queue source with every fragment before scanning.

Value-domain modules extracted out of the queue (texture view, texture
swizzle, shader binding, argument buffer layout, pipeline write policy,
resource barrier batch, tile mapping, temporal scaler, queue configuration,
and the diagnostics leaves) are isolated by the build graph rather than by
banned spellings. `CommandQueueImpl` and its nested replay types live in the
anonymous namespace of `d3d12_command_queue.cpp` and its fragments, so an
independent translation unit cannot name them at all — a literal ban on
`CommandQueueImpl` or `ReplayState` would restate what the language already
guarantees, and it silently stops holding the moment code moves into a new
file. The only remaining failure mode is a module pulling the queue back into
its own include closure, so that is the single check Builder keeps.

These modules are stateless free-function domains: they own no mutable state,
take no locks, and register no asynchronous callbacks. Ownership, locking, and
capture discipline are enforced where they can actually be violated — the
submission model, under `.clang-tidy-model` and the closed-model contracts
above — not by scanning value-domain sources for spellings.

Two checks were evaluated against the whole tree and deliberately removed
rather than baselined. `bugprone-multi-level-implicit-pointer-conversion` fired
16 times, all on the WoW64 thunk layer's `UInt32ToPtr` helper; the
`sm50_ptr64_t` typedef makes those handles 8 bytes on both sides and
`COMPATIBLE_STRUCT32` asserts the layouts at compile time, so the check
restates a guarantee the code already proves, and its count grows with every
new thunk. `concurrency-mt-unsafe` fired 30 times, all `getenv`/`localtime`; it
keys on function names rather than on whether a concurrent writer exists, and
it did not report the one genuine data race in that file (unsynchronised
`static bool initialized` lazy init). Thread-safety attributes and TSan cover
that ground far better.

Existing clang-tidy findings are recorded in
`tools/audit/clang-tidy-baseline.json`. Update it only after reviewing every
changed diagnostic:

```sh
scripts/dxmt-builder audit --scope dx12-metal4 --update-baseline \
  --baseline-reason "reviewed legacy diagnostics"
```

`--deep` uses its own baseline. Compiler errors and architecture-policy errors
are never accepted into either baseline.

The deep set includes the command queue, sequencer, retirement, fence,
allocator, resource, and Metal4 adapter paths. Legacy monolithic translation
units run serially because the path-sensitive analyzer can take several
minutes and substantial memory. A timeout, checker crash, missing result, or
planned/actual TU coverage difference is an audit failure. Once a path moves
into the independent submission model, it may not be removed from deep
analysis for performance reasons.

## Git hooks

Enable the tracked DXMT hooks once per clone:

```sh
scripts/setup-git-hooks.sh
```

The `commit-msg` hook validates new commits immediately. The `pre-push` hook
validates every commit included in the outgoing ref updates, so commits created
through amend, rebase, cherry-pick, or a bypassed commit hook are checked before
they reach the remote. Subjects must use `type(scope): subject`, for example
`fix(builder): validate commit messages before push`. Both `type` and `scope`
must be selected from the reviewed repository keywords in
`scripts/conventional-commit-keywords.sh`; arbitrary keywords, compound scopes,
and spelling or case variants are rejected. Additions to the vocabulary must be
reviewed in that file before they can be used in a commit.

The type describes the kind of change, while the scope names the affected
module or feature domain. Do not repeat a type as a scope: use `test(d3d11)`
rather than `test(test)`. CI is a repository subsystem, so use `chore(ci)`,
`perf(ci)`, or `fix(ci)` rather than using `ci` as the type.

Push CI classifies every commit in the pushed range by its reviewed type and
uses the union of their routes:

- `test(...)` invokes the reusable native Windows D3D conformance workflow.
- Runtime-affecting types (`feat`, `fix`, `refactor`, `perf`, `build`, `revert`,
  and `merge`) invoke the Nightly build, except for repository-only scopes
  (`ci`, `feishu`, `git`, and `repo`).
- `docs`, `style`, `chore`, and test-only pushes do not invoke Nightly.

The push workflow also validates every subject with the same
`scripts/check-commit-message.sh` policy used by `commit-msg` and `pre-push`.
This remote check is authoritative even when a contributor bypasses local hooks
or force-pushes. Any invalid type, scope, or subject format fails commit routing
and prevents downstream build and test jobs from starting.

A mixed push can therefore invoke both workflows. Pushes do not emit immediate
event cards. After all selected jobs finish, one aggregated Feishu result is
sent. This notification is mandatory for every push: when no substantive CI
job is selected, it still lists every pushed commit and reports both Nightly
and Windows D3D as not triggered. Manual dispatch of the push workflow always
invokes Nightly. A standalone manual Windows D3D run sends its own final result
because it has no parent push workflow to aggregate it.

## Setup LLVM

First of all, you need to build LLVM (or use a pre-built LLVM package) no matter if you want to use DXMT with Wine or as a native library. DXMT currently standardizes Wine, test helpers, native tools, and native libraries on `x86_64`; the supported Windows targets are x86 and x86_64.

This is an example of building `x86_64` LLVM
```sh
# in root directory of the project
mkdir -p ./.cache/toolchains/llvm-darwin-local-build
git clone --depth 1 --branch llvmorg-15.0.7 https://github.com/llvm/llvm-project.git .cache/toolchains/llvm-project

cmake -B ./.cache/toolchains/llvm-darwin-local-build -S ./.cache/toolchains/llvm-project/llvm \
  -DCMAKE_INSTALL_PREFIX="$(pwd)/.cache/toolchains/llvm-darwin-local" \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DLLVM_HOST_TRIPLE=x86_64-apple-darwin \
  -DLLVM_ENABLE_ASSERTIONS=On \
  -DLLVM_ENABLE_ZSTD=Off \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD="" \
  -DLLVM_BUILD_TOOLS=Off \
  -DBUG_REPORT_URL="https://github.com/3Shain/dxmt" \
  -DPACKAGE_VENDOR="DXMT" \
  -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off \
  -G Ninja
cmake --build ./.cache/toolchains/llvm-darwin-local-build
cmake --install ./.cache/toolchains/llvm-darwin-local-build
```

Then `./.cache/toolchains/llvm-darwin-local` contains what we need later.

## Setup Wine

> Skip this part if you are NOT doing a **Cross Build**

See [winehq.org - MacOS Building](https://gitlab.winehq.org/wine/wine/-/wikis/MacOS-Building)

Wine is built as a **64-bit host with WoW64** for 32-bit Windows apps. DXMT
does **not** ship native i386 PE DLLs; the product path is x86_64 PE + x86_64
unixlib. Wine itself may still enable i386 modules for WoW64 loading.

For managed CI and `scripts/dxmt-builder` cross builds, Wine is prepared with:

```sh
scripts/dxmt-builder bootstrap wine-x64
```

That pulls the configured remote Wine branch (see `WINE_GIT_URL` /
`WINE_GIT_BRANCH` / `WINE_ACCESS_TOKEN`), incrementally rebuilds when possible,
and publishes an install tree under `.cache/managed/deps/`. DXMT `build` then
links against that tree (`ResolveWine`); it does not recompile Wine.

The `external/wine` submodule remains available for local meson workflows. The
default Wine configure arguments for that path are defined by
`wine_configure_args` in [meson.options](/meson.options).

## Build DXMT

### Cross Build

Build (64-bit) Windows PE+ dlls **and** 64-bit unixlib (a Mach-O library with `.so` extension by convention).

```sh
scripts/dxmt-builder build --profile gcc-x64-release-full runtime
```

Only x86_64 Windows PE profiles are supported (`gcc-x64-release-full`,
`llvm-mingw-x64-*`). Native i386 DXMT builds are not part of the product.

There are other compile options in [meson.options](/meson.options).

#### apitrace integration

DXMT can link the apitrace recorder directly for development traces. The
integration is gated by a dedicated boolean switch and is independent of the
Meson build type. Pass `-Dapitrace_builtin=true` at configure time to compile
the recorder into the produced DLLs; leave the default `false` for normal
builds. When the flag is off, no apitrace source files participate in the
compile and no apitrace symbols land in `d3d11.dll`, `d3d12.dll`, `dxgi.dll`,
or `winemetal.so`.

The default source checkout is the `external/apitrace` submodule. When enabled,
Meson builds only the required static apitrace recorder libraries into the DXMT
build directory through `scripts/build-apitrace.sh`; it does not build apitrace
Windows DLLs or tools.

At runtime, the integration is opt-in through environment variables:

- `DXMT_APITRACE_ENABLED=1` — activate the in-process recorder. The historical
  misspelling `DXMT_APITRACE_ENBALED=1` is accepted as a spelling-compatible
  alias and is checked first; new scripts should prefer the corrected name.
- `APITRACE_TRACE_BUNDLE=/abs/path/to/trace.apitrace` — concrete output bundle
  for the current process. The PE side propagates this bundle root to the unix
  side via `__wine_set_unix_env`, so the D3D and Metal streams for the process
  stay together. When unset, the PE side picks
  `<exe-basename>_dxmt_apitrace/trace-<ts>.apitrace`.
- `APITRACE_METAL_VERBOSE=1` — emit per-call info logs from the DXMT-side hook
  layer. Useful for confirming the recorder is wired in; noisy in long runs.

When the recorder is active, every produced bundle directory contains three
co-located streams:

- `callstream.jsonl` — D3D11/D3D12/DXGI calls captured on the PE side. Hook
  insertion points are documented in the link-trace plan (§4.3) and cover
  device/swapchain creation, draw/dispatch, resource barriers, command list
  submission, present, map/unmap, and `UpdateSubresource`.
- `metal-callstream.jsonl` — Metal calls recorded by the unix side via the
  existing `winemetal_unix.c` hooks.
- `translation-links.jsonl` — `d3d_sequence ↔ metal_sequence_{begin,end}`
  spans emitted at command-buffer commit time. The `d3d_sequence` field is
  sourced from the apitrace D3D counter when `DXMT_APITRACE_D3D` is defined,
  and falls back to the dxmt internal chunk id otherwise.

The concrete bundle root is created exactly once by the PE side on the first
hook that calls `ensure_session_open()`; the unix side reads the normalized
`APITRACE_TRACE_BUNDLE`, so the three streams for that process are guaranteed
to live under the same directory.

`apitrace_builtin` is a developer-facing build flag and is intentionally absent
from `dxmt.conf`. The recorder is for in-tree debugging of the D3D↔Metal
translation path, not for end-user diagnostics.

#### `clangd` configuration for proper language server support 

Since this project contains code runs on macOS/Windows(Wine)/both, `clangd` may not always be able to find the correct sysroot for e.g. libc++ include files. Add clangd argument `--query-driver=**/x86_64-w64-mingw32-**` may solve this problem. 

### Native Build

Build all components as Mach-O .dylib

```sh
scripts/dxmt-builder build --profile apple-clang-x86_64-release runtime
```

#### Side notes on building x86_64 target from arm64 device/environment

Apparently the simplist solution is to use a x86_64 shell, but you can also set following environment variables

```sh
export CFLAGS='-arch x86_64'
export CXXFLAGS='-arch x86_64'
# then perform `meson setup ...`
```


# Debugging
The following environment variables can be used for **debugging** purposes.

## Validation layer (GPU hang forensics)

One switch for denser hang reports (Metal4 commit feedback classification +
fence/encoder dumps + IOGPU hooks when `DXMT_LOG_PATH` is set):

```sh
export DXMT_VALIDATION=1
export DXMT_LOG_PATH=/tmp/dxmt-logs   # required for native hang/IOGPU files
mkdir -p "$DXMT_LOG_PATH"
```

For real GPU-fault reproduction, keep `MTL_DEBUG_LAYER` and
`MTL_SHADER_VALIDATION` disabled unless explicitly running a validation A/B.
Apple's validation layers can assert on GPTK/D3DMetal-supported runtime
patterns and are not a correctness gate for the production path.

`DXMT_VALIDATION=1` (alias `DXMT_DIAG_VALIDATION=1`) expands PE-side defaults
when `d3d12.dll` loads:

- `DXMT_DIAG_GPU_HANG_DENSE=1` — full fence-edge + encoder dump on the hang CB
- `DXMT_DIAG_ERROR_SNAPSHOT=1` — retain the last successful Metal completion
  and dump it beside the first failing CB
- `DXMT_DIAG_ROOT_CAUSE_DENSE=1` — root-cause PE dumps
- `DXMT_DIAG_METAL_PSO_LABELS=1` — PSO labels map shader-validation hits
- `DXMT_DIAG_METAL_RESIDENCY=1` — residency diagnostics
- `DXMT_DIAG_DXMT_QUEUE=1` / `DXMT_DIAG_METAL4_QUEUE_MONITOR=1`
- best-effort `MTL_DEBUG_LAYER` / `MTL_SHADER_VALIDATION` if still unset

On GPU hang, look for:

1. stderr block `========== DXMT GPU HANG VALIDATION REPORT ==========`
   (ranked suspects: reverse-stage, residual waits, self-waits, concurrent CB,
   sparse, under-bound fences; all encoders; exact reverse edges with resource,
   allocation, Metal handle, GPU address/resource ID, range/view/access/stage)
2. `$DXMT_LOG_PATH/dxmt-metal4-native.log` — mirrored hang lines
3. `$DXMT_LOG_PATH/dxmt-iogpu-native.log` — IOGPU error code + fault address
4. Metal validation messages on stderr when `MTL_*` layers are on

**GPTK fence stage note:** D3DMetal `UpdateFence` uses `afterStages:17`
(Vertex|Mesh) for the PreRaster fence. DXMT keeps full `WMTRenderStagePreRaster`
(25, includes Object) for wait/update — forcing mask 17 caused early soft hangs
(~frame 12, D3D12 fence spin, no IOGPU error). Metal4 reverse Fragment→PreRaster
still splits the pass; deptrack re-establishes the Fragment→PreRaster wait.

**Note:** Apple Metal validation layers must be present in the **host Wine
process environment before Metal initializes**. Prefer setting them in
GameHub/wine-launch env, not only after PE `d3d12.dll` attach.

Also useful individually:

- `MTL_SHADER_VALIDATION=1` Enable Metal shader validation layer
- `MTL_DEBUG_LAYER=1` Enable Metal API validation layer
- `MTL_CAPTURE_ENABLED=1` Enable Metal frame capture
- `DXMT_DIAG_GPU_HANG_DENSE=1` Dense hang CB fence/encoder dump (without full validation pack)
- `DXMT_DIAG_ERROR_SNAPSHOT=1` Preserve and dump the final successful completion
  for direct comparison with the first failing command buffer
- `DXMT_DIAG_METAL_PSO_LABELS=1`: Labels Metal render/mesh PSOs with the stable DXMT pipeline key so shader-validation errors can be mapped back to pipeline dumps.
- `DXMT_DIAG_METAL_RESIDENCY=1`: Emits bounded drawable/layer residency diagnostics and labels otherwise unnamed drawable textures.
- `DXMT_CAPTURE_EXECUTABLE="the executable name without extension"` Must be set to enable Metal frame capture. Press F10 to generate a capture. The captured result will be stored in the same directory as the executable.
- `DXMT_CAPTURE_FRAME=n` Automatically capture n-th frame. Useful for debugging a replay.
- `DXMT_LOG_LEVEL=none|error|warn|info|debug` Controls message logging.
- `DXMT_LOG_PATH=/some/directory` Changes path where log files are stored. Set to `none` to disable log file creation entirely, without disabling logging.
- `DXMT_SHADER_CACHE=0`: Disables DXMT-managed shader cache files. The Metal framework PSO cache is still controlled by Metal itself.
- `DXMT_SHADER_CACHE_PATH=/some/absolute/darwin/directory`: Path to DXMT-managed shader cache files and the per-executable Metal framework cache directory. Defaults to `$(getconf DARWIN_USER_CACHE_DIR)/dxmt/<executable name with extension>_<executable path hash>`.
- `DXMT_USE_DEFAULT_METAL_CACHE=1`: Leaves Metal's framework cache path at the system default instead of placing it under `DXMT_SHADER_CACHE_PATH`.
- `DXMT_DUMP_PATH=/some/directory`: Changes path where shader and pipeline dump files are stored. Falls back to `DXMT_LOG_PATH` when unset.
- `DXMT_DUMP_PIPELINES=0|problem|all`: Controls pipeline dumping. `problem` dumps known problematic pipelines, currently including mesh pipelines with rasterization enabled and no pixel shader; `all` dumps every graphics pipeline.
- `DXMT_DUMP_PIPELINE_KEY=hex-key|field=value[ field=value...][,field=value...]`: Restricts pipeline dumping to one logged pipeline key or to fields from the dumped pipeline manifest. Plain hex keys match the current process key. Field filters match stable manifest tokens such as `VS.bytes=5200 PS.bytes=5328 inputs=5 rtv0=87`; comma-separated groups are ORed and tokens inside a group are ANDed.
- `DXMT_DUMP_PIPELINE_VS|HS|DS|GS|PS=sha1|null`: Restricts pipeline dumping by stable shader SHA1 for the selected stage. Multiple stage filters are combined, which is useful when a pipeline key changes between process launches.
- `DXMT_DUMP_COMPUTE_SHADERS=sha1-or-prefix[,sha1-or-prefix...]|all`: Dumps compute shader bytecode when a matching compute pipeline is requested. Prefix matching is supported so shader function names such as `cs_a4e4c668_...` can be used directly as filters.


### Logs
When used with Wine, DXMT will print log messages to `stderr`. Additionally, standalone log files can optionally be generated by setting the `DXMT_LOG_PATH` variable, where log files in the given directory will be called `app_d3d11.log`, `app_dxgi.log` etc., where `app` is the name of the game executable.
