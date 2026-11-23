#[[============================================================================
 pgo.cmake —— 生产可用的 PGO（Profile-Guided Optimization）+ LTO 集成模块
----------------------------------------------------------------------------
 适用工具链（本工作区实测环境）：
   MSYS2 UCRT64 + clang 22.1.8 (x86_64-w64-windows-gnu) + libstdc++ + ninja
   llvm-profdata / libclang_rt.profile 位于 clang64 前缀（ucrt64 不含）

 用法（开箱即用，推荐）：

   # 主 CMakeLists.txt 里只写两行：
   #   option(PGO "启用 PGO+LTO 自动构建（配置期自举）" ON)
   #   include(<workspace>/cmake/pgo.cmake)
   #
   # 然后像普通项目一样构建（一次构建命令即产出优化版本）：
   cmake -S proj -B build -G Ninja -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe
   cmake --build build
   #
   # AUTO 模式首次配置会自动：在 <build>/pgo-bootstrap 做插桩构建 → 跑训练 →
   # 合并 profile → 本构建按 USE 编译。之后（profile 已存在）配置只做复用检查；
   # 源码有改动时按 PGO_AUTO_REBOOTSTRAP（默认 ON）自动重新训练。

 用法（手动两阶段，需要精细控制时用）：

   # 1) 插桩构建 + 训练 + 合并 profile
   cmake -S proj -B build-inst -G Ninja \
         -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe \
         -DPGO_MODE=GENERATE -DPGO_LTO=THIN \
         -DPGO_PROFILE_DIR=<绝对路径>/profile
   cmake --build build-inst                     # 构建插桩程序
   cmake --build build-inst --target pgo-train   # 跑训练负载 + 合并 profdata

   # 2) 用 profile 构建优化版
   cmake -S proj -B build-opt -G Ninja \
         -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe \
         -DPGO_MODE=USE -DPGO_LTO=THIN \
         -DPGO_PROFILE_DIR=<绝对路径>/profile
   cmake --build build-opt

 主要变量（全部可用 -D 传入）：
   PGO               便捷开关 ON/OFF                等价于 PGO_MODE=AUTO/OFF
   PGO_MODE          OFF | AUTO | GENERATE | USE   默认 OFF
                     AUTO = 配置期自举 + 本构建 USE（开箱即用）
   PGO_KIND          IR | FE                      默认 IR（clang：IRPGO 优于 FEPGO）
   PGO_LTO           OFF | THIN | FULL            默认 OFF
   PGO_SCOPE         GLOBAL | TARGET              默认 GLOBAL
   PGO_PROFILE_DIR   profile 数据目录              默认 ${CMAKE_BINARY_DIR}/pgo-profile
   PGO_PROFDATA      合并后的 profile 文件          默认 ${PGO_PROFILE_DIR}/profile.profdata
   PGO_PROFDATA_TOOL llvm-profdata 可执行文件       默认自动探测
   PGO_RESOURCE_DIR  clang resource-dir            默认自动探测（ucrt64 需指向 clang64）
   PGO_LINKER        PGO/LTO 阶段链接器             默认 lld（GNU ld 无法用于 PGO/LTO，见报告）
   PGO_STRICT        USE 模式下 profile 缺失即报错    默认 ON
   PGO_KEEP_WARNINGS 保留 -Wprofile-instr-unprofiled / -Wbackend-plugin 默认 OFF
   PGO_WERROR_OUT_OF_DATE  profile 与代码不匹配时报错  默认 OFF
   PGO_ALLOW_ANY_LINKER  允许非 lld 链接器（不推荐）   默认 OFF
   PGO_VERBOSE       打印配置摘要                    默认 ON
   PGO_EXTRA_COMPILE_FLAGS / PGO_EXTRA_LINK_FLAGS   追加标志

 布尔简写（可直接写在工程 CMakeLists.txt 里，也支持 -D 传入）：
   set(PGO ON/OFF)          ON -> PGO_MODE=AUTO，OFF -> PGO_MODE=OFF
   set(PGO_MODE ON/OFF)     ON -> AUTO，OFF -> OFF
   set(PGO_LTO ON/OFF)      ON -> THIN，OFF -> OFF
   set(PGO_SCOPE ON/OFF)    ON -> GLOBAL，OFF -> TARGET
   set(PGO_KIND ON/OFF)     ON -> IR，OFF -> FE
   字符串值（AUTO/USE/GENERATE、THIN/FULL、GLOBAL/TARGET、IR/FE）保持不变。
   注意：显式设置 PGO_MODE（普通变量或 cache）时，PGO=ON 不会再覆盖它。

 AUTO 模式专用变量：
   PGO_BOOTSTRAP_DIR     自举子构建目录              默认 ${CMAKE_BINARY_DIR}/pgo-bootstrap
   PGO_REBOOTSTRAP       强制重新训练                默认 OFF
   PGO_AUTO_REBOOTSTRAP  源码比 profile 新时自动重训   默认 ON
   PGO_FORWARD_CACHE     自动向子构建透传用户缓存变量   默认 ON
   PGO_FORWARD_VARS      额外要透传的变量名列表         默认空
   PGO_EXTRA_BOOTSTRAP_ARGS  追加给子构建的 -D 参数     默认空

 提供的 target：
   pgo-train   运行通过 pgo_add_training_target() 注册的全部训练程序并合并 profile
   pgo-merge   仅合并 ${PGO_PROFILE_DIR} 下的 profraw -> ${PGO_PROFDATA}
   pgo-check   校验 ${PGO_PROFDATA} 可被 llvm-profdata 读取并打印摘要
   pgo-clean   删除 profraw / profdata（重新训练用，避免脏数据）

 提供的函数：
   pgo_apply(<target>...)                   PGO_SCOPE=TARGET 时对指定 target 生效
   pgo_add_training_target(NAME <name> EXECUTABLE <tgt> [COMMAND <args...>]
                           [REPEAT <n>] [WORKING_DIRECTORY <dir>] [ENV <K=V>...])
   pgo_status()                             打印当前 PGO 配置摘要

 说明：模块只做“标志注入 + profile 生命周期管理”，不改动工程结构；
       所有结论与失败模式见同目录 REPORT.md，使用指南见 USAGE-GUIDE.md。
============================================================================]]

include_guard(DIRECTORY)

if(CMAKE_VERSION VERSION_LESS 3.20)
  message(FATAL_ERROR "[pgo] 需要 CMake >= 3.20（当前 ${CMAKE_VERSION}）")
endif()

set(_PGO_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(_PGO_SCRIPT "${_PGO_MODULE_DIR}/pgo-merge.cmake")

# ----------------------------------------------------------------------------
# 0. 输入规范化
# ----------------------------------------------------------------------------
# ---- 便捷开关：PGO=ON/OFF 等价于 PGO_MODE=AUTO/OFF ----
# 只在 PGO_MODE 未被显式设置（CMakeLists 普通变量 / -D / cache）时生效，
# 否则 `option(PGO ... OFF)` 或 `set(PGO ON)` 会把用户显式设置的 PGO_MODE 顶掉。
# 子构建（bootstrap）里另外用 PGO_EMBEDDED 抑制，避免工程里写死的 `set(PGO ON)` 造成递归。
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

# AUTO 模式相关开关
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

# 非法取值直接失败（配置期发现，不要等到半个构建完成）
set(_pgo_valid_modes OFF AUTO GENERATE USE)
set(_pgo_valid_kinds IR FE)
set(_pgo_valid_ltos OFF THIN FULL)
set(_pgo_valid_scopes GLOBAL TARGET)
if(NOT PGO_MODE IN_LIST _pgo_valid_modes)
  message(FATAL_ERROR "[pgo] PGO_MODE='${PGO_MODE}' 非法，取值：${_pgo_valid_modes}")
endif()
if(NOT PGO_KIND IN_LIST _pgo_valid_kinds)
  message(FATAL_ERROR "[pgo] PGO_KIND='${PGO_KIND}' 非法，取值：${_pgo_valid_kinds}")
endif()
if(NOT PGO_LTO IN_LIST _pgo_valid_ltos)
  message(FATAL_ERROR "[pgo] PGO_LTO='${PGO_LTO}' 非法，取值：${_pgo_valid_ltos}")
endif()
if(NOT PGO_SCOPE IN_LIST _pgo_valid_scopes)
  message(FATAL_ERROR "[pgo] PGO_SCOPE='${PGO_SCOPE}' 非法，取值：${_pgo_valid_scopes}")
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

# 目录/文件路径统一成正斜杠（CMake/CPM/FetchContent 与 clang 在 Windows 下都要求）
if(NOT DEFINED PGO_PROFILE_DIR)
  set(PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo-profile")
endif()
# `include()` 可能发生在 project() 之外，此时 CMAKE_BINARY_DIR 为空
get_filename_component(PGO_PROFILE_DIR "${PGO_PROFILE_DIR}" ABSOLUTE)
file(TO_CMAKE_PATH "${PGO_PROFILE_DIR}" PGO_PROFILE_DIR)

if(NOT DEFINED PGO_PROFDATA)
  set(PGO_PROFDATA "${PGO_PROFILE_DIR}/profile.profdata")
endif()
get_filename_component(PGO_PROFDATA "${PGO_PROFDATA}" ABSOLUTE)
file(TO_CMAKE_PATH "${PGO_PROFDATA}" PGO_PROFDATA)

if(CMAKE_CONFIGURATION_TYPES)
  message(WARNING
    "[pgo] 检测到多配置生成器（CMAKE_CONFIGURATION_TYPES=${CMAKE_CONFIGURATION_TYPES}）。"
    "本模块只测过单配置生成器（Ninja），多配置下请自行确认各配置的标志均符合预期。")
endif()

# ----------------------------------------------------------------------------
# 1. 完全不激活：PGO_MODE=OFF 且 PGO_LTO=OFF（默认）→ 登记状态，不注入任何东西
#    注意：PGO_MODE=OFF 但 PGO_LTO!=OFF 是合法用法（“只要 LTO，不要 PGO”），
#          必须继续走下去注入 LTO 标志。
# ----------------------------------------------------------------------------
if(NOT PGO_ACTIVE AND PGO_LTO STREQUAL "OFF")
  # OFF 模式：提供 no-op 函数，让工程代码无需条件包裹
  function(pgo_apply)
  endfunction()
  function(pgo_add_training_target)
  endfunction()
  function(pgo_status)
    message(STATUS "[pgo] PGO_MODE=OFF（未启用）")
  endfunction()
  if(PGO_VERBOSE)
    message(STATUS "[pgo] 未启用（PGO_MODE=OFF，PGO_LTO=OFF）：不注入任何编译/链接标志")
  endif()
  return()
endif()

# ----------------------------------------------------------------------------
# 2. 编译器能力检查
# ----------------------------------------------------------------------------
set(_pgo_is_clang FALSE)
set(_pgo_is_gnu FALSE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_pgo_is_clang TRUE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_pgo_is_gnu TRUE)
else()
  message(FATAL_ERROR
    "[pgo] 不支持的编译器 '${CMAKE_CXX_COMPILER_ID}'（本模块支持 Clang 与 GNU；"
    "MSVC 的 /GL /GENPROFILE /USEPROFILE 流程未实测，请勿使用）")
endif()

if(_pgo_is_gnu AND PGO_KIND STREQUAL "IR")
  # GCC 没有 IRPGO/FEPGO 之分
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

# 优化级别检查：PGO/LTO 在 -O0 下没有意义。只看**当前构建类型**对应的 flags
# （不能遍历所有配置：Release 的默认 -O3 会掩盖 Debug 构建实际没有优化这一事实）。
set(_pgo_cur_flags "${CMAKE_CXX_FLAGS}")
if(CMAKE_BUILD_TYPE)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _pgo_bt)
  if(DEFINED CMAKE_CXX_FLAGS_${_pgo_bt})
    set(_pgo_cur_flags "${_pgo_cur_flags} ${CMAKE_CXX_FLAGS_${_pgo_bt}}")
  endif()
endif()
if(NOT _pgo_cur_flags MATCHES "(^| )-O([1-3s]|fast)")
  message(WARNING
    "[pgo] CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}' 的编译 flags 中未发现 -O1/-O2/-O3"
    "（当前：'${_pgo_cur_flags}'），PGO/LTO 不会带来收益。建议 -DCMAKE_BUILD_TYPE=Release。")
endif()

# ----------------------------------------------------------------------------
# 3. profile runtime（libclang_rt.profile）与 llvm-profdata 探测
#    —— 本机核心坑：ucrt64 前缀没有 libclang_rt.profile-*.a，也没有 llvm-profdata，
#       两者都必须从 clang64 前缀取。
# ----------------------------------------------------------------------------
set(_pgo_resource_dir "")
set(_pgo_resource_dir_reason "")

if(_pgo_is_clang)
  # 先问编译器自己的 resource-dir（无论用户是否显式指定都需要，用于判断是否需要 -resource-dir）
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE _pgo_default_rd
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  file(TO_CMAKE_PATH "${_pgo_default_rd}" _pgo_default_rd)
  string(REGEX MATCH "clang/([0-9]+)" _m "${_pgo_default_rd}")
  set(_pgo_ver "${CMAKE_MATCH_1}")

  if(DEFINED PGO_RESOURCE_DIR AND NOT PGO_RESOURCE_DIR STREQUAL "")
    set(_pgo_resource_dir "${PGO_RESOURCE_DIR}")
    set(_pgo_resource_dir_reason "用户指定 PGO_RESOURCE_DIR")
  else()
    # 编译器自带 resource-dir 是否带 profile runtime？
    file(GLOB _pgo_rd_hits
      "${_pgo_default_rd}/lib/windows/libclang_rt.profile-*.a"
      "${_pgo_default_rd}/lib/libclang_rt.profile-*.a"
      "${_pgo_default_rd}/lib/*/libclang_rt.profile*.a")
    if(_pgo_rd_hits)
      set(_pgo_resource_dir "${_pgo_default_rd}")
      set(_pgo_resource_dir_reason "编译器自带 resource-dir 已含 profile runtime")
    else()
      # 退而求其次：在编译器安装前缀的兄弟节点里找同版本 resource-dir
      #   C:/msys64/ucrt64/bin/clang++ -> 前缀 C:/msys64/ucrt64 -> 上级 C:/msys64
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
        # .../<prefix>/lib/clang/<ver>/lib/windows/libclang_rt.profile-x86_64.a -> 回到 clang/<ver>
        string(REGEX REPLACE "/lib/(windows|[^/]+)/libclang_rt\\.profile[^/]*$" "" _pgo_cand_rd "${_pgo_hit}")
        set(_pgo_resource_dir "${_pgo_cand_rd}")
        set(_pgo_resource_dir_reason "编译器 resource-dir 缺 profile runtime，自动改用 ${_pgo_cand_rd}")
      else()
        message(FATAL_ERROR
          "[pgo] 在 ${_pgo_default_rd} 及其兄弟前缀（${_pgo_root}/*/lib/clang/${_pgo_ver}）下都找不到 "
          "libclang_rt.profile-*.a。\n"
          "      PGO 需要编译器运行时库 libclang_rt.profile；请安装含 compiler-rt 的前缀"
          "（MSYS2 下为 clang64），或显式传入 -DPGO_RESOURCE_DIR=<含 lib/windows/libclang_rt.profile-*.a 的 resource-dir>。")
      endif()
    endif()
  endif()

  if(NOT EXISTS "${_pgo_resource_dir}/lib")
    message(FATAL_ERROR "[pgo] PGO_RESOURCE_DIR='${_pgo_resource_dir}' 不是一个有效的 clang resource-dir")
  endif()
  get_filename_component(_pgo_default_rd_norm "${_pgo_default_rd}" ABSOLUTE)
  get_filename_component(_pgo_resource_dir_norm "${_pgo_resource_dir}" ABSOLUTE)
  set(_pgo_need_resource_dir TRUE)
  if(_pgo_default_rd_norm STREQUAL _pgo_resource_dir_norm)
    set(_pgo_need_resource_dir FALSE)
  endif()
endif()

# llvm-profdata（GCC 不需要）
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
      # 与 resource-dir 同前缀（clang64）通常也带 llvm-profdata
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
      "[pgo] 找不到 llvm-profdata（PGO 合并 profile 必需）。请显式传入 -DPGO_PROFDATA_TOOL=<路径>。\n"
      "      本机（MSYS2）通常在 C:/msys64/clang64/bin/llvm-profdata.exe。")
  endif()
  file(TO_CMAKE_PATH "${_pgo_profdata_tool}" _pgo_profdata_tool)
endif()

# ----------------------------------------------------------------------------
# 3.5 AUTO 模式：配置期自举
#     在 <build>/pgo-bootstrap 里做「插桩构建 + 训练 + 合并」，随后本构建按 USE 编译。
#     用户视角：cmake -S . -B build && cmake --build build 即可拿到优化版本。
#     限制：clang + 单配置生成器（GCC 的 .gcda 与 build 目录绑定，无法独立目录自举）。
# ----------------------------------------------------------------------------
if(PGO_AUTO)
  if(NOT _pgo_is_clang)
    message(FATAL_ERROR
      "[pgo] PGO_MODE=AUTO 只支持 clang：GCC 的 .gcda 名字里编码了对象文件所在 build 目录，\n"
      "      无法在独立目录里自举。GCC 请用手动三阶段（同一 build 目录）：\n"
      "        cmake -B build -DPGO_MODE=GENERATE && cmake --build build --target pgo-train\n"
      "        cmake -B build -DPGO_MODE=USE      && cmake --build build")
  endif()
  if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "[pgo] PGO_MODE=AUTO 需要单配置生成器（Ninja / Makefiles）")
  endif()

  if(NOT PGO_BOOTSTRAP_DIR)
    set(PGO_BOOTSTRAP_DIR "${CMAKE_BINARY_DIR}/pgo-bootstrap")
  endif()
  file(TO_CMAKE_PATH "${PGO_BOOTSTRAP_DIR}" PGO_BOOTSTRAP_DIR)

  # ---- 判断是否需要重新自举 ----
  set(_pgo_need_boot FALSE)
  set(_pgo_boot_why "")
  if(PGO_REBOOTSTRAP)
    set(_pgo_need_boot TRUE)
    set(_pgo_boot_why "PGO_REBOOTSTRAP=ON")
  elseif(NOT EXISTS "${PGO_PROFDATA}")
    set(_pgo_need_boot TRUE)
    set(_pgo_boot_why "profile 尚不存在")
  elseif(PGO_AUTO_REBOOTSTRAP)
    # 源码比 profile 新 → 自动重训（可用 -DPGO_AUTO_REBOOTSTRAP=OFF 关闭）
    file(GLOB_RECURSE _pgo_srcs LIST_DIRECTORIES false
      "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
      "${CMAKE_SOURCE_DIR}/*.c" "${CMAKE_SOURCE_DIR}/*.cc" "${CMAKE_SOURCE_DIR}/*.cpp"
      "${CMAKE_SOURCE_DIR}/*.cxx" "${CMAKE_SOURCE_DIR}/*.c++"
      "${CMAKE_SOURCE_DIR}/*.h" "${CMAKE_SOURCE_DIR}/*.hh" "${CMAKE_SOURCE_DIR}/*.hpp"
      "${CMAKE_SOURCE_DIR}/*.hxx" "${CMAKE_SOURCE_DIR}/*.cppm" "${CMAKE_SOURCE_DIR}/*.ixx"
      "${CMAKE_SOURCE_DIR}/*.mpp")
    # 注意：实测 `PATTERN ... EXCLUDE` 对绝对路径的 GLOB_RECURSE 不可靠（会漏排），
    #      这里改用显式过滤，只保留源码树内、非构建/依赖目录的文件。
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
        set(_pgo_boot_why "源码比 profile 新（如 ${_s}）")
        break()
      endif()
    endforeach()
    if(PGO_VERBOSE AND NOT _pgo_need_boot)
      message(STATUS "[pgo] AUTO：检查了 ${CMAKE_SOURCE_DIR} 下源码（profile mtime=${_pgo_pd_ts}）")
    endif()
  endif()

  if(_pgo_need_boot)
    message(STATUS "[pgo] AUTO：需要自举（${_pgo_boot_why}）")
    message(STATUS "[pgo]   子构建目录：${PGO_BOOTSTRAP_DIR}")
    message(STATUS "[pgo]   步骤：配置插桩构建 → 构建 + 训练 + 合并 profile（首次较慢）")

    # 清理上一次的 profraw（否则新旧数据会混在一起）。只在本构建目录内自动清理。
    if(PGO_PROFILE_DIR MATCHES "^${CMAKE_BINARY_DIR}")
      file(REMOVE_RECURSE "${PGO_PROFILE_DIR}")
    else()
      file(GLOB _pgo_old_raws "${PGO_PROFILE_DIR}/*.profraw" "${PGO_PROFILE_DIR}/*/*.profraw")
      if(_pgo_old_raws)
        message(STATUS "[pgo]   注意：PGO_PROFILE_DIR 在构建目录之外，请自行确认旧 profraw 不会混入"
                       "（发现 ${_pgo_old_raws}）")
      endif()
    endif()

    # 向子构建透传用户缓存变量（非 CMAKE_/PGO_/内部）
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
        "[pgo] 自举配置失败（rc=${_pgo_rc}）。完整日志：${_pgo_boot_cfg_log}\n"
        "------ 输出尾部 ------\n${_pgo_err}")
    endif()

    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${PGO_BOOTSTRAP_DIR}" --target pgo-train
      RESULT_VARIABLE _pgo_rc2 OUTPUT_VARIABLE _pgo_out2 ERROR_VARIABLE _pgo_err2)
    file(WRITE "${_pgo_boot_build_log}" "${_pgo_out2}\n${_pgo_err2}\n")
    if(NOT _pgo_rc2 EQUAL 0)
      message(FATAL_ERROR
        "[pgo] 自举构建/训练失败（rc=${_pgo_rc2}）。完整日志：${_pgo_boot_build_log}\n"
        "------ 输出尾部 ------\n${_pgo_err2}")
    endif()
    if(NOT EXISTS "${PGO_PROFDATA}")
      message(FATAL_ERROR
        "[pgo] 自举已执行但未生成 ${PGO_PROFDATA}。\n"
        "      请确认工程用 pgo_add_training_target() 注册了训练负载（只有注册了才会真正训练）。")
    endif()
    file(SIZE "${PGO_PROFDATA}" _pgo_pd_bytes)
    math(EXPR _pgo_pd_kib "${_pgo_pd_bytes} / 1024")
    message(STATUS "[pgo] 自举完成：${PGO_PROFDATA}（${_pgo_pd_kib} KiB）→ 本构建按 USE 编译")
  else()
    message(STATUS "[pgo] AUTO：复用已有 profile ${PGO_PROFDATA}")
  endif()

  set(PGO_AUTO TRUE)
  set(PGO_USE TRUE)
  set(PGO_GENERATE FALSE)
  set(PGO_ACTIVE TRUE)
endif()

# ----------------------------------------------------------------------------
# 4. USE 模式：profile 可用性检查（strict 时配置期即失败）
# ----------------------------------------------------------------------------
if(PGO_USE)
  if(_pgo_is_clang)
    if(EXISTS "${PGO_PROFDATA}")
      file(SIZE "${PGO_PROFDATA}" _pgo_profdata_size)
      if(_pgo_profdata_size EQUAL 0)
        if(PGO_STRICT)
          message(FATAL_ERROR "[pgo] PGO_PROFDATA='${PGO_PROFDATA}' 存在但为空文件（0 字节）。")
        else()
          message(WARNING "[pgo] PGO_PROFDATA 为空文件，忽略 profile（PGO_STRICT=OFF）")
          set(PGO_USE FALSE)
        endif()
      endif()
    else()
      if(PGO_STRICT)
        message(FATAL_ERROR
          "[pgo] PGO_MODE=USE 需要已合并的 profile，但 '${PGO_PROFDATA}' 不存在。\n"
          "      先执行 GENERATE 阶段：cmake --build <build-inst> --target pgo-train\n"
          "      或显式指定 -DPGO_PROFDATA=<.../profile.profdata>（PGO_STRICT=OFF 可退化为普通构建）。")
      else()
        message(WARNING "[pgo] 找不到 '${PGO_PROFDATA}'，本次退化为普通构建（PGO_STRICT=OFF）")
        set(PGO_USE FALSE)
        if(NOT PGO_LTO)
          set(PGO_ACTIVE FALSE)
        endif()
      endif()
    endif()
  else()
    # GCC：profile 是 .gcda 文件，目录形式即可
    if(NOT IS_DIRECTORY "${PGO_PROFILE_DIR}")
      if(PGO_STRICT)
        message(FATAL_ERROR "[pgo] GCC 的 PGO_MODE=USE 需要目录 '${PGO_PROFILE_DIR}'（内含 .gcda）。")
      else()
        message(WARNING "[pgo] 找不到 '${PGO_PROFILE_DIR}'，退化为普通构建（PGO_STRICT=OFF）")
        set(PGO_USE FALSE)
      endif()
    else()
      # GCC 的 .gcda 名字里编码了**对象文件所在目录**（实测形如
      # <dir>/C~/Users/.../<build-dir>/CMakeFiles#<target>.dir#<src>.gcda），
      # 因此 GENERATE 与 USE 必须在同一个 build 目录里做（不像 clang 只需一个 profdata 文件）。
      file(GLOB_RECURSE _pgo_gcda_files "${PGO_PROFILE_DIR}/*.gcda")
      list(LENGTH _pgo_gcda_files _pgo_ngcda)
      if(_pgo_ngcda EQUAL 0)
        if(PGO_STRICT)
          message(FATAL_ERROR
            "[pgo] '${PGO_PROFILE_DIR}' 下没有 .gcda：先跑 GENERATE 阶段的 pgo-train。")
        else()
          message(WARNING "[pgo] '${PGO_PROFILE_DIR}' 下没有 .gcda，退化为普通构建")
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
            "[pgo] GCC 的 .gcda 路径与对象文件位置绑定：当前 build 目录 '${CMAKE_BINARY_DIR}' 在 "
            "'${PGO_PROFILE_DIR}' 里没有对应的 .gcda，profile 不会生效（编译时会报 -Wmissing-profile）。\n"
            "      GCC 请**在同一个 build 目录内分两阶段**：先 GENERATE + pgo-train，再把 PGO_MODE 改成 USE 重新配置并重建。")
        endif()
      endif()
    endif()
  endif()
endif()

# ----------------------------------------------------------------------------
# 5. 链接器：PGO 插桩与 LTO 都必须用 lld（GNU ld 实测失败，见 REPORT.md）
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
            "[pgo] CMAKE_${_lt}_LINKER_FLAGS 已包含非 lld 的 -fuse-ld。\n"
            "      PGO 插桩（__profc_/__profd_ COMDAT）与 LTO（bitcode）在 GNU ld 下会链接失败，"
            "必须使用 lld。\n"
            "      如确实要用其他链接器，请显式设置 -DPGO_ALLOW_ANY_LINKER=ON（风险自负）。")
        else()
          message(WARNING "[pgo] 使用非 lld 链接器，PGO/LTO 很可能链接失败（PGO_ALLOW_ANY_LINKER=ON）")
        endif()
      endif()
    else()
      set(CMAKE_${_lt}_LINKER_FLAGS "${CMAKE_${_lt}_LINKER_FLAGS} -fuse-ld=${PGO_LINKER}")
    endif()
  endforeach()
endif()

# ----------------------------------------------------------------------------
# 6. 标志组装
# ----------------------------------------------------------------------------
set(_pgo_compile_flags "")
set(_pgo_link_flags "")

if(PGO_GENERATE)
  if(_pgo_is_clang)
    if(PGO_KIND STREQUAL "IR")
      # IRPGO：编译 + 链接都要带（链接阶段负责把 libclang_rt.profile 拉进来）
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
    # 未训练到的函数会产生 -Wprofile-instr-unprofiled；profile 条目与当前代码哈希不匹配
    # （例如某个 target 没参与训练）会产生 -Wbackend-plugin。两者在“只训练主程序”的常见
    # 布局下都会出现，LLVM 官方亦建议用 -Wno-backend-plugin 抑制（见 REPORT.md）。
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
    # 未参与训练的 target 会报 -Wmissing-profile（与 clang 的 -Wbackend-plugin 对应）
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
    # GCC 无 thin/full 之分：`-flto=thin` 会报
    # "cc1plus: error: unrecognized argument to '-flto=' option: 'thin'"（实测），统一用 -flto
    if(PGO_LTO STREQUAL "THIN")
      message(STATUS "[pgo] GCC 不支持 -flto=thin，已改用 -flto（GCC 无 thin/full 之分）")
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
# 7. 注入
# ----------------------------------------------------------------------------
if(PGO_SCOPE STREQUAL "GLOBAL")
  # 追加到 CMAKE_*_FLAGS：对本次 include 之前/之后创建的所有 target 都生效
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
# 8. 函数
# ----------------------------------------------------------------------------

# pgo_apply(<target>...) —— PGO_SCOPE=TARGET 时对指定 target 生效；
# GLOBAL 作用域下也可调用（幂等：不重复添加）
function(pgo_apply)
  if(ARGC EQUAL 0)
    message(FATAL_ERROR "[pgo] pgo_apply() 需要至少一个 target")
  endif()
  if(PGO_SCOPE STREQUAL "GLOBAL")
    message(STATUS "[pgo] pgo_apply() 在 PGO_SCOPE=GLOBAL 下无必要（所有 target 已通过 CMAKE_*_FLAGS 注入）")
    return()
  endif()
  foreach(_t ${ARGN})
    if(NOT TARGET ${_t})
      message(FATAL_ERROR "[pgo] pgo_apply() 的 target '${_t}' 不存在（请在 add_library/add_executable 之后调用）")
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
    message(FATAL_ERROR "[pgo] pgo_add_training_target() 需要 NAME 与 EXECUTABLE")
  endif()
  if(NOT TARGET ${T_EXECUTABLE})
    message(FATAL_ERROR "[pgo] 训练程序 target '${T_EXECUTABLE}' 不存在")
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
  # LLVM_PROFILE_FILE：IRPGO 默认名同目录会互相覆盖（实测 3 次运行只剩 1 个 .profraw），
  # 这里统一用 %m（签名）+ %p（pid）保证每次运行独立落盘。
  # 注意：`cmake -E env K=V <cmd>` 必须是同一条 COMMAND，不能拆成两条。
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
    COMMENT "[pgo] 运行训练程序 ${T_NAME} (x${T_REPEAT})，profile -> ${PGO_PROFILE_DIR}"
    USES_TERMINAL
    VERBATIM)
  add_dependencies(pgo-train pgo-run-${T_NAME})
endfunction()

function(pgo_status)
  message(STATUS "[pgo] ---- 配置摘要 ----")
  if(PGO_AUTO)
    message(STATUS "[pgo]  模式：AUTO（配置期自举 + 本构建 USE）；自举目录 = ${PGO_BOOTSTRAP_DIR}")
  elseif(PGO_MODE STREQUAL "OFF" AND NOT PGO_LTO STREQUAL "OFF")
    message(STATUS "[pgo]  模式：仅 LTO（PGO_MODE=OFF, PGO_LTO=${PGO_LTO}）")
  endif()
  message(STATUS "[pgo]  PGO_MODE        = ${PGO_MODE}   PGO_KIND = ${PGO_KIND}   PGO_LTO = ${PGO_LTO}")
  if(_pgo_is_clang)
    set(_pgo_link_disp "${PGO_LINKER}")
  else()
    set(_pgo_link_disp "(编译器默认链接器)")
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
# 9. target：训练 / 合并 / 校验 / 清理
# ----------------------------------------------------------------------------
if(_pgo_is_clang)
  # pgo-train：跑完所有注册的训练程序后自动合并（一条命令完成 GENERATE 阶段）
  add_custom_target(pgo-train
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            "-DPGO_PROFDATA=${PGO_PROFDATA}"
            "-DPGO_PROFDATA_TOOL=${_pgo_profdata_tool}"
            -P "${_PGO_SCRIPT}"
    COMMENT "[pgo] 训练完成，合并 profraw -> ${PGO_PROFDATA}"
    VERBATIM)

  # 只合并（训练已单独跑过时使用）
  add_custom_target(pgo-merge
    COMMAND "${CMAKE_COMMAND}"
            "-DPGO_PROFILE_DIR=${PGO_PROFILE_DIR}"
            "-DPGO_PROFDATA=${PGO_PROFDATA}"
            "-DPGO_PROFDATA_TOOL=${_pgo_profdata_tool}"
            -P "${_PGO_SCRIPT}"
    COMMENT "[pgo] 合并 profraw -> ${PGO_PROFDATA}"
    VERBATIM)

  add_custom_target(pgo-check
    COMMAND "${_pgo_profdata_tool}" show --detailed-summary "${PGO_PROFDATA}"
    COMMENT "[pgo] 校验 profile（llvm-profdata show --detailed-summary）"
    VERBATIM)
else()
  # GCC：gcda 直接可用，无需合并
  add_custom_target(pgo-train
    COMMENT "[pgo] 训练完成（GCC：gcda 无需合并，直接位于 ${PGO_PROFILE_DIR}）")
  add_custom_target(pgo-merge
    COMMENT "[pgo] GCC 的 gcda 无需合并（pgo-merge 为空操作）")
  add_custom_target(pgo-check
    COMMENT "[pgo] GCC 模式下请直接查看 ${PGO_PROFILE_DIR} 下的 .gcda")
endif()

add_custom_target(pgo-clean
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${PGO_PROFILE_DIR}"
  COMMENT "[pgo] 删除 ${PGO_PROFILE_DIR}"
  VERBATIM)

if(PGO_VERBOSE)
  pgo_status()
  if(PGO_GENERATE)
    message(STATUS "[pgo] 提示：GENERATE 阶段请用 `cmake --build <dir> --target pgo-train` 触发训练与合并")
  elseif(PGO_AUTO)
    message(STATUS "[pgo] 提示：AUTO 模式下直接 `cmake --build <dir>` 即可；"
                   "需要重新训练时加 -DPGO_REBOOTSTRAP=ON 重新配置")
  endif()
endif()
