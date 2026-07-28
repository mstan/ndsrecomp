include_guard(GLOBAL)

# finder/config.cpp includes toml++ as a single header from recompiler/
# third_party/, which the blanket third_party/ ignore keeps out of the tree.
# Stage the pinned release so a fresh checkout configures without an
# out-of-band manual drop.
set(NDSRECOMP_TOMLPP_VERSION "3.4.0" CACHE STRING
    "toml++ release to stage into recompiler/third_party")
set(NDSRECOMP_TOMLPP_SHA256
    "6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e"
    CACHE STRING "Expected SHA-256 of the toml++ single header")
set(NDSRECOMP_TOMLPP_HEADER "" CACHE FILEPATH
    "Local toml++ single header to stage instead of downloading")

# Cached so ndsrecomp_link_tomlpp() resolves it from any directory scope.
set(NDSRECOMP_TOMLPP_DIR "${CMAKE_CURRENT_LIST_DIR}/../recompiler/third_party"
    CACHE INTERNAL "Directory holding the staged toml++ single header")

function(_ndsrecomp_stage_tomlpp destination)
    if(EXISTS "${destination}")
        return()
    endif()

    if(NDSRECOMP_TOMLPP_HEADER)
        if(NOT EXISTS "${NDSRECOMP_TOMLPP_HEADER}")
            message(FATAL_ERROR
                "NDSRECOMP_TOMLPP_HEADER does not exist: "
                "${NDSRECOMP_TOMLPP_HEADER}")
        endif()
        configure_file("${NDSRECOMP_TOMLPP_HEADER}" "${destination}" COPYONLY)
        message(STATUS "ndsrecomp: staged toml++ from "
                       "${NDSRECOMP_TOMLPP_HEADER}")
        return()
    endif()

    message(STATUS
        "ndsrecomp: fetching toml++ v${NDSRECOMP_TOMLPP_VERSION} header")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/marzer/tomlplusplus/v${NDSRECOMP_TOMLPP_VERSION}/toml.hpp"
        "${destination}"
        EXPECTED_HASH "SHA256=${NDSRECOMP_TOMLPP_SHA256}"
        STATUS status
        TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${destination}")
        list(GET status 1 error)
        message(FATAL_ERROR
            "Failed to fetch toml++ (${error}). Pass "
            "-DNDSRECOMP_TOMLPP_HEADER=<path to toml.hpp> to stage a local "
            "copy instead.")
    endif()
endfunction()

_ndsrecomp_stage_tomlpp("${NDSRECOMP_TOMLPP_DIR}/toml.hpp")

# toml++ trips -Wdeprecated-literal-operator on C++20 clang, so keep it off the
# ordinary include path and out of the project's warning budget.
function(ndsrecomp_link_tomlpp target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "ndsrecomp_link_tomlpp: unknown target ${target}")
    endif()
    target_include_directories("${target}" SYSTEM PRIVATE
        "${NDSRECOMP_TOMLPP_DIR}")
endfunction()
