winemetal_unix_src = [
  'winemetal_unix.c',
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

winemetal_unix_ld_args = [
  '-Wl,-install_name,@rpath/winemetal.so',
  wine_build_path != '' ? winemac_so.full_path() : winemac_so,
  wine_build_path != '' ? ntdll_so.full_path() : ntdll_so,
  '-weak_framework', 'CoreFoundation', '-weak_framework', 'Metal',
  '-framework', 'MetalFX', '-weak_framework', 'Foundation',
  '-weak_framework', 'CoreGraphics', '-weak_framework', 'QuartzCore',
  '-weak_framework', 'ColorSync',  '-weak_framework', 'Cocoa',
  '-lsqlite3',
]
winemetal_unix_link_depends = []
if wine_build_path != ''
  winemetal_unix_link_depends += [winemac_so, ntdll_so]
endif

winemetal_unix_deps = [ airconv_dep_darwin ] + apitrace_metal_native_deps
winemetal_unix_include_dirs = [ dxmt_include_path ] + apitrace_metal_include_path
winemetal_unix_c_args = ['-ObjC']
if dxmt_apitrace_metal_enabled
  winemetal_unix_c_args += ['-DDXMT_APITRACE_METAL=1']
endif

shared_library('winemetal', winemetal_unix_src,
  name_prefix         : '',
  dependencies        : winemetal_unix_deps,
  include_directories : winemetal_unix_include_dirs,
  c_args              : winemetal_unix_c_args,
  link_args           : winemetal_unix_ld_args,
  link_depends        : winemetal_unix_link_depends,
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
