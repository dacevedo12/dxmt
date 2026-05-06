vkd3d_proton_root = meson.current_source_dir()

vkd3d_idl_compiler = find_program('widl', required: false)
if not vkd3d_idl_compiler.found()
  vkd3d_idl_compiler = find_program('widl-stable', 'x86_64-w64-mingw32-widl')
endif

vkd3d_idl_generator = generator(vkd3d_idl_compiler,
  output: [ '@BASENAME@.h' ],
  arguments: [ '-h', '-o', '@OUTPUT@', '@INPUT@' ])

vkd3d_idl = [
  'include/vkd3d_d3d12.idl',
  'include/vkd3d_d3d12sdklayers.idl',
  'include/vkd3d_d3dcommon.idl',
  'include/vkd3d_dxcapi.idl',
  'include/vkd3d_dxgi.idl',
  'include/vkd3d_dxgi1_2.idl',
  'include/vkd3d_dxgi1_3.idl',
  'include/vkd3d_dxgi1_4.idl',
  'include/vkd3d_dxgi1_5.idl',
  'include/vkd3d_dxgibase.idl',
  'include/vkd3d_dxgiformat.idl',
  'include/vkd3d_dxgitype.idl',
  'include/vkd3d_swapchain_factory.idl',
  'include/vkd3d_command_list_vkd3d_ext.idl',
  'include/vkd3d_command_queue_vkd3d_ext.idl',
  'include/vkd3d_device_vkd3d_ext.idl',
  'include/vkd3d_core_interface.idl',
]

vkd3d_generated_headers = []
foreach idl_file : vkd3d_idl
  vkd3d_generated_headers += vkd3d_idl_generator.process(idl_file)
endforeach

vkd3d_vulkan_include = []
if fs.is_dir('/usr/local/opt/vulkan-headers/include')
  vkd3d_vulkan_include += '/usr/local/opt/vulkan-headers/include'
elif fs.is_dir('/usr/local/Cellar/vulkan-headers/1.4.328.1/include')
  vkd3d_vulkan_include += '/usr/local/Cellar/vulkan-headers/1.4.328.1/include'
elif fs.is_dir('/opt/homebrew/opt/vulkan-headers/include')
  vkd3d_vulkan_include += '/opt/homebrew/opt/vulkan-headers/include'
endif

vkd3d_private_includes = include_directories(
  '.',
  'include',
  'include/private',
  'tests',
  vkd3d_vulkan_include,
)

vkd3d_common_src = [
  'libs/vkd3d-common/debug.c',
  'libs/vkd3d-common/memory.c',
  'libs/vkd3d-common/utf8.c',
  'libs/vkd3d-common/profiling.c',
  'libs/vkd3d-common/string.c',
  'libs/vkd3d-common/file_utils.c',
  'libs/vkd3d-common/platform.c',
]

vkd3d_test_c_args = cc.get_supported_arguments([
  '-UNOMINMAX',
  '-Wno-format',
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
  '-Wno-unused-function',
  '-Wno-sign-compare',
  '-Wno-pointer-sign',
  '-Wno-cast-function-type',
  '-Wno-declaration-after-statement',
  '-Wno-incompatible-pointer-types',
  '-Wno-microsoft-anon-tag',
  '-Wno-unknown-pragmas',
  '-Wno-attributes',
])

d3d12_common_lib = static_library('d3d12-common',
  vkd3d_common_src,
  vkd3d_generated_headers,
  dependencies: [ dependency('threads') ],
  include_directories: vkd3d_private_includes,
  c_args: vkd3d_test_c_args,
  install: false)

d3d12_common_dep = declare_dependency(
  link_with: d3d12_common_lib,
  dependencies: [ dependency('threads') ],
  include_directories: vkd3d_private_includes)

lib_d3d12 = cc.find_library('d3d12')
lib_dxgi = cc.find_library('dxgi')

d3d12_test_utils_lib = static_library('d3d12-test-utils',
  'tests/d3d12_test_utils.c',
  vkd3d_generated_headers,
  dependencies: [ lib_d3d12, lib_dxgi, d3d12_common_dep ],
  include_directories: vkd3d_private_includes,
  c_args: vkd3d_test_c_args,
  install: false)

d3d12_suite_src = [
  'tests/d3d12_dxvk_interop_device.c',
  'tests/d3d12_clip_cull_distance.c',
  'tests/d3d12_enhanced_barriers.c',
  'tests/d3d12_sampler_feedback.c',
  'tests/d3d12_geometry_shader.c',
  'tests/d3d12_win32_exclusive.c',
  'tests/d3d12_root_signature.c',
  'tests/d3d12_depth_stencil.c',
  'tests/d3d12_render_target.c',
  'tests/d3d12_tessellation.c',
  'tests/d3d12_sm_advanced.c',
  'tests/d3d12_mesh_shader.c',
  'tests/d3d12_descriptors.c',
  'tests/d3d12_raytracing.c',
  'tests/d3d12_robustness.c',
  'tests/d3d12_workgraphs.c',
  'tests/d3d12_streamout.c',
  'tests/d3d12_pso_blob.c',
  'tests/d3d12_resource.c',
  'tests/d3d12_bindless.c',
  'tests/d3d12_dstorage.c',
  'tests/d3d12_shaders.c',
  'tests/d3d12_command.c',
  'tests/d3d12_sparse.c',
  'tests/d3d12_device.c',
  'tests/d3d12_clear.c',
  'tests/d3d12_query.c',
  'tests/d3d12_sync.c',
  'tests/d3d12_copy.c',
  'tests/d3d12_vrs.c',
  'tests/d3d12_pso.c',
  'tests/d3d12_rov.c',
  'tests/d3d12_va.c',
  'tests/d3d12.c',
]

d3d12_suite_test = executable('d3d12-suite',
  d3d12_suite_src,
  vkd3d_generated_headers,
  dependencies: [ lib_d3d12, lib_dxgi, d3d12_common_dep ],
  include_directories: vkd3d_private_includes,
  c_args: vkd3d_test_c_args,
  link_with: [ d3d12_test_utils_lib ],
  install: false)

d3d12_descriptor_performance = executable('d3d12-descriptor-performance',
  'tests/descriptor_performance.c',
  vkd3d_generated_headers,
  dependencies: [ lib_d3d12, lib_dxgi, d3d12_common_dep ],
  include_directories: vkd3d_private_includes,
  c_args: vkd3d_test_c_args,
  link_with: [ d3d12_test_utils_lib ],
  install: false)

d3d12_pso_library_bloat = executable('d3d12-pso-library-bloat',
  'tests/pso_library_bloat.c',
  vkd3d_generated_headers,
  dependencies: [ lib_d3d12, lib_dxgi, d3d12_common_dep ],
  include_directories: vkd3d_private_includes,
  c_args: vkd3d_test_c_args,
  link_with: [ d3d12_test_utils_lib ],
  install: false)

d3d12_common_test = executable('d3d12-common-test',
  'tests/d3d12_common.c',
  'vkd3d_test_standalone.c',
  dependencies: [ d3d12_common_dep ],
  include_directories: vkd3d_private_includes,
  c_args: vkd3d_test_c_args,
  install: false)

d3d12_invalid_usage = executable('d3d12-invalid-usage',
  'tests/d3d12_invalid_usage.c',
  vkd3d_generated_headers,
  dependencies: [ lib_d3d12, lib_dxgi, d3d12_common_dep ],
  include_directories: vkd3d_private_includes,
  c_args: vkd3d_test_c_args,
  link_with: [ d3d12_test_utils_lib ],
  install: false)

d3d12_api_d3d = executable('d3d12-api-d3d',
  'tests/d3d12_api_d3d.c',
  vkd3d_generated_headers,
  dependencies: [ lib_d3d12, lib_dxgi, d3d12_common_dep ],
  include_directories: vkd3d_private_includes,
  c_args: vkd3d_test_c_args,
  link_with: [ d3d12_test_utils_lib ],
  install: false)

test('d3d12-suite-list', d3d12_suite_test, args: ['--list-tests'])
test('d3d12-common-test', d3d12_common_test)
