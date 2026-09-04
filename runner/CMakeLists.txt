cmake_minimum_required(VERSION 3.20)
project(nds_runner VERSION 0.6.10 LANGUAGES C CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsCompilerCache.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsJobPools.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NdsTomlPlusPlus.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../external/arm-recomp-core/cmake/ArmRecompCore.cmake")
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

arm_recomp_core_sources(NDS_ARM_RECOMP_SOURCES armv4t)
arm_recomp_core_include_dirs(NDS_ARM_RECOMP_INCLUDE_DIRS armv4t)

option(NDS_BOOTSTRAP_FIRMWARE
    "Build with BIOS banks only so guest-produced firmware RAM can be captured"
    OFF)
option(NDS_ENABLE_COMPUTE_RENDERER
    "Build the preferred melonDS OpenGL 4.3 compute renderer"
    ON)
set(NDS_SDL_BACKEND "SDL3" CACHE STRING
    "Host presentation backend: SDL3, SDL2, or NONE")
set_property(CACHE NDS_SDL_BACKEND PROPERTY STRINGS SDL3 SDL2 NONE)
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
option(NDS_PGO_PARTIAL_TRAINING
    "Keep functions the training route never executed on normal optimization \
instead of letting -fprofile-use size-optimize them as cold"
    ON)
option(NDS_PGO_REQUIRE_PROFILE
    "Fail configuration when NDS_PGO_MODE=USE and no .gcda profile data is \
present, instead of silently producing an unoptimized binary"
    ON)
set(NDS_GENERATED_DIR "" CACHE PATH
    "Optional ignored generated directory override for worktree builds")
set(NDS_SM64DS_BANK_DIR "" CACHE PATH
    "Optional ignored directory containing generated sm64ds_arm7/arm9 banks")
set(NDS_TITLE_BANK_DIR "" CACHE PATH
    "Optional ignored directory containing generated title banks")
set(NDS_TITLE_ROM_SHA1 "" CACHE STRING
    "Exact lowercase ROM SHA-1 allowed to register NDS_TITLE_BANK_DIR")
# Multi-title form: parallel lists, one bank directory per exact ROM SHA-1.
# One runner binary can then serve several cartridges (e.g. a two-in-one
# repository like Pokemon Black/White), registering only the bank group whose
# SHA-1 matches the inserted ROM. The singular variables above remain fully
# supported and are folded in as one extra group.
set(NDS_TITLE_BANK_DIRS "" CACHE STRING
    "Semicolon list of generated title bank directories (parallel to NDS_TITLE_ROM_SHA1S)")
set(NDS_TITLE_ROM_SHA1S "" CACHE STRING
    "Semicolon list of exact lowercase ROM SHA-1s (parallel to NDS_TITLE_BANK_DIRS)")

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
# Fold the singular variables and the list variables into one set of
# (bank dir, ROM SHA-1) groups. Every group is validated identically; the
# generated registry gates each group's banks behind its own SHA-1, so one
# runner binary serves exactly the cartridges it was configured for.
set(TITLE_GROUP_DIRS)
set(TITLE_GROUP_SHA1S)
if(NDS_TITLE_BANK_DIR)
    list(APPEND TITLE_GROUP_DIRS "${NDS_TITLE_BANK_DIR}")
    list(APPEND TITLE_GROUP_SHA1S "${NDS_TITLE_ROM_SHA1}")
endif()
if(NDS_TITLE_BANK_DIRS)
    list(LENGTH NDS_TITLE_BANK_DIRS _title_dirs_len)
    list(LENGTH NDS_TITLE_ROM_SHA1S _title_sha1s_len)
    if(NOT _title_dirs_len EQUAL _title_sha1s_len)
        message(FATAL_ERROR
            "NDS_TITLE_BANK_DIRS (${_title_dirs_len}) and "
            "NDS_TITLE_ROM_SHA1S (${_title_sha1s_len}) must be parallel "
            "lists of equal length")
    endif()
    list(APPEND TITLE_GROUP_DIRS ${NDS_TITLE_BANK_DIRS})
    list(APPEND TITLE_GROUP_SHA1S ${NDS_TITLE_ROM_SHA1S})
endif()

set(TITLE_BANK_BODIES)
set(TITLE_BANK_DISPATCH)
set(TITLE_BANK_SOURCES)
set(TITLE_GROUP_COUNT 0)
list(LENGTH TITLE_GROUP_DIRS TITLE_GROUP_COUNT)
if(TITLE_GROUP_COUNT GREATER 0)
    math(EXPR _title_last "${TITLE_GROUP_COUNT} - 1")
    foreach(_gi RANGE 0 ${_title_last})
        list(GET TITLE_GROUP_DIRS ${_gi} _group_dir)
        list(GET TITLE_GROUP_SHA1S ${_gi} _group_sha1)
        string(LENGTH "${_group_sha1}" _group_sha1_len)
        if(NOT _group_sha1_len EQUAL 40 OR _group_sha1 MATCHES "[^0-9a-f]")
            message(FATAL_ERROR
                "title bank group ${_gi}: ROM SHA-1 must be exactly 40 "
                "lowercase hex digits (got '${_group_sha1}')")
        endif()
        file(GLOB _group_bodies CONFIGURE_DEPENDS
            "${_group_dir}/*_[0-9][0-9].c")
        file(GLOB _group_dispatch CONFIGURE_DEPENDS
            "${_group_dir}/*_dispatch.c")
        list(SORT _group_bodies)
        list(SORT _group_dispatch)
        # Optional explicit registration priority. Runtime dispatch is
        # first-match-wins in registration order; the default order is this
        # alphabetical sort. A title may ship registration-order.txt in its
        # bank dir listing bank names (one per line, comments with #) that
        # must register FIRST, in the listed order; unlisted banks follow in
        # sorted order. Content validation still gates every row, so for
        # banks whose byte content differs this changes only which candidate
        # is CONSULTED first, never which code runs. It exists so a
        # whole-module bank (e.g. an ARM7 alias) can precede single-page
        # generation banks that would otherwise suppress its superblock
        # coalescing (see MPH, beads-0vo).
        set(_order_file "${_group_dir}/registration-order.txt")
        if(EXISTS "${_order_file}")
            file(STRINGS "${_order_file}" _order_names)
            set(_ordered)
            foreach(_name IN LISTS _order_names)
                string(STRIP "${_name}" _name)
                if(_name STREQUAL "" OR _name MATCHES "^#")
                    continue()
                endif()
                set(_want "${_group_dir}/${_name}_dispatch.c")
                list(FIND _group_dispatch "${_want}" _idx)
                if(_idx EQUAL -1)
                    message(FATAL_ERROR
                        "registration-order.txt names bank '${_name}' but "
                        "${_want} does not exist in the group")
                endif()
                list(APPEND _ordered "${_want}")
                list(REMOVE_ITEM _group_dispatch "${_want}")
            endforeach()
            list(INSERT _group_dispatch 0 ${_ordered})
        endif()
        if(NOT _group_bodies OR NOT _group_dispatch)
            message(FATAL_ERROR
                "title bank group ${_gi} (${_group_dir}) must contain "
                "generated bank body and dispatch sources")
        endif()
        set(TITLE_GROUP_${_gi}_SHA1 "${_group_sha1}")
        set(TITLE_GROUP_${_gi}_DISPATCH ${_group_dispatch})
        list(APPEND TITLE_BANK_BODIES ${_group_bodies})
        list(APPEND TITLE_BANK_DISPATCH ${_group_dispatch})
    endforeach()
    set(TITLE_BANK_SOURCES ${TITLE_BANK_BODIES} ${TITLE_BANK_DISPATCH})
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
    ${NDS_ARM_RECOMP_INCLUDE_DIRS}
    ${NDS_SM64DS_BANK_DIR}
    ${TITLE_GROUP_DIRS})
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
    # One SHA-1 gate per configured bank group: only the group generated from
    # the exact inserted cartridge registers, so a multi-title runner never
    # selects another game's banks by address coincidence.
    # Groups accumulate rather than return on the first match. Several groups
    # may legitimately carry the SAME ROM SHA-1 -- that is how one cartridge
    # gets banks from more than one generated directory (e.g. the main image in
    # one, an ahead-of-time overlay bank in another, without regenerating or
    # polluting either tree). Returning on the first match silently dropped
    # every later group: its nds_register_dispatch call was emitted but sat
    # after an unconditional return, so the banks linked, the registry reported
    # success, and the rows were simply never in the dispatch list.
    file(APPEND "${TITLE_BANK_REGISTRY}"
        "    if (!rom_sha1) return false;\n"
        "    bool registered = false;\n")
    math(EXPR _title_last "${TITLE_GROUP_COUNT} - 1")
    foreach(_gi RANGE 0 ${_title_last})
        file(APPEND "${TITLE_BANK_REGISTRY}"
            "    if (std::strcmp(rom_sha1, "
            "\"${TITLE_GROUP_${_gi}_SHA1}\") == 0) {\n")
        foreach(src IN LISTS TITLE_GROUP_${_gi}_DISPATCH)
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
                "        nds_register_dispatch(${bank_cpu}, "
                "g_dispatch_${bank}, g_dispatch_${bank}_len, ${bank_mask});\n")
        endforeach()
        file(APPEND "${TITLE_BANK_REGISTRY}"
            "        registered = true;\n"
            "    }\n")
    endforeach()
    file(APPEND "${TITLE_BANK_REGISTRY}" "    return registered;\n")
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
add_library(nds_armv4t STATIC ${NDS_ARM_RECOMP_SOURCES})
target_include_directories(nds_armv4t PUBLIC
    ${NDS_ARM_RECOMP_INCLUDE_DIRS})

add_executable(nds_runner
    src/runtime_arm.cpp
    # Out-of-line runtime_tick / runtime_should_yield / runtime_unwinding for
    # live shards published before those became header inlines. Must stay a
    # separate TU: it defines NDS_RUNTIME_ABI_SHIMS to suppress the inlines.
    src/runtime_abi_shims.cpp
    src/savestate.cpp
    src/savestate_slots.cpp
    src/live_overlay.cpp
    src/live_overlay_platform.cpp
    src/battery_save.cpp
    src/firmware_state.cpp
    src/generated_firmware.cpp
    src/firmware_user_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/freebios/freebios_images.cpp
    src/bus.cpp
    src/cart_backup.cpp
    src/cp15.cpp
    src/direct_boot.cpp
    src/io.cpp
    src/wifi_net.cpp
    src/network_savestate_guard.cpp
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
    src/coverage_manifest.cpp
    src/diagnostics.cpp
    src/dispatch_timing.cpp
    src/emu_profile.cpp
    src/pc_profile.cpp
    src/host_prof_ring.cpp
    src/host_unwind_x64.cpp
    src/host_profile.cpp
    src/host_info.cpp
    src/perf_governor.cpp
    src/profile_report.cpp
    src/scheduler.cpp
    src/debug_server.cpp
    src/frontend_config.cpp
    src/relative_mouse_touch.cpp
    src/frontend.cpp
    src/main.cpp
    src/runtime_exports.def
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
set_target_properties(nds_runner PROPERTIES ENABLE_EXPORTS ON)
ndsrecomp_limit_link(nds_runner)
if(NOT NDS_PGO_MODE STREQUAL "OFF")
    if(MSVC)
        message(FATAL_ERROR "NDS_PGO_MODE currently supports GCC/MinGW only")
    endif()
    if(NOT NDS_PGO_MODE STREQUAL "GENERATE" AND
       NOT NDS_PGO_MODE STREQUAL "USE")
        message(FATAL_ERROR "NDS_PGO_MODE must be OFF, GENERATE, or USE")
    endif()

    # A compiler cache must not stand between the profile data and the
    # optimized objects. ccache's handling of -fprofile-use inputs is not
    # something a release artifact should depend on, so PGO builds always
    # compile directly.
    if(NOT NDSRECOMP_COMPILER_CACHE STREQUAL "OFF")
        message(FATAL_ERROR
            "NDS_PGO_MODE=${NDS_PGO_MODE} requires "
            "-DNDSRECOMP_COMPILER_CACHE=OFF so profile data, not a cache "
            "hit, decides each object")
    endif()

    # Deliberately no -fprofile-dir. On mingw-gcc 15.2 it mangles the object
    # directory into a nested path under the profile directory (prof/C~/Users/
    # .../objdir/x.gcda); for this tree's path depth gcc cannot create it and
    # reports "Cannot create directory ... Skip", writing no profile at all.
    # Verified empirically 2026-08-27. So profiles stay beside the object
    # files, which makes the build directory the profile store: the GENERATE
    # and USE phases MUST run in the same build directory. That is also the
    # cheap path, because PGO flags are private to nds_runner and switching
    # modes in place never recompiles a generated bank TU.
    #
    # gcc derives the profile name from the object file name, so the two
    # phases must also produce identically named objects. Reconfiguring one
    # build directory satisfies that automatically.

    if(NDS_PGO_MODE STREQUAL "GENERATE")
        # Instrument only runner-owned host code. Generated ARM banks and
        # portable support libraries remain byte-for-byte baseline objects,
        # so switching modes in place never recompiles a bank TU.
        # -fprofile-update=atomic is required: the runner counts from the
        # emulation, audio and presentation threads at once, and mingw-gcc's
        # non-atomic counters lose and corrupt updates under that.
        target_compile_options(nds_runner PRIVATE
            -fprofile-generate -fprofile-update=atomic)
        target_link_options(nds_runner PRIVATE -fprofile-generate)
    else()
        # -fprofile-use with no profile data present is not an error: gcc
        # warns under -Wmissing-profile and still exits 0, so an untrained
        # tree would quietly produce a binary with none of the optimization
        # the release is being labelled with. Verified empirically on
        # mingw-gcc 15.2. Refuse to configure instead.
        file(GLOB_RECURSE _nds_pgo_probe
            "${CMAKE_CURRENT_BINARY_DIR}/*.gcda")
        list(LENGTH _nds_pgo_probe _nds_pgo_count)
        if(_nds_pgo_count EQUAL 0)
            if(NDS_PGO_REQUIRE_PROFILE)
                message(FATAL_ERROR
                    "NDS_PGO_MODE=USE found no .gcda profile data under "
                    "${CMAKE_CURRENT_BINARY_DIR}. Configure this same build "
                    "directory with NDS_PGO_MODE=GENERATE, build it, run the "
                    "training route, then reconfigure with USE. Set "
                    "-DNDS_PGO_REQUIRE_PROFILE=OFF only to deliberately "
                    "accept an untrained binary.")
            endif()
            message(WARNING
                "NDS_PGO_MODE=USE with no profile data: this binary will not "
                "actually be profile-optimized")
        else()
            message(STATUS
                "ndsrecomp: PGO USE with ${_nds_pgo_count} profile file(s)")
        endif()
        # -fprofile-correction is required, not defensive: the runner updates
        # counters from the emulation, audio and presentation threads, and
        # mingw-gcc profile output under threads is inconsistent often enough
        # that gcc would otherwise reject the profile outright.
        # -Wmissing-profile keeps per-TU training gaps visible in the log
        # rather than silent.
        target_compile_options(nds_runner PRIVATE
            -fprofile-use -fprofile-correction -Wmissing-profile)
        if(NDS_PGO_PARTIAL_TRAINING)
            # The training routes cover attract, FMV and one adventure route.
            # Without this, every function they never reach - multiplayer,
            # wifi, the settings menus - is treated as cold and size-
            # optimized, so a narrow training set would pessimize everything
            # it did not visit.
            target_compile_options(nds_runner PRIVATE
                -fprofile-partial-training)
        endif()
        target_link_options(nds_runner PRIVATE -fprofile-use)
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
        src/melonds_compute/TextureUpscale.cpp
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
else()
    target_sources(nds_runner PRIVATE
        src/melonds_compute/TextureUpscaleStub.cpp)
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
# beads-yjp.28: stamped into the coverage manifest so an ingested playthrough
# can be tied to the build whose static coverage it was measured against.
# beads-yjp.44: re-read at BUILD time, not configure time. The old
# execute_process froze the id at `cmake -B`, so every incremental build after
# that reported a stale commit -- a shipped v0.6.3 runner reported 12ee1b9
# while containing 8de5d86-only behaviour, which made its field diagnostics
# bundle lie about what code the player ran. The script writes the header only
# when the id actually changes, so an unchanged HEAD costs no recompile, and it
# still degrades to "unknown" with no git or outside a work tree.
set(NDS_BUILD_ID_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/nds_build_id.h")
set(NDS_BUILD_ID_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/NdsBuildIdScript.cmake")
# Seed at configure time so the header exists before the first compile even if
# the custom target has not run yet.
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DNDS_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DNDS_OUT_FILE=${NDS_BUILD_ID_HEADER}"
    "-DNDS_VERSION_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../VERSION"
    "-DNDS_TITLE_DIR=${NDS_TITLE_BANK_DIR}"
    "-DNDS_TITLE_VERSION=${NDS_TITLE_VERSION}"
    -P "${NDS_BUILD_ID_SCRIPT}")
add_custom_target(nds_build_id ALL
    BYPRODUCTS "${NDS_BUILD_ID_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DNDS_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DNDS_OUT_FILE=${NDS_BUILD_ID_HEADER}"
        "-DNDS_VERSION_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../VERSION"
        "-DNDS_TITLE_DIR=${NDS_TITLE_BANK_DIR}"
        "-DNDS_TITLE_VERSION=${NDS_TITLE_VERSION}"
        -P "${NDS_BUILD_ID_SCRIPT}"
    COMMENT "Stamping git HEAD into nds_build_id.h"
    VERBATIM)
add_dependencies(nds_runner nds_build_id)

target_include_directories(nds_runner PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"   # nds_build_id.h (build-time)
    $<$<BOOL:${NDS_ENABLE_COMPUTE_RENDERER}>:${CMAKE_CURRENT_SOURCE_DIR}/src/melonds_compute>
    $<$<BOOL:${NDS_ENABLE_PCAP_BACKEND}>:${CMAKE_CURRENT_SOURCE_DIR}/vendor/melonds/net>
    src
    vendor/melonds                                     # vendored melonDS GPU3D + Wifi + shims (GPL-3.0-or-later)
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t   # runtime_arm.h
    ${NDS_ARM_RECOMP_INCLUDE_DIRS}                     # shared ARM core headers
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

# SDL is a host-only presentation layer. SDL3 is the default runtime frontend;
# SDL2 and a no-frontend headless build are explicit compatibility choices.
if(NDS_SDL_BACKEND STREQUAL "SDL3")
    find_package(SDL3 CONFIG REQUIRED)
    if(NOT TARGET SDL3::SDL3)
        message(FATAL_ERROR "NDS_SDL_BACKEND=SDL3 requires target SDL3::SDL3")
    endif()
    target_compile_definitions(nds_runner PRIVATE
        NDS_HAVE_SDL3=1
        NDS_SDL_BACKEND_NAME="SDL3")
    target_link_libraries(nds_runner PRIVATE SDL3::SDL3)
elseif(NDS_SDL_BACKEND STREQUAL "SDL2")
    find_package(SDL2 CONFIG REQUIRED)
    if(NOT TARGET SDL2::SDL2)
        message(FATAL_ERROR "NDS_SDL_BACKEND=SDL2 requires target SDL2::SDL2")
    endif()
    target_compile_definitions(nds_runner PRIVATE
        NDS_HAVE_SDL2=1
        NDS_SDL_BACKEND_NAME="SDL2")
    target_link_libraries(nds_runner PRIVATE SDL2::SDL2)
elseif(NDS_SDL_BACKEND STREQUAL "NONE")
    target_compile_definitions(nds_runner PRIVATE NDS_SDL_BACKEND_NAME="NONE")
else()
    message(FATAL_ERROR "NDS_SDL_BACKEND must be SDL3, SDL2, or NONE")
endif()
if(NDS_ENABLE_COMPUTE_RENDERER AND NDS_SDL_BACKEND STREQUAL "NONE")
    message(FATAL_ERROR "NDS_ENABLE_COMPUTE_RENDERER requires NDS_SDL_BACKEND=SDL3 or SDL2")
endif()
if(CMAKE_DL_LIBS)
    target_link_libraries(nds_runner PRIVATE ${CMAKE_DL_LIBS})
endif()
if(WIN32)
    target_link_libraries(nds_runner PRIVATE ws2_32)
    if(NDS_ENABLE_PCAP_BACKEND)
        target_link_libraries(nds_runner PRIVATE iphlpapi)
    endif()
endif()

# The dual-CPU boot nests deeply enough to exhaust the 8 MB main-thread stack
# macOS gives a process by default, faulting in ___chkstk_darwin before the
# firmware hands off. Linux's 8 MB soft limit is raisable per-shell; the macOS
# main thread size is fixed at link time, so ask for it here.
if(APPLE)
    target_link_options(nds_runner PRIVATE -Wl,-stack_size,0x4000000)
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
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
      ${NDS_ARM_RECOMP_INCLUDE_DIRS})
  add_test(NAME net_ring_test COMMAND net_ring_test)

  add_executable(relative_mouse_touch_test
      tests/relative_mouse_touch_test.cpp
      src/relative_mouse_touch.cpp)
  target_include_directories(relative_mouse_touch_test PRIVATE src)
  add_test(NAME relative_mouse_touch_test COMMAND relative_mouse_touch_test)

  add_executable(perf_governor_test
      tests/perf_governor_test.cpp
      src/perf_governor.cpp)
  target_include_directories(perf_governor_test PRIVATE src)
  add_test(NAME perf_governor_test COMMAND perf_governor_test)

  # Always-on host CPU sampler (host_profile.h). Three things are gated here:
  # the sample ring's residency/eviction/tear rules, the leaf histogram, and --
  # the one that actually needs proving -- the x64 .pdata/.xdata unwinder, both
  # against a synthetic table the test builds itself AND against the OS's own
  # RtlCaptureStackBackTrace over real compiler-generated prologues.
  add_executable(host_prof_test
      tests/host_prof_test.cpp
      src/host_prof_ring.cpp
      src/host_unwind_x64.cpp
      src/host_profile.cpp)
  target_include_directories(host_prof_test PRIVATE src)
  if(WIN32)
    # beads-yjp.70: a scratch DLL with REAL toolchain-emitted unwind data, so
    # the test can load it, walk it, FreeLibrary it, and then drive both
    # sampler-side reads at its unmapped range. A module that vanishes out from
    # under the module map used to fault the sampler and kill the process
    # silently (no SEH in a MinGW build); this is the gate on that.
    add_library(host_prof_probe_dll MODULE tests/host_prof_probe_dll.c)
    set_target_properties(host_prof_probe_dll PROPERTIES
        PREFIX "" OUTPUT_NAME "host_prof_probe")
    add_dependencies(host_prof_test host_prof_probe_dll)
    target_compile_definitions(host_prof_test PRIVATE
        "NDS_HOST_PROF_PROBE_DLL=\"$<TARGET_FILE:host_prof_probe_dll>\"")
  endif()
  add_test(NAME host_prof_test COMMAND host_prof_test)

  add_executable(battery_save_test
      tests/battery_save_test.cpp
      src/battery_save.cpp)
  target_include_directories(battery_save_test PRIVATE src)
  add_test(NAME battery_save_test COMMAND battery_save_test)

  add_executable(firmware_state_test
      tests/firmware_state_test.cpp
      src/firmware_state.cpp
      src/battery_save.cpp)
  target_include_directories(firmware_state_test PRIVATE src)
  add_test(NAME firmware_state_test COMMAND firmware_state_test)

  add_executable(savestate_test
      tests/savestate_test.cpp
      tests/savestate_test_stubs.cpp
      src/savestate.cpp
      src/bus.cpp
      src/cp15.cpp
      src/scheduler.cpp
      src/spu.cpp
      src/runtime_arm.cpp
      src/dispatch_timing.cpp
      src/emu_profile.cpp
      # scheduler_run_round() samples both cores' parked PCs into the always-on
      # histogram, whose table lives in this translation unit.
      src/pc_profile.cpp)
  target_include_directories(savestate_test PRIVATE
      src
      vendor/melonds
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
      ${NDS_ARM_RECOMP_INCLUDE_DIRS}
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support)
  ndsrecomp_link_tomlpp(savestate_test)
  target_link_libraries(savestate_test PRIVATE nds_armv4t nds_support)
  add_test(NAME savestate_test COMMAND savestate_test)

  # Bit-exactness rail for the fused timed-access fast path (yjp.70 ph2 B):
  # sweeps every region boundary / width / seq / CPU / CP15 configuration
  # asserting the inline path == the retained reference implementations.
  # Same link set as savestate_test: the real bus + CP15 + runtime, device
  # calls stubbed.
  add_executable(mem_timing_test
      tests/mem_timing_test.cpp
      tests/savestate_test_stubs.cpp
      src/savestate.cpp
      src/bus.cpp
      src/cp15.cpp
      src/scheduler.cpp
      src/spu.cpp
      src/runtime_arm.cpp
      src/dispatch_timing.cpp
      src/emu_profile.cpp
      src/pc_profile.cpp)
  target_include_directories(mem_timing_test PRIVATE
      src
      vendor/melonds
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
      ${NDS_ARM_RECOMP_INCLUDE_DIRS}
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support)
  ndsrecomp_link_tomlpp(mem_timing_test)
  target_link_libraries(mem_timing_test PRIVATE nds_armv4t nds_support)
  add_test(NAME mem_timing_test COMMAND mem_timing_test)

  add_executable(savestate_slots_test
      tests/savestate_slots_test.cpp
      src/savestate_slots.cpp)
  target_include_directories(savestate_slots_test PRIVATE
      src
      vendor/melonds
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
      ${NDS_ARM_RECOMP_INCLUDE_DIRS})
  add_test(NAME savestate_slots_test COMMAND savestate_slots_test)

  add_executable(network_savestate_guard_test
      tests/network_savestate_guard_test.cpp
      src/network_savestate_guard.cpp)
  target_include_directories(network_savestate_guard_test PRIVATE src)
  add_test(NAME network_savestate_guard_test
           COMMAND network_savestate_guard_test)

  add_executable(video_savestate_test
      tests/video_savestate_test.cpp
      tests/video_savestate_test_stubs.cpp
      src/vram.cpp
      src/gpu2d.cpp
      src/gpu3d.cpp
      src/dispatch_timing.cpp
      src/emu_profile.cpp
      # The worker threads in these units register themselves with the
      # always-on host CPU sampler (host_profile.h), so the sampler's own
      # units come along.
      src/host_profile.cpp
      src/host_prof_ring.cpp
      src/host_unwind_x64.cpp
      vendor/melonds/GPU3D.cpp
      vendor/melonds/GPU3D_Soft.cpp
      vendor/melonds/Savestate.cpp)
  target_include_directories(video_savestate_test PRIVATE
      src
      vendor/melonds
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
      ${NDS_ARM_RECOMP_INCLUDE_DIRS})
  target_compile_definitions(video_savestate_test PRIVATE
      NDS_BUILD_VIDEO_SAVESTATE_TEST=1)
  add_test(NAME video_savestate_test COMMAND video_savestate_test)

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

  add_executable(live_overlay_preflight_test
      tests/live_overlay_preflight_test.cpp
      src/live_overlay.cpp
      src/live_overlay_platform.cpp
      # live_overlay_poll() opens an NDS_EMU_OVERLAY region, whose accumulators
      # and tick calibration live in these two translation units.
      src/emu_profile.cpp
      src/dispatch_timing.cpp
      # The worker threads in these units register themselves with the
      # always-on host CPU sampler (host_profile.h), so the sampler's own
      # units come along.
      src/host_profile.cpp
      src/host_prof_ring.cpp
      src/host_unwind_x64.cpp)
  target_include_directories(live_overlay_preflight_test PRIVATE
      src
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
      ${NDS_ARM_RECOMP_INCLUDE_DIRS})
  set_target_properties(live_overlay_preflight_test PROPERTIES ENABLE_EXPORTS ON)
  if(WIN32)
    # beads-yjp.68: the codegen-gate test builds a REAL shard DLL that binds to
    # this executable's runner data symbols, exactly as a field shard binds to
    # nds_runner.exe through src/runtime_exports.def. Same mechanism, smaller
    # surface: an explicit .def names the exports, --out-implib names the
    # import library the shard links against. ENABLE_EXPORTS alone produces
    # neither under MinGW.
    target_link_options(live_overlay_preflight_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/live_overlay_preflight_exports.def"
        "-Wl,--out-implib,${CMAKE_CURRENT_BINARY_DIR}/liblive_overlay_preflight_test.dll.a")
    set_property(TARGET live_overlay_preflight_test APPEND PROPERTY
        LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/live_overlay_preflight_exports.def")
  endif()
  if(CMAKE_DL_LIBS)
    target_link_libraries(live_overlay_preflight_test PRIVATE ${CMAKE_DL_LIBS})
  endif()
  add_test(NAME live_overlay_preflight_test
           COMMAND live_overlay_preflight_test)

  add_executable(live_overlay_platform_test
      tests/live_overlay_platform_test.cpp
      src/live_overlay_platform.cpp)
  target_include_directories(live_overlay_platform_test PRIVATE src)
  add_test(NAME live_overlay_platform_test COMMAND live_overlay_platform_test)

  add_executable(dispatch_lookup_test
      tests/dispatch_lookup_test.cpp)
  target_include_directories(dispatch_lookup_test PRIVATE
      src
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
      ${NDS_ARM_RECOMP_INCLUDE_DIRS})
  add_test(NAME dispatch_lookup_test COMMAND dispatch_lookup_test)

  # beads-yjp.70 phase 2A: the emitted inline code-fetch cost must equal
  # runtime_code_cycles() exactly. Links the real bus/cp15 units (with the
  # savestate test's device stubs) so it pins the runtime, not a model.
  add_executable(code_cycles_fold_test
      tests/code_cycles_fold_test.cpp
      tests/savestate_test_stubs.cpp
      src/savestate.cpp
      src/bus.cpp
      src/cp15.cpp
      src/scheduler.cpp
      src/spu.cpp
      src/runtime_arm.cpp
      src/dispatch_timing.cpp
      src/emu_profile.cpp
      src/pc_profile.cpp)
  target_include_directories(code_cycles_fold_test PRIVATE
      src
      vendor/melonds
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
      ${NDS_ARM_RECOMP_INCLUDE_DIRS}
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/support)
  ndsrecomp_link_tomlpp(code_cycles_fold_test)
  target_link_libraries(code_cycles_fold_test PRIVATE nds_armv4t nds_support)
  add_test(NAME code_cycles_fold_test COMMAND code_cycles_fold_test)

  add_executable(gpu2d_window_test
      tests/gpu2d_window_test.cpp
      # The test #includes src/gpu2d.cpp wholesale, so it inherits the
      # NDS_EMU_RASTER2D region in nds_gpu2d_render_scanline and needs the
      # accumulators and tick calibration those two units define.
      src/emu_profile.cpp
      src/dispatch_timing.cpp
      # The worker threads in these units register themselves with the
      # always-on host CPU sampler (host_profile.h), so the sampler's own
      # units come along.
      src/host_profile.cpp
      src/host_prof_ring.cpp
      src/host_unwind_x64.cpp)
  target_include_directories(gpu2d_window_test PRIVATE
      src
      ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/armv4t
      ${NDS_ARM_RECOMP_INCLUDE_DIRS})
  add_test(NAME gpu2d_window_test COMMAND gpu2d_window_test)

  if(Python3_Interpreter_FOUND)
    # The deadline-bounded per-instruction machinery has no value anyone can
    # assert; its correctness is a set of structural properties (fast path
    # present and slice-bounded, every re-arm site zeroing it, faithful path
    # still selectable, no divergent shard semantics). Each is one named
    # assertion in the test.
    add_test(
        NAME machinery_perf_guards_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_machinery_perf_guards.py")
    # beads-yjp.54: the emu-time partition's value is WHERE its regions sit,
    # not any value it computes. Deleting one region still builds, still runs,
    # and silently folds that cost back into an enclosing bucket.
    add_test(
        NAME emu_attrib_guards_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_emu_attrib_guards.py")
    # beads-yjp.48: WHEN a halted core's clock may stop is a shape property of
    # scheduler_run_round(), not a value. Shortening a halted ARM7's catch-up
    # target to its own next timer overflow still built, still booted, still
    # matched every frame hash -- and reddened all eight G1 firmware scenarios
    # with an SPU mismatch 0.15 s downstream of the real defect.
    add_test(
        NAME scheduler_halt_fidelity_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scheduler_halt_fidelity.py")
    add_test(
        NAME savestate_frontend_guards_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_savestate_frontend_guards.py")
    # Two runners may share one live-overlay cache directory, so the shard
    # index read-modify-write has to survive concurrent writers.
    add_test(
        NAME live_index_lock_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/live_index_lock_test.py"
                --work "${CMAKE_CURRENT_BINARY_DIR}/live-index-lock-work")
    # beads-yjp.51: the pending-work queue is what makes a player's backlog
    # converge across runs and across sessions. Its format is schema-versioned
    # and must tolerate every corruption, its order must be hottest-first by
    # execution weight, and it must survive a hard kill of the writer.
    add_test(
        NAME live_queue_persist_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/live_queue_persist_test.py")
    # The shard wrapper's static_cpu must follow -DNDS_STATIC_CPU, which is
    # what the runner cross-checks against the bank's metadata CPU.
    add_test(
        NAME live_shard_wrapper_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/live_shard_wrapper_test.py"
                --gcc "${CMAKE_C_COMPILER}"
                --work "${CMAKE_CURRENT_BINARY_DIR}/live-shard-wrapper-work")
    # beads-yjp.68: the producer-codegen load gate. A shard built at
    # kCodegenVersion-1 must be refused OUTRIGHT and moved into the cache's
    # quarantine subtree; one built at kCodegenVersion must pass the gate.
    # Both halves are driven through the real loader (prepare_bank_dll) with
    # real DLLs, because that is the only code path a cached shard can take.
    add_test(
        NAME live_shard_codegen_gate_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/live_shard_codegen_gate_test.py"
                --cc "${CMAKE_C_COMPILER}"
                --runner-test "$<TARGET_FILE:live_overlay_preflight_test>"
                --implib-dir "${CMAKE_CURRENT_BINARY_DIR}"
                --codegen-identity-header
                "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src/codegen_identity.h"
                --work "${CMAKE_CURRENT_BINARY_DIR}/live-shard-codegen-gate-work")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_test(
          NAME live_overlay_linux_loader_test
          COMMAND "${Python3_EXECUTABLE}"
                  "${CMAKE_CURRENT_SOURCE_DIR}/tests/live_overlay_linux_loader_test.py"
                  --cc "${CMAKE_C_COMPILER}"
                  --runner-test "$<TARGET_FILE:live_overlay_preflight_test>"
                  --work "${CMAKE_CURRENT_BINARY_DIR}/live-overlay-linux-loader-work")
      add_test(
          NAME live_overlay_linux_process_test
          COMMAND "$<TARGET_FILE:live_overlay_preflight_test>"
                  --spawn-command
                  "${CMAKE_CURRENT_BINARY_DIR}/live-overlay-linux-process-work")
    endif()
    add_test(
        NAME m7_collect_two_instance_evidence_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_collect_two_instance_evidence.py")
    # beads-yjp.55: the coverage ingest could only ever promote the TARGET of
    # an interpreted transfer, which the finder had already discovered. The
    # code still being interpreted lives in the holes between compiled
    # functions and is recorded only in the root map. Promoting one seed per
    # interpreted span is what reaches it -- and it must stay exactly ONE, per
    # span and per mode, or the finder chops each body at every extra seed.
    add_test(
        NAME coverage_span_promotion_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_coverage_span_promotion.py")
  endif()
