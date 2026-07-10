dxmt_test_include_dirs = include_directories('include')
d3d10_test_dep = cpp.find_library('d3d10')
d3d11_test_dep = cpp.find_library('d3d11')
d3d12_test_dep = cpp.find_library('d3d12')
d3dcompiler_library = 'd3dcompiler_47'
if get_option('windows_oracle_only')
  d3dcompiler_library = 'd3dcompiler'
endif
d3dcompiler_test_dep = cpp.find_library(d3dcompiler_library)
dxgi_test_dep = cpp.find_library('dxgi')

windows_oracle_cpp_args = []
d3d11_windows_oracle_dependencies = []
if get_option('windows_oracle_only')
  windows_oracle_cpp_args = cpp.get_supported_arguments([
    '/bigobj',
    '/utf-8',
    '/DNOMINMAX',
    '/DWIN32_LEAN_AND_MEAN',
    '/D_WIN32_WINNT=0x0A00',
  ])
  d3d11_windows_oracle_dependencies += cpp.find_library('dxguid')
endif

dxmt_test_main_lib = static_library(
  'dxmt-wine-test-main',
  files(
    'support/main.cpp',
    'support/scheduler.cpp',
    'support/scheduler_plan.cpp',
  ),
  dependencies: [gtest_dep],
  include_directories: [dxmt_test_include_dirs],
  cpp_args: windows_oracle_cpp_args,
  install: false,
)

dxmt_test_dep = declare_dependency(
  link_with: [dxmt_test_main_lib],
  dependencies: [gtest_dep],
  include_directories: [dxmt_test_include_dirs],
)

subdir('d3d10')
subdir('d3d11')
subdir('d3d12')

if not get_option('windows_oracle_only')
dxmt_wine_framework_tests = executable(
  'dxmt-wine-framework-tests',
  files(
    'wine/framework_spec.cpp',
    'wine/scheduler_spec.cpp',
    'wine/placeholder_spec.cpp',
    'wine/layout_spec.cpp',
  ),
  dependencies: [dxmt_test_dep],
  install: false,
)
endif

dxmt_wine_d3d10_tests = executable(
  'dxmt-wine-d3d10-tests',
  d3d10_unit_sources,
  dependencies: [dxmt_test_dep] + d3d10_unit_dependencies,
  cpp_args: windows_oracle_cpp_args,
  install: false,
)

dxmt_wine_d3d11_tests = executable(
  'dxmt-wine-d3d11-tests',
  d3d11_unit_sources,
  dependencies: [dxmt_test_dep] + d3d11_unit_dependencies + d3d11_windows_oracle_dependencies,
  cpp_args: windows_oracle_cpp_args,
  install: false,
)

dxmt_wine_d3d12_tests = executable(
  'dxmt-wine-d3d12-tests',
  d3d12_unit_sources,
  dependencies: [dxmt_test_dep] + d3d12_unit_dependencies,
  cpp_args: windows_oracle_cpp_args,
  install: false,
)

configure_file(
  input: 'd3d12/run-windows-oracle.bat',
  output: 'run-windows-oracle.bat',
  copy: true,
)

if get_option('windows_oracle_only')
  subdir_done()
endif

commit_message_policy_test = find_program('../scripts/check-commit-message.sh')
test(
  'commit-message-policy',
  commit_message_policy_test,
  args: ['--self-test'],
  suite: ['unit', 'repository'],
  timeout: 30,
)

python3_native = find_program('python3', native: true)

test(
  'ci-routing-policy',
  python3_native,
  args: [files('ci/ci_routing_policy_test.py')],
  suite: ['unit', 'repository'],
  timeout: 30,
)

test(
  'public-api-boundary-policy',
  python3_native,
  args: [files('ci/test_public_api_boundary.py')],
  suite: ['unit', 'repository'],
  timeout: 30,
)

test(
  'residency-architecture-policy',
  python3_native,
  args: [files('ci/test_residency_architecture.py')],
  suite: ['unit', 'repository'],
  timeout: 30,
)

test(
  'd3d12-coverage-validator-tests',
  python3_native,
  args: [files('coverage/test_check_d3d12_coverage.py')],
  suite: ['unit', 'coverage'],
  timeout: 30,
)

dxmt_wine_test_scheduler = executable(
  'dxmt-wine-test-scheduler',
  files('support/wine_suite_scheduler.cpp'),
  include_directories: [include_directories('support')],
  install: false,
)

test(
  'dxmt-wine-unit-tests',
  wine_test_launcher,
  args: ['wine-exec', '--', dxmt_wine_test_scheduler],
  suite: ['unit', 'wine'],
  # Large parameterized D3D12 matrices need well over the default 120s budget.
  timeout: 1800,
  env: wine_test_env,
  is_parallel: false,
  workdir: meson.project_source_root(),
)

alias_target(
  'dxmt-wine-tests',
  dxmt_wine_test_scheduler,
  dxmt_wine_framework_tests,
  dxmt_wine_d3d10_tests,
  dxmt_wine_d3d11_tests,
  dxmt_wine_d3d12_tests,
  d3d12_differential_oracle,
)

alias_target(
  'dxmt-wine-tests-framework',
  dxmt_wine_test_scheduler,
  dxmt_wine_framework_tests,
)
alias_target(
  'dxmt-wine-tests-d3d10',
  dxmt_wine_test_scheduler,
  dxmt_wine_d3d10_tests,
)
alias_target(
  'dxmt-wine-tests-d3d11',
  dxmt_wine_test_scheduler,
  dxmt_wine_d3d11_tests,
)
alias_target(
  'dxmt-wine-tests-d3d12',
  dxmt_wine_test_scheduler,
  dxmt_wine_d3d12_tests,
  d3d12_differential_oracle,
)

# d3d9 host-native, wine-free tests (the 'host' suite CI gate).
subdir('dx9')
