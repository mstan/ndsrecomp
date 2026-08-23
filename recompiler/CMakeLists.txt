cmake_minimum_required(VERSION 3.20)
project(nds_recompile VERSION 0.0.1 LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsCompilerCache.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsJobPools.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsTomlPlusPlus.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../external/arm-recomp-core/cmake/ArmRecompCore.cmake")

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Shared ARM core (decoder + IR + interpreter + NDS ARMv5TE codegen profile).
arm_recomp_core_sources(NDS_ARM_RECOMP_SOURCES armv5te_nds)
arm_recomp_core_include_dirs(NDS_ARM_RECOMP_INCLUDE_DIRS armv5te_nds)
add_library(nds_armv4t STATIC ${NDS_ARM_RECOMP_SOURCES})
target_include_directories(nds_armv4t PUBLIC ${NDS_ARM_RECOMP_INCLUDE_DIRS})

# Decode + codegen coverage for the ARMv5TE extensions (not exercised by
# the BIOS audits). Build with `cmake --build <dir> --target armv5te_decode_test`
# and run the binary; exit code 0 = pass.
add_executable(armv5te_decode_test tests/armv5te_decode_test.cpp)
target_link_libraries(armv5te_decode_test PRIVATE nds_armv4t)

add_executable(interpreter_cycle_test
    tests/interpreter_cycle_test.cpp)
target_link_libraries(interpreter_cycle_test PRIVATE nds_armv4t)

add_executable(hle_manifest_test
    tests/hle_manifest_test.cpp
    finder/config.cpp
    support/sha1.cpp)
target_include_directories(hle_manifest_test PRIVATE
    finder support ${NDS_ARM_RECOMP_INCLUDE_DIRS})
ndsrecomp_link_tomlpp(hle_manifest_test)
add_test(NAME hle_manifest_test COMMAND hle_manifest_test)

# The recompiler driver: config + function finder + audit/emit.
add_executable(nds_recompile
    src/main.cpp
    finder/config.cpp
    finder/function_finder.cpp
    finder/reloc_scan.cpp
    support/sha1.cpp)
target_include_directories(nds_recompile PRIVATE
    finder support armv4t ${NDS_ARM_RECOMP_INCLUDE_DIRS})
ndsrecomp_link_tomlpp(nds_recompile)
target_link_libraries(nds_recompile PRIVATE nds_armv4t)

# Pins the interior-entry fail-closed contract: every generated body's resume
# switch must keep its `default: runtime_dispatch_bad_entry(...); return;` arm.
# Content-validated overlay banks rely on it — losing the arm would turn an
# interior-PC entry into a silent no-op instead of a Tier-3 fallback.
add_executable(interior_entry_fail_closed_test
    tests/interior_entry_fail_closed_test.cpp
    support/sha1.cpp)
target_include_directories(interior_entry_fail_closed_test PRIVATE support)
add_test(NAME interior_entry_fail_closed
    COMMAND interior_entry_fail_closed_test
            $<TARGET_FILE:nds_recompile>
            ${CMAKE_CURRENT_BINARY_DIR}/interior_entry_pin)

add_executable(live_transfer_validation_test
    tests/live_transfer_validation_test.cpp
    support/sha1.cpp)
target_include_directories(live_transfer_validation_test PRIVATE support)
add_test(NAME live_transfer_validation
    COMMAND live_transfer_validation_test
            $<TARGET_FILE:nds_recompile>
            ${CMAKE_CURRENT_BINARY_DIR}/live_transfer_validation)
