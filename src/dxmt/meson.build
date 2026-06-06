dxmt_src = [
  'dxmt_format.cpp',
  'dxmt_names.cpp',
  'dxmt_command_queue.cpp',
  'dxmt_command.cpp',
  'dxmt_capture.cpp',
  'dxmt_info.cpp',
  'dxmt_device.cpp',
  'dxmt_buffer.cpp',
  'dxmt_texture.cpp',
  'dxmt_context.cpp',
  'dxmt_dynamic.cpp',
  'dxmt_staging.cpp',
  'dxmt_hud_state.cpp',
  'dxmt_allocation.cpp',
  'dxmt_presenter.cpp',
  'dxmt_sampler.cpp',
  'dxmt_resource_initializer.cpp',
  'dxmt_shader_cache.cpp',
  'dxmt_scaler.cpp',
  'dxmt_subresource.cpp',
  'dxmt_deptrack.cpp',
]

if dxmt_apitrace_metal_enabled
  dxmt_src += ['dxmt_apitrace.cpp']
endif

if apitrace_d3d_pe_deps.length() > 0
  dxmt_src += ['dxmt_apitrace_d3d.cpp']
endif
dxmt_link_depends = apitrace_d3d_pe_link_depends

dxmt_shaders = [
  'dxmt_command.metal'
]

dxmt_shaders_ir = metalir_generator.process(dxmt_shaders)

dxmt_shaders_lib = metallib_generator.process(dxmt_shaders_ir)

dxmt_shaders_header = hexdump_generator.process(dxmt_shaders_lib)

dxmt_version_override = get_option('dxmt_version')
if dxmt_version_override == ''
  dxmt_version = vcs_tag(
    command: ['git', 'describe', '--always'],
    input:  '../../version.h.in',
    output: 'version.h',
  )
else
  dxmt_version_conf = configuration_data()
  dxmt_version_conf.set('VCS_TAG', dxmt_version_override)
  dxmt_version = configure_file(
    input: '../../version.h.in',
    output: 'version.h',
    configuration: dxmt_version_conf,
  )
endif

dxmt_lib = static_library('dxmt', dxmt_src, dxmt_shaders_header, dxmt_version,
  dependencies        : [ util_dep, airconv_forward_dep, winemetal_dep ] + apitrace_d3d_pe_deps,
  include_directories : [ dxmt_include_path ],
  link_depends        : dxmt_link_depends,
)

dxmt_dep = declare_dependency(
  link_with           : [ dxmt_lib ],
  include_directories : [ dxmt_include_path, include_directories('.') ],
  dependencies        : apitrace_d3d_pe_deps,
)

if dxmt_crossbuild
dxmt12_lib = static_library('dxmt12', dxmt_src, dxmt_shaders_header, dxmt_version,
  dependencies        : [ util_dep, airconv_dx12_metal4_forward_dep, winemetal4_dep ] + apitrace_d3d_pe_deps,
  include_directories : [ dxmt_include_path ],
  cpp_args            : ['-DDXMT_DX12_METAL4=1'],
  link_depends        : dxmt_link_depends,
)

dxmt12_dep = declare_dependency(
  link_with           : [ dxmt12_lib ],
  include_directories : [ dxmt_include_path, include_directories('.') ],
  dependencies        : apitrace_d3d_pe_deps,
)
else
dxmt12_dep = dxmt_dep
endif
