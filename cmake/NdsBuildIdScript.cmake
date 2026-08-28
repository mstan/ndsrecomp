# NdsBuildIdScript.cmake -- run with `cmake -P` at BUILD time (and once at
# configure time to seed the file) to stamp the real git HEAD into a generated
# header.
#
# Why not the old configure-time execute_process: it froze the id at `cmake -B`
# time, so every incremental build after that reported a stale commit. This was
# not theoretical -- a shipped v0.6.3 runner reported build_id 12ee1b9 while
# containing behaviour that only exists from 8de5d86, which made a field
# diagnostics bundle actively misleading about what code the player was
# running. The id has to be re-read on every build, which means a build-time
# command, not a configure-time variable.
#
# Expects: NDS_SRC_DIR (a directory inside the git work tree), NDS_OUT_FILE.
# Optional: NDS_VERSION_FILE (framework VERSION file), NDS_TITLE_DIR (a
# directory inside the GAME repo's work tree -- the game's own generated bank
# dir works), NDS_TITLE_VERSION (explicit override).
#
# The game/title version is a separate axis from the runner build id: a field
# bundle needs to say both which framework built the runner AND which revision
# of the title repo produced the banks it is executing, because a bank
# regression and a framework regression look identical in a perf report.
#
# The file is only rewritten when its content actually changes, so an
# unchanged HEAD does not force main.cpp to recompile and the runner to relink
# on every single build.

if(NOT DEFINED NDS_SRC_DIR OR NOT DEFINED NDS_OUT_FILE)
    message(FATAL_ERROR "NdsBuildIdScript: NDS_SRC_DIR and NDS_OUT_FILE required")
endif()

set(_id "unknown")
set(_dirty 0)

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${NDS_SRC_DIR}"
        OUTPUT_VARIABLE _git_id
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_rc)
    if(_git_rc EQUAL 0 AND _git_id)
        set(_id "${_git_id}")
        # Tracked-file modifications only. --untracked-files=no keeps this off
        # the multi-second full-tree scan a plain `git status` would do on a
        # repo this size, and an untracked scratch file is not a change to the
        # code that got built.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${NDS_SRC_DIR}"
            OUTPUT_VARIABLE _git_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _status_rc)
        if(_status_rc EQUAL 0 AND NOT _git_status STREQUAL "")
            set(_dirty 1)
        endif()
    endif()
endif()

set(_display "${_id}")
if(_dirty)
    # A dirty tree is a build that matches NO commit. Say so in the id itself
    # rather than only in a sibling flag, because the id is what gets pasted
    # into reports.
    set(_display "${_id}-dirty")
endif()

# Framework version string from the repo-root VERSION file.
set(_framework_version "unknown")
if(NDS_VERSION_FILE AND EXISTS "${NDS_VERSION_FILE}")
    file(READ "${NDS_VERSION_FILE}" _framework_version)
    string(STRIP "${_framework_version}" _framework_version)
endif()

# Game/title version. An explicit -DNDS_TITLE_VERSION wins; otherwise derive
# it from the game repo's own git HEAD, which is what a game repo actually
# has. Absent both, "none" -- this runner was not built against a title.
set(_title_version "none")
if(NDS_TITLE_VERSION AND NOT NDS_TITLE_VERSION STREQUAL "")
    set(_title_version "${NDS_TITLE_VERSION}")
elseif(GIT_FOUND AND NDS_TITLE_DIR AND EXISTS "${NDS_TITLE_DIR}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
        WORKING_DIRECTORY "${NDS_TITLE_DIR}"
        OUTPUT_VARIABLE _title_git
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _title_rc)
    if(_title_rc EQUAL 0 AND _title_git)
        set(_title_version "${_title_git}")
    endif()
endif()

set(_content
"// GENERATED at build time by cmake/NdsBuildIdScript.cmake -- do not edit.
#pragma once
#define NDS_RUNNER_BUILD_ID \"${_display}\"
#define NDS_RUNNER_BUILD_COMMIT \"${_id}\"
#define NDS_RUNNER_BUILD_DIRTY ${_dirty}
#define NDS_FRAMEWORK_VERSION \"${_framework_version}\"
#define NDS_GAME_VERSION \"${_title_version}\"
")

set(_previous "")
if(EXISTS "${NDS_OUT_FILE}")
    file(READ "${NDS_OUT_FILE}" _previous)
endif()
if(NOT _previous STREQUAL _content)
    file(WRITE "${NDS_OUT_FILE}" "${_content}")
    message(STATUS "nds build id: ${_display}")
endif()
