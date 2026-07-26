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

if cpu_family != 'x86' and cpu_family != 'x86_64'
  error('DXMT currently supports only x86 and x86_64 targets')
endif

if get_option('windows_oracle_only')
  if host_machine.system() != 'windows' or dxmt_crossbuild
    error('windows_oracle_only requires a native Windows build')
  endif
  subdir('external/googletest')
  subdir('tests')
  subdir_done()
endif

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
  # Lock contracts (DXMT_CAPABILITY / DXMT_GUARDED_BY / DXMT_REQUIRES and the
  # residency lock ordering) are only meaningful if the compiler checks them on
  # every build.  GCC ignores these entirely, so get_supported_arguments()
  # drops them there; on clang they turn the annotations into real gates.
  # -beta is required for member lock *ordering* -- without it acquired_before
  # /_after silently degrade into comments.
  '-Wthread-safety',
  '-Wthread-safety-beta',
  # The codebase is clean under these today, so a violation is always a new
  # defect -- fail the build rather than emit a warning that scrolls past.
  # Promote the whole group, not just -analysis: handing a guarded member to a
  # helper by reference without the lock is reported under
  # -Wthread-safety-reference, which -Werror=thread-safety-analysis leaves as a
  # warning that exits zero.  There is no global -Werror to catch it after that.
  '-Werror=thread-safety',
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
  # winemetal4/unix carries real DXMT_CAPABILITY / DXMT_REQUIRES contracts on
  # the residency locks, and it only ever builds as a native target.  Without
  # these the annotations are dead at compile time and are checked only by
  # `audit --deep`, which does not run on every build.
  '-Wthread-safety',
  '-Wthread-safety-beta',
  '-Werror=thread-safety',
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

dxmt_apitrace_metal_enabled = get_option('apitrace_builtin')

if dxmt_apitrace_metal_enabled
  add_project_arguments('-DDXMT_APITRACE_ENABLED=1', language: 'cpp')
endif

apitrace_source_path = ''
apitrace_build_path = ''
apitrace_metal_include_path = []
apitrace_metal_native_deps = []
apitrace_metal_native_link_depends = []
apitrace_d3d_pe_deps = []
apitrace_d3d_pe_link_depends = []
if dxmt_apitrace_metal_enabled
  apitrace_source_path = get_option('apitrace_source_path')
  if apitrace_source_path == ''
    error('apitrace_source_path is required when apitrace integration is enabled')
  endif
  if not apitrace_source_path.startswith('/')
    apitrace_source_path = join_paths(meson.project_source_root(), apitrace_source_path)
  endif

  apitrace_build_path = get_option('apitrace_build_path')
  if apitrace_build_path == ''
    apitrace_build_path = join_paths(meson.project_build_root(), 'subprojects/apitrace')
  elif not apitrace_build_path.startswith('/')
    apitrace_build_path = join_paths(meson.project_source_root(), apitrace_build_path)
  endif

  apitrace_metal_header = join_paths(apitrace_source_path, 'src/metal/include/apitrace/metal_capi.h')
  if not fs.is_file(apitrace_metal_header)
    error('apitrace metal header not found: ' + apitrace_metal_header)
  endif

  apitrace_build_stamp = custom_target(
    'apitrace_metal_build',
    output: 'apitrace-metal.stamp',
    command: [
      find_program('scripts/build-apitrace.sh'),
      apitrace_source_path,
      apitrace_build_path,
      '@OUTPUT@',
      'native',
    ],
    build_by_default: true,
    build_always_stale: true,
  )
  apitrace_metal_archive = join_paths(apitrace_build_path, 'libapitrace_core.a')
  apitrace_platform_archive = join_paths(apitrace_build_path, 'libapitrace_platform_apple_metal.a')
  apitrace_metal_native_link_depends = [
    apitrace_build_stamp,
    apitrace_platform_archive,
    apitrace_metal_archive,
  ]
  apitrace_metal_include_path = []
  apitrace_metal_native_deps = [
    declare_dependency(
      compile_args : ['-I' + join_paths(apitrace_source_path, 'src/metal/include')],
      link_args : [apitrace_platform_archive, apitrace_metal_archive],
      sources : [apitrace_build_stamp],
    ),
  ]

  if dxmt_crossbuild
    apitrace_pe_build_path = join_paths(meson.project_build_root(), 'subprojects/apitrace-pe')
    apitrace_pe_toolchain = join_paths(meson.project_source_root(), 'scripts/apitrace-mingw-toolchain.cmake')
    apitrace_pe_build_stamp = custom_target(
      'apitrace_pe_build',
      output: 'apitrace-pe.stamp',
      command: [
        find_program('scripts/build-apitrace.sh'),
        apitrace_source_path,
        apitrace_pe_build_path,
        '@OUTPUT@',
        'mingw',
        apitrace_pe_toolchain,
      ],
      build_by_default: true,
      build_always_stale: true,
    )
    apitrace_pe_core_archive = join_paths(apitrace_pe_build_path, 'libapitrace_core.a')
    apitrace_pe_d3d11_archive = join_paths(apitrace_pe_build_path, 'libapitrace_d3d11_capture.a')
    apitrace_pe_d3d12_archive = join_paths(apitrace_pe_build_path, 'libapitrace_d3d12_capture.a')
    apitrace_d3d_pe_link_depends = [
      apitrace_pe_build_stamp,
      apitrace_pe_core_archive,
      apitrace_pe_d3d12_archive,
      apitrace_pe_d3d11_archive,
    ]
    apitrace_d3d_pe_deps = [
      declare_dependency(
        compile_args : [
          '-DDXMT_APITRACE_D3D=1',
          '-I' + join_paths(apitrace_source_path, 'src/api/include'),
          '-I' + join_paths(apitrace_source_path, 'src/trace/include'),
          '-I' + join_paths(apitrace_source_path, 'src/capture/include'),
          '-I' + join_paths(apitrace_source_path, 'src/retrace/include'),
          '-I' + join_paths(apitrace_source_path, 'src/d3d11/include'),
          '-I' + join_paths(apitrace_source_path, 'src/d3d12/include'),
        ],
        link_args : [
          apitrace_pe_core_archive,
          apitrace_pe_d3d12_archive,
          apitrace_pe_d3d11_archive,
          '-lwinpthread',
        ],
        sources : [apitrace_pe_build_stamp],
      ),
    ]
  endif
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
xxd = find_program('xxd')
dxmt_builder_path = get_option('dxmt_builder_path')
dxmt_builder_config_path = get_option('dxmt_builder_config_path')
dxmt_cache_root = get_option('dxmt_cache_root')

if dxmt_builder_path != ''
  dxmt_builder = find_program(dxmt_builder_path)
  dxmt_builder_arguments = []
  if dxmt_builder_config_path != ''
    dxmt_builder_arguments += [ '--config', dxmt_builder_config_path ]
  endif
  metalir_generator = generator(dxmt_builder,
    output: ['@BASENAME@.air'],
    arguments: dxmt_builder_arguments + [
      'internal', 'cache-command', '--namespace', 'metal',
      '--output', '@OUTPUT@', '--input', '@INPUT@', '--',
      xcrun.full_path(), '-sdk', 'macosx', 'metal', '-o', '@OUTPUT@', '-c', '@INPUT@', '@EXTRA_ARGS@',
    ],
  )
  metallib_generator = generator(dxmt_builder,
    output: ['@BASENAME@.metallib'],
    arguments: dxmt_builder_arguments + [
      'internal', 'cache-command', '--namespace', 'metal',
      '--output', '@OUTPUT@', '--input', '@INPUT@', '--',
      xcrun.full_path(), '-sdk', 'macosx', 'metallib', '-o', '@OUTPUT@', '@INPUT@',
    ],
  )
  hexdump_generator = generator(dxmt_builder,
    output: ['@BASENAME@.h'],
    arguments: dxmt_builder_arguments + [
      'internal', 'cache-command', '--namespace', 'metal',
      '--output', '@OUTPUT@', '--input', '@INPUT@', '--',
      xxd.full_path(), '-n', '@BASENAME@', '-i', '@INPUT@', '@OUTPUT@',
    ],
  )
else
  metalir_generator = generator(xcrun,
    output    : [ '@BASENAME@.air'],
    arguments : [ '-sdk', 'macosx', 'metal', '-o', '@OUTPUT@', '-c', '@INPUT@', '@EXTRA_ARGS@'],
  )
  metallib_generator = generator(xcrun,
    output    : [ '@BASENAME@.metallib'],
    arguments : [ '-sdk', 'macosx', 'metallib', '-o', '@OUTPUT@', '@INPUT@'],
  )
  hexdump_generator = generator(xxd,
    output    : [ '@BASENAME@.h'],
    arguments : [ '-n', '@BASENAME@', '-i', '@INPUT@', '@OUTPUT@'],
  )
endif

dxmt_debug = get_option('dxmt_debug')

if dxmt_debug
add_project_arguments('-DDXMT_DEBUG=1', language: 'cpp')
endif

add_project_arguments('-DDXMT_PAGE_SIZE=4096', language: 'cpp')

wine_runtime_root = ''
wine_runtime_manifest = []
wine_test_env = environment()
wine_runtime_required = dxmt_crossbuild and (
  get_option('enable_tests') or dxmt_apitrace_metal_enabled
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
    if wine_runtime_required
      foreach packaging_tool : [
        'scripts/pack_runtime_deps.sh',
        'scripts/relocate_wine_runtime.sh',
      ]
        if not fs.is_file(join_paths(wine_source_path, packaging_tool))
          error('Managed Wine runtime cache requires dependency packaging tool: ' + packaging_tool)
        endif
      endforeach
    endif

    wine_build_artifacts = {
      'libwinecrt0': 'wine-libwinecrt0.a',
      'libntdll': 'wine-libntdll.a',
      'dbghelp': 'wine-dbghelp.dll',
      'winebuild': 'wine-winebuild',
      'winemac': 'wine-winemac.so',
      'ntdll_so': 'wine-ntdll.so',
      'runtime_manifest': 'wine-runtime-cache.properties',
    }
    wine_runtime_root = join_paths(wine_build_path, 'dxmt-install')

    wine_build_target = custom_target(
      'wine_build',
      output: [
        wine_build_artifacts['libwinecrt0'],
        wine_build_artifacts['libntdll'],
        wine_build_artifacts['dbghelp'],
        wine_build_artifacts['winebuild'],
        wine_build_artifacts['winemac'],
        wine_build_artifacts['ntdll_so'],
        wine_build_artifacts['runtime_manifest'],
      ],
      command: [
        find_program('scripts/build-wine.sh'),
        wine_source_path,
        wine_build_path,
        wine_runtime_root,
        wine_runtime_required.to_string(),
        windows_builtin_install_dir,
        unix_install_dir,
        '@OUTPUT0@',
        '@OUTPUT1@',
        '@OUTPUT2@',
        '@OUTPUT3@',
        '@OUTPUT4@',
        '@OUTPUT5@',
        '@OUTPUT6@',
        get_option('wine_configure_args'),
      ],
      build_by_default: true,
      build_always_stale: true,
    )
    wine_runtime_manifest = wine_build_target[6]
  elif wine_install_path != ''
    wine_runtime_root = wine_install_path
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

if wine_runtime_root != ''
  wine_test_env.set('DXMT_TEST_WINE_ROOT', wine_runtime_root)
  wine_test_env.set('DXMT_TEST_REQUIRE_RUNTIME', '1')
  wine_test_env.set('DXMT_EXPERIMENT_DX12_SUPPORT', '1')
endif

if wine_runtime_required and wine_runtime_root == ''
  error('D3D tests and apitrace runtime validation require a complete Wine cache')
endif
if wine_runtime_required and wine_build_path == ''
  wine_launcher_found = fs.is_file(join_paths(wine_runtime_root, 'bin/wine')) or fs.is_file(join_paths(wine_runtime_root, 'bin/wine64'))
  wine_runtime_deps_found = true
  foreach runtime_dylib : [
    'libfreetype.6.dylib',
    'libgcrypt.20.dylib',
    'libgmp.10.dylib',
    'libgnutls.30.dylib',
    'libSDL2-2.0.0.dylib',
    'libMoltenVK.dylib',
  ]
    wine_runtime_deps_found = wine_runtime_deps_found and fs.is_file(join_paths(wine_runtime_root, 'lib', runtime_dylib))
  endforeach
  if not wine_launcher_found or not fs.is_file(join_paths(wine_runtime_root, 'bin/wineserver')) or not wine_runtime_deps_found
    error('wine_install_path must contain a runnable Wine development cache with bundled runtime dependencies')
  endif
endif

native_compiler_arch = 'x86_64'

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
  if not dxmt_crossbuild
    error('Tests require a Windows cross-build directory and run through Wine')
  endif

  # Prefer the managed builder as the single Wine launcher for tests/benchmarks.
  wine_test_launcher = find_program('scripts/dxmt-builder')
  subdir('external/googletest')
  subdir('tests')
  subdir('external/google-benchmark')
  subdir('benchmarks')
endif
