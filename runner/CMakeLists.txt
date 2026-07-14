cmake_minimum_required(VERSION 3.20)
project(nds_runner C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)   # the recompiled banks are huge; -O matters
endif()
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# The recompiled ARM9 BIOS bank (generated; compiled as C). Generated TUs
# trade warning-cleanliness for uniform emission — silence the expected
# unused-temp warnings rather than touch generated/.
set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/../generated)
file(GLOB FW_BANK_BODIES CONFIGURE_DEPENDS "${GEN}/fw_*_[0-9][0-9].c")
file(GLOB FW_BANK_DISPATCH CONFIGURE_DEPENDS "${GEN}/fw_*_dispatch.c")
list(SORT FW_BANK_BODIES)
list(SORT FW_BANK_DISPATCH)
add_library(nds_banks STATIC
    ${GEN}/arm9_bios.c
    ${GEN}/arm9_bios_dispatch.c
    ${GEN}/arm7_bios.c
    ${GEN}/arm7_bios_dispatch.c
    ${FW_BANK_BODIES}
    ${FW_BANK_DISPATCH})
target_include_directories(nds_banks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t)
set_source_files_properties(
    ${GEN}/arm9_bios.c ${GEN}/arm9_bios_dispatch.c
    ${GEN}/arm7_bios.c ${GEN}/arm7_bios_dispatch.c
    ${FW_BANK_BODIES}
    ${FW_BANK_DISPATCH}
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-but-set-variable")

# Every immutable bank is CPU-specific even though both use one runtime ABI.
# Tell the C compiler which core owns each source so it can fold the emitted
# timing selection without changing generated C or the two-CPU scheduler.
set(ARM9_BANK_SOURCES ${GEN}/arm9_bios.c ${GEN}/arm9_bios_dispatch.c)
set(ARM7_BANK_SOURCES ${GEN}/arm7_bios.c ${GEN}/arm7_bios_dispatch.c)
foreach(src IN LISTS FW_BANK_BODIES FW_BANK_DISPATCH)
    if(src MATCHES "/fw_arm9_")
        list(APPEND ARM9_BANK_SOURCES ${src})
    elseif(src MATCHES "/fw_arm7_")
        list(APPEND ARM7_BANK_SOURCES ${src})
    endif()
endforeach()
set_source_files_properties(${ARM9_BANK_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS "NDS_STATIC_CPU=0")
set_source_files_properties(${ARM7_BANK_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS "NDS_STATIC_CPU=1")

# SHA-1 helper (reused from the recompiler support lib).
add_library(nds_support STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support/sha1.cpp)
target_include_directories(nds_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support)

# The runner: runtime ABI + DS bus + CP15 + driver.
# The portable ARMv4T/v5TE core (decoders + IR + the Tier-3 interpreter).
add_library(nds_armv4t STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t/arm_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t/thumb_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t/arm_ir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t/interpreter.cpp)
target_include_directories(nds_armv4t PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t)

add_executable(nds_runner
    src/runtime_arm.cpp
    src/bus.cpp
    src/cp15.cpp
    src/io.cpp
    src/wifi.cpp
    src/spu.cpp
    src/vram.cpp
    src/gpu2d.cpp
    src/tier3.cpp
    src/scheduler.cpp
    src/debug_server.cpp
    src/frontend.cpp
    src/main.cpp)
target_include_directories(nds_runner PRIVATE
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t   # runtime_arm.h, interpreter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support)  # sha1.h
target_link_libraries(nds_runner PRIVATE nds_banks nds_support nds_armv4t)

# SDL is a host-only presentation layer. The runner remains buildable without
# it for headless deterministic/oracle work; --interactive reports that the
# configured binary has no frontend in that case.
find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
    target_compile_definitions(nds_runner PRIVATE NDS_HAVE_SDL2=1)
    target_link_libraries(nds_runner PRIVATE SDL2::SDL2)
endif()
if(WIN32)
    target_link_libraries(nds_runner PRIVATE ws2_32)
endif()
