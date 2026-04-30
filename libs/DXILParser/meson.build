dxil_parser_src = files([
  'DXILParser.cpp',
])

dxil_llvm_deps = [
  '-lLLVMBitReader',
  '-lLLVMCore',
  '-lLLVMRemarks',
  '-lLLVMBitstreamReader',
  '-lLLVMBinaryFormat',
  '-lLLVMSupport',
  '-lLLVMDemangle',
]

dxil_parser_args = [
  '-Wno-extern-c-compat',
]

dxil_llvm_args = [
  '-DDXILPARSER_USE_LLVM=1',
  '-fno-exceptions',
  '-funwind-tables',
  '-fno-rtti',
  '-D_FILE_OFFSET_BITS=64',
  '-D__STDC_CONSTANT_MACROS',
  '-D__STDC_FORMAT_MACROS',
  '-D__STDC_LIMIT_MACROS',
]

dxil_target_include_dirs = [ dxmt_include_path ]
dxil_target_cpp_args = dxil_parser_args
dxil_target_link_args = []
dxil_target_llvm_deps = []

if not dxmt_crossbuild
  dxil_target_llvm_path = get_option('native_llvm_path')

  if not dxil_target_llvm_path.startswith('/')
    dxil_target_llvm_include_path = include_directories(
      join_paths('../..', dxil_target_llvm_path, 'include'))
    dxil_target_llvm_path = join_paths(meson.project_source_root(), dxil_target_llvm_path)
  else
    dxil_target_llvm_include_path = include_directories(
      join_paths(dxil_target_llvm_path, 'include'))
  endif

  dxil_target_include_dirs += dxil_target_llvm_include_path
  dxil_target_cpp_args += dxil_llvm_args
  dxil_target_llvm_deps += dxil_llvm_deps
  dxil_target_link_args += [
    '-L'+join_paths(dxil_target_llvm_path, 'lib'),
    '-lm', '-lz', '-lcurses', '-lxml2',
  ]

  if dxil_target_llvm_path.startswith('/usr/local/opt')
    dxil_target_link_args = [
      dxil_target_link_args,
      '/usr/local/opt/zstd/lib/libzstd.a', '/usr/local/opt/llvm@15/lib/libunwind.a'
    ]
  endif
endif

dxil_parser_lib = static_library('DXILParser', dxil_parser_src,
  include_directories : dxil_target_include_dirs,
  cpp_args            : [ dxil_target_cpp_args ],
  link_args           : dxil_target_link_args,
)

dxil_parser_dep = declare_dependency(
  link_with           : [ dxil_parser_lib ],
  include_directories : [ dxmt_include_path ],
  link_args           : [ dxil_target_link_args, dxil_target_llvm_deps ],
)

if cpu_family == 'x86_64' or cpu_family == 'aarch64' or not dxmt_crossbuild

dxil_native_llvm_path = get_option('native_llvm_path')

if not dxil_native_llvm_path.startswith('/')
  dxil_llvm_include_path_native = include_directories(
    join_paths('../..', dxil_native_llvm_path, 'include'))
  dxil_native_llvm_path = join_paths(meson.project_source_root(), dxil_native_llvm_path)
else
  dxil_llvm_include_path_native = include_directories(
    join_paths(dxil_native_llvm_path, 'include'))
endif

dxil_llvm_ld_flags_native = [
  '-L'+join_paths(dxil_native_llvm_path, 'lib'),
  '-lm', '-lz', '-lcurses', '-lxml2',
]

if dxil_native_llvm_path.startswith('/usr/local/opt')
  dxil_llvm_ld_flags_native = [
    dxil_llvm_ld_flags_native,
    '/usr/local/opt/zstd/lib/libzstd.a', '/usr/local/opt/llvm@15/lib/libunwind.a'
  ]
endif

dxil_parser_native_lib = static_library('DXILParserNative', dxil_parser_src,
  include_directories : [
    dxmt_native_include_path,
    include_directories('..'),
    dxil_llvm_include_path_native,
  ],
  cpp_args            : [ dxil_parser_args, dxil_llvm_args ],
  link_args           : dxil_llvm_ld_flags_native,
  native              : dxmt_crossbuild,
)

dxil_parser_native_dep = declare_dependency(
  link_with           : [ dxil_parser_native_lib ],
  include_directories : [ dxmt_native_include_path, include_directories('..') ],
  link_args           : [ dxil_llvm_ld_flags_native, dxil_llvm_deps ],
)

endif
