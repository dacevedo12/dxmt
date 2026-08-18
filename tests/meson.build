subdir('dx11')

# The d3d9 conformance suite drives two corpora as child processes: Wine's own
# d3d9 test modules and a vendored vkd3d shader runner. Both are PEs, so the
# suite only exists in a cross build; the corpus lint below it is host-native
# and runs anywhere.
python3_native = find_program('python3', native: true)

shader_oracle_runner = []
subdir('d3d9')
subdir('d3d9/shader-oracle')

# Source-level gates over src/d3d9. They read the tree and need neither Wine nor
# a GPU, so they hold on every configuration, cross or native.
foreach lint : [
  [ 'd3d9-clang-format', 'test_d3d9_format.py' ],
  [ 'd3d9-format-tables', 'test_d3d9_format_tables.py' ],
  [ 'd3d9-descriptor-initialization', 'test_descriptor_initialization.py' ],
]
  test(lint[0], python3_native,
    args: [ files('ci' / lint[1]) ],
    suite: 'host',
    timeout: 120,
  )
endforeach

if dxmt_crossbuild
  # Read at run time and looked up beside the test image, so a build tree and a
  # packaged drop resolve them the same way.
  foreach baseline : [
    'wine_conformance_baseline.txt',
    'wine_known_skips.txt',
    'shader_oracle_baseline.txt',
  ]
    configure_file(input: 'd3d9' / baseline, output: baseline, copy: true)
  endforeach

  d3d9_conformance = executable('dxmt-d3d9-conformance',
    d3d9_unit_sources + files('d3d9/dxmt_test_main.cpp'),
    include_directories: [ include_directories('d3d9') ],
    cpp_args: d3d9_unit_cpp_args,
    install: false,
  )

  # Two tiers, split because they need different things of the machine. The
  # numeric corpus draws into an offscreen target and reads it back, which a
  # session without a window server still does correctly. Wine's modules create
  # real windows, take focus and change the display mode, so they need one.
  #
  # Both need a wine exe_wrapper to launch the PE; meson reports them skipped on
  # a configuration without one.
  test('d3d9-shader-oracle', d3d9_conformance,
    args: [ 'Corpus' ],
    suite: 'shader-oracle',
    timeout: 900,
    is_parallel: false,
  )

  test('d3d9-wine-conformance', d3d9_conformance,
    args: [ 'Wine' ],
    suite: 'conformance',
    timeout: 3600,
    is_parallel: false,
  )
endif
