#[[============================================================================
 pgo.cmake -- production-ready PGO (Profile-Guided Optimization) + LTO
             integration module
----------------------------------------------------------------------------
 Toolchains covered (measured in this workspace):
   MSYS2 UCRT64 + clang 22.1.8 (x86_64-w64-windows-gnu) + libstdc++ + ninja
   llvm-profdata / libclang_rt.profile live in the clang64 prefix
   (ucrt64 does not provide them)

 Usage (out of the box, recommended):

   # Add two lines to the main CMakeLists.txt:
   #   option(PGO "enable automatic PGO+LTO builds (configure-time bootstrap)" ON)
   #   include(<workspace>/cmake/pgo.cmake)
   #
   # Then build like any other project (a single build command produces the
   # optimized binaries):
   cmake -S proj -B build -G Ninja -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe
   cmake --build build
   #
   # The first AUTO configure runs an instrumented build in <build>/pgo-bootstrap,
   # trains it, merges the profile, and then compiles this build with USE.
   # Later configures only verify that the profile can be reused; when sources
   # are newer, PGO_AUTO_REBOOTSTRAP (default ON) retrains automatically.

 Usage (manual two-stage flow, for fine-grained control):

   # 1) Instrumented build + training + profile merge
   cmake -S proj -B build-inst -G Ninja \
         -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe \
         -DPGO_MODE=GENERATE -DPGO_LTO=THIN \
         -DPGO_PROFILE_DIR=<absolute path>/profile
   cmake --build build-inst                     # build the instrumented program
   cmake --build build-inst --target pgo-train  # run the training workload + merge profdata

   # 2) Build the optimized version with the profile
   cmake -S proj -B build-opt -G Ninja \
         -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe \
         -DPGO_MODE=USE -DPGO_LTO=THIN \
         -DPGO_PROFILE_DIR=<absolute path>/profile
   cmake --build build-opt

 Main variables (all can be passed with -D):
   PGO               convenience switch ON/OFF        equivalent to PGO_MODE=AUTO/OFF
   PGO_MODE          OFF | AUTO | GENERATE | USE   default OFF
                     AUTO = configure-time bootstrap + USE in this build (out of the box)
   PGO_KIND          IR | FE                      default IR (clang: IRPGO is better than FEPGO)
   PGO_LTO           OFF | THIN | FULL            default OFF
   PGO_SCOPE         GLOBAL | TARGET              default GLOBAL
   PGO_PROFILE_DIR   profile data directory       default ${CMAKE_BINARY_DIR}/pgo-profile
   PGO_PROFDATA      merged profile file          default ${PGO_PROFILE_DIR}/profile.profdata
   PGO_PROFDATA_TOOL llvm-profdata executable     default auto-detected
   PGO_RESOURCE_DIR  clang resource-dir           default auto-detected (ucrt64 must point at clang64)
   PGO_LINKER        linker for the PGO/LTO stage default lld (GNU ld cannot be used for PGO/LTO, see the report)
   PGO_STRICT        fail when the profile is missing in USE mode  default ON
   PGO_KEEP_WARNINGS keep -Wprofile-instr-unprofiled / -Wbackend-plugin  default OFF
   PGO_WERROR_OUT_OF_DATE  fail when the profile and the code disagree  default OFF
   PGO_ALLOW_ANY_LINKER  allow a non-lld linker (not recommended)  default OFF
   PGO_VERBOSE       print the configuration summary  default ON
   PGO_EXTRA_COMPILE_FLAGS / PGO_EXTRA_LINK_FLAGS   extra flags

 Boolean shorthands (can be set in the project CMakeLists.txt or passed with -D):
   set(PGO ON/OFF)          ON -> PGO_MODE=AUTO, OFF -> PGO_MODE=OFF
   set(PGO_MODE ON/OFF)     ON -> AUTO, OFF -> OFF
   set(PGO_LTO ON/OFF)      ON -> THIN, OFF -> OFF
   set(PGO_SCOPE ON/OFF)    ON -> GLOBAL, OFF -> TARGET
   set(PGO_KIND ON/OFF)     ON -> IR, OFF -> FE
   String values (AUTO/USE/GENERATE, THIN/FULL, GLOBAL/TARGET, IR/FE) are unchanged.
   Note: when PGO_MODE is set explicitly (normal variable or cache), PGO=ON does
   not override it.

 AUTO-mode-only variables:
   PGO_BOOTSTRAP_DIR     bootstrap sub-build directory  default ${CMAKE_BINARY_DIR}/pgo-bootstrap
   PGO_REBOOTSTRAP       force a retrain                 default OFF
   PGO_AUTO_REBOOTSTRAP  retrain when sources are newer than the profile  default ON
   PGO_FORWARD_CACHE     forward user cache variables to the sub-build    default ON
   PGO_FORWARD_VARS      extra variable names to forward default empty
   PGO_EXTRA_BOOTSTRAP_ARGS  extra -D arguments for the sub-build  default empty

 Targets provided:
   pgo-train   run every training program registered with pgo_add_training_target() and merge the profile
   pgo-merge   merge only the profraw files under ${PGO_PROFILE_DIR} -> ${PGO_PROFDATA}
   pgo-check   verify that llvm-profdata can read ${PGO_PROFDATA} and print a summary
   pgo-clean   delete profraw / profdata (for retraining, avoids stale data)

 Functions provided:
   pgo_apply(<target>...)                   effective for the given targets when PGO_SCOPE=TARGET
   pgo_add_training_target(NAME <name> EXECUTABLE <tgt> [COMMAND <args...>]
                           [REPEAT <n>] [WORKING_DIRECTORY <dir>] [ENV <K=V>...])
   pgo_status()                             print the current PGO configuration summary

 Notes: the module only injects flags and manages the profile lifecycle; it does
        not change the project structure.  All conclusions and failure modes are
        documented in REPORT.md in this directory; see USAGE-GUIDE.md for a guide.
============================================================================]]

include_guard(DIRECTORY)

if(CMAKE_VERSION VERSION_LESS 3.20)
  message(FATAL_ERROR "[pgo] requires CMake >= 3.20 (current: ${CMAKE_VERSION})")
endif()

set(_PGO_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(_PGO_SCRIPT "${_PGO_MODULE_DIR}/pgo-merge.cmake")

# ----------------------------------------------------------------------------
# 0. Input normalization
# ----------------------------------------------------------------------------
# ---- Convenience switch: PGO=ON/OFF is equivalent to PGO_MODE=AUTO/OFF ----
# Only applies when PGO_MODE was not set explicitly (normal CMakeLists variable
# / -D / cache); otherwise `option(PGO ... OFF)` or `set(PGO ON)` would
# override the user's explicit PGO_MODE.
# Sub-builds (bootstrap) suppress it with PGO_EMBEDDED so that a hard-coded
# `set(PGO ON)` in the project cannot recurse.
get_property(_pgo_mode_from_cache CACHE PGO_MODE PROPERTY TYPE)
if(DEFINED PGO AND NOT "${PGO}" STREQUAL "" AND NOT PGO_EMBEDDED
   AND "${_pgo_mode_from_cache}" STREQUAL ""
   AND NOT DEFINED PGO_MODE)
  if(PGO)
    set(PGO_MODE AUTO)
  else()
    set(PGO_MODE OFF)
  endif()
endif()

foreach(_v PGO_MODE PGO_KIND PGO_LTO PGO_SCOPE)
  if(NOT DEFINED ${_v})
    if(_v STREQUAL "PGO_MODE")
      set(PGO_MODE OFF)
    elseif(_v STREQUAL "PGO_KIND")
      set(PGO_KIND IR)
    elseif(_v STREQUAL "PGO_LTO")
      set(PGO_LTO OFF)
    else()
      set(PGO_SCOPE GLOBAL)
    endif()
  endif()
  string(TOUPPER "${${_v}}" ${_v})
  string(STRIP "${${_v}}" ${_v})
endforeach()

# Boolean shorthand for CMakeLists:
#   set(PGO_MODE ON)   -> AUTO      set(PGO_MODE OFF)   -> OFF
#   set(PGO_LTO ON)    -> THIN      set(PGO_LTO OFF)    -> OFF
#   set(PGO_SCOPE ON)  -> GLOBAL    set(PGO_SCOPE OFF)  -> TARGET
#   set(PGO_KIND ON)   -> IR        set(PGO_KIND OFF)   -> FE
# String values (AUTO/USE/GENERATE, THIN/FULL, GLOBAL/TARGET, IR/FE) are kept.
if(PGO_MODE STREQUAL "ON" OR PGO_MODE STREQUAL "TRUE" OR
   PGO_MODE STREQUAL "YES" OR PGO_MODE STREQUAL "Y" OR PGO_MODE STREQUAL "1")
  set(PGO_MODE AUTO)
elseif(PGO_MODE STREQUAL "OFF" OR PGO_MODE STREQUAL "FALSE" OR
       PGO_MODE STREQUAL "NO" OR PGO_MODE STREQUAL "N" OR PGO_MODE STREQUAL "0")
  set(PGO_MODE OFF)
endif()

if(PGO_LTO STREQUAL "ON" OR PGO_LTO STREQUAL "TRUE" OR
   PGO_LTO STREQUAL "YES" OR PGO_LTO STREQUAL "Y" OR PGO_LTO STREQUAL "1")
  set(PGO_LTO THIN)
elseif(PGO_LTO STREQUAL "OFF" OR PGO_LTO STREQUAL "FALSE" OR
       PGO_LTO STREQUAL "NO" OR PGO_LTO STREQUAL "N" OR PGO_LTO STREQUAL "0")
  set(PGO_LTO OFF)
endif()

if(PGO_SCOPE STREQUAL "ON" OR PGO_SCOPE STREQUAL "TRUE" OR
   PGO_SCOPE STREQUAL "YES" OR PGO_SCOPE STREQUAL "Y" OR PGO_SCOPE STREQUAL "1")
  set(PGO_SCOPE GLOBAL)
elseif(PGO_SCOPE STREQUAL "OFF" OR PGO_SCOPE STREQUAL "FALSE" OR
       PGO_SCOPE STREQUAL "NO" OR PGO_SCOPE STREQUAL "N" OR PGO_SCOPE STREQUAL "0")
  set(PGO_SCOPE TARGET)
endif()

if(PGO_KIND STREQUAL "ON" OR PGO_KIND STREQUAL "TRUE" OR
   PGO_KIND STREQUAL "YES" OR PGO_KIND STREQUAL "Y" OR PGO_KIND STREQUAL "1")
  set(PGO_KIND IR)
elseif(PGO_KIND STREQUAL "OFF" OR PGO_KIND STREQUAL "FALSE" OR
       PGO_KIND STREQUAL "NO" OR PGO_KIND STREQUAL "N" OR PGO_KIND STREQUAL "0")
  set(PGO_KIND FE)
endif()

foreach(_v PGO_STRICT PGO_ALLOW_ANY_LINKER PGO_VERBOSE PGO_KEEP_WARNINGS PGO_WERROR_OUT_OF_DATE)
  if(NOT DEFINED ${_v})
    if(_v STREQUAL "PGO_STRICT" OR _v STREQUAL "PGO_VERBOSE")
      set(${_v} ON)
    else()
      set(${_v} OFF)
    endif()
  endif()
endforeach()

# AUTO-mode switches
foreach(_v PGO_AUTO PGO_REBOOTSTRAP PGO_EMBEDDED)
  if(NOT DEFINED ${_v})
    set(${_v} OFF)
  endif()
endforeach()
foreach(_v PGO_AUTO_REBOOTSTRAP PGO_FORWARD_CACHE)
  if(NOT DEFINED ${_v})
    set(${_v} ON)
  endif()
endforeach()

if(NOT PGO_LINKER)
  set(PGO_LINKER lld)
endif()

# Fail fast on invalid values (catch them at configure time, not halfway
# through a build).
set(_pgo_valid_modes OFF AUTO GENERATE USE)
set(_pgo_valid_kinds IR FE)
set(_pgo_valid_ltos OFF THIN FULL)
set(_pgo_valid_scopes GLOBAL TARGET)
if(NOT PGO_MODE IN_LIST _pgo_valid_modes)
  message(FATAL_ERROR "[pgo] PGO_MODE='${PGO_MODE}' is invalid; expected one of: ${_pgo_valid_modes}")
endif()
if(NOT PGO_KIND IN_LIST _pgo_valid_kinds)
  message(FATAL_ERROR "[pgo] PGO_KIND='${PGO_KIND}' is invalid; expected one of: ${_pgo_valid_kinds}")
endif()
if(NOT PGO_LTO IN_LIST _pgo_valid_ltos)
  message(FATAL_ERROR "[pgo] PGO_LTO='${PGO_LTO}' is invalid; expected one of: ${_pgo_valid_ltos}")
endif()
if(NOT PGO_SCOPE IN_LIST _pgo_valid_scopes)
  message(FATAL_ERROR "[pgo] PGO_SCOPE='${PGO_SCOPE}' is invalid; expected one of: ${_pgo_valid_scopes}")
endif()

set(PGO_GENERATE FALSE)
set(PGO_USE FALSE)
set(PGO_ACTIVE FALSE)
if(PGO_MODE STREQUAL "AUTO")
  set(PGO_AUTO TRUE)
  set(PGO_ACTIVE TRUE)
elseif(PGO_MODE STREQUAL "GENERATE")
  set(PGO_GENERATE TRUE)
  set(PGO_ACTIVE TRUE)
elseif(PGO_MODE STREQUAL "USE")
  set(PGO_USE TRUE)
  set(PGO_ACTIVE TRUE)
endif()

# Normalize directory/file paths to forward slashes (required by
# CMake/CPM/FetchContent and clang on Windows).
if(NOT DEFINED PGO_PROFILE_DIR)
  set(PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo-profile")
endif()
# include() may run outside project(); CMAKE_BINARY_DIR is empty in that case.
get_filename_component(PGO_PROFILE_DIR "${PGO_PROFILE_DIR}" ABSOLUTE)
file(TO_CMAKE_PATH "${PGO_PROFILE_DIR}" PGO_PROFILE_DIR)

if(NOT DEFINED PGO_PROFDATA)
  set(PGO_PROFDATA "${PGO_PROFILE_DIR}/profile.profdata")
endif()
get_filename_component(PGO_PROFDATA "${PGO_PROFDATA}" ABSOLUTE)
file(TO_CMAKE_PATH "${PGO_PROFDATA}" PGO_PROFDATA)

if(CMAKE_CONFIGURATION_TYPES)
  message(WARNING
    "[pgo] multi-configuration generator detected "
    "(CMAKE_CONFIGURATION_TYPES=${CMAKE_CONFIGURATION_TYPES})."
    "This module was only tested with single-config generators (Ninja); "
    "verify that the flags are correct for every configuration.")
endif()

# ----------------------------------------------------------------------------
# 1. Completely inactive: PGO_MODE=OFF and PGO_LTO=OFF (default) -> record
#    state, inject nothing.
#    Note: PGO_MODE=OFF with PGO_LTO!=OFF is a legal combination
#    ("LTO only, no PGO"), so execution must continue to inject the LTO flags.
# ----------------------------------------------------------------------------
if(NOT PGO_ACTIVE AND PGO_LTO STREQUAL "OFF")
  # OFF mode: provide no-op functions so project code needs no conditional
  # wrappers.
  function(pgo_apply)
  endfunction()
  function(pgo_add_training_target)
  endfunction()
  function(pgo_status)
    message(STATUS "[pgo] PGO_MODE=OFF (not enabled)")
  endfunction()
  if(PGO_VERBOSE)
    message(STATUS "[pgo] not enabled (PGO_MODE=OFF, PGO_LTO=OFF): no compiler/linker flags injected")
  endif()
  return()
endif()

# ----------------------------------------------------------------------------
# 2. Compiler capability checks
# ----------------------------------------------------------------------------
set(_pgo_is_clang FALSE)
set(_pgo_is_gnu FALSE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_pgo_is_clang TRUE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_pgo_is_gnu TRUE)
else()
  message(FATAL_ERROR
    "[pgo] unsupported compiler '${CMAKE_CXX_COMPILER_ID}' "
    "(this module supports Clang and GNU; the MSVC /GL /GENPROFILE "
    "/USEPROFILE flow has not been tested, do not use it)")
endif()

if(_pgo_is_gnu AND PGO_KIND STREQUAL "IR")
  # GCC has no IRPGO/FEPGO distinction.
  set(PGO_KIND GNU)
endif()

# ----------------------------------------------------------------------------
# 2.1 LLVM archiver/ranlib must match the compiler when LTO produces bitcode.
#     CMake may auto-pick C:/Program Files/LLVM/bin/llvm-ar (e.g. LLVM 22)
#     while clang++ is LLVM 23.  llvm-ar then rejects LLVM 23 bitcode objects:
#       'benchmark.cc.obj': Not an int attribute
#       (Producer: 'LLVM23.1.5' Reader: 'LLVM 22.1.0')
#     Prefer the compiler's own bin directory, then sibling MSYS2 prefixes.
# ----------------------------------------------------------------------------
if(_pgo_is_clang AND (PGO_GENERATE OR PGO_USE OR NOT PGO_LTO STREQUAL "OFF"))
  get_filename_component(_pgo_cxx_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_pgo_cxx_prefix "${_pgo_cxx_bin}" DIRECTORY)
  get_filename_component(_pgo_msys_root "${_pgo_cxx_prefix}" DIRECTORY)
  if(WIN32)
    set(_pgo_tool_suffix ".exe")
  else()
    set(_pgo_tool_suffix "${CMAKE_EXECUTABLE_SUFFIX}")
  endif()
  set(_pgo_tool_dirs
      "${_pgo_cxx_bin}"
      "${_pgo_msys_root}/clang64/bin"
      "${_pgo_msys_root}/ucrt64/bin"
      "${_pgo_msys_root}/mingw64/bin")
  foreach(_pgo_tool IN ITEMS llvm-ar llvm-ranlib)
    set(_pgo_tool_path "")
    foreach(_pgo_dir ${_pgo_tool_dirs})
      if(EXISTS "${_pgo_dir}/${_pgo_tool}${_pgo_tool_suffix}")
        set(_pgo_tool_path "${_pgo_dir}/${_pgo_tool}${_pgo_tool_suffix}")
        break()
      endif()
    endforeach()
    if(_pgo_tool STREQUAL "llvm-ar" AND _pgo_tool_path)
      set(CMAKE_AR "${_pgo_tool_path}" CACHE FILEPATH
          "LLVM archiver matching the compiler for PGO/LTO" FORCE)
      set(CMAKE_AR "${_pgo_tool_path}")
    elseif(_pgo_tool STREQUAL "llvm-ranlib" AND _pgo_tool_path)
      set(CMAKE_RANLIB "${_pgo_tool_path}" CACHE FILEPATH
          "LLVM ranlib matching the compiler for PGO/LTO" FORCE)
      set(CMAKE_RANLIB "${_pgo_tool_path}")
    endif()
  endforeach()
  if(PGO_VERBOSE)
    message(STATUS "[pgo] archiver = ${CMAKE_AR}")
    message(STATUS "[pgo] ranlib   = ${CMAKE_RANLIB}")
  endif()
endif()

# Optimization-level check: PGO/LTO is pointless at -O0.  Only inspect the flags
# for the **current build type** (do not scan every configuration: Release's
# default -O3 would hide the fact that the Debug build is not optimized).
set(_pgo_cur_flags "${CMAKE_CXX_FLAGS}")
if(CMAKE_BUILD_TYPE)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _pgo_bt)
  if(DEFINED CMAKE_CXX_FLAGS_${_pgo_bt})
    set(_pgo_cur_flags "${_pgo_cur_flags} ${CMAKE_CXX_FLAGS_${_pgo_bt}}")
  endif()
endif()
if(NOT _pgo_cur_flags MATCHES "(^| )-O([1-3s]|fast)")
  message(WARNING
    "[pgo] no -O1/-O2/-O3 found in the compile flags for "
    "CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}' (current: '${_pgo_cur_flags}'); "
    "PGO/LTO will not help. Consider -DCMAKE_BUILD_TYPE=Release.")
endif()

# ----------------------------------------------------------------------------
# 3. Detecting the profile runtime (libclang_rt.profile) and llvm-profdata
#    Note the main pitfall on this machine: the ucrt64 prefix has neither
#    libclang_rt.profile-*.a nor llvm-profdata; both must come from clang64.
# ----------------------------------------------------------------------------
set(_pgo_resource_dir "")
set(_pgo_resource_dir_reason "")

if(_pgo_is_clang)
  # Ask the compiler for its own resource-dir first (needed even when the user
  # specified one explicitly, to decide whether -resource-dir is required).
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE _pgo_default_rd
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  file(TO_CMAKE_PATH "${_pgo_default_rd}" _pgo_default_rd)
  string(REGEX MATCH "clang/([0-9]+)" _m "${_pgo_default_rd}")
  set(_pgo_ver "${CMAKE_MATCH_1}")

  if(DEFINED PGO_RESOURCE_DIR AND NOT PGO_RESOURCE_DIR STREQUAL "")
    set(_pgo_resource_dir "${PGO_RESOURCE_DIR}")
    set(_pgo_resource_dir_reason "user-specified PGO_RESOURCE_DIR")
  else()
    # Does the compiler's own resource-dir contain the profile runtime?
    file(GLOB _pgo_rd_hits
      "${_pgo_default_rd}/lib/windows/libclang_rt.profile-*.a"
      "${_pgo_default_rd}/lib/libclang_rt.profile-*.a"
      "${_pgo_default_rd}/lib/*/libclang_rt.profile*.a")
    if(_pgo_rd_hits)
      set(_pgo_resource_dir "${_pgo_default_rd}")
      set(_pgo_resource_dir_reason "the compiler's own resource-dir already contains the profile runtime")
    else()
      # Fallback: look for a same-version resource-dir under sibling prefixes
      #   C:/msys64/ucrt64/bin/clang++ -> prefix C:/msys64/ucrt64 -> parent C:/msys64
      get_filename_component(_pgo_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
      get_filename_component(_pgo_prefix "${_pgo_bin_dir}" DIRECTORY)
      get_filename_component(_pgo_root "${_pgo_prefix}" DIRECTORY)
      set(_pgo_candidates "")
      if(_pgo_ver)
        file(GLOB _pgo_candidates
          "${_pgo_root}/*/lib/clang/${_pgo_ver}/lib/windows/libclang_rt.profile-*.a")
      endif()
      if(_pgo_candidates)
        list(GET _pgo_candidates 0 _pgo_hit)
        # .../<prefix>/lib/clang/<ver>/lib/windows/libclang_rt.profile-x86_64.a -> back to clang/<ver>
        string(REGEX REPLACE "/lib/(windows|[^/]+)/libclang_rt\\.profile[^/]*$" "" _pgo_cand_rd "${_pgo_hit}")
        set(_pgo_resource_dir "${_pgo_cand_rd}")
        set(_pgo_resource_dir_reason "the compiler resource-dir lacks the profile runtime; using ${_pgo_cand_rd}")
      else()
        message(FATAL_ERROR
          "[pgo] cannot find libclang_rt.profile-*.a under ${_pgo_default_rd} "
          "or its sibling prefixes (${_pgo_root}/*/lib/clang/${_pgo_ver}).\n"
          "      PGO needs the compiler runtime library libclang_rt.profile; "
          "install a prefix that contains compiler-rt (clang64 on MSYS2), or pass "
          "-DPGO_RESOURCE_DIR=<resource-dir containing lib/windows/libclang_rt.profile-*.a>.")
      endif()
    endif()
  endif()

  if(NOT EXISTS "${_pgo_resource_dir}/lib")
    message(FATAL_ERROR "[pgo] PGO_RESOURCE_DIR='${_pgo_resource_dir}' is not a valid clang resource-dir")
  endif()
  get_filename_component(_pgo_default_rd_norm "${_pgo_default_rd}" ABSOLUTE)
  get_filename_component(_pgo_resource_dir_norm "${_pgo_resource_dir}" ABSOLUTE)
  set(_pgo_need_resource_dir TRUE)
  if(_pgo_default_rd_norm STREQUAL _pgo_resource_dir_norm)
    set(_pgo_need_resource_dir FALSE)
  endif()
endif()

# llvm-profdata (not needed for GCC)
set(_pgo_profdata_tool "")
if(_pgo_is_clang)
  if(DEFINED PGO_PROFDATA_TOOL AND NOT PGO_PROFDATA_TOOL STREQUAL "")
    set(_pgo_profdata_tool "${PGO_PROFDATA_TOOL}")
  else()
    get_filename_component(_pgo_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_cand
        "${_pgo_bin_dir}/llvm-profdata${CMAKE_EXECUTABLE_SUFFIX}"
        "${_pgo_bin_dir}/../bin/llvm-profdata${CMAKE_EXECUTABLE_SUFFIX}")
      if(EXISTS "${_cand}")
        set(_pgo_profdata_tool "${_cand}")
        break()
      endif()
    endforeach()
    if(NOT _pgo_profdata_tool)
      # MSYS2 commonly uses a ucrt64 compiler while llvm-profdata lives in
      # clang64.  The compiler's own prefix and its sibling prefixes must be
      # searched; relying only on PATH fails in that setup.
      get_filename_component(_pgo_prof_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
      get_filename_component(_pgo_prof_prefix "${_pgo_prof_bin_dir}" DIRECTORY)
      get_filename_component(_pgo_prof_root "${_pgo_prof_prefix}" DIRECTORY)
      file(GLOB _pgo_profdata_candidates
        "${_pgo_prof_root}/*/bin/llvm-profdata${CMAKE_EXECUTABLE_SUFFIX}")
      if(_pgo_profdata_candidates)
        list(GET _pgo_profdata_candidates 0 _pgo_profdata_tool)
      endif()
    endif()
    if(NOT _pgo_profdata_tool AND _pgo_ver)
      # The prefix that owns the resource-dir (clang64) usually provides
      # llvm-profdata too.
      get_filename_component(_pgo_rd_prefix "${_pgo_resource_dir}/../../.." ABSOLUTE)
      if(EXISTS "${_pgo_rd_prefix}/bin/llvm-profdata${CMAKE_EXECUTABLE_SUFFIX}")
        set(_pgo_profdata_tool "${_pgo_rd_prefix}/bin/llvm-profdata${CMAKE_EXECUTABLE_SUFFIX}")
      endif()
    endif()
    if(NOT _pgo_profdata_tool)
      find_program(_pgo_profdata_tool NAMES llvm-profdata llvm-profdata.exe)
    endif()
  endif()
  if(NOT _pgo_profdata_tool)
    message(FATAL_ERROR
      "[pgo] cannot find llvm-profdata (required to merge PGO profiles). "
      "Pass -DPGO_PROFDATA_TOOL=<path> explicitly.\n"
      "      On this machine (MSYS2) it is usually at "
      "C:/msys64/clang64/bin/llvm-profdata.exe.")
  endif()
  file(TO_CMAKE_PATH "${_pgo_profdata_tool}" _pgo_profdata_tool)
endif()

# ----------------------------------------------------------------------------
# 3.5 AUTO mode: configure-time bootstrap
#     Performs "instrumented build + training + merge" in <build>/pgo-bootstrap,
#     then compiles this build with USE.
#     User perspective: cmake -S . -B build && cmake --build build yields the
#     optimized binaries.
#     Restriction: clang + a single-config generator (GCC's .gcda files are tied
#     to the build directory, so a separate bootstrap directory cannot work).
# ----------------------------------------------------------------------------
if(PGO_AUTO)
  if(NOT _pgo_is_clang)
    message(FATAL_ERROR
      "[pgo] PGO_MODE=AUTO only supports clang: GCC encodes the object file's "
      "build directory in the .gcda name,\n"
      "      so bootstrapping from a separate directory is impossible. "
      "For GCC use the manual three-stage flow (same build directory):\n"
      "        cmake -B build -DPGO_MODE=GENERATE && cmake --build build --target pgo-train\n"
      "        cmake -B build -DPGO_MODE=USE      && cmake --build build")
  endif()
  if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "[pgo] PGO_MODE=AUTO requires a single-config generator (Ninja / Makefiles)")
  endif()

  if(NOT PGO_BOOTSTRAP_DIR)
    set(PGO_BOOTSTRAP_DIR "${CMAKE_BINARY_DIR}/pgo-bootstrap")
  endif()
  file(TO_CMAKE_PATH "${PGO_BOOTSTRAP_DIR}" PGO_BOOTSTRAP_DIR)

  # ---- Decide whether a re-bootstrap is needed ----
  set(_pgo_need_boot FALSE)
  set(_pgo_boot_why "")
  if(PGO_REBOOTSTRAP)
    set(_pgo_need_boot TRUE)
    set(_pgo_boot_why "PGO_REBOOTSTRAP=ON")
  elseif(NOT EXISTS "${PGO_PROFDATA}")
    set(_pgo_need_boot TRUE)
    set(_pgo_boot_why "the profile does not exist yet")
  elseif(PGO_AUTO_REBOOTSTRAP)
    # Sources newer than the profile -> retrain automatically (disable with
    # -DPGO_AUTO_REBOOTSTRAP=OFF).
    file(GLOB_RECURSE _pgo_srcs LIST_DIRECTORIES false
      "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
      "${CMAKE_SOURCE_DIR}/*.c" "${CMAKE_SOURCE_DIR}/*.cc" "${CMAKE_SOURCE_DIR}/*.cpp"
      "${CMAKE_SOURCE_DIR}/*.cxx" "${CMAKE_SOURCE_DIR}/*.c++"
      "${CMAKE_SOURCE_DIR}/*.h" "${CMAKE_SOURCE_DIR}/*.hh" "${CMAKE_SOURCE_DIR}/*.hpp"
      "${CMAKE_SOURCE_DIR}/*.hxx" "${CMAKE_SOURCE_DIR}/*.cppm" "${CMAKE_SOURCE_DIR}/*.ixx"
      "${CMAKE_SOURCE_DIR}/*.mpp")
    # Note: `PATTERN ... EXCLUDE` proved unreliable with absolute-path
    # GLOB_RECURSE (some entries were not excluded), so filter explicitly and
    # keep only files inside the source tree that are not in build/dependency
    # directories.
    set(_pgo_srcs_kept "")
    foreach(_f ${_pgo_srcs})
      string(FIND "${_f}" "${CMAKE_BINARY_DIR}/" _pgo_pos_bin)
      if(_pgo_pos_bin EQUAL 0)
        continue()
      endif()
      if(_f MATCHES "[/\\\\](build[^/\\\\]*|out|\\.git|\\.vendor|_deps|\\.cpm-cache|CMakeFiles)[/\\\\]")
        continue()
      endif()
      list(APPEND _pgo_srcs_kept "${_f}")
    endforeach()
    set(_pgo_srcs "${_pgo_srcs_kept}")
    file(TIMESTAMP "${PGO_PROFDATA}" _pgo_pd_ts "%Y%m%d%H%M%S" UTC)
    foreach(_s ${_pgo_srcs})
      file(TIMESTAMP "${_s}" _pgo_s_ts "%Y%m%d%H%M%S" UTC)
      if(_pgo_s_ts GREATER _pgo_pd_ts)
        set(_pgo_need_boot TRUE)
        set(_pgo_boot_why "sources are newer than the profile (e.g. ${_s})")
        break()
      endif()
    endforeach()
    if(PGO_VERBOSE AND NOT _pgo_need_boot)
      message(STATUS "[pgo] AUTO: checked sources under ${CMAKE_SOURCE_DIR} (profile mtime=${_pgo_pd_ts})")
    endif()
  endif()

  if(_pgo_need_boot)
    message(STATUS "[pgo] AUTO: bootstrap required (${_pgo_boot_why})")
    message(STATUS "[pgo]   sub-build directory: ${PGO_BOOTSTRAP_DIR}")
    message(STATUS "[pgo]   steps: configure the instrumented build -> build + train + merge the profile (slow the first time)")

    # Remove profraw files from the previous run (otherwise old and new data
    # mix).  Automatic cleanup happens only inside this build directory.
    if(PGO_PROFILE_DIR MATCHES "^${CMAKE_BINARY_DIR}")
      file(REMOVE_RECURSE "${PGO_PROFILE_DIR}")
    else()
      file(GLOB _pgo_old_raws "${PGO_PROFILE_DIR}/*.profraw" "${PGO_PROFILE_DIR}/*/*.profraw")
      if(_pgo_old_raws)
        message(STATUS "[pgo]   note: PGO_PROFILE_DIR is outside the build "
                       "directory; make sure old profraw files do not mix in "
                       "(found ${_pgo_old_raws})")
      endif()
    endif()

    # Forward user cache variables to the sub-build (excluding CMAKE_/PGO_/internal).
    set(_pgo_forward "")
    if(PGO_FORWARD_CACHE)
      get_cmake_property(_pgo_cache_vars CACHE_VARIABLES)
      foreach(_cv ${_pgo_cache_vars})
        if(_cv MATCHES "^CMAKE_")
          if(NOT _cv MATCHES "^CMAKE_(PREFIX_PATH|TOOLCHAIN_FILE)$")
            continue()
          endif()
        endif()
        if(_cv MATCHES "^PGO_|^CPACK_|^CTEST_")
          continue()
        endif()
        get_property(_pgo_cv_type CACHE "${_cv}" PROPERTY TYPE)
        if(_pgo_cv_type STREQUAL "INTERNAL" OR _pgo_cv_type STREQUAL "STATIC")
          continue()
        endif()
        list(APPEND _pgo_forward "${_cv}")
      endforeach()
    endif()
    foreach(_v ${PGO_FORWARD_VARS})
      list(APPEND _pgo_forward "${_v}")
    endforeach()
    list(REMOVE_DUPLICATES _pgo_forward)

    set(_pgo_boot_args
      -S "${CMAKE_SOURCE_DIR}"
      -B "${PGO_BOOTSTRAP_DIR}"
      -DPGO_MODE=GENERATE
      -DPGO_EMBEDDED=ON
      "-DPGO_KIND=${PGO_KIND}"
      "-DPGO_LTO=${PGO_LTO}"
      "-DPGO_SCOPE=${PGO_SCOPE}"
      "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
      "-DPGO_PROFDATA=${PGO_PROFDATA}"
      "-DPGO_RESOURCE_DIR=${_pgo_resource_dir}"
      "-DPGO_PROFDATA_TOOL=${_pgo_profdata_tool}"
      "-DPGO_KEEP_WARNINGS=${PGO_KEEP_WARNINGS}"
      "-DPGO_VERBOSE=OFF")
    if(CMAKE_GENERATOR)
      list(APPEND _pgo_boot_args -G "${CMAKE_GENERATOR}")
    endif()
    if(CMAKE_MAKE_PROGRAM)
      list(APPEND _pgo_boot_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
    endif()
    if(CMAKE_BUILD_TYPE)
      list(APPEND _pgo_boot_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    endif()
    if(CMAKE_CXX_COMPILER)
      list(APPEND _pgo_boot_args "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    endif()
    if(CMAKE_C_COMPILER)
      list(APPEND _pgo_boot_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
    endif()
    foreach(_cv ${_pgo_forward})
      list(APPEND _pgo_boot_args "-D${_cv}=${${_cv}}")
    endforeach()
    if(PGO_EXTRA_BOOTSTRAP_ARGS)
      list(APPEND _pgo_boot_args ${PGO_EXTRA_BOOTSTRAP_ARGS})
    endif()

    set(_pgo_boot_cfg_log "${CMAKE_BINARY_DIR}/pgo-bootstrap-configure.log")
    set(_pgo_boot_build_log "${CMAKE_BINARY_DIR}/pgo-bootstrap-build.log")

    execute_process(COMMAND "${CMAKE_COMMAND}" ${_pgo_boot_args}
      RESULT_VARIABLE _pgo_rc OUTPUT_VARIABLE _pgo_out ERROR_VARIABLE _pgo_err)
    file(WRITE "${_pgo_boot_cfg_log}" "${_pgo_out}\n${_pgo_err}\n")
    if(NOT _pgo_rc EQUAL 0)
      message(FATAL_ERROR
        "[pgo] bootstrap configuration failed (rc=${_pgo_rc}). Full log: ${_pgo_boot_cfg_log}\n"
        "------ tail of the output ------\n${_pgo_err}")
    endif()

    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${PGO_BOOTSTRAP_DIR}" --target pgo-train
      RESULT_VARIABLE _pgo_rc2 OUTPUT_VARIABLE _pgo_out2 ERROR_VARIABLE _pgo_err2)
    file(WRITE "${_pgo_boot_build_log}" "${_pgo_out2}\n${_pgo_err2}\n")
    if(NOT _pgo_rc2 EQUAL 0)
      message(FATAL_ERROR
        "[pgo] bootstrap build/training failed (rc=${_pgo_rc2}). Full log: ${_pgo_boot_build_log}\n"
        "------ tail of the output ------\n${_pgo_err2}")
    endif()
    if(NOT EXISTS "${PGO_PROFDATA}")
      message(FATAL_ERROR
        "[pgo] the bootstrap ran but did not produce ${PGO_PROFDATA}.\n"
        "      Make sure the project registers a training workload with "
        "pgo_add_training_target() (training only happens when one is registered).")
    endif()
    file(SIZE "${PGO_PROFDATA}" _pgo_pd_bytes)
    math(EXPR _pgo_pd_kib "${_pgo_pd_bytes} / 1024")
    message(STATUS "[pgo] bootstrap complete: ${PGO_PROFDATA} (${_pgo_pd_kib} KiB) -> this build compiles with USE")
  else()
    message(STATUS "[pgo] AUTO: reusing the existing profile ${PGO_PROFDATA}")
  endif()

  set(PGO_AUTO TRUE)
  set(PGO_USE TRUE)
  set(PGO_GENERATE FALSE)
  set(PGO_ACTIVE TRUE)
endif()

# ----------------------------------------------------------------------------
# 4. USE mode: profile availability check (fails at configure time when strict)
# ----------------------------------------------------------------------------
if(PGO_USE)
  if(_pgo_is_clang)
    if(EXISTS "${PGO_PROFDATA}")
      file(SIZE "${PGO_PROFDATA}" _pgo_profdata_size)
      if(_pgo_profdata_size EQUAL 0)
        if(PGO_STRICT)
          message(FATAL_ERROR "[pgo] PGO_PROFDATA='${PGO_PROFDATA}' exists but is empty (0 bytes).")
        else()
          message(WARNING "[pgo] PGO_PROFDATA is empty; ignoring the profile (PGO_STRICT=OFF)")
          set(PGO_USE FALSE)
        endif()
      endif()
    else()
      if(PGO_STRICT)
        message(FATAL_ERROR
          "[pgo] PGO_MODE=USE needs a merged profile, but '${PGO_PROFDATA}' does not exist.\n"
          "      Run the GENERATE stage first: cmake --build <build-inst> --target pgo-train\n"
          "      or pass -DPGO_PROFDATA=<.../profile.profdata> explicitly "
          "(PGO_STRICT=OFF degrades to a normal build).")
      else()
        message(WARNING "[pgo] cannot find '${PGO_PROFDATA}'; degrading to a normal build (PGO_STRICT=OFF)")
        set(PGO_USE FALSE)
        if(NOT PGO_LTO)
          set(PGO_ACTIVE FALSE)
        endif()
      endif()
    endif()
  else()
    # GCC: the profile consists of .gcda files; a directory is enough.
    if(NOT IS_DIRECTORY "${PGO_PROFILE_DIR}")
      if(PGO_STRICT)
        message(FATAL_ERROR "[pgo] GCC PGO_MODE=USE requires the directory '${PGO_PROFILE_DIR}' (containing .gcda files).")
      else()
        message(WARNING "[pgo] cannot find '${PGO_PROFILE_DIR}'; degrading to a normal build (PGO_STRICT=OFF)")
        set(PGO_USE FALSE)
      endif()
    else()
      # GCC encodes the **directory of the object file** in the .gcda name
      # (measured: <dir>/C~/Users/.../<build-dir>/CMakeFiles#<target>.dir#<src>.gcda),
      # so GENERATE and USE must run in the same build directory (unlike clang,
      # which only needs a single profdata file).
      file(GLOB_RECURSE _pgo_gcda_files "${PGO_PROFILE_DIR}/*.gcda")
      list(LENGTH _pgo_gcda_files _pgo_ngcda)
      if(_pgo_ngcda EQUAL 0)
        if(PGO_STRICT)
          message(FATAL_ERROR
            "[pgo] no .gcda files under '${PGO_PROFILE_DIR}': run the GENERATE stage's pgo-train first.")
        else()
          message(WARNING "[pgo] no .gcda files under '${PGO_PROFILE_DIR}'; degrading to a normal build")
          set(PGO_USE FALSE)
        endif()
      else()
        set(_pgo_bdir_key "${CMAKE_BINARY_DIR}")
        string(REPLACE ":" "~" _pgo_bdir_key "${_pgo_bdir_key}")
        string(REPLACE "\\" "/" _pgo_bdir_key "${_pgo_bdir_key}")
        set(_pgo_gcda_hit FALSE)
        foreach(_f ${_pgo_gcda_files})
          string(REPLACE "\\" "/" _ff "${_f}")
          string(FIND "${_ff}" "${_pgo_bdir_key}" _pos)
          if(NOT _pos EQUAL -1)
            set(_pgo_gcda_hit TRUE)
            break()
          endif()
        endforeach()
        if(NOT _pgo_gcda_hit)
          message(WARNING
            "[pgo] GCC .gcda paths are tied to the object file locations: the current build directory '${CMAKE_BINARY_DIR}' has "
            "no matching .gcda under '${PGO_PROFILE_DIR}', so the profile will not take effect "
            "(the compiler will report -Wmissing-profile).\n"
            "      For GCC **run both stages in the same build directory**: first GENERATE + pgo-train, then reconfigure with PGO_MODE=USE and rebuild.")
        endif()
      endif()
    endif()
  endif()
endif()

# ----------------------------------------------------------------------------
# 5. Linker: both PGO instrumentation and LTO require lld (GNU ld failed in testing; see REPORT.md)
# ----------------------------------------------------------------------------
set(_pgo_need_lld FALSE)
if(PGO_GENERATE OR NOT PGO_LTO STREQUAL "OFF")
  set(_pgo_need_lld TRUE)
endif()

if(_pgo_need_lld AND _pgo_is_clang)
  foreach(_lt EXE SHARED MODULE)
    if(CMAKE_${_lt}_LINKER_FLAGS MATCHES "fuse-ld=")
      if(NOT CMAKE_${_lt}_LINKER_FLAGS MATCHES "fuse-ld=(lld|${PGO_LINKER})")
        if(NOT PGO_ALLOW_ANY_LINKER)
          message(FATAL_ERROR
            "[pgo] CMAKE_${_lt}_LINKER_FLAGS already contains a non-lld -fuse-ld.\n"
            "      PGO instrumentation (__profc_/__profd_ COMDAT) and LTO (bitcode) "
            "fail to link with GNU ld;"
            " lld is required.\n"
            "      If you really need another linker, set "
            "-DPGO_ALLOW_ANY_LINKER=ON explicitly (at your own risk).")
        else()
          message(WARNING "[pgo] using a non-lld linker; PGO/LTO will probably fail to link (PGO_ALLOW_ANY_LINKER=ON)")
        endif()
      endif()
    else()
      set(CMAKE_${_lt}_LINKER_FLAGS "${CMAKE_${_lt}_LINKER_FLAGS} -fuse-ld=${PGO_LINKER}")
    endif()
  endforeach()
endif()

# ----------------------------------------------------------------------------
# 6. Flag assembly
# ----------------------------------------------------------------------------
set(_pgo_compile_flags "")
set(_pgo_link_flags "")

if(PGO_GENERATE)
  if(_pgo_is_clang)
    if(PGO_KIND STREQUAL "IR")
      # IRPGO: needed for both compile and link (the link step pulls in
      # libclang_rt.profile).
      list(APPEND _pgo_compile_flags "-fprofile-generate=${PGO_PROFILE_DIR}")
      list(APPEND _pgo_link_flags "-fprofile-generate=${PGO_PROFILE_DIR}")
    else()
      list(APPEND _pgo_compile_flags "-fprofile-instr-generate")
      list(APPEND _pgo_link_flags "-fprofile-instr-generate")
    endif()
    if(_pgo_need_resource_dir)
      list(APPEND _pgo_compile_flags "-resource-dir=${_pgo_resource_dir}")
      list(APPEND _pgo_link_flags "-resource-dir=${_pgo_resource_dir}")
    endif()
  else()
    list(APPEND _pgo_compile_flags "-fprofile-generate=${PGO_PROFILE_DIR}" "-fprofile-update=prefer-atomic")
    list(APPEND _pgo_link_flags "-fprofile-generate=${PGO_PROFILE_DIR}")
  endif()
endif()

if(PGO_USE)
  if(_pgo_is_clang)
    if(PGO_KIND STREQUAL "IR")
      list(APPEND _pgo_compile_flags "-fprofile-use=${PGO_PROFDATA}")
    else()
      list(APPEND _pgo_compile_flags "-fprofile-instr-use=${PGO_PROFDATA}")
    endif()
    # Functions that were never trained produce -Wprofile-instr-unprofiled;
    # profile entries whose hash does not match the current code (for example a
    # target that did not take part in training) produce -Wbackend-plugin.
    # Both occur in the common "train the main program only" layout, and LLVM
    # itself recommends suppressing them with -Wno-backend-plugin (see REPORT.md).
    if(NOT PGO_KEEP_WARNINGS)
      list(APPEND _pgo_compile_flags "-Wno-profile-instr-unprofiled" "-Wno-backend-plugin")
    endif()
    if(PGO_WERROR_OUT_OF_DATE)
      list(APPEND _pgo_compile_flags "-Werror=profile-instr-out-of-date" "-Werror=backend-plugin")
    endif()
    if(_pgo_need_resource_dir)
      list(APPEND _pgo_compile_flags "-resource-dir=${_pgo_resource_dir}")
    endif()
  else()
    list(APPEND _pgo_compile_flags "-fprofile-use=${PGO_PROFILE_DIR}" "-fprofile-correction")
    # Targets that did not take part in training report -Wmissing-profile (the
    # GCC counterpart of clang's -Wbackend-plugin).
    if(NOT PGO_KEEP_WARNINGS)
      list(APPEND _pgo_compile_flags "-Wno-missing-profile")
    endif()
    if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
      list(APPEND _pgo_compile_flags "-fprofile-partial-training")
    endif()
  endif()
endif()

if(NOT PGO_LTO STREQUAL "OFF")
  if(_pgo_is_clang)
    if(PGO_LTO STREQUAL "THIN")
      list(APPEND _pgo_compile_flags "-flto=thin")
      list(APPEND _pgo_link_flags "-flto=thin")
    else()
      list(APPEND _pgo_compile_flags "-flto")
      list(APPEND _pgo_link_flags "-flto")
    endif()
  else()
    # GCC has no thin/full distinction: `-flto=thin` reports
    # "cc1plus: error: unrecognized argument to '-flto=' option: 'thin'"
    # (measured), so always use -flto.
    if(PGO_LTO STREQUAL "THIN")
      message(STATUS "[pgo] GCC does not support -flto=thin; using -flto instead (GCC has no thin/full distinction)")
    endif()
    list(APPEND _pgo_compile_flags "-flto")
    list(APPEND _pgo_link_flags "-flto")
  endif()
endif()

if(DEFINED PGO_EXTRA_COMPILE_FLAGS)
  list(APPEND _pgo_compile_flags ${PGO_EXTRA_COMPILE_FLAGS})
endif()
if(DEFINED PGO_EXTRA_LINK_FLAGS)
  list(APPEND _pgo_link_flags ${PGO_EXTRA_LINK_FLAGS})
endif()

# ----------------------------------------------------------------------------
# 7. Injection
# ----------------------------------------------------------------------------
if(PGO_SCOPE STREQUAL "GLOBAL")
  # Append to CMAKE_*_FLAGS so every target created before or after this
  # include is covered.
  foreach(_f ${_pgo_compile_flags})
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_f}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_f}")
  endforeach()
  foreach(_f ${_pgo_link_flags})
    foreach(_lt EXE SHARED MODULE)
      set(CMAKE_${_lt}_LINKER_FLAGS "${CMAKE_${_lt}_LINKER_FLAGS} ${_f}")
    endforeach()
  endforeach()
  set(_PGO_GLOBAL_COMPILE_FLAGS "${_pgo_compile_flags}" CACHE INTERNAL "pgo global compile flags")
  set(_PGO_GLOBAL_LINK_FLAGS "${_pgo_link_flags}" CACHE INTERNAL "pgo global link flags")
endif()

# ----------------------------------------------------------------------------
# 8. Functions
# ----------------------------------------------------------------------------

# pgo_apply(<target>...) -- applies to the given targets when PGO_SCOPE=TARGET;
# can also be called in GLOBAL scope (idempotent: flags are not added twice)
function(pgo_apply)
  if(ARGC EQUAL 0)
    message(FATAL_ERROR "[pgo] pgo_apply() needs at least one target")
  endif()
  if(PGO_SCOPE STREQUAL "GLOBAL")
    message(STATUS "[pgo] pgo_apply() is unnecessary with PGO_SCOPE=GLOBAL (all targets already get the flags through CMAKE_*_FLAGS)")
    return()
  endif()
  foreach(_t ${ARGN})
    if(NOT TARGET ${_t})
      message(FATAL_ERROR "[pgo] pgo_apply(): target '${_t}' does not exist (call it after add_library/add_executable)")
    endif()
    target_compile_options(${_t} PRIVATE ${_pgo_compile_flags})
    target_link_options(${_t} PRIVATE ${_pgo_link_flags})
  endforeach()
endfunction()

# pgo_add_training_target(NAME <name> EXECUTABLE <tgt> [COMMAND <args...>]
#                         [REPEAT <n>] [WORKING_DIRECTORY <dir>] [ENV <K=V>...])
function(pgo_add_training_target)
  cmake_parse_arguments(T "" "NAME;EXECUTABLE;REPEAT;WORKING_DIRECTORY" "COMMAND;ENV;INPUTS" ${ARGN})
  if(NOT T_NAME OR NOT T_EXECUTABLE)
    message(FATAL_ERROR "[pgo] pgo_add_training_target() requires NAME and EXECUTABLE")
  endif()
  if(NOT TARGET ${T_EXECUTABLE})
    message(FATAL_ERROR "[pgo] training executable target '${T_EXECUTABLE}' does not exist")
  endif()
  if(NOT T_REPEAT)
    set(T_REPEAT 1)
  endif()
  set(_wd "${CMAKE_CURRENT_BINARY_DIR}")
  if(T_WORKING_DIRECTORY)
    set(_wd "${T_WORKING_DIRECTORY}")
  endif()

  set(_cmds "")
  list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E make_directory "${PGO_PROFILE_DIR}")
  # On Windows the instrumented executable may need runtime DLLs from the
  # compiler prefix (e.g. ucrt64/bin: libstdc++/winpthread/absl).  Builds run
  # from IDEs or CMake may not have that directory in PATH, so add it
  # explicitly for the training process.  User-provided ENV entries are
  # appended afterwards and therefore win.
  set(_pgo_train_env "")
  if(WIN32)
    get_filename_component(_pgo_train_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    list(APPEND _pgo_train_env "PATH=${_pgo_train_compiler_dir}")
  endif()
  if(T_ENV)
    list(APPEND _pgo_train_env ${T_ENV})
  endif()
  # LLVM_PROFILE_FILE: the default IRPGO name overwrites itself in the same
  # directory (measured: 3 runs left only 1 .profraw), so always use %m
  # (signature) + %p (pid) to give every run its own file.
  # Note: `cmake -E env K=V <cmd>` must be a single COMMAND, not two.
  foreach(_i RANGE 1 ${T_REPEAT})
    if(_pgo_is_clang)
      list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E env
        "LLVM_PROFILE_FILE=${PGO_PROFILE_DIR}/%m_%p.profraw"
        ${_pgo_train_env}
        "$<TARGET_FILE:${T_EXECUTABLE}>" ${T_COMMAND})
    else()
      list(APPEND _cmds COMMAND
        "$<TARGET_FILE:${T_EXECUTABLE}>" ${T_COMMAND})
    endif()
  endforeach()

  add_custom_target(pgo-run-${T_NAME}
    ${_cmds}
    DEPENDS ${T_EXECUTABLE} ${T_INPUTS}
    WORKING_DIRECTORY "${_wd}"
    COMMENT "[pgo] running training program ${T_NAME} (x${T_REPEAT}), profile -> ${PGO_PROFILE_DIR}"
    USES_TERMINAL
    VERBATIM)
  add_dependencies(pgo-train pgo-run-${T_NAME})
endfunction()

function(pgo_status)
  message(STATUS "[pgo] ---- configuration summary ----")
  if(PGO_AUTO)
    message(STATUS "[pgo]  mode: AUTO (configure-time bootstrap + USE in this build); bootstrap dir = ${PGO_BOOTSTRAP_DIR}")
  elseif(PGO_MODE STREQUAL "OFF" AND NOT PGO_LTO STREQUAL "OFF")
    message(STATUS "[pgo]  mode: LTO only (PGO_MODE=OFF, PGO_LTO=${PGO_LTO})")
  endif()
  message(STATUS "[pgo]  PGO_MODE        = ${PGO_MODE}   PGO_KIND = ${PGO_KIND}   PGO_LTO = ${PGO_LTO}")
  if(_pgo_is_clang)
    set(_pgo_link_disp "${PGO_LINKER}")
  else()
    set(_pgo_link_disp "(compiler default linker)")
  endif()
  message(STATUS "[pgo]  PGO_SCOPE       = ${PGO_SCOPE}  link = ${_pgo_link_disp}")
  message(STATUS "[pgo]  PROFILE_DIR     = ${PGO_PROFILE_DIR}")
  if(_pgo_is_clang)
    message(STATUS "[pgo]  PROFDATA        = ${PGO_PROFDATA}")
    message(STATUS "[pgo]  llvm-profdata   = ${_pgo_profdata_tool}")
    message(STATUS "[pgo]  resource-dir    = ${_pgo_resource_dir}  (${_pgo_resource_dir_reason})")
  endif()
  message(STATUS "[pgo]  compile flags   = ${_pgo_compile_flags}")
  if(_pgo_link_flags)
    message(STATUS "[pgo]  link flags      = ${_pgo_link_flags}")
  endif()
  message(STATUS "[pgo] --------------------")
endfunction()

# ----------------------------------------------------------------------------
# 9. Targets: train / merge / check / clean
# ----------------------------------------------------------------------------
if(_pgo_is_clang)
  # pgo-train: merge automatically after all registered training programs run
  # (one command completes the GENERATE stage).
  add_custom_target(pgo-train
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            "-DPGO_PROFDATA=${PGO_PROFDATA}"
            "-DPGO_PROFDATA_TOOL=${_pgo_profdata_tool}"
            -P "${_PGO_SCRIPT}"
    COMMENT "[pgo] training complete; merging profraw -> ${PGO_PROFDATA}"
    VERBATIM)

  # Merge only (used when training already ran separately).
  add_custom_target(pgo-merge
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            "-DPGO_PROFDATA=${PGO_PROFDATA}"
            "-DPGO_PROFDATA_TOOL=${_pgo_profdata_tool}"
            -P "${_PGO_SCRIPT}"
    COMMENT "[pgo] merging profraw -> ${PGO_PROFDATA}"
    VERBATIM)

  add_custom_target(pgo-check
    COMMAND "${_pgo_profdata_tool}" show --detailed-summary "${PGO_PROFDATA}"
    COMMENT "[pgo] checking the profile (llvm-profdata show --detailed-summary)"
    VERBATIM)
else()
  # GCC: .gcda files are usable as-is, no merge needed.
  add_custom_target(pgo-train
    COMMENT "[pgo] training complete (GCC: no merge needed; .gcda files are in ${PGO_PROFILE_DIR})")
  add_custom_target(pgo-merge
    COMMENT "[pgo] GCC .gcda files need no merge (pgo-merge is a no-op)")
  add_custom_target(pgo-check
    COMMENT "[pgo] in GCC mode inspect the .gcda files under ${PGO_PROFILE_DIR} directly")
endif()

add_custom_target(pgo-clean
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${PGO_PROFILE_DIR}"
  COMMENT "[pgo] removing ${PGO_PROFILE_DIR}"
  VERBATIM)

if(PGO_VERBOSE)
  pgo_status()
  if(PGO_GENERATE)
    message(STATUS "[pgo] hint: at the GENERATE stage use `cmake --build <dir> --target pgo-train` to trigger training and merging")
  elseif(PGO_AUTO)
    message(STATUS "[pgo] hint: in AUTO mode just run `cmake --build <dir>`; "
                   "add -DPGO_REBOOTSTRAP=ON when reconfiguring to retrain")
  endif()
endif()
