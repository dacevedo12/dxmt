d3d12_res = wrc_generator.process('version.rc')

d3d12_src = [
  'd3d12.cpp',
  'd3d12_device.cpp',
  'd3d12_command_queue.cpp',
  'd3d12_command_allocator.cpp',
  'd3d12_command_list.cpp',
  'd3d12_resource.cpp',
  'd3d12_heap.cpp',
  'd3d12_descriptor_heap.cpp',
  'd3d12_root_signature.cpp',
  'd3d12_pipeline.cpp',
  'd3d12_fence.cpp',
  'd3d12_query.cpp',
  'd3d12_frame_interop.cpp',
]

d3d12core_src = [
  'd3d12core.cpp',
]

d3d12_ld_args      = ['-L' + windows_native_install_dir]
d3d12_link_depends = []

d3d12_dll = shared_library('d3d12', d3d12_src, d3d12_res,
  name_prefix         : '',
  dependencies        : [
    dxgi_dep,
    dxil_parser_dep,
    airconv_forward_dep,
    dxmt_dep,
    winemetal_dep,
    util_dep ],
  include_directories : [ dxmt_include_path ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'd3d12.def',
  link_args           : d3d12_ld_args,
  link_depends        : [ d3d12_link_depends ],
)

d3d12core_dll = shared_library('d3d12core', d3d12core_src,
  name_prefix         : '',
  dependencies        : [
    dxgi_dep,
    util_dep ],
  include_directories : [ dxmt_include_path ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'd3d12core.def',
  link_args           : d3d12_ld_args,
  link_depends        : [ d3d12_link_depends ],
)

d3d12_dep = declare_dependency(
  link_with           : [ d3d12_dll ],
  include_directories : [ dxmt_include_path ],
)

if wine_builtin_dll
custom_target('postprocess_libd3d12',
  input : d3d12_dll,
  output: 'd3d12.dll.postproc',
  command : [ wine_build_path != '' ? winebuild.full_path() : winebuild, '--builtin', '@INPUT@' ],
  depends : d3d12_dll,
  build_by_default : true
)
endif

if wine_builtin_dll
custom_target('postprocess_libd3d12core',
  input : d3d12core_dll,
  output: 'd3d12core.dll.postproc',
  command : [ wine_build_path != '' ? winebuild.full_path() : winebuild, '--builtin', '@INPUT@' ],
  depends : d3d12core_dll,
  build_by_default : true
)
endif
