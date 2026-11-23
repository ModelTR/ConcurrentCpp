# File: CMakeLists.txt
# Author: modelTR (mxtian_ran@163.com)
# Brief Serval implemention of concurrent quene (descriptions need to be enriched)
# Version 0.2
# Date 2026-11-02
# Copyright MIT License (c) 2026

cmake_minimum_required(VERSION 4.0.0)

cmake_policy(SET CMP0141 NEW)

set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
    "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

project(CoucurrentCpp LANGUAGES CXX)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND NOT MSVC)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")
    # Using lld linker globly
    add_link_options(-fuse-ld=lld)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- PGO / LTO -------------------------------------------------------------
# PGO=ON selects PGO_MODE=AUTO: configure-time bootstrap (instrumented build +
# training + profile merge), then this build uses the merged profile.
# PGO_LTO=THIN and PGO_SCOPE=TARGET keep the optimization on ConcurrentBench
# only; the benchmark library and absl stay out of the profile.
if(NOT MSVC)
    option(PGO "Enable PGO+LTO for the benchmark build" OFF)
    set(PGO_LTO FULL)
    set(PGO_SCOPE GLOBAL)
    include(cmake/pgo.cmake)
endif()
# ---------------------------------------------------------------------------

include(FetchContent)
include(cmake/CPM.cmake)

file(GLOB SRC_FILES src/*.cpp)

# --- google::benchmark ----------------------------------------------------
CPMAddPackage(
    NAME benchmark
    GIT_REPOSITORY https://v4.gh-proxy.org/https://github.com/google/benchmark.git
    GIT_TAG v1.9.5
    GIT_SHALLOW
    SYSTEM
    OPTIONS 
    "BENCHMARK_ENABLE_TESTING OFF"
    "BENCHMARK_ENABLE_INSTALL OFF"
    "BENCHMARK_ENABLE_WERROR OFF"
)
# ---------------------------------------------------------------------------

# --- googletest ------------------------------------------------------------

CPMAddPackage(
  NAME googletest
  GIT_REPOSITORY https://v4.gh-proxy.org/https://github.com/google/googletest.git
  GIT_TAG v1.18.0
  GIT_SHALLOW FALSE
  OPTIONS "INSTALL_GTEST OFF" "gtest_build_tests OFF" "gmock_build_tests OFF"
)

# --- absl ------------------------------------------------------------------
CPMAddPackage(
  NAME absl
  GIT_REPOSITORY https://v4.gh-proxy.org/https://github.com/abseil/abseil-cpp.git
  GIT_TAG 20260817.0
  GIT_SHALLOW
  OPTIONS "ABSL_BUILD_TESTING OFF" "ABSL_ENABLE_INSTALL OFF"
)
# Windows hasn't O_NONBLOCK
if(TARGET absl_time_zone)
  target_compile_definitions(absl_time_zone PRIVATE O_NONBLOCK=0)
endif()
# ---------------------------------------------------------------------------

add_executable(CoucurrentCpp
  main.cpp
  ${SRC_FILES}
)

target_include_directories(CoucurrentCpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(CoucurrentCpp PRIVATE
    absl::synchronization
)

if(NOT MSVC)
    target_link_libraries(CoucurrentCpp PRIVATE atomic)
endif()


if(MSVC)
    target_compile_options(CoucurrentCpp PRIVATE /W4 /Zc:preprocessor)
else()
    target_compile_options(CoucurrentCpp PRIVATE
        -Wall -Wextra)
endif()

# Benchmark

find_package(Threads REQUIRED)

add_executable(ConcurrentBench benchmark/benchmark.cpp)
target_include_directories(ConcurrentBench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmark
)
target_link_libraries(ConcurrentBench PRIVATE
    absl::synchronization
    benchmark::benchmark
    Threads::Threads
)

if(NOT MSVC)
    target_compile_options(ConcurrentBench PRIVATE -Wall -Wextra)
    target_link_libraries(ConcurrentBench PRIVATE atomic)
else()
    target_compile_options(ConcurrentBench PRIVATE /W4 /Zc:preprocessor)
endif()

# --- PGO training workload -------------------------------------------------6
# One repetition is enough to produce profile data; the regular benchmark
# defaults (9 repetitions, full console report) are intentionally overridden.
if(NOT MSVC)
    pgo_apply(ConcurrentBench)
    pgo_add_training_target(
    NAME concurrent-bench
    EXECUTABLE ConcurrentBench
    COMMAND
        --benchmark_filter=.*
        --benchmark_repetitions=3
        --benchmark_min_time=0.01s
    REPEAT 1
    )
endif()
# ---------------------------------------------------------------------------

# --- Tests ----------------------------------------------------------------
enable_testing()

file(GLOB TEST_FILES tests/*_test.cpp)

add_executable(ConcurrentTests ${TEST_FILES})
target_include_directories(ConcurrentTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(ConcurrentTests PRIVATE
    GTest::gtest_main
    absl::synchronization
    Threads::Threads
)

if(NOT MSVC)
    target_link_libraries(ConcurrentTests PRIVATE atomic)
    target_compile_options(ConcurrentTests PRIVATE -Wall -Wextra)
else()
    target_compile_options(ConcurrentTests PRIVATE /W4 /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(ConcurrentTests)
# ---------------------------------------------------------------------------