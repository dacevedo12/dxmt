google_benchmark_include_dirs = include_directories('.', 'include', is_system: true)
google_benchmark_threads_dep = dependency('threads')
google_benchmark_platform_deps = []
if host_machine.system() == 'windows'
  google_benchmark_platform_deps += cpp.find_library('shlwapi')
endif

google_benchmark_sources = files(
  'src/benchmark.cc',
  'src/benchmark_api_internal.cc',
  'src/benchmark_name.cc',
  'src/benchmark_register.cc',
  'src/benchmark_runner.cc',
  'src/check.cc',
  'src/colorprint.cc',
  'src/commandlineflags.cc',
  'src/complexity.cc',
  'src/console_reporter.cc',
  'src/counter.cc',
  'src/csv_reporter.cc',
  'src/json_reporter.cc',
  'src/perf_counters.cc',
  'src/reporter.cc',
  'src/statistics.cc',
  'src/string_util.cc',
  'src/sysinfo.cc',
  'src/timers.cc',
)

# The include directories are already marked is_system, which covers every
# consumer of these headers.  It does not cover the library's own sources: they
# reach mutex.h through a quoted include resolved relative to the including
# file, so the project-wide -Werror=thread-safety applies to upstream's
# CAPABILITY/ACQUIRE annotations and fails four translation units.  We do not
# audit upstream's lock contracts, so drop the warning for this target only -
# every DXMT target keeps it.
google_benchmark_vendor_args = cpp.get_supported_arguments(['-Wno-thread-safety'])

google_benchmark_lib = static_library(
  'dxmt-google-benchmark',
  google_benchmark_sources,
  cpp_args: [
    '-DBENCHMARK_STATIC_DEFINE',
    '-DBENCHMARK_VERSION="1.9.5"',
  ] + google_benchmark_vendor_args,
  dependencies: [google_benchmark_threads_dep] + google_benchmark_platform_deps,
  include_directories: [google_benchmark_include_dirs],
  install: false,
)

google_benchmark_dep = declare_dependency(
  link_with: [google_benchmark_lib],
  compile_args: ['-DBENCHMARK_STATIC_DEFINE'],
  dependencies: [google_benchmark_threads_dep] + google_benchmark_platform_deps,
  include_directories: [google_benchmark_include_dirs],
)
