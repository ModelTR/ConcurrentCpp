# File: include/concurrent_quene.hpp
# Author: modelTR (mxtian_ran@163.com)
# Brief Serval implemention of concurrent quene (descriptions need to be enriched)
# Version 0.1
# Date 2026-11-02
# Copyright MIT License (c) 2026

cmake_minimum_required(VERSION 4.4.0)

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

include(FetchContent)
include(cmake/CPM.cmake)

file(GLOB SRC_FILES src/*.cpp)

# --- goolgle::benchmark ----------------------------------------------------
CPMAddPackage(
    NAME benchmark
    GIT_REPOSITORY https://gh-proxy.com/https://github.com/google/benchmark.git
    GIT_TAG v1.9.5
    GIT_SHALLOW
    SYSTEM
    OPTIONS 
    "BENCHMARK_ENABLE_TESTING OFF"
    "BENCHMARK_ENABLE_INSTALL OFF"
    "BENCHMARK_ENABLE_WERROR OFF"
)
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
    benchmark::benchmark_main
)

if(MSVC)
    target_compile_options(CoucurrentCpp PRIVATE /W4 /Zc:preprocessor)
else()
    target_compile_options(CoucurrentCpp PRIVATE
        -Wall -Wextra)
endif()
