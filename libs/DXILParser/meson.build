dxil_parser_src = files([
  'DXILParser.cpp',
])

dxil_parser_args = [
  '-Wno-extern-c-compat',
]

dxil_parser_lib = static_library('DXILParser', dxil_parser_src,
  include_directories : [ dxmt_include_path ],
  cpp_args            : [ dxil_parser_args ],
)

dxil_parser_dep = declare_dependency(
  link_with           : [ dxil_parser_lib ],
  include_directories : [ dxmt_include_path ],
)

if cpu_family == 'x86_64' or cpu_family == 'aarch64' or not dxmt_crossbuild

dxil_parser_native_lib = static_library('DXILParserNative', dxil_parser_src,
  include_directories : [ dxmt_native_include_path, include_directories('..') ],
  cpp_args            : [ dxil_parser_args ],
  native              : dxmt_crossbuild,
)

dxil_parser_native_dep = declare_dependency(
  link_with           : [ dxil_parser_native_lib ],
  include_directories : [ dxmt_native_include_path, include_directories('..') ],
)

endif
