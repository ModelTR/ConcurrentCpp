#[[ pgo-merge.cmake -- invoked in script mode by pgo.cmake: cmake -P
     -DPGO_PROFILE_DIR=<directory containing .profraw files>
     -DPGO_PROFDATA=<output .profdata>
     -DPGO_PROFDATA_TOOL=<path to llvm-profdata>
     [-DPGO_MERGE_EXTRA_ARGS=<extra merge arguments, semicolon separated>]
   Merges every .profraw under the directory into a single indexed profile.
   Why script mode: add_custom_command can only expand globs at configure
   time; new .profraw files produced by training only appear at build time.
   Script mode rescans the directory on every build.
   Why batching: the Windows command line is limited to 32767 characters, so
   a large number of profraw files must be merged in two levels.]]
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
