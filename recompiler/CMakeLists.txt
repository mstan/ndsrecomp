cmake_minimum_required(VERSION 3.20)
project(nds_recompile VERSION 0.0.1 LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsCompilerCache.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsJobPools.cmake")

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Portable ARM core (decoder + IR + codegen). Ported from gbarecomp
# src/armv4t; ARMv5TE + CP15 extensions land here through Phase 1.
add_library(nds_armv4t STATIC
    armv4t/arm_decode.cpp
    armv4t/thumb_decode.cpp
    armv4t/arm_ir.cpp
    armv4t/arm_codegen.cpp)
target_include_directories(nds_armv4t PUBLIC armv4t)

# Decode + codegen coverage for the ARMv5TE extensions (not exercised by
# the BIOS audits). Build with `cmake --build <dir> --target armv5te_decode_test`
# and run the binary; exit code 0 = pass.
add_executable(armv5te_decode_test tests/armv5te_decode_test.cpp)
target_link_libraries(armv5te_decode_test PRIVATE nds_armv4t)

add_executable(interpreter_cycle_test
    tests/interpreter_cycle_test.cpp
    armv4t/interpreter.cpp)
target_link_libraries(interpreter_cycle_test PRIVATE nds_armv4t)

add_executable(hle_manifest_test
    tests/hle_manifest_test.cpp
    finder/config.cpp
    support/sha1.cpp)
target_include_directories(hle_manifest_test PRIVATE
    finder support third_party armv4t)
add_test(NAME hle_manifest_test COMMAND hle_manifest_test)

# The recompiler driver: config + function finder + audit/emit.
add_executable(nds_recompile
    src/main.cpp
    finder/config.cpp
    finder/function_finder.cpp
    support/sha1.cpp)
target_include_directories(nds_recompile PRIVATE
    finder support third_party armv4t)
target_link_libraries(nds_recompile PRIVATE nds_armv4t)
