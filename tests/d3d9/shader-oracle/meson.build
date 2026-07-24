# vkd3d's shader_runner as an in-tree numeric d3d9 oracle.
#
# Each corpus/*.shader_test draws a quad through the real IDirect3D9 API, reads
# the render target back and probes pixels with per-probe ulps tolerance, which
# is the numeric tier the airconv DXSO->AIR goldens cannot provide. The corpus
# here is the hand-assembled d3dbc-hex set (no HLSL compiler needed); the runner
# still compiles any HLSL variant blocks through the wine builtin d3dcompiler_47
# at device-run time.
#
# Shape mirrors tests/vkd3d-proton: the PE executable BUILDS as part of the
# normal cross build (so a PR cloner's CI produces it), a wine-free corpus lint
# joins the required 'host' suite, and the actual device run is a separate
# 'shader-oracle' suite that meson skips wherever no PE exe_wrapper (wine) is
# configured. The vendored sources are LGPL-2.1 (see LICENSE / README.md).

# The corpus integrity lint is host-native and needs neither wine nor a GPU, so
# it runs on every configuration, exactly like the tree's other host python lints.
test('d3d9-shader-oracle-corpus-lint', python3_native,
  args: [ files('corpus_lint.py') ],
  suite: 'host')

# The runner is a Windows/PE binary: it uses d3d9.h and the mingw SDK D3D
# headers and links the d3dcompiler_47 / user32 / gdi32 import libraries. It can
# only be produced by the cross toolchain, so gate the build on a cross build
# the same way the rest of the PE test surface is reached.
if not dxmt_crossbuild
  subdir_done()
endif

# widl generates the IDxcCompiler3 interface header + GUIDs from dxcompiler.idl,
# the same program tests/vkd3d-proton uses for its d3d12 idl surface. utils.h
# pulls in dxcompiler.h for the type; the d3d9 executor never instantiates DXC
# (SM6 is out of range), it only dlopen()s dxcompiler.dll if present.
shader_oracle_widl = find_program('widl', required: false)
if not shader_oracle_widl.found()
  shader_oracle_widl = find_program('widl-stable', 'x86_64-w64-mingw32-widl', required: false)
endif
if not shader_oracle_widl.found()
  # No widl means the interface header cannot be produced; skip the runner build
  # rather than failing configure. The corpus lint above still guards the corpus.
  warning('widl not found; skipping the d3d9 shader-oracle runner build')
  subdir_done()
endif

shader_oracle_idl_inc = [
  meson.current_source_dir() / 'runner' / 'idl',
  meson.current_source_dir() / 'runner' / 'include',
]
shader_oracle_widl_args = []
foreach inc : shader_oracle_idl_inc
  shader_oracle_widl_args += [ '-I', inc ]
endforeach

dxcompiler_header = custom_target('dxcompiler.h',
  input: 'runner/idl/dxcompiler.idl',
  output: 'dxcompiler.h',
  command: [ shader_oracle_widl, shader_oracle_widl_args, '-h', '-o', '@OUTPUT@', '@INPUT@' ])

dxcompiler_ids = custom_target('dxcompiler_i.c',
  input: 'runner/idl/dxcompiler.idl',
  output: 'dxcompiler_i.c',
  command: [ shader_oracle_widl, shader_oracle_widl_args, '-u', '-o', '@OUTPUT@', '@INPUT@' ])

# runner/local is searched first so its config.h and the vkd3d_d3dcommon.h /
# vkd3d_d3d12.h / vkd3d_dxgiformat.h redirects win over the (absent) generated
# vkd3d headers; then the vendored include tree and the runner sources.
shader_oracle_inc = include_directories(
  'runner/local',
  'runner/include',
  'runner/include/private',
  'runner',
  '.',  # for the generated dxcompiler.h
)

# -municode selects wmainCRTStartup so vkd3d_test.h's wmain wrapper is the entry
# point (it widens argv and calls the shared main), matching vkd3d's own build.
# The dxmt tree defines NOMINMAX project-wide (to keep the windows.h min()/max()
# macros out of the C++ frontend); vkd3d's shader_runner instead relies on those
# macros, which vkd3d_windows.h emits under `#ifndef NOMINMAX`. Undefine it for
# just these vendored translation units so vkd3d's own min()/max() are used
# verbatim, rather than inventing a divergent definition.
shader_oracle_c_args = cc.get_supported_arguments([
  '-UNOMINMAX',
  '-municode',
  '-DVKD3D_CROSSTEST=1',
  # vkd3d targets Vista; the dxmt tree defaults to 0xa00. Undefine first so the
  # vkd3d value applies without a -Wmacro-redefined diagnostic.
  '-U_WIN32_WINNT',
  '-D_WIN32_WINNT=0x0600',
  '-D__USE_MINGW_ANSI_STDIO=0',
  '-Wno-format',
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
  '-Wno-unused-function',
  '-Wno-sign-compare',
  '-Wno-pointer-sign',
  '-Wno-declaration-after-statement',
  # The vendored sources are kept byte-identical to upstream vkd3d, which does
  # not build with -Wimplicit-fallthrough; suppress it here rather than editing
  # the fall-through in the shared shader_runner_d3d9.c switch.
  '-Wno-implicit-fallthrough',
])

shader_oracle_runner = executable('d3d9-shader-oracle-runner',
  'runner/shader_runner.c',
  'runner/shader_runner_d3d9.c',
  'runner/local/d3d9_only_stubs.c',
  dxcompiler_header,
  dxcompiler_ids,
  include_directories: shader_oracle_inc,
  c_args: shader_oracle_c_args,
  link_args: [ '-municode' ],
  dependencies: [
    cc.find_library('d3dcompiler_47'),
    cc.find_library('user32'),
    cc.find_library('gdi32'),
  ],
  install: false)

# One test per corpus file. These need a live Metal device plus a wine
# exe_wrapper to launch the PE, so they are NOT in the 'host' suite; on a
# configuration without an exe_wrapper meson reports them as skipped. On a
# device runner (wine as exe_wrapper, dxmt d3d9.dll on WINEDLLPATH) they run the
# real oracle. See README.md for the standalone driver the maintainers use.
foreach case : [
  'dp2add', 'loop', 'lrp', 'mnxn', 'nrm', 'pow', 'rep', 'texdepth', 'texreg',
]
  test('d3d9-shader-oracle-' + case, shader_oracle_runner,
    args: [ files('corpus' / (case + '.shader_test')) ],
    suite: 'shader-oracle')
endforeach
