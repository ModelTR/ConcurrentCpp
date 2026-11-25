# File: CMakeLists.txt
# Author: modelTR (mxtian_ran@163.com)
# Brief Serval implemention of concurrent quene (descriptions need to be enriched)
# Version 0.3
# Date 2026-11-24
# Copyright MIT License (c) 2026

cmake_minimum_required(VERSION 4.0.0)

cmake_policy(SET CMP0141 NEW)

set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
    "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

project(CoucurrentCpp LANGUAGES CXX)

option(USE_CHACHE ON)
option(USE_SYSTEM_DEPS
    "Prefer system benchmark/googletest/absl packages on non-MSVC toolchains" ON)

if(USE_CHACHE)
    find_program(SCCACHE_EXECUTABLE NAMES sccache)
    if(SCCACHE_EXECUTABLE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_EXECUTABLE}")
        set(CMAKE_C_COMPILER_LAUNCHER "${SCCACHE_EXECUTABLE}")
        message(STATUS "sccache launcher: ${SCCACHE_EXECUTABLE}")
    else()
        find_program(CCACHE_PROGRAM ccache)
        if(CCACHE_PROGRAM AND NOT MSVC)
            set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
            set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
            message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
        else()
            message(WARNING "Can't find sccache or ccache")
        endif()
    endif()
endif()



set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    if(WIN32)
        #static flag is not valid on Linux or macOS.
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")
    endif()
    if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_link_options(-fuse-ld=lld)
    endif()
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# PGO is supported for Clang, MSVC and GCC.  GCC uses its .gcda profile
# mechanism and does not need lld; keep LTO disabled for GCC by default.
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"
   OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    option(PGO "Enable PGO for the benchmark build" OFF)
    if(NOT DEFINED PGO_LTO)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(PGO_LTO OFF)
        elseif(MSVC OR WIN32)
            set(PGO_LTO FULL)
        else()
            find_program(PGO_LLD_EXECUTABLE NAMES ld.lld lld)
            if(PGO_LLD_EXECUTABLE)
                set(PGO_LTO FULL)
            else()
                set(PGO_LTO OFF)
                message(STATUS
                    "PGO/LTO: lld not found; default LTO disabled on this "
                    "platform (pass -DPGO_LTO=FULL to force it)")
            endif()
        endif()
    endif()
    set(PGO_SCOPE GLOBAL)
    # Set -DPGO_TRAINING_FILTER=.* to train on every benchmark.
    set(PGO_TRAINING_FILTER ".*" CACHE STRING
        "Google Benchmark filter for the PGO training workload")
    # Forward the filter to the AUTO bootstrap sub-build; PGO_ variables are
    # otherwise excluded from automatic cache forwarding.
    set(PGO_FORWARD_VARS PGO_TRAINING_FILTER)
    include(cmake/pgo.cmake)
else()
    message(STATUS
        "PGO is enabled only for Clang, GCC and MSVC; "
        "skipping for ${CMAKE_CXX_COMPILER_ID}")
endif()
# ---------------------------------------------------------------------------

include(FetchContent)
include(cmake/CPM.cmake)
include(cmake/runtime-dlls.cmake)

file(GLOB SRC_FILES src/*.cpp)

# On MSYS2, keep system dependency lookup inside the compiler's own prefix.
# A mingw64 compiler linked against ucrt64 shared libraries produces binaries
# whose runtime DLLs are not on the compiler's PATH (0xc0000135), and mixes
# msvcrt/ucrt runtimes.  When no package exists in that prefix, CPM is used and
# the dependencies are built statically with the same compiler.
set(_coucurrent_system_package_args)
if(CMAKE_CXX_COMPILER)
    set(_coucurrent_compiler "${CMAKE_CXX_COMPILER}")
    if(NOT EXISTS "${_coucurrent_compiler}")
        find_program(_coucurrent_compiler NAMES "${CMAKE_CXX_COMPILER}" NO_CACHE)
    endif()
    if(_coucurrent_compiler)
        get_filename_component(_coucurrent_cxx_bin "${_coucurrent_compiler}" DIRECTORY)
        get_filename_component(_coucurrent_cxx_prefix "${_coucurrent_cxx_bin}" DIRECTORY)
        file(TO_CMAKE_PATH "${_coucurrent_cxx_prefix}" _coucurrent_cxx_prefix)
        if(_coucurrent_cxx_prefix MATCHES
           "msys64/(ucrt64|mingw64|clang64|clang32|mingw32|clangarm64)$")
            set(_coucurrent_system_package_args
                PATHS "${_coucurrent_cxx_prefix}" NO_DEFAULT_PATH)
        endif()
    endif()
endif()

# --- google::benchmark ----------------------------------------------------
if(NOT MSVC AND USE_SYSTEM_DEPS)
    find_package(benchmark CONFIG QUIET ${_coucurrent_system_package_args})
endif()

if(MSVC OR NOT TARGET benchmark::benchmark)
    CPMAddPackage(
        NAME benchmark
        GIT_REPOSITORY https://v4.gh-proxy.org/https://github.com/google/benchmark.git
        GIT_TAG v1.10.0
        GIT_SHALLOW
        SYSTEM
        OPTIONS
        "BENCHMARK_ENABLE_TESTING OFF"
        "BENCHMARK_ENABLE_INSTALL OFF"
        "BENCHMARK_ENABLE_WERROR OFF"
    )
endif()

# --- googletest ------------------------------------------------------------
if(NOT MSVC AND USE_SYSTEM_DEPS)
    find_package(GTest CONFIG QUIET ${_coucurrent_system_package_args})
    if(NOT TARGET GTest::gtest_main)
        find_package(gtest CONFIG QUIET ${_coucurrent_system_package_args})
    endif()
endif()

if(MSVC OR NOT TARGET GTest::gtest_main)
    CPMAddPackage(
        NAME googletest
        GIT_REPOSITORY https://v4.gh-proxy.org/https://github.com/google/googletest.git
        GIT_TAG v1.18.0
        GIT_SHALLOW
        SYSTEM
        OPTIONS
        "INSTALL_GTEST OFF"
        "gtest_build_tests OFF"
        "gmock_build_tests OFF"
    )
endif()



# --- absl ------------------------------------------------------------------
if(NOT MSVC AND USE_SYSTEM_DEPS)
    find_package(absl CONFIG QUIET ${_coucurrent_system_package_args})
endif()

if(MSVC OR NOT TARGET absl::synchronization)
    CPMAddPackage(
        NAME absl
        GIT_REPOSITORY https://v4.gh-proxy.org/https://github.com/abseil/abseil-cpp.git
        GIT_TAG 20260817.0
        GIT_SHALLOW
        OPTIONS
        "ABSL_BUILD_TESTING OFF"
        "ABSL_ENABLE_INSTALL OFF"
    )
    # Windows hasn't O_NONBLOCK
    if(TARGET absl_time_zone)
        target_compile_definitions(absl_time_zone PRIVATE O_NONBLOCK=0)
    endif()
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


# --- PGO training workload -------------------------------------------------
# The regular benchmark defaults (9 repetitions, full console report) are intentionally overridden with 3.
# The default filter keeps training fast and deterministic.
if((MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"
    OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
   AND (PGO OR PGO_MODE STREQUAL "GENERATE"))
    pgo_apply(ConcurrentBench)
    # pgo.cmake puts the compiler directory on PATH for the training process
    # (needed for pgort140.dll on MSVC and the MSYS2 runtime DLLs on Clang)
    pgo_add_training_target(
    NAME concurrent-bench
    EXECUTABLE ConcurrentBench
    COMMAND
        --benchmark_filter=${PGO_TRAINING_FILTER}
        --benchmark_repetitions=3
        --benchmark_min_time=0.01s
    REPEAT 1
    )
endif()
# ---------------------------------------------------------------------------

# --- Tests ----------------------------------------------------------------
# include(CTest) calls enable_testing() and adds the BUILD_TESTING option.
include(CTest)

if(BUILD_TESTING)
    file(GLOB TEST_FILES tests/*_test.cpp)

    enable_testing()

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
    if(NOT MSVC)
        # System benchmark/gtest/absl are DLLs on MSYS2.  Put the compiler's
        # runtime directory first so ctest works from any shell, not only from
        # an UCRT64 shell or the verify script.
        get_filename_component(_test_runtime_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        gtest_discover_tests(ConcurrentTests
            PROPERTIES TIMEOUT 60 LABELS unit
                ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${_test_runtime_dir}"
        )
    else()
        gtest_discover_tests(ConcurrentTests
            PROPERTIES TIMEOUT 60 LABELS unit
        )
    endif()
endif()
# ---------------------------------------------------------------------------