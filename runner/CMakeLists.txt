cmake_minimum_required(VERSION 3.20)
project(nds_runner VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)   # the recompiled banks are huge; -O matters
endif()
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

option(NDS_BOOTSTRAP_FIRMWARE
    "Build with BIOS banks only so guest-produced firmware RAM can be captured"
    OFF)
set(NDS_SM64DS_BANK_DIR "" CACHE PATH
    "Optional ignored directory containing generated sm64ds_arm7/arm9 banks")

# The recompiled ARM9 BIOS bank (generated; compiled as C). Generated TUs
# trade warning-cleanliness for uniform emission — silence the expected
# unused-temp warnings rather than touch generated/.
set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/../generated)
if(NDS_BOOTSTRAP_FIRMWARE)
    set(FW_BANK_BODIES)
    set(FW_BANK_DISPATCH)
else()
    file(GLOB FW_BANK_BODIES CONFIGURE_DEPENDS "${GEN}/fw_*_[0-9][0-9].c")
    file(GLOB FW_BANK_DISPATCH CONFIGURE_DEPENDS "${GEN}/fw_*_dispatch.c")
endif()
list(SORT FW_BANK_BODIES)
list(SORT FW_BANK_DISPATCH)
set(SM64DS_BANK_SOURCES)
if(NDS_SM64DS_BANK_DIR)
    file(GLOB SM64DS_BANK_SOURCES CONFIGURE_DEPENDS
        "${NDS_SM64DS_BANK_DIR}/sm64ds_arm7_*.c"
        "${NDS_SM64DS_BANK_DIR}/sm64ds_arm9_*.c")
    list(SORT SM64DS_BANK_SOURCES)
    if(NOT SM64DS_BANK_SOURCES)
        message(FATAL_ERROR
            "NDS_SM64DS_BANK_DIR has no generated sm64ds bank sources")
    endif()
endif()
add_library(nds_banks STATIC
    ${GEN}/arm9_bios.c
    ${GEN}/arm9_bios_dispatch.c
    ${GEN}/arm7_bios.c
    ${GEN}/arm7_bios_dispatch.c
    ${FW_BANK_BODIES}
    ${FW_BANK_DISPATCH}
    ${SM64DS_BANK_SOURCES})
target_include_directories(nds_banks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
    ${NDS_SM64DS_BANK_DIR})
set_source_files_properties(
    ${GEN}/arm9_bios.c ${GEN}/arm9_bios_dispatch.c
    ${GEN}/arm7_bios.c ${GEN}/arm7_bios_dispatch.c
    ${FW_BANK_BODIES}
    ${FW_BANK_DISPATCH}
    ${SM64DS_BANK_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-but-set-variable")
if(SM64DS_BANK_SOURCES AND NOT MSVC)
    # Game banks are on the hot path (the ARM7 runtime-RAM bank carries the
    # sound engine); boot correctness is gate-proven (G1/G2/G3), so compile
    # them optimized. -g0 keeps the large generated TUs cheap to link.
    set_source_files_properties(${SM64DS_BANK_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w;-O2;-g0")
endif()

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
foreach(src IN LISTS SM64DS_BANK_SOURCES)
    if(src MATCHES "sm64ds_arm9_")
        list(APPEND ARM9_BANK_SOURCES ${src})
    elseif(src MATCHES "sm64ds_arm7_")
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
    src/gpu3d.cpp
    src/tier3.cpp
    src/profile_report.cpp
    src/scheduler.cpp
    src/debug_server.cpp
    src/frontend.cpp
    src/main.cpp
    vendor/melonds/GPU3D.cpp
    vendor/melonds/GPU3D_Soft.cpp
    vendor/melonds/Savestate.cpp)
target_include_directories(nds_runner PRIVATE
    src
    vendor/melonds                                     # vendored melonDS GPU3D + shims (GPL-3.0-or-later)
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t   # runtime_arm.h, interpreter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support)  # sha1.h
if(NOT MSVC)
    # Unmodified upstream melonDS sources carry their own warning posture;
    # keep the runner's -Wall -Wextra signal clean without editing vendored
    # code.
    set_source_files_properties(
        vendor/melonds/GPU3D.cpp
        vendor/melonds/GPU3D_Soft.cpp
        vendor/melonds/Savestate.cpp
        PROPERTIES COMPILE_OPTIONS "-w")
endif()
target_link_libraries(nds_runner PRIVATE nds_banks nds_support nds_armv4t)
if(NDS_BOOTSTRAP_FIRMWARE)
    target_compile_definitions(nds_runner PRIVATE NDS_BOOTSTRAP_FIRMWARE=1)
elseif(EXISTS "${GEN}/fw_arm9_calibration_save_dispatch.c")
    # The seven core captures are enough for an authentic reset-to-menu/game
    # boot.  Interaction-specific captures are optional and are registered
    # only when the complete extended set has been generated locally.
    target_compile_definitions(nds_runner PRIVATE NDS_HAVE_FW_EXTENDED_BANKS=1)
endif()
if(SM64DS_BANK_SOURCES)
    target_compile_definitions(nds_runner PRIVATE NDS_HAVE_SM64DS_BANKS=1)
    if(EXISTS "${NDS_SM64DS_BANK_DIR}/sm64ds_arm7_ram_dispatch.c")
        target_compile_definitions(nds_runner PRIVATE
            NDS_HAVE_SM64DS_RAM_BANKS=1)
    endif()
endif()

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
