include_guard(GLOBAL)

set(NDSRECOMP_COMPILER_CACHE "AUTO" CACHE STRING
    "Compiler cache: AUTO, OFF, or a ccache/sccache executable")
set_property(CACHE NDSRECOMP_COMPILER_CACHE PROPERTY STRINGS AUTO OFF)

unset(_ndsrecomp_cache_program CACHE)
unset(_ndsrecomp_cache_program)
if(NOT NDSRECOMP_COMPILER_CACHE STREQUAL "OFF")
    if(NDSRECOMP_COMPILER_CACHE STREQUAL "AUTO")
        find_program(_ndsrecomp_cache_program
            NAMES sccache ccache
            HINTS C:/msys64/mingw64/bin)
    elseif(EXISTS "${NDSRECOMP_COMPILER_CACHE}")
        set(_ndsrecomp_cache_program "${NDSRECOMP_COMPILER_CACHE}")
    else()
        find_program(_ndsrecomp_cache_program
            NAMES "${NDSRECOMP_COMPILER_CACHE}")
        if(NOT _ndsrecomp_cache_program)
            message(FATAL_ERROR
                "NDSRECOMP_COMPILER_CACHE does not name an executable: "
                "${NDSRECOMP_COMPILER_CACHE}")
        endif()
    endif()
endif()

function(_ndsrecomp_configure_compiler_launcher language)
    set(launcher "CMAKE_${language}_COMPILER_LAUNCHER")
    set(managed "NDSRECOMP_MANAGED_${language}_COMPILER_LAUNCHER")

    if(NDSRECOMP_COMPILER_CACHE STREQUAL "OFF")
        if(DEFINED ${managed} AND NOT "${${managed}}" STREQUAL "" AND
           "${${launcher}}" STREQUAL "${${managed}}")
            unset(${launcher} CACHE)
        endif()
        unset(${managed} CACHE)
        return()
    endif()

    # An explicit launcher belongs to the parent project/user. The only value
    # this module may update is the one it previously installed itself.
    if(DEFINED ${launcher} AND NOT "${${launcher}}" STREQUAL "" AND
       (NOT DEFINED ${managed} OR
        NOT "${${launcher}}" STREQUAL "${${managed}}"))
        return()
    endif()
    if(_ndsrecomp_cache_program)
        set(${launcher} "${_ndsrecomp_cache_program}" CACHE STRING
            "Launcher for ${language} compilation" FORCE)
        set(${managed} "${_ndsrecomp_cache_program}" CACHE INTERNAL
            "Compiler launcher selected by ndsrecomp" FORCE)
    elseif(DEFINED ${managed})
        if("${${launcher}}" STREQUAL "${${managed}}")
            unset(${launcher} CACHE)
        endif()
        unset(${managed} CACHE)
    endif()
endfunction()

_ndsrecomp_configure_compiler_launcher(C)
_ndsrecomp_configure_compiler_launcher(CXX)

if(_ndsrecomp_cache_program)
    message(STATUS "ndsrecomp: compiler cache enabled: ${_ndsrecomp_cache_program}")
elseif(NDSRECOMP_COMPILER_CACHE STREQUAL "AUTO")
    message(STATUS "ndsrecomp: compiler cache not found")
endif()
