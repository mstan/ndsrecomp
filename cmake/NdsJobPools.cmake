include_guard(GLOBAL)

set(NDSRECOMP_COMPILE_JOBS "4" CACHE STRING
    "Maximum concurrent ndsrecomp compiler processes under Ninja")
set(NDSRECOMP_LINK_JOBS "1" CACHE STRING
    "Maximum concurrent ndsrecomp linker processes under Ninja")

foreach(variable IN ITEMS
        NDSRECOMP_COMPILE_JOBS NDSRECOMP_LINK_JOBS)
    if(NOT "${${variable}}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "${variable} must be a positive integer")
    endif()
endforeach()

function(_ndsrecomp_define_pool name depth)
    if(NOT CMAKE_GENERATOR MATCHES "Ninja")
        return()
    endif()
    get_property(pools GLOBAL PROPERTY JOB_POOLS)
    if(pools)
        list(FILTER pools EXCLUDE REGEX "^${name}=")
    endif()
    list(APPEND pools "${name}=${depth}")
    set_property(GLOBAL PROPERTY JOB_POOLS "${pools}")
endfunction()

function(ndsrecomp_limit_generated_compiles target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "ndsrecomp_limit_generated_compiles: unknown target ${target}")
    endif()
    if(CMAKE_GENERATOR MATCHES "Ninja")
        _ndsrecomp_define_pool(ndsrecomp_compile
                               "${NDSRECOMP_COMPILE_JOBS}")
        set_property(TARGET "${target}" PROPERTY JOB_POOL_COMPILE
            ndsrecomp_compile)
    endif()
endfunction()

if(CMAKE_GENERATOR MATCHES "Ninja")
    _ndsrecomp_define_pool(ndsrecomp_compile "${NDSRECOMP_COMPILE_JOBS}")
    # All project compiler rules share one pool. Otherwise the generated pool
    # can overlap its full depth with an unbounded wave of ordinary sources.
    set(CMAKE_JOB_POOL_COMPILE ndsrecomp_compile)
endif()

function(ndsrecomp_limit_link target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "ndsrecomp_limit_link: unknown target ${target}")
    endif()
    if(CMAKE_GENERATOR MATCHES "Ninja")
        _ndsrecomp_define_pool(ndsrecomp_link "${NDSRECOMP_LINK_JOBS}")
        set_property(TARGET "${target}" PROPERTY JOB_POOL_LINK ndsrecomp_link)
    endif()
endfunction()
