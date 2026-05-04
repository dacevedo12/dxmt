# Host-native deterministic unit tests for the pure D3D9 logic layer.
#
# Unlike tests/dx9/smokes (a single PE that needs a live device under
# wine + Metal), these are built with the BUILD-machine compiler and run
# directly under `meson test` — no wine, no GPU, no flake. They exercise
# the standalone conversion/validation helpers in src/d3d9 against
# expected values mined from Wine's d3d9 conformance suite (the de-facto
# D3D9 specification — Microsoft published no deterministic probes).
#
# Tagged into the 'host' suite so CI can run the whole wine-free tier
# with `meson test --suite host`.

dx9_unit_inc = include_directories('../../../src/d3d9')

# [ test name suffix, test source, src/d3d9 unit-under-test ]
dx9_unit_tests = [
  ['fvf-decl',                'test_fvf_decl.cpp',              'd3d9_fvf.cpp'],
  ['render-state-defaults',   'test_render_state_defaults.cpp', 'd3d9_state_defaults.cpp'],
  ['sampler-state-defaults',  'test_sampler_state_defaults.cpp', 'd3d9_state_defaults.cpp'],
  ['texture-stage-defaults',  'test_texture_stage_defaults.cpp', 'd3d9_state_defaults.cpp'],
  ['stream-source-freq',      'test_stream_source_freq.cpp',    'd3d9_validation.cpp'],
  ['vertex-decl-validation',  'test_vertex_decl_validation.cpp', 'd3d9_validation.cpp'],
]

foreach spec : dx9_unit_tests
  unit_exe = executable('dx9-unit-' + spec[0],
    [ spec[1], '../../../src/d3d9/' + spec[2] ],
    include_directories : [ dxmt_native_include_path, dx9_unit_inc ],
    native              : true,
    install             : false,
  )
  test('dx9-unit-' + spec[0], unit_exe, suite : 'host')
endforeach

# T2 — DXSO -> AIR shader-translation goldens. Drives the native airconv
# CLI (built for the build machine — wine-free) over a corpus of known
# D3D9 shaders. Only registered where the native airconv is built (the
# darwin subdir, included for x86_64 / aarch64 / native builds).
if is_variable('airconv_native_exe')
  test('dx9-airconv-golden', python3, args: [files('airconv_golden.py'), airconv_native_exe], suite: 'host')
endif
