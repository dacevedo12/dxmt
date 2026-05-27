project('dxmt', ['c', 'cpp'], version : 'v0.1', meson_version: '>=1.3.0', default_options : [ 
  'cpp_std=c++20', 
  'warning_level=2', 
  'build.cpp_std=c++20',
  'b_ndebug=if-release',
])

fs = import('fs')

dxmt_crossbuild = meson.is_cross_build()

cpp = meson.get_compiler('cpp')
cc = meson.get_compiler('c')
clang = meson.get_compiler('c', native: true)
clangpp = meson.get_compiler('cpp', native: true)

cpu_family = host_machine.cpu_family()

dxmt_include_dirs = [
  './include',
  './libs'
]

dxmt_native_include_dirs = [
  './include/native/windows',
  './include/native/directx',
]

if not dxmt_crossbuild
  dxmt_include_dirs += dxmt_native_include_dirs
endif

source_prefix_path = fs.relative_to(
    meson.project_source_root(),
    meson.project_build_root()
)

compiler_args = [
  '-Wimplicit-fallthrough',
  # gcc
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-cast-function-type', # Needed for GetProcAddress.
  # clang
  '-Wno-unused-private-field',
  '-Wno-microsoft-exception-spec',
  '-Wno-extern-c-compat',
  '-Wno-unused-const-variable',
  '-Wno-missing-braces',
  '-fblocks',
  '-fmacro-prefix-map=' + source_prefix_path + '/=',
]

if cpu_family == 'x86'
  compiler_args += [
    '-msse',
    '-msse2',
    '-msse3',
    '-mfpmath=sse',
  ]
endif

native_compiler_args = [
  '-Wimplicit-fallthrough',
  '-Wno-unused-parameter',
  '-Wno-unused-private-field',
  '-Wno-unused-const-variable',
  '-Wno-missing-braces',
  '-fblocks',
]

link_args = []

compiler_args += [
  '-DNOMINMAX',
  '-D_WIN32_WINNT=0xa00',
]

link_args += [
  '-static',
  # -static is sufficient for clang, while c runtime will be linked to api-ms-win-crt-* (ucrt)
  # '-static-libgcc',
  # '-static-libstdc++',
  # We need to set the section alignment for debug symbols to
  # work properly as well as avoiding a memcpy from the Wine loader.
  '-Wl,--file-alignment=4096',
]

# Wine's built-in back traces only work with dwarf4 symbols
if get_option('debug')
  compiler_args += [
    '-gdwarf-4',
  ]
endif

dxmt_include_path = include_directories(dxmt_include_dirs)
dxmt_native_include_path = include_directories(dxmt_native_include_dirs)

apitrace_source_path = get_option('apitrace_source_path')
if apitrace_source_path != '' and not apitrace_source_path.startswith('/')
  apitrace_source_path = join_paths(meson.project_source_root(), apitrace_source_path)
endif

apitrace_build_path = get_option('apitrace_build_path')
if apitrace_build_path != '' and not apitrace_build_path.startswith('/')
  apitrace_build_path = join_paths(meson.project_source_root(), apitrace_build_path)
endif
if apitrace_build_path == '' and apitrace_source_path != ''
  apitrace_build_path = join_paths(apitrace_source_path, 'build/cmake')
endif

dxmt_apitrace_metal_enabled = apitrace_source_path != ''
apitrace_metal_include_path = []
apitrace_metal_native_deps = []
if dxmt_apitrace_metal_enabled
  apitrace_metal_header = join_paths(apitrace_source_path, 'src/metal/include/apitrace/metal_capi.h')
  if not fs.is_file(apitrace_metal_header)
    error('apitrace metal header not found: ' + apitrace_metal_header)
  endif
  apitrace_metal_archive = join_paths(apitrace_build_path, 'libapitrace_core.a')
  if not fs.is_file(apitrace_metal_archive)
    error('apitrace core archive not found: ' + apitrace_metal_archive)
  endif
  apitrace_platform_archive = join_paths(apitrace_build_path, 'libapitrace_platform_apple_metal.a')
  if not fs.is_file(apitrace_platform_archive)
    error('apitrace metal platform archive not found: ' + apitrace_platform_archive)
  endif
  apitrace_metal_include_path = [include_directories(join_paths(apitrace_source_path, 'src/metal/include'))]
  apitrace_metal_native_deps = [
    clangpp.find_library('apitrace_platform_apple_metal', dirs : [apitrace_build_path], static : true),
    clangpp.find_library('apitrace_core', dirs : [apitrace_build_path], static : true),
  ]
endif

if not fs.is_file('include/native/directx/d3d11.h')
  error('Don\'t forget to clone submodules. `git submodule update --init --recursive`')
endif

if dxmt_crossbuild
  res_ext = '.o'
  wrc = find_program('windres')
  wrc_generator = generator(wrc,
    output    : [ '@BASENAME@' + res_ext ],
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@' ],
  )
else
  wrc           = find_program('touch')
  wrc_generator = generator(wrc, output : [ '@BASENAME@_ignored.h' ], arguments : [ '@OUTPUT@' ] )
endif

xcrun = find_program('xcrun')

metalir_generator = generator(xcrun,
  output    : [ '@BASENAME@.air'],
  arguments : [ '-sdk', 'macosx', 'metal', '-o', '@OUTPUT@', '-c', '@INPUT@', '@EXTRA_ARGS@'],
)

metallib_generator = generator(xcrun,
  output    : [ '@BASENAME@.metallib'],
  arguments : [ '-sdk', 'macosx', 'metallib', '-o', '@OUTPUT@', '@INPUT@'],
)

xxd = find_program('xxd')

hexdump_generator = generator(xxd,
  output    : [ '@BASENAME@.h'],
  arguments : [ '-n', '@BASENAME@', '-i', '@INPUT@', '@OUTPUT@'],
)

dxmt_debug = get_option('dxmt_debug')

if dxmt_debug
add_project_arguments('-DDXMT_DEBUG=1', language: 'cpp')
endif

add_project_arguments('-DDXMT_PAGE_SIZE=4096', language: 'cpp')

dxmt_version = vcs_tag(
  command: ['git', 'describe', '--always'],
  input:  'version.h.in',
  output: 'version.h',
)

if dxmt_crossbuild
  wine_builtin_dll = get_option('wine_builtin_dll')

  wine_source_path = get_option('wine_source_path')
  if wine_source_path != '' and not wine_source_path.startswith('/')
    wine_source_path = join_paths(meson.project_source_root(), wine_source_path)
  endif

  wine_build_path = get_option('wine_build_path')
  if wine_build_path != '' and not wine_build_path.startswith('/')
    wine_build_path = join_paths(meson.project_source_root(), wine_build_path)
  endif

  wine_install_path = get_option('wine_install_path')
  if wine_install_path != '' and not wine_install_path.startswith('/')
    wine_install_path = join_paths(meson.project_source_root(), wine_install_path)
  endif

  if cpu_family == 'x86'
    # Enable stdcall fixup on 32-bit
    link_args += [
      '-Wl,--enable-stdcall-fixup',
      '-Wl,--kill-at',
    ]
    # Fix stack alignment issues with mingw on 32-bit
    compiler_args += [
      '-mpreferred-stack-boundary=2'
    ]
    if wine_builtin_dll
      windows_native_install_dir = 'i386-windows'
    else
      windows_native_install_dir = 'syswow64'
    endif
    windows_builtin_install_dir = 'i386-windows'
  elif cpu_family == 'x86_64'
    if wine_builtin_dll
      windows_native_install_dir = 'x86_64-windows'
    else
      windows_native_install_dir = 'system32'
    endif
    windows_builtin_install_dir = 'x86_64-windows'
  elif cpu_family == 'aarch64'
    if wine_builtin_dll
      windows_native_install_dir = 'aarch64-windows'
    else
      windows_native_install_dir = 'system32'
    endif
    windows_builtin_install_dir = 'aarch64-windows'
  else
    error('invalid cpu family')
  endif

  unix_install_dir = cpu_family + '-unix'

  wine_build_target = []
  wine_build_artifacts = {}
  if wine_build_path != ''
    if wine_source_path == ''
      error('wine_source_path is required when wine_build_path is used')
    endif

    wine_build_artifacts = {
      'libwinecrt0': 'wine-libwinecrt0.a',
      'libntdll': 'wine-libntdll.a',
      'dbghelp': 'wine-dbghelp.dll',
      'winebuild': 'wine-winebuild',
      'winemac': 'wine-winemac.so',
      'ntdll_so': 'wine-ntdll.so',
    }

    wine_build_target = custom_target(
      'wine_build',
      output: [
        wine_build_artifacts['libwinecrt0'],
        wine_build_artifacts['libntdll'],
        wine_build_artifacts['dbghelp'],
        wine_build_artifacts['winebuild'],
        wine_build_artifacts['winemac'],
        wine_build_artifacts['ntdll_so'],
      ],
      command: [
        find_program('scripts/build-wine.sh'),
        wine_source_path,
        wine_build_path,
        windows_builtin_install_dir,
        unix_install_dir,
        '@OUTPUT0@',
        '@OUTPUT1@',
        '@OUTPUT2@',
        '@OUTPUT3@',
        '@OUTPUT4@',
        '@OUTPUT5@',
        get_option('wine_configure_args'),
      ],
      build_by_default: true,
    )
  endif
else
  wine_builtin_dll = false
  windows_native_install_dir = ''
  wine_source_path = ''
  wine_build_path = ''
  wine_install_path = ''
  wine_build_target = []
  wine_build_artifacts = {}
endif

native_compiler_arch = cpu_family == 'aarch64' ? 'arm64' : 'x86_64'

add_project_arguments(cpp.get_supported_arguments(compiler_args), language: 'cpp')
add_project_arguments(clangpp.get_supported_arguments(native_compiler_args), language: 'cpp', native: true)
add_project_arguments(cc.get_supported_arguments(compiler_args), language: 'c')
add_project_arguments(clang.get_supported_arguments(native_compiler_args), language: 'c', native: true)
add_project_link_arguments(cpp.get_supported_link_arguments(link_args), language: 'cpp')
add_project_link_arguments(cc.get_supported_link_arguments(link_args), language: 'c')
if dxmt_crossbuild
  add_project_arguments(['-arch', native_compiler_arch], language: 'c',  native: true)
  add_project_arguments(['-arch', native_compiler_arch], language: 'cpp',  native: true)
  add_project_link_arguments(['-arch', native_compiler_arch], language: 'cpp',  native: true)
endif

subdir('libs')
subdir('src')

if get_option('enable_tests')
subdir('tests')
endif
