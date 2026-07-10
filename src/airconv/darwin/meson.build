native_llvm_path = get_option('native_llvm_path')
if not native_llvm_path.startswith('/')
  native_llvm_path = join_paths(meson.project_source_root(), native_llvm_path)
endif
# Managed LLVM can live outside both Meson's source and build roots.
llvm_include_arg_darwin = '-I' + join_paths(native_llvm_path, 'include')

llvm_ld_flags_darwin = [
  '-L'+join_paths(native_llvm_path, 'lib'),
  '-lm', '-lz', '-lcurses', '-lxml2'
]

if native_llvm_path.startswith('/usr/local/opt') or native_llvm_path.startswith('/opt/homebrew/opt')
  llvm_homebrew_prefix = native_llvm_path.startswith('/opt/homebrew/opt') ? '/opt/homebrew/opt' : '/usr/local/opt'
  llvm_ld_flags_darwin = [
    llvm_ld_flags_darwin,
    join_paths(llvm_homebrew_prefix, 'zstd/lib/libzstd.a'),
    join_paths(native_llvm_path, 'lib/libunwind.a')
  ]
endif

airconv_lib_darwin = static_library('airconv', airconv_src,
  include_directories : [ dxmt_include_path ],
  cpp_args            : [ airconv_args, llvm_include_arg_darwin ],
  dependencies        : [ dxbc_parser_native_dep, dxil_parser_native_dep ],
  link_args           : llvm_ld_flags_darwin,
  native              : dxmt_crossbuild
)

airconv_dep_darwin = declare_dependency(
  link_with           : [ airconv_lib_darwin ],
  include_directories : [ dxmt_include_path, include_directories('..') ],
  link_args           : [ llvm_ld_flags_darwin, llvm_deps ] # meh
)

airconv_dx12_metal4_lib_darwin = static_library('airconv_dx12_metal4', airconv_dx12_metal4_src,
  include_directories : [ dxmt_include_path ],
  cpp_args            : [ airconv_args, llvm_include_arg_darwin ],
  dependencies        : [ dxbc_parser_native_dep, dxil_parser_native_dep ],
  link_args           : llvm_ld_flags_darwin,
  native              : dxmt_crossbuild
)

airconv_dx12_metal4_dep_darwin = declare_dependency(
  link_with           : [ airconv_dx12_metal4_lib_darwin ],
  include_directories : [ dxmt_include_path, include_directories('..') ],
  link_args           : [ llvm_ld_flags_darwin, llvm_deps ]
)

# Exposed as a global so both the upstream airconv unit tests and the
# wine-free d3d9 shader-translation goldens (tests/dx9/unit/airconv_golden.py)
# can drive this build-machine binary.
airconv_cli_darwin = executable('airconv', airconv_src + airconv_cli_src,
  include_directories : [ dxmt_include_path ],
  cpp_args            : [ airconv_args, llvm_include_arg_darwin ],
  dependencies        : [ dxbc_parser_native_dep, dxil_parser_native_dep ],
  link_args           : [ llvm_ld_flags_darwin, llvm_deps ],
  native              : dxmt_crossbuild
)
