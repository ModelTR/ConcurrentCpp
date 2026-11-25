# MSYS2 UCRT64 toolchain.
#
# The UCRT presets must never silently fall back to mingw64.  The compiler
# kind is supplied by the preset (clang or gcc); the UCRT64 prefix is
# discovered from the environment, PATH and common MSYS2 installations.
# Configuration fails if no UCRT64 prefix is found.
#
# Supported overrides:
#   MSYS2_UCRT64_PREFIX   direct path to the ucrt64 prefix
#   MSYS2_ROOT            path to the MSYS2 root
#   MINGW_PREFIX / MSYSTEM_PREFIX  when they point at ucrt64

if(NOT WIN32)
    message(FATAL_ERROR "[toolchain] the MSYS2 UCRT64 presets are Windows-only")
endif()

# try_compile() does not forward ordinary cache variables to the child
# project, so the kind must be declared as a platform variable.  The
# environment fallback keeps manual -DCMAKE_TOOLCHAIN_FILE= usage working.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MSYS2_UCRT64_COMPILER_KIND)
if(NOT MSYS2_UCRT64_COMPILER_KIND AND DEFINED ENV{MSYS2_UCRT64_COMPILER_KIND})
    set(MSYS2_UCRT64_COMPILER_KIND "$ENV{MSYS2_UCRT64_COMPILER_KIND}")
endif()
if(NOT MSYS2_UCRT64_COMPILER_KIND MATCHES "^(clang|gcc)$")
    message(FATAL_ERROR
        "[toolchain] MSYS2_UCRT64_COMPILER_KIND must be clang or gcc "
        "(set by the UCRT preset)")
endif()

if(MSYS2_UCRT64_COMPILER_KIND STREQUAL "clang")
    set(_ucrt_c_name "clang.exe")
    set(_ucrt_cxx_name "clang++.exe")
    set(_ucrt_probe_name "clang++.exe")
else()
    set(_ucrt_c_name "gcc.exe")
    set(_ucrt_cxx_name "g++.exe")
    set(_ucrt_probe_name "g++.exe")
endif()

function(_ucrt_normalize_path out value)
    set(_normalized "${value}")
    file(TO_CMAKE_PATH "${_normalized}" _normalized)
    if(_normalized MATCHES "^/([A-Za-z])/(.*)$")
        string(TOUPPER "${CMAKE_MATCH_1}" _drive)
        set(_normalized "${_drive}:/${CMAKE_MATCH_2}")
    endif()
    set(${out} "${_normalized}" PARENT_SCOPE)
endfunction()

set(_ucrt_candidates "")

if(DEFINED MSYS2_UCRT64_PREFIX AND NOT "${MSYS2_UCRT64_PREFIX}" STREQUAL "")
    _ucrt_normalize_path(_candidate "${MSYS2_UCRT64_PREFIX}")
    list(APPEND _ucrt_candidates "${_candidate}")
endif()
if(DEFINED ENV{MSYS2_UCRT64_PREFIX})
    _ucrt_normalize_path(_candidate "$ENV{MSYS2_UCRT64_PREFIX}")
    list(APPEND _ucrt_candidates "${_candidate}")
endif()
if(DEFINED ENV{MSYS2_ROOT})
    _ucrt_normalize_path(_root "$ENV{MSYS2_ROOT}")
    get_filename_component(_root_name "${_root}" NAME)
    if(_root_name STREQUAL "ucrt64")
        list(APPEND _ucrt_candidates "${_root}")
    else()
        list(APPEND _ucrt_candidates "${_root}/ucrt64")
    endif()
endif()

foreach(_env_name IN ITEMS MINGW_PREFIX MSYSTEM_PREFIX)
    if(DEFINED ENV{${_env_name}})
        _ucrt_normalize_path(_candidate "$ENV{${_env_name}}")
        get_filename_component(_candidate_name "${_candidate}" NAME)
        if(_candidate_name STREQUAL "ucrt64")
            list(APPEND _ucrt_candidates "${_candidate}")
        endif()
    endif()
endforeach()

find_program(_ucrt_pacman NAMES pacman.exe pacman NO_CACHE)
if(_ucrt_pacman)
    _ucrt_normalize_path(_pacman_path "${_ucrt_pacman}")
    get_filename_component(_pacman_path "${_pacman_path}" DIRECTORY) # usr/bin
    get_filename_component(_pacman_path "${_pacman_path}" DIRECTORY) # usr
    get_filename_component(_pacman_path "${_pacman_path}" DIRECTORY) # root
    list(APPEND _ucrt_candidates "${_pacman_path}/ucrt64")
endif()

find_program(_ucrt_probe_compiler NAMES "${_ucrt_probe_name}" NO_CACHE)
if(_ucrt_probe_compiler)
    _ucrt_normalize_path(_probe_path "${_ucrt_probe_compiler}")
    get_filename_component(_probe_bin "${_probe_path}" DIRECTORY)
    get_filename_component(_probe_prefix "${_probe_bin}" DIRECTORY)
    get_filename_component(_probe_prefix_name "${_probe_prefix}" NAME)
    if(_probe_prefix_name STREQUAL "ucrt64")
        list(APPEND _ucrt_candidates "${_probe_prefix}")
    endif()
endif()

foreach(_root IN ITEMS
        "C:/msys64" "C:/msys2" "C:/tools/msys64" "C:/tools/msys2"
        "D:/msys64" "D:/msys2"
        "$ENV{ProgramFiles}/msys64" "$ENV{LOCALAPPDATA}/Programs/msys64")
    _ucrt_normalize_path(_candidate "${_root}/ucrt64")
    list(APPEND _ucrt_candidates "${_candidate}")
endforeach()

set(MSYS2_UCRT64_PREFIX "")
foreach(_candidate IN LISTS _ucrt_candidates)
    if(_candidate
       AND EXISTS "${_candidate}/bin/${_ucrt_c_name}"
       AND EXISTS "${_candidate}/bin/${_ucrt_cxx_name}")
        set(MSYS2_UCRT64_PREFIX "${_candidate}")
        break()
    endif()
endforeach()

if(NOT MSYS2_UCRT64_PREFIX)
    message(FATAL_ERROR
        "[toolchain] MSYS2 UCRT64 (${MSYS2_UCRT64_COMPILER_KIND}) was not found, "
        "so the UCRT preset cannot run.\n"
        "  Set MSYS2_ROOT=<msys2 root> or MSYS2_UCRT64_PREFIX=<root>/ucrt64.\n"
        "  Expected compiler: ucrt64/bin/${_ucrt_cxx_name}.\n"
        "  The build will not fall back to mingw64.")
endif()

set(MSYS2_UCRT64_PREFIX "${MSYS2_UCRT64_PREFIX}" CACHE PATH
    "Detected MSYS2 UCRT64 prefix" FORCE)
set(CMAKE_C_COMPILER "${MSYS2_UCRT64_PREFIX}/bin/${_ucrt_c_name}"
    CACHE FILEPATH "MSYS2 UCRT64 C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${MSYS2_UCRT64_PREFIX}/bin/${_ucrt_cxx_name}"
    CACHE FILEPATH "MSYS2 UCRT64 C++ compiler" FORCE)
list(PREPEND CMAKE_PREFIX_PATH "${MSYS2_UCRT64_PREFIX}")
# Ninja and the other MSYS2 tools live in the same prefix.  Adding it to the
# program search path keeps the configure step working when the IDE or the
# caller starts CMake from an environment without ucrt64/bin on PATH.
list(PREPEND CMAKE_PROGRAM_PATH "${MSYS2_UCRT64_PREFIX}/bin")

# Resolve sccache/ccache to an absolute path inside the same prefix, so the
# launcher cannot be silently selected from another PATH entry.
foreach(_launcher_var IN ITEMS
        CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER)
    if(DEFINED ${_launcher_var} AND NOT "${${_launcher_var}}" STREQUAL "")
        set(_launcher "${${_launcher_var}}")
        if(NOT IS_ABSOLUTE "${_launcher}")
            find_program(_launcher_path NAMES "${_launcher}"
                PATHS "${MSYS2_UCRT64_PREFIX}/bin" NO_DEFAULT_PATH)
            if(NOT _launcher_path)
                message(FATAL_ERROR
                    "[toolchain] compiler launcher '${_launcher}' was not found "
                    "in ${MSYS2_UCRT64_PREFIX}/bin")
            endif()
            set(${_launcher_var} "${_launcher_path}" CACHE FILEPATH
                "Resolved compiler launcher" FORCE)
        endif()
    endif()
endforeach()

message(STATUS "[toolchain] MSYS2 UCRT64 prefix: ${MSYS2_UCRT64_PREFIX}")
message(STATUS "[toolchain] C compiler:  ${CMAKE_C_COMPILER}")
message(STATUS "[toolchain] C++ compiler: ${CMAKE_CXX_COMPILER}")
