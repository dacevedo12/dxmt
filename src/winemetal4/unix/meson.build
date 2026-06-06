winemetal4_unix_src = [
  'winemetal_unix.c',
  'metal4_cpp.cpp',
  'cache.c',
]

if wine_build_path != ''
  winemac_so = wine_build_target[4]
  ntdll_so = wine_build_target[5]
elif wine_install_path != ''
  winemac_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'winemac.so')
  ntdll_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'ntdll.so')
else
  error('One of the options -Dwine_build_path or -Dwine_install_path is required')
endif

winemetal4_unix_ld_args = [
  '-Wl,-install_name,@rpath/winemetal4.so',
  '-Wl,-exported_symbols_list,' + join_paths(meson.current_source_dir(), 'exports.list'),
  wine_build_path != '' ? winemac_so.full_path() : winemac_so,
  wine_build_path != '' ? ntdll_so.full_path() : ntdll_so,
  '-weak_framework', 'CoreFoundation', '-weak_framework', 'Metal',
  '-framework', 'MetalFX', '-weak_framework', 'Foundation',
  '-weak_framework', 'CoreGraphics', '-weak_framework', 'QuartzCore',
  '-weak_framework', 'ColorSync',  '-weak_framework', 'Cocoa',
  '-lsqlite3',
]
winemetal4_unix_link_depends = []
if wine_build_path != ''
  winemetal4_unix_link_depends += [winemac_so, ntdll_so]
endif
winemetal4_unix_link_depends += files('exports.list')
winemetal4_unix_link_depends += apitrace_metal_native_link_depends

winemetal4_unix_deps = [ airconv_dx12_metal4_dep_darwin ] + apitrace_metal_native_deps
winemetal4_unix_include_dirs = [
  dxmt_include_path,
  include_directories('../../../external/metal-cpp'),
] + apitrace_metal_include_path
winemetal4_unix_c_args = ['-ObjC']
if dxmt_apitrace_metal_enabled
  winemetal4_unix_c_args += ['-DDXMT_APITRACE_METAL=1']
endif

shared_library('winemetal4', winemetal4_unix_src,
  name_prefix         : '',
  dependencies        : winemetal4_unix_deps,
  include_directories : winemetal4_unix_include_dirs,
  c_args              : winemetal4_unix_c_args,
  cpp_args            : ['-fblocks'],
  link_args           : winemetal4_unix_ld_args,
  link_depends        : winemetal4_unix_link_depends,
  native              : true,
  name_suffix         : 'so',
  install_rpath       : '',
  build_rpath         : '@loader_path/:@loader_path/../../',
  override_options    : { 'b_asneeded' : false },
)

meson.add_install_script(
  'install.sh', get_option('strip').to_string(), unix_install_dir,
  install_tag: 'runtime'
)
