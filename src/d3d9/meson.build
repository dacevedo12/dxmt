d3d9_res = wrc_generator.process('version.rc')

d3d9_src = [
  'd3d9.cpp',
  'd3d9_buffer.cpp',
  'd3d9_clear_quad.cpp',
  'd3d9_cube_texture.cpp',
  'd3d9_device.cpp',
  'd3d9_format.cpp',
  'd3d9_fvf.cpp',
  'd3d9_interface.cpp',
  'd3d9_mem.cpp',
  'd3d9_query.cpp',
  'd3d9_shader.cpp',
  'd3d9_shader_scan.cpp',
  'd3d9_state_block.cpp',
  'd3d9_state_defaults.cpp',
  'd3d9_surface.cpp',
  'd3d9_swapchain.cpp',
  'd3d9_texture.cpp',
  'd3d9_validation.cpp',
  'd3d9_vertex_declaration.cpp',
  'd3d9_volume.cpp',
  'd3d9_volume_texture.cpp',
]

d3d9_ld_args      = ['-L' + windows_native_install_dir]
d3d9_link_depends = []

d3d9_dll = shared_library('d3d9', d3d9_src, d3d9_res,
  name_prefix         : '',
  dependencies        : [ util_dep, winemetal_dep, airconv_forward_dep, dxmt_dep ],
  include_directories : [ dxmt_include_path, include_directories('../dxmt') ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'd3d9.def',
  link_args           : d3d9_ld_args,
  link_depends        : [ d3d9_link_depends ],
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : [ dxmt_include_path ],
)

if wine_builtin_dll
custom_target('postprocess_libd3d9',
  input : d3d9_dll,
  output: 'd3d9.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : d3d9_dll,
  build_by_default : true
)
endif

