cmake_minimum_required(VERSION 3.20)
project(nds_runner VERSION 0.0.1 LANGUAGES C CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsCompilerCache.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsJobPools.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsTomlPlusPlus.cmake")
enable_testing()

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
option(NDS_ENABLE_COMPUTE_RENDERER
    "Build the preferred melonDS OpenGL 4.3 compute renderer"
    ON)
option(NDS_ENABLE_PCAP_BACKEND
    "Build the vendored melonDS Net_PCap backend (dynamically loads Npcap/WinPcap at runtime; off by default per docs/adr-melonds-wifi-vendoring.md §2)"
    OFF)
option(NDS_PROFILE_HLE_HEAT
    "Build candidate-only sampled leaf heat instrumentation"
    OFF)
option(NDS_PROFILE_MEM_TIMING
    "Build candidate-only runtime memory-timing census instrumentation"
    OFF)
set(NDS_PGO_MODE "OFF" CACHE STRING
    "Runner-only GCC PGO mode: OFF, GENERATE, or USE")
set_property(CACHE NDS_PGO_MODE PROPERTY STRINGS OFF GENERATE USE)
set(NDS_GENERATED_DIR "" CACHE PATH
    "Optional ignored generated directory override for worktree builds")
set(NDS_SM64DS_BANK_DIR "" CACHE PATH
    "Optional ignored directory containing generated sm64ds_arm7/arm9 banks")
set(NDS_TITLE_BANK_DIR "" CACHE PATH
    "Optional ignored directory containing generated title banks")
set(NDS_TITLE_ROM_SHA1 "" CACHE STRING
    "Exact lowercase ROM SHA-1 allowed to register NDS_TITLE_BANK_DIR")

# The recompiled ARM9 BIOS bank (generated; compiled as C). Generated TUs
# trade warning-cleanliness for uniform emission — silence the expected
# unused-temp warnings rather than touch generated/.
if(NDS_GENERATED_DIR)
    set(GEN "${NDS_GENERATED_DIR}")
else()
    set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/../generated)
endif()
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
set(TITLE_BANK_BODIES)
set(TITLE_BANK_DISPATCH)
set(TITLE_BANK_SOURCES)
if(NDS_TITLE_BANK_DIR)
    string(LENGTH "${NDS_TITLE_ROM_SHA1}" NDS_TITLE_ROM_SHA1_LENGTH)
    if(NOT NDS_TITLE_ROM_SHA1_LENGTH EQUAL 40 OR
       NDS_TITLE_ROM_SHA1 MATCHES "[^0-9a-f]")
        message(FATAL_ERROR
            "NDS_TITLE_ROM_SHA1 must be exactly 40 lowercase hex digits "
            "when NDS_TITLE_BANK_DIR is set")
    endif()
    file(GLOB TITLE_BANK_BODIES CONFIGURE_DEPENDS
        "${NDS_TITLE_BANK_DIR}/*_[0-9][0-9].c")
    file(GLOB TITLE_BANK_DISPATCH CONFIGURE_DEPENDS
        "${NDS_TITLE_BANK_DIR}/*_dispatch.c")
    list(SORT TITLE_BANK_BODIES)
    list(SORT TITLE_BANK_DISPATCH)
    set(TITLE_BANK_SOURCES ${TITLE_BANK_BODIES} ${TITLE_BANK_DISPATCH})
    if(NOT TITLE_BANK_BODIES OR NOT TITLE_BANK_DISPATCH)
        message(FATAL_ERROR
            "NDS_TITLE_BANK_DIR must contain generated bank body and "
            "dispatch sources")
    endif()
endif()
add_library(nds_banks STATIC
    ${GEN}/arm9_bios.c
    ${GEN}/arm9_bios_dispatch.c
    ${GEN}/arm7_bios.c
    ${GEN}/arm7_bios_dispatch.c
    ${GEN}/freebios_arm9.c
    ${GEN}/freebios_arm9_dispatch.c
    ${GEN}/freebios_arm7.c
    ${GEN}/freebios_arm7_dispatch.c
    ${FW_BANK_BODIES}
    ${FW_BANK_DISPATCH}
    ${SM64DS_BANK_SOURCES}
    ${TITLE_BANK_SOURCES})
ndsrecomp_limit_generated_compiles(nds_banks)
ndsrecomp_limit_link(nds_banks)
target_include_directories(nds_banks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
    ${NDS_SM64DS_BANK_DIR}
    ${NDS_TITLE_BANK_DIR})
set_source_files_properties(
    ${GEN}/arm9_bios.c ${GEN}/arm9_bios_dispatch.c
    ${GEN}/arm7_bios.c ${GEN}/arm7_bios_dispatch.c
    ${GEN}/freebios_arm9.c ${GEN}/freebios_arm9_dispatch.c
    ${GEN}/freebios_arm7.c ${GEN}/freebios_arm7_dispatch.c
    ${FW_BANK_BODIES}
    ${FW_BANK_DISPATCH}
    ${SM64DS_BANK_SOURCES}
    ${TITLE_BANK_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-but-set-variable")
if(SM64DS_BANK_SOURCES AND NOT MSVC)
    # Game banks are on the hot path (the ARM7 runtime-RAM bank carries the
    # sound engine); boot correctness is gate-proven (G1/G2/G3), so compile
    # them optimized. -g0 keeps the large generated TUs cheap to link.
    set_source_files_properties(${SM64DS_BANK_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w;-O2;-g0")
endif()
if(TITLE_BANK_SOURCES AND NOT MSVC)
    set_source_files_properties(${TITLE_BANK_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w;-O2;-g0")
endif()

# Every immutable bank is CPU-specific even though both use one runtime ABI.
# Tell the C compiler which core owns each source so it can fold the emitted
# timing selection without changing generated C or the two-CPU scheduler.
set(ARM9_BANK_SOURCES ${GEN}/arm9_bios.c ${GEN}/arm9_bios_dispatch.c
    ${GEN}/freebios_arm9.c ${GEN}/freebios_arm9_dispatch.c)
set(ARM7_BANK_SOURCES ${GEN}/arm7_bios.c ${GEN}/arm7_bios_dispatch.c
    ${GEN}/freebios_arm7.c ${GEN}/freebios_arm7_dispatch.c)
set(SM64DS_ARM9_BANK_SOURCES)
set(SM64DS_HLE_PROFILE_SOURCES)
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
        list(APPEND SM64DS_ARM9_BANK_SOURCES ${src})
        if(src MATCHES "/sm64ds_arm9_([0-9][0-9]|dispatch)\\.c$")
            list(APPEND SM64DS_HLE_PROFILE_SOURCES ${src})
        endif()
    elseif(src MATCHES "sm64ds_arm7_")
        list(APPEND ARM7_BANK_SOURCES ${src})
    endif()
endforeach()
foreach(src IN LISTS TITLE_BANK_SOURCES)
    if(src MATCHES "([_/]|^)arm9_" OR src MATCHES "_arm9_")
        list(APPEND ARM9_BANK_SOURCES ${src})
    elseif(src MATCHES "([_/]|^)arm7_" OR src MATCHES "_arm7_")
        list(APPEND ARM7_BANK_SOURCES ${src})
    else()
        message(FATAL_ERROR
            "Cannot infer title-bank CPU from source name: ${src}. "
            "Include _arm9_ or _arm7_ in every bank name.")
    endif()
endforeach()
set_source_files_properties(${ARM9_BANK_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS "NDS_STATIC_CPU=0")
set_source_files_properties(${ARM7_BANK_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS "NDS_STATIC_CPU=1")

# Build one registry translation unit from the configured dispatch filenames.
# This keeps game names and the number of overlay/runtime generations out of
# shared runner source. Every generated table is registered only after the
# exact configured ROM hash matches.
set(TITLE_BANK_REGISTRY
    "${CMAKE_CURRENT_BINARY_DIR}/title_bank_registry.cpp")
file(WRITE "${TITLE_BANK_REGISTRY}"
    "#include \"title_banks.h\"\n"
    "#include \"state.h\"\n"
    "#include <cstring>\n\n")
foreach(src IN LISTS TITLE_BANK_DISPATCH)
    get_filename_component(dispatch_stem "${src}" NAME_WE)
    string(REGEX REPLACE "_dispatch$" "" bank "${dispatch_stem}")
    file(APPEND "${TITLE_BANK_REGISTRY}"
        "extern \"C\" const DispatchEntry g_dispatch_${bank}[];\n"
        "extern \"C\" const unsigned g_dispatch_${bank}_len;\n")
endforeach()
file(APPEND "${TITLE_BANK_REGISTRY}"
    "\nbool nds_register_configured_title_banks(const char* rom_sha1) {\n")
if(TITLE_BANK_DISPATCH)
    file(APPEND "${TITLE_BANK_REGISTRY}"
        "    if (!rom_sha1 || std::strcmp(rom_sha1, "
        "\"${NDS_TITLE_ROM_SHA1}\") != 0) return false;\n")
    foreach(src IN LISTS TITLE_BANK_DISPATCH)
        get_filename_component(dispatch_stem "${src}" NAME_WE)
        string(REGEX REPLACE "_dispatch$" "" bank "${dispatch_stem}")
        if(bank MATCHES "(^|_)arm9($|_)")
            set(bank_cpu "NDS_ARM9")
            set(bank_mask "0xFFFF0000u")
        elseif(bank MATCHES "(^|_)arm7($|_)")
            set(bank_cpu "NDS_ARM7")
            set(bank_mask "0x00000000u")
        else()
            message(FATAL_ERROR
                "Cannot infer title-bank CPU from dispatch name: ${bank}")
        endif()
        file(APPEND "${TITLE_BANK_REGISTRY}"
            "    nds_register_dispatch(${bank_cpu}, g_dispatch_${bank}, "
            "g_dispatch_${bank}_len, ${bank_mask});\n")
    endforeach()
    file(APPEND "${TITLE_BANK_REGISTRY}" "    return true;\n")
else()
    file(APPEND "${TITLE_BANK_REGISTRY}"
        "    (void)rom_sha1;\n"
        "    return false;\n")
endif()
file(APPEND "${TITLE_BANK_REGISTRY}" "}\n")

# SHA-1 helper (reused from the recompiler support lib).
add_library(nds_support STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support/sha1.cpp)
target_include_directories(nds_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support)

# Vendored libslirp 4.8.0 (BSD-3-Clause), built via melonDS's own hand-written
# CMakeLists.txt (not upstream's meson.build, which wants real GLib2 -- see
# docs/adr-melonds-wifi-vendoring.md §9). That CMakeLists ships its own glib
# shim (glib/glib.c, not real GLib) and already wires ws2_32/iphlpapi on
# WIN32 internally, so no extra link setup is needed beyond depending on the
# `slirp` target it defines. cmake_minimum_required(VERSION 3.16) there is
# satisfied by this project's own C:\msys64\mingw64\bin\cmake.exe (4.2.2,
# verified by direct invocation, not assumed); __WIN32__ (which
# Net_Slirp.h/.cpp guard socket includes on) is defined by this mingw-w64
# installation's own g++ (verified via `g++ -dM -E -`, not assumed).
add_subdirectory(vendor/melonds/net/libslirp EXCLUDE_FROM_ALL)
if(NOT MSVC)
    # libslirp's own CMakeLists.txt does not set its own warning posture, so
    # it inherits this project's top-level -Wall -Wextra (add_compile_options
    # is directory-scoped and propagates into add_subdirectory'd targets).
    # Same "don't edit vendored code to satisfy our own warning flags"
    # rationale as the GPU3D/Wifi/net -w suppressions below, applied to a
    # separate CMake target instead of a source-file property since libslirp
    # builds as its own target (`slirp`), not as sources listed directly in
    # nds_runner.
    target_compile_options(slirp PRIVATE -w)
endif()

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
    src/battery_save.cpp
    src/generated_firmware.cpp
    src/firmware_user_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/freebios/freebios_images.cpp
    src/bus.cpp
    src/cart_backup.cpp
    src/cp15.cpp
    src/direct_boot.cpp
    src/io.cpp
    src/wifi_net.cpp
    src/net/net_ring.cpp
    src/net/net_classify.cpp
    src/net/net_capture.cpp
    src/net/net_pcap.cpp
    src/net/net_sanitize.cpp
    src/net/net_replay.cpp
    src/net/wfc_provider.cpp
    src/spu.cpp
    src/vram.cpp
    src/gpu2d.cpp
    src/gpu3d.cpp
    src/title_patches.cpp
    src/tier3.cpp
    src/profile_report.cpp
    src/scheduler.cpp
    src/debug_server.cpp
    src/frontend_config.cpp
    src/relative_mouse_touch.cpp
    src/frontend.cpp
    src/main.cpp
    "${TITLE_BANK_REGISTRY}"
    vendor/melonds/GPU3D.cpp
    vendor/melonds/GPU3D_Soft.cpp
    vendor/melonds/Savestate.cpp
    # Vendored melonDS Wifi device model + net glue (GPL-3.0-or-later),
    # tag 1.0rc commit e3fa6f4224e0d706df3ee262ae41cfb0deadc593, patched per
    # vendor/melonds/patches/ (WifiAP.cpp AP identity + net-ring hooks;
    # Wifi.cpp net-ring hooks; Net_Slirp.cpp non-blocking poll). Wired to
    # bus.cpp/scheduler.cpp via src/wifi_net.cpp, which is now the sole
    # Wi-Fi device model -- the hand-written src/wifi.cpp was retired.
    # Net_PCap.cpp is added below, gated behind NDS_ENABLE_PCAP_BACKEND.
    vendor/melonds/Wifi.cpp
    vendor/melonds/WifiAP.cpp
    vendor/melonds/net/Net.cpp
    vendor/melonds/net/PacketDispatcher.cpp
    vendor/melonds/net/Net_Slirp.cpp)
ndsrecomp_limit_link(nds_runner)
if(NOT NDS_PGO_MODE STREQUAL "OFF")
    if(MSVC)
        message(FATAL_ERROR "NDS_PGO_MODE currently supports GCC/MinGW only")
    elseif(NDS_PGO_MODE STREQUAL "GENERATE")
        # Instrument only runner-owned host code. Generated ARM banks and
        # portable support libraries remain byte-for-byte baseline objects.
        target_compile_options(nds_runner PRIVATE
            -fprofile-generate -fprofile-update=atomic)
        target_link_options(nds_runner PRIVATE -fprofile-generate)
    elseif(NDS_PGO_MODE STREQUAL "USE")
        target_compile_options(nds_runner PRIVATE
            -fprofile-use -fprofile-correction)
        target_link_options(nds_runner PRIVATE -fprofile-use)
    else()
        message(FATAL_ERROR
            "NDS_PGO_MODE must be OFF, GENERATE, or USE")
    endif()
endif()
if(NDS_PROFILE_MEM_TIMING)
    # Keep the production memory-timing helper object unchanged. Only the
    # helper implementation and passive query command see the census define.
    set_property(SOURCE src/bus.cpp src/debug_server.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS NDS_PROFILE_MEM_TIMING=1)
endif()
if(NDS_PROFILE_HLE_HEAT)
    # Only title ARM9 sources can contain the selected wrappers/descriptors.
    # Do not force the hundreds of unrelated firmware-bank TUs to rebuild for
    # a default-off profiling experiment.
    set_property(SOURCE ${SM64DS_HLE_PROFILE_SOURCES} APPEND PROPERTY
        COMPILE_DEFINITIONS NDS_PROFILE_HLE_HEAT=1)
    set_property(SOURCE src/main.cpp src/runtime_arm.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS NDS_PROFILE_HLE_HEAT=1)
endif()
if(NDS_ENABLE_COMPUTE_RENDERER)
    target_sources(nds_runner PRIVATE
        vendor/melonds/GPU3D_Compute.cpp
        vendor/melonds/GPU3D_Texcache.cpp
        vendor/melonds/GPU3D_TexcacheOpenGL.cpp
        src/melonds_compute/GPU_OpenGL.cpp
        src/melonds_compute/PlatformCompute.cpp
        src/melonds_compute/ComputeHost.cpp
        vendor/melonds/OpenGLSupport.cpp
        vendor/melonds/glad/glad.c
        vendor/melonds/xxhash/xxhash.c)
    target_compile_definitions(nds_runner PRIVATE
        NDS_HAVE_COMPUTE_RENDERER=1
        MELONDS_GL_HEADER="glad/glad.h")
endif()
if(NDS_ENABLE_PCAP_BACKEND)
    # Net_PCap.cpp only needs <pcap/pcap.h> (the freely-distributable libpcap
    # public headers, BSD-licensed, vendored wholesale at vendor/melonds/net/
    # pcap/ -- same tree melonDS itself vendors) at compile time; the actual
    # wpcap.dll/Packet.dll is loaded at runtime via Platform::DynamicLibrary_*
    # (src/wifi_net.cpp), so there is no link-time Npcap/WinPcap SDK
    # dependency. Off by default: docs/adr-melonds-wifi-vendoring.md §2.
    target_sources(nds_runner PRIVATE vendor/melonds/net/Net_PCap.cpp)
    target_compile_definitions(nds_runner PRIVATE NDS_ENABLE_PCAP_BACKEND=1)
endif()
target_include_directories(nds_runner PRIVATE
    $<$<BOOL:${NDS_ENABLE_COMPUTE_RENDERER}>:${CMAKE_CURRENT_SOURCE_DIR}/src/melonds_compute>
    $<$<BOOL:${NDS_ENABLE_PCAP_BACKEND}>:${CMAKE_CURRENT_SOURCE_DIR}/vendor/melonds/net>
    src
    vendor/melonds                                     # vendored melonDS GPU3D + Wifi + shims (GPL-3.0-or-later)
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t   # runtime_arm.h, interpreter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support   # sha1.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/freebios)     # embedded FreeBIOS (BSD-2-Clause)
ndsrecomp_link_tomlpp(nds_runner)
if(NOT MSVC)
    # Unmodified upstream melonDS sources carry their own warning posture;
    # keep the runner's -Wall -Wextra signal clean without editing vendored
    # code.
    set_source_files_properties(
        vendor/melonds/GPU3D.cpp
        vendor/melonds/GPU3D_Soft.cpp
        vendor/melonds/Savestate.cpp
        vendor/melonds/Wifi.cpp
        vendor/melonds/WifiAP.cpp
        vendor/melonds/net/Net.cpp
        vendor/melonds/net/PacketDispatcher.cpp
        vendor/melonds/net/Net_Slirp.cpp
        PROPERTIES COMPILE_OPTIONS "-w")
    if(NDS_ENABLE_PCAP_BACKEND)
        set_source_files_properties(vendor/melonds/net/Net_PCap.cpp
            PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    if(NDS_ENABLE_COMPUTE_RENDERER)
        set_source_files_properties(
            vendor/melonds/GPU3D_Compute.cpp
            vendor/melonds/GPU3D_Texcache.cpp
            vendor/melonds/GPU3D_TexcacheOpenGL.cpp
            vendor/melonds/OpenGLSupport.cpp
            vendor/melonds/glad/glad.c
            vendor/melonds/xxhash/xxhash.c
            PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endif()
if(NDS_ENABLE_COMPUTE_RENDERER)
    if(MSVC)
        set_source_files_properties(vendor/melonds/OpenGLSupport.cpp
            PROPERTIES COMPILE_OPTIONS
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/melonds_compute/PlatformCompute.h")
    else()
        set_source_files_properties(vendor/melonds/OpenGLSupport.cpp
            PROPERTIES COMPILE_OPTIONS
            "-w;-include;${CMAKE_CURRENT_SOURCE_DIR}/src/melonds_compute/PlatformCompute.h")
    endif()
endif()
target_link_libraries(nds_runner PRIVATE nds_banks nds_support nds_armv4t slirp)
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
    if(EXISTS "${NDS_SM64DS_BANK_DIR}/sm64ds_arm9_ram_dispatch.c")
        target_compile_definitions(nds_runner PRIVATE
            NDS_HAVE_SM64DS_ARM9_RAM_BANKS=1)
    endif()
    if(EXISTS "${NDS_SM64DS_BANK_DIR}/sm64ds_arm9_ram_gameplay_dispatch.c")
        target_compile_definitions(nds_runner PRIVATE
            NDS_HAVE_SM64DS_ARM9_GAMEPLAY_RAM_BANKS=1)
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
if(NDS_ENABLE_COMPUTE_RENDERER AND NOT TARGET SDL2::SDL2)
    message(FATAL_ERROR "NDS_ENABLE_COMPUTE_RENDERER requires SDL2")
endif()
if(NDS_ENABLE_COMPUTE_RENDERER AND CMAKE_DL_LIBS)
    target_link_libraries(nds_runner PRIVATE ${CMAKE_DL_LIBS})
endif()
if(NDS_ENABLE_PCAP_BACKEND AND CMAKE_DL_LIBS)
    target_link_libraries(nds_runner PRIVATE ${CMAKE_DL_LIBS})
endif()
if(WIN32)
    target_link_libraries(nds_runner PRIVATE ws2_32)
    if(NDS_ENABLE_PCAP_BACKEND)
        target_link_libraries(nds_runner PRIVATE iphlpapi)
    endif()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

  add_executable(frontend_config_test
      tests/frontend_config_test.cpp
      src/frontend_config.cpp)
target_include_directories(frontend_config_test PRIVATE src)
ndsrecomp_link_tomlpp(frontend_config_test)
  add_test(NAME frontend_config_test COMMAND frontend_config_test)

  add_executable(wfc_provider_test
      tests/wfc_provider_test.cpp
      src/net/wfc_provider.cpp)
  target_include_directories(wfc_provider_test PRIVATE src)
  add_test(NAME wfc_provider_test COMMAND wfc_provider_test)

  add_executable(net_ring_test
      tests/net_ring_test.cpp
      src/net/net_ring.cpp)
  target_include_directories(net_ring_test PRIVATE
      src
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t)
  add_test(NAME net_ring_test COMMAND net_ring_test)

  add_executable(relative_mouse_touch_test
      tests/relative_mouse_touch_test.cpp
      src/relative_mouse_touch.cpp)
  target_include_directories(relative_mouse_touch_test PRIVATE src)
  add_test(NAME relative_mouse_touch_test COMMAND relative_mouse_touch_test)

  add_executable(battery_save_test
      tests/battery_save_test.cpp
      src/battery_save.cpp)
  target_include_directories(battery_save_test PRIVATE src)
  add_test(NAME battery_save_test COMMAND battery_save_test)

  # AUXSPI backup-chip protocol: pins the write-enable latch's lifetime across
  # chip-select transfers, which is what silently dropped every cartridge save.
  add_executable(cart_backup_test
      tests/cart_backup_test.cpp
      src/cart_backup.cpp)
  target_include_directories(cart_backup_test PRIVATE src)
  add_test(NAME cart_backup_test COMMAND cart_backup_test)

  # Direct boot has no LLE run to be checked against from the inside, so the
  # sequence is pinned here against melonDS's SetupDirectBoot instead.
  add_executable(direct_boot_test
      tests/direct_boot_test.cpp
      src/direct_boot.cpp)
  target_include_directories(direct_boot_test PRIVATE src)
  add_test(NAME direct_boot_test COMMAND direct_boot_test)

  # Generated firmware is pinned against melonDS Firmware(0); with an output
  # path argument it writes the image so the oracle byte-diff can be re-run
  # (ndsref firmware_dump produces the golden bytes).
  add_executable(generated_firmware_test
      tests/generated_firmware_test.cpp
      src/generated_firmware.cpp)
  target_include_directories(generated_firmware_test PRIVATE src)
  add_test(NAME generated_firmware_test COMMAND generated_firmware_test)

  # Wiimmfi M8: format round-trip, corruption/truncation detection, and
  # sanitizer determinism -- pure host-side logic with zero melonDS/
  # emulator dependency, so this links only the three net/* sources it
  # actually needs (matching frontend_config_test's minimal-link style).
  # In-memory firmware user-settings patches (touch calibration, startup
  # mode, per-instance MAC, player name). Pure byte/CRC logic with no
  # emulator dependency, so it links one source, matching
  # frontend_config_test's minimal-link style.
  add_executable(firmware_user_settings_test
      tests/firmware_user_settings_test.cpp
      src/firmware_user_settings.cpp
      src/generated_firmware.cpp)
  target_include_directories(firmware_user_settings_test PRIVATE src)
  add_test(NAME firmware_user_settings_test
           COMMAND firmware_user_settings_test)

  add_executable(net_capture_test
      tests/net_capture_test.cpp
      src/net/net_capture.cpp
      src/net/net_pcap.cpp
      src/net/net_sanitize.cpp)
  target_include_directories(net_capture_test PRIVATE src)
  add_test(NAME net_capture_test COMMAND net_capture_test)

  if(Python3_Interpreter_FOUND)
    add_test(
        NAME m7_collect_two_instance_evidence_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_collect_two_instance_evidence.py")
  endif()
