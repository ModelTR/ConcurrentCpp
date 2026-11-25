#[[============================================================================
 pgo.cmake -- production-ready PGO (Profile-Guided Optimization) + LTO
             integration module
----------------------------------------------------------------------------
 Project scope: Windows-first.  Supported toolchains:
   MSYS2 UCRT64 clang++  (GNU target + libstdc++) + Ninja
   MSYS2 UCRT64 g++      (GCC PGO: -fprofile-generate/-fprofile-use) + Ninja
   MSYS2 clang64/mingw64 clang++ (tool locations are discovered, not assumed)
   Standalone LLVM (clang++ + llvm-profdata + ld.lld in the same bin directory)
   MSVC cl (VS 2019+) + Ninja: /GL + /LTCG, with the linker-side
   /GENPROFILE and /USEPROFILE switches.
   Not supported: clang-cl (no MSVC-target profile runtime in this toolchain).

 Tool discovery is path-agnostic: hints are derived from the compiler location,
 MSYSTEM_PREFIX / MINGW_PREFIX, CMAKE_PREFIX_PATH and PATH.  Override with
 PGO_LLVM_BIN_DIR, PGO_PROFDATA_TOOL or PGO_LLD when the tools live elsewhere.

 Usage (out of the box, recommended):

   # Add two lines to the main CMakeLists.txt:
   #   option(PGO "enable automatic PGO+LTO builds (configure-time bootstrap)" ON)
   #   include(<workspace>/cmake/pgo.cmake)
   #
   # Then build like any other project (a single build command produces the
   # optimized binaries):
   cmake -S proj -B build -G Ninja -DCMAKE_CXX_COMPILER=<msys2>/ucrt64/bin/clang++.exe
   cmake --build build
   #
   # The first AUTO configure runs an instrumented build in <build>/pgo-bootstrap,
   # trains it, merges the profile, and then compiles this build with USE.
   # Later configures only verify that the profile can be reused; when sources
   # are newer, PGO_AUTO_REBOOTSTRAP (default ON) retrains automatically.

 Usage (manual two-stage flow, for fine-grained control):

   # 1) Instrumented build + training + profile merge
   cmake -S proj -B build-inst -G Ninja \
         -DCMAKE_CXX_COMPILER=<msys2>/ucrt64/bin/clang++.exe \
         -DPGO_MODE=GENERATE -DPGO_LTO=THIN \
         -DPGO_PROFILE_DIR=<absolute path>/profile
   cmake --build build-inst                     # build the instrumented program
   cmake --build build-inst --target pgo-train  # run the training workload + merge profdata

   # 2) Build the optimized version with the profile
   cmake -S proj -B build-opt -G Ninja \
         -DCMAKE_CXX_COMPILER=<msys2>/ucrt64/bin/clang++.exe \
         -DPGO_MODE=USE -DPGO_LTO=THIN \
         -DPGO_PROFILE_DIR=<absolute path>/profile
   cmake --build build-opt

 Main variables (all can be passed with -D):
   PGO               convenience switch ON/OFF        equivalent to PGO_MODE=AUTO/OFF
   PGO_MODE          OFF | AUTO | GENERATE | USE   default OFF
                     AUTO = configure-time bootstrap + USE in this build (out of the box)
   PGO_KIND          IR | FE                      default IR (clang: IRPGO is better than FEPGO)
                     MSVC: ignored (native /GENPROFILE flow, reported as MSVC)
                     GCC: reported as GNU (no IR/FE split)
   PGO_LTO           OFF | THIN | FULL            default OFF
                     MSVC/GCC: THIN is treated as FULL (no thin LTO)
   PGO_SCOPE         GLOBAL | TARGET              default GLOBAL
   PGO_PROFILE_DIR   profile data directory       default ${CMAKE_BINARY_DIR}/pgo-profile
   PGO_PROFDATA      merged profile file          default ${PGO_PROFILE_DIR}/profile.profdata
                     MSVC: ignored, each training executable uses
                     ${PGO_PROFILE_DIR}/<target>.pgd
                     GCC: ignored, uses the .gcda files under PGO_PROFILE_DIR
   PGO_PROFDATA_TOOL llvm-profdata executable     auto-detected (clang only)
   PGO_RESOURCE_DIR  clang resource-dir           auto-detected (clang only)
   PGO_LLVM_BIN_DIR  directory with llvm-* tools  default empty (derived from the compiler)
   PGO_LLD           path to ld.lld                auto-detected (clang; required on Windows)
   PGO_LINKER        linker for the PGO/LTO stage Windows: lld; elsewhere: compiler default
   PGO_GCC_PREFIX_PATH  GCC -fprofile-prefix-path default ${CMAKE_BINARY_DIR} (GCC only)
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
   pgo-merge   clang: merge profraw -> profdata; MSVC: check .pgd/.pgc; GCC: validate .gcda
   pgo-check   print a profile summary for the active toolchain
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

if(CMAKE_SCRIPT_MODE_FILE)
  # Script-mode entry point: cmake -P <this file> with the -D variables
  # documented below.  The merge logic used to live in pgo-merge.cmake; it
  # is inlined here so the PGO integration stays a single file.
  if(PGO_MERGE_KIND STREQUAL "MSVC")
    # MSVC: link.exe merges <pgd>!*.pgc when /USEPROFILE:PGD=<pgd> is used.
    # This branch only validates the data and prints a pgomgr summary.
    if(NOT IS_DIRECTORY "${PGO_PROFILE_DIR}")
      message(FATAL_ERROR "[pgo-merge] profile directory does not exist: ${PGO_PROFILE_DIR}")
    endif()
    file(GLOB _pgds "${PGO_PROFILE_DIR}/*.pgd")
    list(LENGTH _pgds _npgds)
    if(_npgds EQUAL 0)
      message(FATAL_ERROR
        "[pgo-merge] no .pgd files found under ${PGO_PROFILE_DIR}.\n"
        "      At the GENERATE stage, build and run the training program first "
        "(target: pgo-train).")
    endif()
    file(GLOB _pgcs "${PGO_PROFILE_DIR}/*.pgc")
    list(LENGTH _pgcs _npgcs)
    if(_npgcs EQUAL 0)
      message(WARNING
        "[pgo-merge] no .pgc files found under ${PGO_PROFILE_DIR}; "
        "the training programs probably did not run")
    endif()
    if(PGO_PGOMGR AND EXISTS "${PGO_PGOMGR}")
      foreach(_pgd ${_pgds})
        get_filename_component(_pgd_base "${_pgd}" NAME_WE)
        # The MSVC runtime writes <pgd-basename>!N.pgc.  Merge them into the
        # .pgd and remove them, so a later /USEPROFILE link only reads the
        # .pgd (pgomgr also guards against double-merging, but deleting keeps
        # the directory clean and makes retraining explicit).
        file(GLOB _pgc_for "${PGO_PROFILE_DIR}/${_pgd_base}!*.pgc")
        if(_pgc_for)
          list(LENGTH _pgc_for _n_for)
          execute_process(
            COMMAND "${PGO_PGOMGR}" /merge ${_pgc_for} "${_pgd}"
            RESULT_VARIABLE _mrc OUTPUT_VARIABLE _mout ERROR_VARIABLE _merr)
          if(_mrc EQUAL 0)
            file(REMOVE ${_pgc_for})
            message(STATUS "[pgo-merge] merged ${_n_for} .pgc -> ${_pgd}")
          else()
            message(WARNING "[pgo-merge] pgomgr /merge failed for ${_pgd} (rc=${_mrc}): ${_merr}")
          endif()
        endif()
        execute_process(
          COMMAND "${PGO_PGOMGR}" /summary "${_pgd}"
          RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        if(_rc EQUAL 0)
          # Print only the three aggregate lines; the full pgomgr output lists
          # every function (hundreds of lines) and is not useful in a build log.
          string(REPLACE "\r" "" _out "${_out}")
          string(REPLACE "\n" ";" _lines "${_out}")
          set(_pgo_summary "")
          foreach(_line ${_lines})
            if(_line MATCHES "Function Count|Static instructions|Dynamic instructions")
              string(APPEND _pgo_summary "${_line}\n")
            endif()
          endforeach()
          message(STATUS "[pgo-merge] ${_pgd}:\n${_pgo_summary}")
        else()
          message(WARNING "[pgo-merge] pgomgr /summary failed for ${_pgd} (rc=${_rc}): ${_err}")
        endif()
      endforeach()
    else()
      message(STATUS "[pgo-merge] pgomgr not found; skipping the profile summary")
    endif()
    file(GLOB _pgcs "${PGO_PROFILE_DIR}/*.pgc")
    list(LENGTH _pgcs _npgcs)
    message(STATUS "[pgo-merge] MSVC: ${_npgds} .pgd / ${_npgcs} .pgc under ${PGO_PROFILE_DIR}")
    return()
  endif()

  if(PGO_MERGE_KIND STREQUAL "GCC")
    # GCC: .gcda files are consumed directly by -fprofile-use.  Validate them
    # and report the count/size; there is nothing to merge.
    if(NOT IS_DIRECTORY "${PGO_PROFILE_DIR}")
      message(FATAL_ERROR "[pgo-merge] profile directory does not exist: ${PGO_PROFILE_DIR}")
    endif()
    file(GLOB_RECURSE _gcdas "${PGO_PROFILE_DIR}/*.gcda")
    list(LENGTH _gcdas _ngcdas)
    if(_ngcdas EQUAL 0)
      message(FATAL_ERROR
        "[pgo-merge] no .gcda files found under ${PGO_PROFILE_DIR}.\n"
        "      At the GENERATE stage, build and run the training program first "
        "(target: pgo-train).")
    endif()
    set(_bytes 0)
    foreach(_f ${_gcdas})
      file(SIZE "${_f}" _size)
      math(EXPR _bytes "${_bytes} + ${_size}")
    endforeach()
    list(GET _gcdas 0 _first)
    math(EXPR _kib "${_bytes} / 1024")
    message(STATUS
      "[pgo-merge] GCC: ${_ngcdas} .gcda file(s), ${_bytes} B (~${_kib} KiB); "
      "first: ${_first}")
    message(STATUS
      "[pgo-merge] GCC .gcda files are read directly by -fprofile-use; no merge step")
    return()
  endif()

  if(NOT DEFINED PGO_PROFILE_DIR OR "${PGO_PROFILE_DIR}" STREQUAL "")
    message(FATAL_ERROR "[pgo-merge] missing PGO_PROFILE_DIR")
  endif()
  if(NOT DEFINED PGO_PROFDATA OR "${PGO_PROFDATA}" STREQUAL "")
    message(FATAL_ERROR "[pgo-merge] missing PGO_PROFDATA")
  endif()
  if(NOT DEFINED PGO_PROFDATA_TOOL OR "${PGO_PROFDATA_TOOL}" STREQUAL "")
    message(FATAL_ERROR "[pgo-merge] missing PGO_PROFDATA_TOOL")
  endif()

  if(NOT EXISTS "${PGO_PROFDATA_TOOL}")
    message(FATAL_ERROR "[pgo-merge] llvm-profdata not found: ${PGO_PROFDATA_TOOL}")
  endif()

  if(NOT IS_DIRECTORY "${PGO_PROFILE_DIR}")
    message(FATAL_ERROR
      "[pgo-merge] profile directory does not exist: ${PGO_PROFILE_DIR}\n"
      "      At the GENERATE stage, build and run the training program first "
      "(target: pgo-train).")
  endif()

  file(GLOB_RECURSE _raws "${PGO_PROFILE_DIR}/*.profraw")
  list(LENGTH _raws _nraws)
  if(_nraws EQUAL 0)
    message(FATAL_ERROR
      "[pgo-merge] no .profraw files found under ${PGO_PROFILE_DIR}.\n"
      "      Possible causes: the training program did not actually run "
      "(pgo-train),\n"
      "      was interrupted (an abnormal exit does not flush profiles),\n"
      "      or LLVM_PROFILE_FILE pointed at another directory.")
  endif()

  get_filename_component(_out_dir "${PGO_PROFDATA}" DIRECTORY)
  file(MAKE_DIRECTORY "${_out_dir}")

  set(_batch 64)   # at most 64 files per batch, staying under the Windows command-line limit
  set(_extra ${PGO_MERGE_EXTRA_ARGS})

  function(_pgo_merge_run)
    # Arguments: <output file> <input files...>
    set(_args ${ARGN})
    list(GET _args 0 _out)
    list(REMOVE_AT _args 0)
    execute_process(
      COMMAND "${PGO_PROFDATA_TOOL}" merge ${_extra} -o "${_out}" ${_args}
      RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "[pgo-merge] llvm-profdata merge failed (rc=${_rc}):\n${_e}")
    endif()
  endfunction()

  if(_nraws LESS_EQUAL _batch)
    _pgo_merge_run("${PGO_PROFDATA}" ${_raws})
  else()
    # Batching -> two-level merge
    set(_parts "")
    set(_i 0)
    while(_i LESS _nraws)
      list(SUBLIST _raws ${_i} ${_batch} _chunk)
      set(_part "${_out_dir}/.pgo-part-${_i}.profdata")
      _pgo_merge_run("${_part}" ${_chunk})
      list(APPEND _parts "${_part}")
      math(EXPR _i "${_i} + ${_batch}")
    endwhile()
    _pgo_merge_run("${PGO_PROFDATA}" ${_parts})
    foreach(_p ${_parts})
      file(REMOVE "${_p}")
    endforeach()
  endif()

  if(NOT EXISTS "${PGO_PROFDATA}")
    message(FATAL_ERROR "[pgo-merge] did not produce ${PGO_PROFDATA}")
  endif()
  file(SIZE "${PGO_PROFDATA}" _size)
  if(_size EQUAL 0)
    message(FATAL_ERROR "[pgo-merge] generated ${PGO_PROFDATA} is empty")
  endif()

  math(EXPR _kb "${_size} / 1024")
  message(STATUS "[pgo-merge] merged ${_nraws} .profraw files -> ${PGO_PROFDATA} (${_size} B ≈ ${_kb} KiB)")
  return()
endif()

include_guard(DIRECTORY)

if(CMAKE_VERSION VERSION_LESS 3.20)
  message(FATAL_ERROR "[pgo] requires CMake >= 3.20 (current: ${CMAKE_VERSION})")
endif()

set(_PGO_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

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

# Optional toolchain hints (all empty by default: the module discovers them).
foreach(_v PGO_LINKER PGO_LLVM_BIN_DIR PGO_LLD PGO_PROFDATA_TOOL PGO_GCC_PREFIX_PATH)
  if(NOT DEFINED ${_v})
    set(${_v} "")
  endif()
endforeach()

# Fail fast on invalid values (catch them at configure time, not halfway
# through a build).
set(_pgo_valid_modes OFF AUTO GENERATE USE)
# MSVC and GNU are internal values assigned after compiler detection; accept
# them as well so an AUTO sub-build can forward the already-normalized kind.
set(_pgo_valid_kinds IR FE MSVC GNU)
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
set(_pgo_is_msvc FALSE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  if(CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    message(FATAL_ERROR
      "[pgo] clang-cl is not supported: the clang-cl driver in this toolchain "
      "has no MSVC-target profile runtime (link fails with LNK1181: cannot "
      "open 'clang_rt.profile.lib').\n"
      "      Use GNU clang (clang++ with libstdc++) or MSVC cl instead.")
  endif()
  set(_pgo_is_clang TRUE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_pgo_is_gnu TRUE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(_pgo_is_msvc TRUE)
else()
  message(FATAL_ERROR
    "[pgo] unsupported compiler '${CMAKE_CXX_COMPILER_ID}' "
    "(this module supports Clang, GNU and MSVC)")
endif()

if(_pgo_is_clang AND (PGO_KIND STREQUAL "MSVC" OR PGO_KIND STREQUAL "GNU"))
  message(FATAL_ERROR
    "[pgo] PGO_KIND='${PGO_KIND}' is an internal value and does not match clang; "
    "use IR or FE")
endif()

if(_pgo_is_gnu)
  # GCC has no IRPGO/FEPGO distinction.
  set(PGO_KIND GNU)
endif()

if(_pgo_is_msvc)
  # MSVC has a single profile format; /GENPROFILE and /USEPROFILE are
  # linker-side switches (measured with VS 2026 / link 14.52: cl itself
  # rejects /GENPROFILE with D9002, while link.exe accepts it).
  if(PGO_KIND STREQUAL "FE")
    message(STATUS "[pgo] MSVC has no FEPGO/IRPGO split; PGO_KIND=FE is ignored")
  endif()
  set(PGO_KIND MSVC)
  if(PGO_LTO STREQUAL "THIN")
    message(STATUS "[pgo] MSVC has no thin LTO; PGO_LTO=THIN is treated as FULL (/LTCG)")
    set(PGO_LTO FULL)
  endif()
endif()

# ----------------------------------------------------------------------------
# 2.0 Resolve the compiler location and build a portable list of tool hints.
#     CMAKE_CXX_COMPILER is normally absolute, but users may pass a bare
#     program name (g++, clang++, cl).  Never assume a fixed MSYS2 root:
#     derive candidates from the compiler, the active MSYS2 prefix
#     (MSYSTEM_PREFIX / MINGW_PREFIX), CMAKE_PREFIX_PATH and PATH.
# ----------------------------------------------------------------------------
set(_pgo_cxx_path "${CMAKE_CXX_COMPILER}")
if(NOT IS_ABSOLUTE "${_pgo_cxx_path}")
  find_program(_pgo_cxx_resolved NAMES "${CMAKE_CXX_COMPILER}")
  if(_pgo_cxx_resolved)
    set(_pgo_cxx_path "${_pgo_cxx_resolved}")
  endif()
endif()
get_filename_component(_pgo_cxx_bin "${_pgo_cxx_path}" DIRECTORY)
get_filename_component(_pgo_cxx_prefix "${_pgo_cxx_bin}" DIRECTORY)
get_filename_component(_pgo_msys_root "${_pgo_cxx_prefix}" DIRECTORY)

set(_pgo_tool_dirs "")
foreach(_pgo_dir IN ITEMS
    "${PGO_LLVM_BIN_DIR}"
    "${_pgo_cxx_bin}"
    "$ENV{MSYSTEM_PREFIX}/bin"
    "$ENV{MINGW_PREFIX}/bin")
  if(_pgo_dir AND NOT "${_pgo_dir}" STREQUAL "/bin")
    list(APPEND _pgo_tool_dirs "${_pgo_dir}")
  endif()
endforeach()
if(_pgo_msys_root)
  foreach(_pgo_prefix IN ITEMS ucrt64 clang64 mingw64)
    list(APPEND _pgo_tool_dirs "${_pgo_msys_root}/${_pgo_prefix}/bin")
  endforeach()
endif()
foreach(_pgo_prefix_path IN LISTS CMAKE_PREFIX_PATH)
  if(_pgo_prefix_path)
    list(APPEND _pgo_tool_dirs "${_pgo_prefix_path}/bin")
  endif()
endforeach()
list(REMOVE_DUPLICATES _pgo_tool_dirs)

# find_program() wrapper: searches the portable hints first, then PATH.
function(_pgo_find_tool out)
  unset(${out} CACHE)
  unset(${out})
  find_program(${out} NAMES ${ARGN} HINTS ${_pgo_tool_dirs})
  if(${out})
    file(TO_CMAKE_PATH "${${out}}" _pgo_tool_path)
    set(${out} "${_pgo_tool_path}" CACHE FILEPATH "" FORCE)
    set(${out} "${_pgo_tool_path}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

# GCC profile paths: GCC >= 9 can strip the build-directory prefix from the
# .gcda names, which makes a profile reusable across build directories and
# enables AUTO.  The prefix must use the platform's native separators.
if(_pgo_is_gnu)
  if(NOT PGO_GCC_PREFIX_PATH AND NOT "${CMAKE_BINARY_DIR}" STREQUAL "")
    set(PGO_GCC_PREFIX_PATH "${CMAKE_BINARY_DIR}")
  endif()
  if(PGO_GCC_PREFIX_PATH)
    get_filename_component(PGO_GCC_PREFIX_PATH "${PGO_GCC_PREFIX_PATH}" ABSOLUTE)
    file(TO_NATIVE_PATH "${PGO_GCC_PREFIX_PATH}" _pgo_gcc_prefix_native)
  else()
    set(_pgo_gcc_prefix_native "")
  endif()
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    message(STATUS
      "[pgo] GCC ${CMAKE_CXX_COMPILER_VERSION} does not support "
      "-fprofile-prefix-path; GCC profile reuse requires the same build directory")
  endif()
endif()

# ----------------------------------------------------------------------------
# 2.1 LLVM archiver/ranlib must match the compiler when LTO produces bitcode.
#     CMake may auto-pick a different LLVM installation's llvm-ar; prefer the
#     compiler's own llvm-ar/llvm-ranlib when available.
# ----------------------------------------------------------------------------
if(_pgo_is_clang AND (PGO_GENERATE OR PGO_USE OR NOT PGO_LTO STREQUAL "OFF"))
  _pgo_find_tool(_pgo_llvm_ar "llvm-ar" "llvm-ar.exe")
  _pgo_find_tool(_pgo_llvm_ranlib "llvm-ranlib" "llvm-ranlib.exe")
  if(_pgo_llvm_ar)
    set(CMAKE_AR "${_pgo_llvm_ar}" CACHE FILEPATH
        "LLVM archiver matching the compiler for PGO/LTO" FORCE)
    set(CMAKE_AR "${_pgo_llvm_ar}")
  endif()
  if(_pgo_llvm_ranlib)
    set(CMAKE_RANLIB "${_pgo_llvm_ranlib}" CACHE FILEPATH
        "LLVM ranlib matching the compiler for PGO/LTO" FORCE)
    set(CMAKE_RANLIB "${_pgo_llvm_ranlib}")
  endif()
  if(PGO_VERBOSE)
    message(STATUS "[pgo] archiver = ${CMAKE_AR}")
    message(STATUS "[pgo] ranlib   = ${CMAKE_RANLIB}")
  endif()
endif()

# lld detection: required on Windows/clang for PGO instrumentation and LTO;
# elsewhere the compiler's default linker is usually fine.
set(_pgo_lld "")
if(_pgo_is_clang)
  if(PGO_LLD)
    set(_pgo_lld "${PGO_LLD}")
    file(TO_CMAKE_PATH "${_pgo_lld}" _pgo_lld)
  else()
    _pgo_find_tool(_pgo_lld "ld.lld" "ld.lld.exe")
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
if(_pgo_is_msvc)
  if(NOT _pgo_cur_flags MATCHES "(^| )[/-](O1|O2|Ox)")
    message(WARNING
      "[pgo] no /O1, /O2 or /Ox found in the compile flags for "
      "CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}' (current: '${_pgo_cur_flags}'); "
      "PGO/LTO will not help. Consider -DCMAKE_BUILD_TYPE=Release.")
  endif()
elseif(NOT _pgo_cur_flags MATCHES "(^| )-O([1-3s]|fast)")
  message(WARNING
    "[pgo] no -O1/-O2/-O3 found in the compile flags for "
    "CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}' (current: '${_pgo_cur_flags}'); "
    "PGO/LTO will not help. Consider -DCMAKE_BUILD_TYPE=Release.")
endif()

# ----------------------------------------------------------------------------
# 2.2 MSVC: locate pgomgr (used by pgo-check for a profile summary).
#     The actual .pgc -> .pgd merge is done by link.exe automatically when
#     /USEPROFILE:PGD=<file> is used, so no external merge step is required.
# ----------------------------------------------------------------------------
set(_pgo_msvc_bin_dir "")
if(_pgo_is_msvc)
  get_filename_component(_pgo_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  # Clear a previous cache entry; find_program() skips the search when the
  # variable is already defined to a non-NOTFOUND value (including "").
  unset(_pgo_pgomgr CACHE)
  unset(_pgo_pgomgr)
  if(_pgo_msvc_bin_dir)
    find_program(_pgo_pgomgr NAMES pgomgr pgomgr.exe HINTS "${_pgo_msvc_bin_dir}")
  endif()
  if(NOT _pgo_pgomgr)
    find_program(_pgo_pgomgr NAMES pgomgr pgomgr.exe)
  endif()
  if(_pgo_pgomgr)
    file(TO_CMAKE_PATH "${_pgo_pgomgr}" _pgo_pgomgr)
  else()
    set(_pgo_pgomgr "")
  endif()
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
      #   <msys2>/ucrt64/bin/clang++ -> prefix <msys2>/ucrt64 -> parent <msys2>
      get_filename_component(_pgo_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
      get_filename_component(_pgo_prefix "${_pgo_bin_dir}" DIRECTORY)
      get_filename_component(_pgo_root "${_pgo_prefix}" DIRECTORY)
      set(_pgo_candidates "")
      if(_pgo_ver)
        file(GLOB _pgo_candidates
          "${_pgo_root}/*/lib/clang/${_pgo_ver}/lib/windows/libclang_rt.profile-*.a"
          "${_pgo_root}/*/lib/clang/${_pgo_ver}/lib/*/libclang_rt.profile*.a"
          "${_pgo_root}/*/lib/clang/${_pgo_ver}/lib/libclang_rt.profile*.a")
      endif()
      if(_pgo_candidates)
        list(GET _pgo_candidates 0 _pgo_hit)
        # .../<prefix>/lib/clang/<ver>/lib/windows/libclang_rt.profile-x86_64.a -> back to clang/<ver>
        string(REGEX REPLACE "/lib/(windows|[^/]+)/libclang_rt\\.profile[^/]*$" "" _pgo_cand_rd "${_pgo_hit}")
        if(NOT _pgo_cand_rd OR NOT EXISTS "${_pgo_cand_rd}/lib")
          string(REGEX REPLACE "/lib/libclang_rt\\.profile[^/]*$" "" _pgo_cand_rd "${_pgo_hit}")
        endif()
        set(_pgo_resource_dir "${_pgo_cand_rd}")
        set(_pgo_resource_dir_reason "the compiler resource-dir lacks the profile runtime; using ${_pgo_cand_rd}")
      else()
        message(FATAL_ERROR
          "[pgo] cannot find libclang_rt.profile-*.a under ${_pgo_default_rd} "
          "or its sibling prefixes (${_pgo_root}/*/lib/clang/${_pgo_ver}).\n"
          "      PGO needs the compiler runtime library libclang_rt.profile; "
          "install a prefix that contains compiler-rt (clang64 on MSYS2), or pass "
          "-DPGO_RESOURCE_DIR=<resource-dir containing lib/*/libclang_rt.profile-*.a>.")
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

# llvm-profdata (clang only; GCC and MSVC do not use it)
set(_pgo_profdata_tool "")
if(_pgo_is_clang)
  if(PGO_PROFDATA_TOOL)
    set(_pgo_profdata_tool "${PGO_PROFDATA_TOOL}")
    file(TO_CMAKE_PATH "${_pgo_profdata_tool}" _pgo_profdata_tool)
  else()
    _pgo_find_tool(_pgo_profdata_tool "llvm-profdata" "llvm-profdata.exe")
  endif()
  if(NOT _pgo_profdata_tool)
    message(FATAL_ERROR
      "[pgo] cannot find llvm-profdata (required to merge PGO profiles).\n"
      "      Pass -DPGO_PROFDATA_TOOL=<path> or -DPGO_LLVM_BIN_DIR=<dir>, "
      "or put llvm-profdata on PATH.")
  endif()
endif()

# ----------------------------------------------------------------------------
# 3.5 AUTO mode: configure-time bootstrap
#     Performs "instrumented build + training + merge" in <build>/pgo-bootstrap,
#     then compiles this build with USE.
#     User perspective: cmake -S . -B build && cmake --build build yields the
#     optimized binaries.
#     Restriction: single-config generator.  GCC >= 9 uses
#     -fprofile-prefix-path, so a bootstrap sub-directory works; older GCC
#     requires the manual same-directory two-stage flow.
# ----------------------------------------------------------------------------
if(PGO_AUTO)
  if(NOT _pgo_is_clang AND NOT _pgo_is_msvc AND NOT _pgo_is_gnu)
    message(FATAL_ERROR
      "[pgo] PGO_MODE=AUTO supports clang, MSVC and GCC (>= 9) only")
  endif()
  if(_pgo_is_gnu AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    message(FATAL_ERROR
      "[pgo] PGO_MODE=AUTO with GCC requires GCC >= 9 "
      "(-fprofile-prefix-path).\n"
      "      For older GCC use the manual two-stage flow in the same build directory:\n"
      "        cmake -B build -DPGO_MODE=GENERATE && cmake --build build --target pgo-train\n"
      "        cmake -B build -DPGO_MODE=USE      && cmake --build build")
  endif()
  if(_pgo_is_gnu)
    # AUTO owns the GCC prefix: the bootstrap sub-build strips its own
    # directory, and this build strips CMAKE_BINARY_DIR, so both produce
    # the same relative .gcda names.  A user value would break that match.
    if(PGO_GCC_PREFIX_PATH AND NOT PGO_GCC_PREFIX_PATH STREQUAL CMAKE_BINARY_DIR)
      message(STATUS "[pgo] AUTO: overriding PGO_GCC_PREFIX_PATH with CMAKE_BINARY_DIR for the GCC profile match")
    endif()
    set(PGO_GCC_PREFIX_PATH "${CMAKE_BINARY_DIR}")
    file(TO_NATIVE_PATH "${PGO_GCC_PREFIX_PATH}" _pgo_gcc_prefix_native)
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
  elseif(_pgo_is_msvc AND NOT EXISTS "${PGO_PROFILE_DIR}")
    set(_pgo_need_boot TRUE)
    set(_pgo_boot_why "the profile directory does not exist yet")
  elseif(_pgo_is_msvc)
    # MSVC keeps one .pgd per training executable; any existing .pgd means
    # the profile can be reused.
    file(GLOB _pgo_auto_pgds "${PGO_PROFILE_DIR}/*.pgd")
    if(NOT _pgo_auto_pgds)
      set(_pgo_need_boot TRUE)
      set(_pgo_boot_why "no .pgd profile exists yet")
    endif()
  elseif(_pgo_is_gnu)
    file(GLOB_RECURSE _pgo_auto_gcdas "${PGO_PROFILE_DIR}/*.gcda")
    if(NOT _pgo_auto_gcdas)
      set(_pgo_need_boot TRUE)
      set(_pgo_boot_why "no .gcda profile exists yet")
    endif()
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
    if(_pgo_is_msvc)
      set(_pgo_pd_ts "00000000000000")
      foreach(_pgd ${_pgo_auto_pgds})
        file(TIMESTAMP "${_pgd}" _pgo_pgd_ts "%Y%m%d%H%M%S" UTC)
        if(_pgo_pgd_ts GREATER _pgo_pd_ts)
          set(_pgo_pd_ts "${_pgo_pgd_ts}")
        endif()
      endforeach()
    elseif(_pgo_is_gnu)
      set(_pgo_pd_ts "00000000000000")
      foreach(_gcda ${_pgo_auto_gcdas})
        file(TIMESTAMP "${_gcda}" _pgo_gcda_ts "%Y%m%d%H%M%S" UTC)
        if(_pgo_gcda_ts GREATER _pgo_pd_ts)
          set(_pgo_pd_ts "${_pgo_gcda_ts}")
        endif()
      endforeach()
    else()
      file(TIMESTAMP "${PGO_PROFDATA}" _pgo_pd_ts "%Y%m%d%H%M%S" UTC)
    endif()
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

    # Remove profile data from the previous run (otherwise old and new data
    # mix).  Automatic cleanup happens only inside this build directory.
    if(_pgo_is_msvc)
      set(_pgo_profile_globs "${PGO_PROFILE_DIR}/*.pgd" "${PGO_PROFILE_DIR}/*.pgc")
    elseif(_pgo_is_gnu)
      set(_pgo_profile_globs "${PGO_PROFILE_DIR}/*.gcda")
    else()
      set(_pgo_profile_globs "${PGO_PROFILE_DIR}/*.profraw" "${PGO_PROFILE_DIR}/*/*.profraw")
    endif()
    if(PGO_PROFILE_DIR MATCHES "^${CMAKE_BINARY_DIR}")
      file(REMOVE_RECURSE "${PGO_PROFILE_DIR}")
    else()
      file(GLOB _pgo_old_raws ${_pgo_profile_globs})
      if(_pgo_old_raws)
        message(STATUS "[pgo]   note: PGO_PROFILE_DIR is outside the build "
                       "directory; make sure old profile files do not mix in "
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
      "-DPGO_KEEP_WARNINGS=${PGO_KEEP_WARNINGS}"
      "-DPGO_VERBOSE=OFF")
    if(_pgo_is_clang)
      list(APPEND _pgo_boot_args
        "-DPGO_RESOURCE_DIR=${_pgo_resource_dir}"
        "-DPGO_PROFDATA_TOOL=${_pgo_profdata_tool}")
    endif()
    if(_pgo_is_gnu)
      # GCC: strip the *bootstrap* build-directory prefix so the .gcda names
      # match the main build directory at USE time.
      file(TO_NATIVE_PATH "${PGO_BOOTSTRAP_DIR}" _pgo_boot_gcc_prefix)
      list(APPEND _pgo_boot_args "-DPGO_GCC_PREFIX_PATH=${_pgo_boot_gcc_prefix}")
    endif()
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
    if(_pgo_is_msvc)
      file(GLOB _pgo_boot_pgds "${PGO_PROFILE_DIR}/*.pgd")
      if(NOT _pgo_boot_pgds)
        message(FATAL_ERROR
          "[pgo] the bootstrap ran but produced no .pgd under ${PGO_PROFILE_DIR}.\n"
          "      Make sure the project registers a training workload with "
          "pgo_add_training_target() (training only happens when one is registered).")
      endif()
      list(LENGTH _pgo_boot_pgds _pgo_boot_npgd)
      message(STATUS "[pgo] bootstrap complete: ${_pgo_boot_npgd} .pgd file(s) under ${PGO_PROFILE_DIR} -> this build compiles with USE")
    elseif(_pgo_is_gnu)
      file(GLOB_RECURSE _pgo_boot_gcdas "${PGO_PROFILE_DIR}/*.gcda")
      list(LENGTH _pgo_boot_gcdas _pgo_boot_ngcda)
      if(_pgo_boot_ngcda EQUAL 0)
        message(FATAL_ERROR
          "[pgo] the bootstrap ran but produced no .gcda under ${PGO_PROFILE_DIR}.\n"
          "      Make sure the project registers a training workload with "
          "pgo_add_training_target() (training only happens when one is registered).")
      endif()
      message(STATUS "[pgo] bootstrap complete: ${_pgo_boot_ngcda} .gcda file(s) under ${PGO_PROFILE_DIR} -> this build compiles with USE")
    else()
      if(NOT EXISTS "${PGO_PROFDATA}")
        message(FATAL_ERROR
          "[pgo] the bootstrap ran but did not produce ${PGO_PROFDATA}.\n"
          "      Make sure the project registers a training workload with "
          "pgo_add_training_target() (training only happens when one is registered).")
      endif()
      file(SIZE "${PGO_PROFDATA}" _pgo_pd_bytes)
      math(EXPR _pgo_pd_kib "${_pgo_pd_bytes} / 1024")
      message(STATUS "[pgo] bootstrap complete: ${PGO_PROFDATA} (${_pgo_pd_kib} KiB) -> this build compiles with USE")
    endif()
  else()
    if(_pgo_is_msvc)
      file(GLOB _pgo_reuse_pgds "${PGO_PROFILE_DIR}/*.pgd")
      list(LENGTH _pgo_reuse_pgds _pgo_reuse_n)
      message(STATUS "[pgo] AUTO: reusing ${_pgo_reuse_n} existing .pgd file(s) under ${PGO_PROFILE_DIR}")
    elseif(_pgo_is_gnu)
      file(GLOB_RECURSE _pgo_reuse_gcdas "${PGO_PROFILE_DIR}/*.gcda")
      list(LENGTH _pgo_reuse_gcdas _pgo_reuse_n)
      message(STATUS "[pgo] AUTO: reusing ${_pgo_reuse_n} existing .gcda file(s) under ${PGO_PROFILE_DIR}")
    else()
      message(STATUS "[pgo] AUTO: reusing the existing profile ${PGO_PROFDATA}")
    endif()
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
  if(_pgo_is_msvc)
    # One .pgd per training executable; link.exe merges <pgd>!*.pgc when
    # /USEPROFILE:PGD=<pgd> is used.
    file(GLOB _pgo_pgd_files "${PGO_PROFILE_DIR}/*.pgd")
    if(NOT _pgo_pgd_files)
      if(PGO_STRICT)
        message(FATAL_ERROR
          "[pgo] PGO_MODE=USE needs at least one .pgd under '${PGO_PROFILE_DIR}', "
          "but none was found.\n"
          "      Run the GENERATE stage first: cmake --build <build-inst> --target pgo-train\n"
          "      (PGO_STRICT=OFF degrades to a normal build).")
      else()
        message(WARNING "[pgo] no .pgd under '${PGO_PROFILE_DIR}'; degrading to a normal build (PGO_STRICT=OFF)")
        set(PGO_USE FALSE)
        if(NOT PGO_LTO)
          set(PGO_ACTIVE FALSE)
        endif()
      endif()
    endif()
  elseif(_pgo_is_clang)
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
    # GCC: the profile is a directory of .gcda files.  With GCC >= 9,
    # -fprofile-prefix-path makes the .gcda names relative to
    # PGO_GCC_PREFIX_PATH, so GENERATE and USE may run in different build
    # directories (and AUTO can use a bootstrap sub-directory).
    if(NOT IS_DIRECTORY "${PGO_PROFILE_DIR}")
      if(PGO_STRICT)
        message(FATAL_ERROR "[pgo] GCC PGO_MODE=USE requires the directory '${PGO_PROFILE_DIR}' (containing .gcda files).")
      else()
        message(WARNING "[pgo] cannot find '${PGO_PROFILE_DIR}'; degrading to a normal build (PGO_STRICT=OFF)")
        set(PGO_USE FALSE)
        if(NOT PGO_LTO)
          set(PGO_ACTIVE FALSE)
        endif()
      endif()
    else()
      file(GLOB_RECURSE _pgo_gcda_files "${PGO_PROFILE_DIR}/*.gcda")
      list(LENGTH _pgo_gcda_files _pgo_ngcda)
      if(_pgo_ngcda EQUAL 0)
        if(PGO_STRICT)
          message(FATAL_ERROR
            "[pgo] no .gcda files under '${PGO_PROFILE_DIR}': run the GENERATE stage's pgo-train first.")
        else()
          message(WARNING "[pgo] no .gcda files under '${PGO_PROFILE_DIR}'; degrading to a normal build")
          set(PGO_USE FALSE)
          if(NOT PGO_LTO)
            set(PGO_ACTIVE FALSE)
          endif()
        endif()
      else()
        message(STATUS
          "[pgo] GCC profile: ${_pgo_ngcda} .gcda file(s) under ${PGO_PROFILE_DIR} "
          "(prefix=${_pgo_gcc_prefix_native})")
      endif()
    endif()
  endif()
endif()

# ----------------------------------------------------------------------------
# 5. Linker selection.
#    Windows/clang: PGO instrumentation and LTO require lld, because the GNU
#    ld shipped with MinGW cannot consume LLVM profile/bitcode objects.
#    Other platforms: the compiler's default linker is usually fine and is
#    left untouched unless the user sets PGO_LINKER explicitly.
# ----------------------------------------------------------------------------
set(_pgo_need_lld FALSE)
if(_pgo_is_clang AND (PGO_GENERATE OR NOT PGO_LTO STREQUAL "OFF"))
  set(_pgo_need_lld TRUE)
endif()

if(_pgo_need_lld AND _pgo_is_clang)
  if(WIN32)
    if(NOT PGO_LINKER)
      if(_pgo_lld)
        set(PGO_LINKER "${_pgo_lld}")
      elseif(NOT PGO_ALLOW_ANY_LINKER)
        message(FATAL_ERROR
          "[pgo] lld was not found.  PGO instrumentation and LTO on Windows "
          "need lld (GNU ld cannot link LLVM profile/bitcode objects).\n"
          "      Install lld (MSYS2: pacman -S mingw-w64-ucrt-x86_64-lld, or the "
          "LLVM/clang package), pass -DPGO_LLD=<path to ld.lld.exe>, or set "
          "-DPGO_LLVM_BIN_DIR=<directory containing ld.lld.exe>.")
      endif()
    endif()
  elseif(NOT PGO_LINKER)
    message(STATUS "[pgo] using the compiler default linker "
                   "(set -DPGO_LINKER=lld to force lld)")
  endif()

  if(PGO_LINKER)
    foreach(_lt EXE SHARED MODULE)
      string(FIND "${CMAKE_${_lt}_LINKER_FLAGS}" "-fuse-ld=" _pgo_ld_pos)
      if(NOT _pgo_ld_pos EQUAL -1)
        string(FIND "${CMAKE_${_lt}_LINKER_FLAGS}" "-fuse-ld=${PGO_LINKER}" _pgo_ld_same)
        if(_pgo_ld_same EQUAL -1 AND NOT PGO_ALLOW_ANY_LINKER)
          message(FATAL_ERROR
            "[pgo] CMAKE_${_lt}_LINKER_FLAGS already contains a different -fuse-ld.\n"
            "      PGO/LTO needs '${PGO_LINKER}'.  Re-run with "
            "-DPGO_ALLOW_ANY_LINKER=ON to override (at your own risk).")
        endif()
      else()
        set(CMAKE_${_lt}_LINKER_FLAGS "${CMAKE_${_lt}_LINKER_FLAGS} -fuse-ld=${PGO_LINKER}")
      endif()
    endforeach()
  endif()
endif()

# ----------------------------------------------------------------------------
# 6. Flag assembly
# ----------------------------------------------------------------------------
set(_pgo_compile_flags "")
set(_pgo_link_flags "")

# GCC >= 9: make .gcda names relative to the build directory.  This keeps
# profiles reusable across build directories and enables AUTO.
if(_pgo_is_gnu AND (PGO_GENERATE OR PGO_USE)
   AND NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9
   AND _pgo_gcc_prefix_native)
  list(APPEND _pgo_compile_flags "-fprofile-prefix-path=${_pgo_gcc_prefix_native}")
endif()

if(_pgo_is_msvc)
  # MSVC PGO/LTO:
  #   compile: /GL (whole-program code generation)
  #   link:    /LTCG
  #   profile: /GENPROFILE:PGD=<file> (GENERATE) and /USEPROFILE:PGD=<file>
  #            (USE) are per-target linker options added by
  #            pgo_add_training_target(), because every training executable
  #            owns its own .pgd.
  # link.exe does not create the directory of the PGD file.
  file(MAKE_DIRECTORY "${PGO_PROFILE_DIR}")
  if(PGO_GENERATE OR PGO_USE OR NOT PGO_LTO STREQUAL "OFF")
    list(APPEND _pgo_compile_flags "/GL")
    list(APPEND _pgo_link_flags "/LTCG")
  endif()
else()
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
    if(PGO_WERROR_OUT_OF_DATE)
      list(APPEND _pgo_compile_flags "-Werror=missing-profile")
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
endif()  # !_pgo_is_msvc

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
  # include is covered.  A flag whose value contains spaces (for example a
  # profile path under "C:/Program Files") must be quoted as one argument.
  foreach(_f ${_pgo_compile_flags})
    set(_fq "${_f}")
    if(_f MATCHES " ")
      set(_fq "\"${_f}\"")
    endif()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_fq}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_fq}")
  endforeach()
  foreach(_f ${_pgo_link_flags})
    set(_fq "${_f}")
    if(_f MATCHES " ")
      set(_fq "\"${_f}\"")
    endif()
    foreach(_lt EXE SHARED MODULE)
      set(CMAKE_${_lt}_LINKER_FLAGS "${CMAKE_${_lt}_LINKER_FLAGS} ${_fq}")
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
    if(_pgo_is_msvc)
      message(STATUS "[pgo] pgo_apply() is unnecessary with PGO_SCOPE=GLOBAL "
                     "(global /GL + /LTCG; training targets get their profile options "
                     "from pgo_add_training_target())")
    else()
      message(STATUS "[pgo] pgo_apply() is unnecessary with PGO_SCOPE=GLOBAL (all targets already get the flags through CMAKE_*_FLAGS)")
    endif()
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

  if(_pgo_is_msvc)
    # MSVC PGO is per-binary: every training executable owns its own .pgd.
    # /GENPROFILE and /USEPROFILE are LINKER options (cl rejects /GENPROFILE
    # with D9002); link.exe also merges the <pgd>!*.pgc files written by the
    # training runs when /USEPROFILE is present.
    get_target_property(_pgo_msvc_done ${T_EXECUTABLE} _PGO_MSVC_PROFILE_APPLIED)
    if(NOT _pgo_msvc_done)
      set(_pgo_pgd "${PGO_PROFILE_DIR}/${T_EXECUTABLE}.pgd")
      file(TO_CMAKE_PATH "${_pgo_pgd}" _pgo_pgd)
      target_compile_options(${T_EXECUTABLE} PRIVATE "/GL")
      target_link_options(${T_EXECUTABLE} PRIVATE "/LTCG")
      if(PGO_GENERATE)
        target_link_options(${T_EXECUTABLE} PRIVATE "/GENPROFILE:PGD=${_pgo_pgd}")
      elseif(PGO_USE)
        target_link_options(${T_EXECUTABLE} PRIVATE "/USEPROFILE:PGD=${_pgo_pgd}")
      endif()
      set_property(TARGET ${T_EXECUTABLE} PROPERTY _PGO_MSVC_PROFILE_APPLIED TRUE)
      if(PGO_VERBOSE)
        message(STATUS "[pgo] MSVC profile for ${T_EXECUTABLE}: ${_pgo_pgd}")
      endif()
    endif()
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
  set(_pgo_env_separator "")
  if(_pgo_cxx_bin)
    # Put the compiler directory first (DLLs: libstdc++/winpthread/pgort140).
    # NOTE: cmake -E env cannot prepend to PATH portably (no --modify in
    # CMake 4.4), so this replaces PATH for the training run.  Programs that
    # need more can append their own PATH via pgo_add_training_target(ENV ...).
    list(APPEND _pgo_train_env "PATH=${_pgo_cxx_bin}")
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
        ${_pgo_env_separator}
        "$<TARGET_FILE:${T_EXECUTABLE}>" ${T_COMMAND})
    else()
      # MSVC/GCC: no LLVM_PROFILE_FILE.  Keep the compiler bin directory on
      # PATH so the MSVC instrumented executable can find pgort140.dll, and
      # point VCPROFILE_PATH at the profile directory: without it the MSVC
      # runtime writes <exe>!N.pgc into the current working directory.
      if(_pgo_is_msvc)
        list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E env
          "VCPROFILE_PATH=${PGO_PROFILE_DIR}"
          ${_pgo_train_env}
          ${_pgo_env_separator}
          "$<TARGET_FILE:${T_EXECUTABLE}>" ${T_COMMAND})
      else()
        list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E env
          ${_pgo_train_env}
          ${_pgo_env_separator}
          "$<TARGET_FILE:${T_EXECUTABLE}>" ${T_COMMAND})
      endif()
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
    if(PGO_LINKER)
      set(_pgo_link_disp "${PGO_LINKER}")
    else()
      set(_pgo_link_disp "(compiler default linker)")
    endif()
  elseif(_pgo_is_msvc)
    set(_pgo_link_disp "link.exe (LTCG)")
  else()
    set(_pgo_link_disp "(compiler default linker)")
  endif()
  message(STATUS "[pgo]  PGO_SCOPE       = ${PGO_SCOPE}  link = ${_pgo_link_disp}")
  message(STATUS "[pgo]  PROFILE_DIR     = ${PGO_PROFILE_DIR}")
  if(_pgo_is_clang)
    message(STATUS "[pgo]  PROFDATA        = ${PGO_PROFDATA}")
    message(STATUS "[pgo]  llvm-profdata   = ${_pgo_profdata_tool}")
    if(_pgo_lld)
      message(STATUS "[pgo]  ld.lld          = ${_pgo_lld}")
    endif()
    if(PGO_LLVM_BIN_DIR)
      message(STATUS "[pgo]  LLVM bin dir    = ${PGO_LLVM_BIN_DIR}")
    endif()
    message(STATUS "[pgo]  resource-dir    = ${_pgo_resource_dir}  (${_pgo_resource_dir_reason})")
  elseif(_pgo_is_msvc)
    message(STATUS "[pgo]  PGD dir         = ${PGO_PROFILE_DIR}  (one <target>.pgd per training executable)")
    message(STATUS "[pgo]  pgomgr          = ${_pgo_pgomgr}")
  elseif(_pgo_is_gnu)
    message(STATUS "[pgo]  GCC profile     = .gcda under ${PGO_PROFILE_DIR}")
    message(STATUS "[pgo]  GCC prefix      = ${_pgo_gcc_prefix_native}")
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
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "[pgo] training complete; merging profraw -> ${PGO_PROFDATA}"
    VERBATIM)

  # Merge only (used when training already ran separately).
  add_custom_target(pgo-merge
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            "-DPGO_PROFDATA=${PGO_PROFDATA}"
            "-DPGO_PROFDATA_TOOL=${_pgo_profdata_tool}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "[pgo] merging profraw -> ${PGO_PROFDATA}"
    VERBATIM)

  add_custom_target(pgo-check
    COMMAND "${_pgo_profdata_tool}" show --detailed-summary "${PGO_PROFDATA}"
    COMMENT "[pgo] checking the profile (llvm-profdata show --detailed-summary)"
    VERBATIM)
elseif(_pgo_is_msvc)
  # MSVC: link.exe merges <pgd>!*.pgc automatically at /USEPROFILE time,
  # so pgo-train/pgo-merge only validate the data and print a pgomgr summary.
  add_custom_target(pgo-train
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_MERGE_KIND=MSVC"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            "-DPGO_PGOMGR=${_pgo_pgomgr}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "[pgo] training complete; checking the MSVC .pgd/.pgc files"
    VERBATIM)

  add_custom_target(pgo-merge
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_MERGE_KIND=MSVC"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            "-DPGO_PGOMGR=${_pgo_pgomgr}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "[pgo] MSVC: link.exe merges <pgd>!*.pgc; checking the profile"
    VERBATIM)

  add_custom_target(pgo-check
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_MERGE_KIND=MSVC"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            "-DPGO_PGOMGR=${_pgo_pgomgr}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "[pgo] checking the MSVC profile (pgomgr /summary)"
    VERBATIM)
else()
  # GCC: .gcda files are consumed directly by -fprofile-use; validate them
  # (count/size) instead of merging.
  add_custom_target(pgo-train
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_MERGE_KIND=GCC"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "[pgo] training complete; checking the GCC .gcda files"
    VERBATIM)
  add_custom_target(pgo-merge
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_MERGE_KIND=GCC"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "[pgo] GCC .gcda files need no merge; checking them"
    VERBATIM)
  add_custom_target(pgo-check
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_MERGE_KIND=GCC"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "[pgo] checking the GCC .gcda profile"
    VERBATIM)
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
