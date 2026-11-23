project(
  'CoucurrentCpp',
  'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++20',
    # CMake's build tree is configured as Release; keep the default Meson
    # build comparable. Override with --buildtype=debug for debugging.
    'buildtype=release',
  ],
)

# Mirrors the non-PGO part of CMakeLists.txt:
#   * CoucurrentCpp   <- main.cpp (+ src/*.cpp)
#   * ConcurrentBench <- benchmark/benchmark.cpp
#   * ConcurrentTests <- tests/*_test.cpp
#
# The PGO/LTO logic in cmake/pgo.cmake is intentionally not ported.
# abseil-cpp, google/benchmark and googletest are pinned to the same
# revisions as the CMake and Bazel builds through subprojects/*.wrap.

cpp = meson.get_compiler('cpp')

# CMake applies warnings to the project targets only, never to absl/benchmark.
# add_project_arguments() has the same scope: subprojects are unaffected.
if cpp.get_argument_syntax() == 'msvc'
  add_project_arguments('/W4', '/Zc:preprocessor', language: 'cpp')
else
  add_project_arguments('-Wall', '-Wextra', language: 'cpp')
endif

# CMake links with -static on non-MSVC toolchains.
exe_link_args = []
if cpp.get_argument_syntax() != 'msvc'
  exe_link_args += ['-static']
endif

project_inc = include_directories('include', 'src')
bench_inc = include_directories('include', 'benchmark')

cmake = import('cmake')

absl_defines = {
  'ABSL_BUILD_TESTING': 'OFF',
  'ABSL_ENABLE_INSTALL': 'OFF',
  'ABSL_PROPAGATE_CXX_STD': 'ON',
}
# Windows has no O_NONBLOCK; CMakeLists.txt applies the same define to
# absl_time_zone. The CMake wrapper has no per-target hook, so apply it to the
# subproject as a whole.
if host_machine.system() == 'windows'
  absl_defines += {'CMAKE_CXX_FLAGS': '-DO_NONBLOCK=0'}
endif
absl_opts = cmake.subproject_options()
absl_opts.add_cmake_defines(absl_defines)
absl_proj = cmake.subproject('abseil-cpp-20260817.0', options: absl_opts)
absl_sync_dep = absl_proj.dependency('absl_synchronization')

benchmark_opts = cmake.subproject_options()
benchmark_opts.add_cmake_defines({
  'BENCHMARK_ENABLE_TESTING': 'OFF',
  'BENCHMARK_ENABLE_INSTALL': 'OFF',
  'BENCHMARK_ENABLE_WERROR': 'OFF',
  'BENCHMARK_ENABLE_LIBPFM': 'OFF',
})
benchmark_proj = cmake.subproject('benchmark-1.9.5', options: benchmark_opts)
benchmark_dep = benchmark_proj.dependency('benchmark')

# --- googletest --------------------------------------------------------------
# Same tag as the CMake and Bazel builds.  Only gtest is needed for the smoke
# tests, so gmock is not built.
gtest_opts = cmake.subproject_options()
gtest_opts.add_cmake_defines({
  'INSTALL_GTEST': 'OFF',
  'gtest_build_tests': 'OFF',
  'gmock_build_tests': 'OFF',
  'BUILD_GMOCK': 'OFF',
})
gtest_proj = cmake.subproject('googletest-1.18.0', options: gtest_opts)
gtest_main_dep = gtest_proj.dependency('gtest_main')

threads_dep = dependency('threads')

deps_main = [absl_sync_dep]
deps_bench = [absl_sync_dep, benchmark_dep, threads_dep]
deps_test = [gtest_main_dep, absl_sync_dep, threads_dep]

if cpp.get_argument_syntax() != 'msvc'
  atomic_dep = cpp.find_library('atomic', required: false)
  if atomic_dep.found()
    deps_main += atomic_dep
    deps_bench += atomic_dep
    deps_test += atomic_dep
  endif
endif

# src/ is currently empty. Meson deliberately has no globbing; add future
# sources explicitly here (the CMake build uses `file(GLOB src/*.cpp)`).
src_files = []

executable(
  'CoucurrentCpp',
  ['main.cpp'] + src_files,
  include_directories: project_inc,
  dependencies: deps_main,
  link_args: exe_link_args,
  install: false,
)

executable(
  'ConcurrentBench',
  'benchmark/benchmark.cpp',
  include_directories: bench_inc,
  dependencies: deps_bench,
  link_args: exe_link_args,
  install: false,
)

# Tests (google/googletest). Meson has no globbing, so list the sources.
test_sources = [
  'tests/smoke_inplace_vector_test.cpp',
  'tests/smoke_primitives_test.cpp',
  'tests/smoke_random_test.cpp',
  'tests/smoke_stacks_test.cpp',
]

test_exe = executable(
  'ConcurrentTests',
  test_sources,
  include_directories: project_inc,
  dependencies: deps_test,
  link_args: exe_link_args,
  install: false,
)

test('smoke', test_exe, timeout: 60)
