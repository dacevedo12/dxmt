# The d3d9 suite drives two corpora, both as separate PEs it spawns rather than
# linked-in code, so the suite itself carries no d3d9 link dependency:
#   - the vendored vkd3d shader_runner over the in-tree numeric corpus
#     (see shader-oracle/), and
#   - Wine's own d3d9 conformance modules, built below.

# Wine's d3d9 tests are the mature reference for this API and the bar this
# frontend is measured against. They are not vendored: the repository already
# pins Wine as the external/wine submodule, which is what wine_source_path
# points at, so the corpus is fetched rather than copied and tracks that pin.
#
# Two trees are needed and they are not interchangeable. The Wine SOURCE tree
# carries include/wine/test.h and the test sources; the Wine INSTALL carries the
# PE SDK headers under include/wine/windows. A checkout without the submodule
# produces no targets, and the suite then reports the modules as unconfigured
# rather than passing on nothing.
#
# STANDALONE makes test.h emit main() and derive the test list from START_TEST,
# so no generated testlist is required. __WINESRC__ is deliberately NOT defined:
# it turns on lints meant for Wine's own sources, including a winbase.h macro
# that redefines strncpy to a deliberate error, which some toolchains then trip
# over through their own CRT headers. Neither the test sources nor test.h read
# it.

wine_conformance_tests = []
wine_tests_source_root = get_option('wine_source_path')

if dxmt_crossbuild and wine_tests_source_root != ''
  if not fs.is_absolute(wine_tests_source_root)
    wine_tests_source_root = meson.project_source_root() / wine_tests_source_root
  endif

  wine_tests_dir = wine_tests_source_root / 'dlls/d3d9/tests'
  wine_tests_headers = wine_tests_source_root / 'include'
  wine_tests_sdk = get_option('wine_install_path') / 'include/wine/windows'

  if not fs.is_file(wine_tests_headers / 'wine/test.h')
    message('wine_source_path has no include/wine/test.h; the Wine d3d9 conformance modules are disabled')
  elif not fs.is_dir(wine_tests_sdk)
    message('wine_install_path has no include/wine/windows; the Wine d3d9 conformance modules are disabled')
  else
    # Wine's headers must precede the toolchain's so its windows.h wins, while
    # the CRT headers stay reachable because the D3D headers include them.
    wine_tests_c_args = cc.get_supported_arguments([
      '-DSTANDALONE',
      # The tree defines NOMINMAX project-wide to keep windows.h's min and max
      # macros out of the C++ frontend, but these tests call them. Undefine it
      # for just these translation units, exactly as the vendored shader runner
      # does, rather than editing the upstream corpus.
      '-UNOMINMAX',
      '-I' + wine_tests_sdk,
      '-I' + wine_tests_headers,
      '-I' + wine_tests_dir,
      # The corpus is upstream Wine's and is kept unmodified, so its warnings
      # are not ours to fix and would otherwise bury a real diagnostic.
      '-w',
    ])

    # Each module's test calls are wrapped so a single function can be skipped,
    # which is what lets the suite re-run a module that crashed and still get
    # results from the functions after the crash. The transform runs at build
    # time because the corpus is fetched from Wine unmodified.
    wine_isolate = files('wine_isolate.py')
    wine_counts = {}
    foreach line : fs.read('wine_module_counts.txt').strip().split('\n')
      if line.startswith('#') or line == ''
        continue
      endif
      parts = line.split(' ')
      wine_counts += { parts[0]: parts[1] }
    endforeach

    # The wine_dbgstr_* helpers in wine/debug.h reach one symbol that a winelib
    # build gets from ntdll and a bare PE does not. It is built once, on its own,
    # so that per-module compile flags cannot reach it: the modules do not agree
    # on those flags, and forcing windows.h into this translation unit as well
    # makes both it and the module emit __fastfail, which then collides at link.
    wine_debug_stubs = static_library('d3d9-wine-debug-stubs',
      files('wine_debug_stubs.c'),
      c_args: wine_tests_c_args,
      install: false)

    # d3d9ex includes wine/test.h before d3d9.h, so test.h's console calls come
    # ahead of windows.h. Forcing windows.h in first fixes that but then precedes
    # the module's own COBJMACROS define, which leaves IUnknown_AddRef and
    # IUnknown_Release undeclared, so the two go together.
    #
    # device.c includes d3d9on12.h and dxgi1_4.h after <initguid.h>, which
    # DEFINES their GUIDs in the object; on a stricter linker those collide with
    # the same GUIDs in the GUID library. -DINITGUID makes it define every GUID
    # itself, so the library is dropped for it alone (below) and nothing collides.
    wine_module_extra_args = {
      'd3d9ex': [ '-DCOBJMACROS', '-include', 'windows.h' ],
      'device': [ '-DINITGUID' ],
    }

    # stateblock runs clean; visual, d3d9ex and device report their failures end
    # to end.
    foreach module : [ 'stateblock', 'visual', 'd3d9ex', 'device' ]
      wine_isolated = custom_target('wine-isolate-' + module,
        input: wine_tests_dir / (module + '.c'),
        output: [ module + '.isolated.c', module + '.functions' ],
        command: [
          python3_native, wine_isolate, '@INPUT@',
          '--output', '@OUTPUT0@',
          '--manifest', '@OUTPUT1@',
          '--expect-count', wine_counts[module],
          '--module', module,
        ])

      wine_module_deps = [
        cc.find_library('d3d9'),
        cc.find_library('user32'),
        cc.find_library('gdi32'),
        cc.find_library('ole32'),
      ]
      # The modules reference the D3D9 interface GUIDs without defining them, so
      # the GUID library is linked in, except device, which defines them itself
      # under -DINITGUID and would otherwise get a duplicate of every one.
      if module != 'device'
        wine_module_deps += cc.find_library('dxguid')
      endif

      wine_conformance_tests += executable('d3d9-wine-' + module + '-test',
        wine_isolated[0],
        c_args: wine_tests_c_args + [
          '-include', meson.current_source_dir() / 'dxmt_conformance.h',
        ] + wine_module_extra_args.get(module, []),
        link_with: wine_debug_stubs,
        dependencies: wine_module_deps,
        install: false)

      # The suite reads the manifest to know which functions a module has.
      wine_conformance_tests += wine_isolated
    endforeach
  endif
endif

d3d9_unit_sources = files(
  'shader_oracle_spec.cpp',
  'wine_conformance_spec.cpp',
)
d3d9_unit_dependencies = []

d3d9_unit_cpp_args = []
if wine_conformance_tests.length() > 0
  d3d9_unit_cpp_args += [ '-DDXMT_D3D9_WINE_TESTS_BUILT' ]
endif

# The shader-oracle corpus lives in the source tree, so the spec cannot find it
# relative to the test image the way it finds the runner. Compile the source
# location in as a last-resort fallback so a plain build-tree run needs no
# environment. The oracle build never wants it: that drop is copied to a machine
# with no source tree, where the copies beside the image are the only correct
# answer.
d3d9_unit_cpp_args += [
  '-DDXMT_D3D9_ORACLE_CORPUS_DIR="'
  + (meson.current_source_dir() / 'shader-oracle' / 'corpus')
  + '"',
]
