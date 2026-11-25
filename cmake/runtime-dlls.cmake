#[[============================================================================
 runtime-dlls.cmake -- keep shared dependency DLLs next to the executables
----------------------------------------------------------------------------
 On MinGW/UCRT builds the compiler runtime (libstdc++-6.dll, libgcc_s_*.dll,
 libwinpthread-1.dll, ...) and the shared dependencies (absl, benchmark, gtest)
 live outside the build tree.  Copying them next to every executable keeps
 training runs, gtest discovery and ctest independent of the caller's PATH.

 Usage (automatic, nothing else to call):

   include(<workspace>/cmake/runtime-dlls.cmake)

 The module defers one directory-level pass until every target exists and
 attaches a POST_BUILD copy command only to executables that actually need it
 (a shared library dependency or a compiler runtime next to the compiler).

 Set COUCURRENT_COPY_RUNTIME_DLLS to OFF before including the module to opt
 out (for example when the binaries are deployed differently).
============================================================================]]

option(COUCURRENT_COPY_RUNTIME_DLLS
    "Copy the compiler runtime and shared dependency DLLs next to the executables"
    ON)

function(coucurrent_copy_runtime_dlls target)
    if(NOT WIN32 OR MSVC OR NOT COUCURRENT_COPY_RUNTIME_DLLS)
        return()
    endif()
    get_target_property(_target_type ${target} TYPE)
    if(NOT _target_type STREQUAL "EXECUTABLE")
        return()
    endif()

    # Compiler runtime: probe the directory the compiler itself lives in, so
    # the module does not need to know the MSYS2 prefix layout.
    get_filename_component(_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_runtime_dlls "")
    foreach(_dll_name IN ITEMS
            libstdc++-6.dll libgcc_s_seh-1.dll libgcc_s_dw2-1.dll
            libwinpthread-1.dll libatomic-1.dll libc++.dll libunwind.dll
            libssp-0.dll)
        if(EXISTS "${_compiler_bin}/${_dll_name}")
            list(APPEND _runtime_dlls "${_compiler_bin}/${_dll_name}")
        endif()
    endforeach()

    # Shared dependencies are derived from the link interface, so callers do
    # not have to repeat the dependency list a second time.
    set(_has_shared_dependency FALSE)
    get_target_property(_linked_targets ${target} LINK_LIBRARIES)
    if(_linked_targets)
        foreach(_dependency IN LISTS _linked_targets)
            if(TARGET ${_dependency})
                get_target_property(_dependency_type ${_dependency} TYPE)
                if(_dependency_type STREQUAL "SHARED_LIBRARY")
                    set(_has_shared_dependency TRUE)
                endif()
            endif()
        endforeach()
    endif()
    # An empty copy_if_different invocation is a hard build error, so the
    # command is emitted only when at least one file will be copied.
    if(NOT _has_shared_dependency AND NOT _runtime_dlls)
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${target}>
            ${_runtime_dlls}
            "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS
        VERBATIM)
endfunction()

function(_coucurrent_enable_runtime_dlls)
    get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        coucurrent_copy_runtime_dlls(${_target})
    endforeach()
endfunction()

# Defer until the directory is fully processed, so every executable target
# already exists and no call site has to remember this step.
cmake_language(DEFER CALL _coucurrent_enable_runtime_dlls)
