# Host-side Metal capability probes.
#
# These are not D3D behaviour tests: they never load a DXMT DLL, never enter
# Wine, and assert nothing about a Direct3D API surface.  They probe the macOS
# Metal stack the D3D12/Metal4 backend is being migrated onto, so they build as
# `native: true` executables next to the other native model specs and register
# into the `native` / `d3d12-model` Meson suites that Builder already drives.
#
# The MSL fixtures are compiled through the repository's Builder-cached
# `metal` / `metallib` / `xxd` generators and embedded into the binary, so the
# test never shells out to a compiler at run time.

# Offline-compiled kernels: .metal -> .air -> .metallib -> C array.
metal_probe_shaders = files('metal_descriptor_heap_probe.metal')
metal_probe_shaders_ir = metalir_generator.process(
  metal_probe_shaders,
  extra_args: ['-std=metal3.1', '--target=air64-apple-macos14.0'],
)
metal_probe_shaders_lib = metallib_generator.process(metal_probe_shaders_ir)
metal_probe_shaders_header = hexdump_generator.process(metal_probe_shaders_lib)

# Source-only fixtures for the runtime-compilation probe.  The negative one is
# expected to fail to compile, so it must never reach the offline chain above.
metal_probe_source_fixtures = hexdump_generator.process(files(
  'metal_handle_reinterpret_accepted.metal',
  'metal_handle_reinterpret_rejected.metal',
))

metal_capability_probe_test = executable(
  'dxmt-metal-capability-probe-tests',
  files('metal_capability_probe_spec.cpp'),
  metal_probe_shaders_header,
  metal_probe_source_fixtures,
  cpp_args: ['-ObjC++'],
  link_args: ['-framework', 'Metal', '-framework', 'Foundation'],
  native: true,
  install: false,
)

# `native` and `d3d12-model` are the suites Builder's native test lane runs;
# `metal-probe` is the narrow selector for local iteration.
test(
  'metal-capability-probe',
  metal_capability_probe_test,
  suite: ['unit', 'native', 'd3d12-model', 'metal-probe'],
  timeout: 180,
)
