#[[ pgo-merge.cmake —— 由 pgo.cmake 以脚本模式调用：cmake -P
     -DPGO_PROFILE_DIR=<含 .profraw 的目录>
     -DPGO_PROFDATA=<输出 .profdata>
     -DPGO_PROFDATA_TOOL=<llvm-profdata 路径>
     [-DPGO_MERGE_EXTRA_ARGS=<额外 merge 参数，分号分隔>]
   把目录下所有 .profraw 合并成单个 indexed profile。
   为什么用脚本模式：add_custom_command 只能在配置期展开 glob，训练产生的新 .profraw
   在构建期才出现；脚本模式让每次构建都重新扫描目录。
   为什么分批：Windows 命令行上限 32767 字符，profraw 数量多时必须分批二级合并。]]
if(NOT DEFINED PGO_PROFILE_DIR OR "${PGO_PROFILE_DIR}" STREQUAL "")
  message(FATAL_ERROR "[pgo-merge] 缺少 PGO_PROFILE_DIR")
endif()
if(NOT DEFINED PGO_PROFDATA OR "${PGO_PROFDATA}" STREQUAL "")
  message(FATAL_ERROR "[pgo-merge] 缺少 PGO_PROFDATA")
endif()
if(NOT DEFINED PGO_PROFDATA_TOOL OR "${PGO_PROFDATA_TOOL}" STREQUAL "")
  message(FATAL_ERROR "[pgo-merge] 缺少 PGO_PROFDATA_TOOL")
endif()

if(NOT EXISTS "${PGO_PROFDATA_TOOL}")
  message(FATAL_ERROR "[pgo-merge] llvm-profdata 不存在：${PGO_PROFDATA_TOOL}")
endif()

if(NOT IS_DIRECTORY "${PGO_PROFILE_DIR}")
  message(FATAL_ERROR
    "[pgo-merge] profile 目录不存在：${PGO_PROFILE_DIR}\n"
    "      GENERATE 阶段请先构建并运行训练程序（target: pgo-train）。")
endif()

file(GLOB_RECURSE _raws "${PGO_PROFILE_DIR}/*.profraw")
list(LENGTH _raws _nraws)
if(_nraws EQUAL 0)
  message(FATAL_ERROR
    "[pgo-merge] 在 ${PGO_PROFILE_DIR} 下没有找到任何 .profraw。\n"
    "      可能原因：训练程序没有真正运行（pgo-train）、被中断（异常退出不落盘）、\n"
    "      或 LLVM_PROFILE_FILE 指向了其他目录。")
endif()

get_filename_component(_out_dir "${PGO_PROFDATA}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")

set(_batch 64)   # 每批最多 64 个文件，避开 Windows 命令行长度上限
set(_extra ${PGO_MERGE_EXTRA_ARGS})

function(_pgo_merge_run)
  # 参数：<输出文件> <输入文件...>
  set(_args ${ARGN})
  list(GET _args 0 _out)
  list(REMOVE_AT _args 0)
  execute_process(
    COMMAND "${PGO_PROFDATA_TOOL}" merge ${_extra} -o "${_out}" ${_args}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "[pgo-merge] llvm-profdata merge 失败（rc=${_rc}）：\n${_e}")
  endif()
endfunction()

if(_nraws LESS_EQUAL _batch)
  _pgo_merge_run("${PGO_PROFDATA}" ${_raws})
else()
  # 分批 -> 二级合并
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
  message(FATAL_ERROR "[pgo-merge] 未生成 ${PGO_PROFDATA}")
endif()
file(SIZE "${PGO_PROFDATA}" _size)
if(_size EQUAL 0)
  message(FATAL_ERROR "[pgo-merge] 生成的 ${PGO_PROFDATA} 是空文件")
endif()

math(EXPR _kb "${_size} / 1024")
message(STATUS "[pgo-merge] 合并 ${_nraws} 个 .profraw -> ${PGO_PROFDATA} (${_size} B ≈ ${_kb} KiB)")
