#[[ pgo-merge.cmake -- invoked in script mode by pgo.cmake: cmake -P
     -DPGO_PROFILE_DIR=<directory containing profile data>
     -DPGO_PROFDATA=<output .profdata>            (clang only)
     -DPGO_PROFDATA_TOOL=<path to llvm-profdata>  (clang only)
     [-DPGO_MERGE_EXTRA_ARGS=<extra merge arguments, semicolon separated>]
     [-DPGO_MERGE_KIND=MSVC -DPGO_PGOMGR=<pgomgr>] (MSVC)
     [-DPGO_MERGE_KIND=GCC]                        (GCC)
   clang: merge every .profraw under the directory into a single indexed
   profile.  MSVC: validate .pgd/.pgc and print a pgomgr summary.  GCC:
   validate the .gcda files (they are consumed directly by -fprofile-use).
   Why script mode: add_custom_command can only expand globs at configure
   time; new profile files produced by training only appear at build time.
   Script mode rescans the directory on every build.
   Why batching: the Windows command line is limited to 32767 characters, so
   a large number of profraw files must be merged in two levels.]]
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
