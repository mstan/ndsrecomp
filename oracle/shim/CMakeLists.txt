# oracle/shim/CMakeLists.txt — builds the nds_oracle reference shim.
#
# Out-of-tree by design: these sources live in the ndsrecomp repo (tracked),
# not in the gitignored melonDS clone. We pull melonDS in as a subdirectory
# with its frontend/renderer/JIT/GDB disabled, enable the oracle event-counter
# hook (the one melonDS edit, applied by ../setup-melonds.sh), and link the
# `core` static lib into our headless executable.
#
# Configure (from ndsrecomp root, after `bash oracle/setup-melonds.sh`):
#   cmake -B oracle/shim/build -S oracle/shim \
#         -DMELONDS_DIR=third_party/melonDS -G Ninja
#   cmake --build oracle/shim/build --target nds_oracle

cmake_minimum_required(VERSION 3.16)
project(nds_oracle CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Path to the vendored melonDS clone (relative to wherever cmake is invoked,
# or absolute). Defaults to the standard setup-melonds.sh location.
set(MELONDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/melonDS"
    CACHE PATH "Path to the melonDS source clone")
get_filename_component(MELONDS_DIR "${MELONDS_DIR}" ABSOLUTE)
if(NOT EXISTS "${MELONDS_DIR}/src/NDS.cpp")
    message(FATAL_ERROR
        "melonDS not found at ${MELONDS_DIR}. Run: bash oracle/setup-melonds.sh")
endif()

# Strip melonDS down to the headless core before pulling it in.
set(BUILD_QT_SDL        OFF CACHE BOOL "" FORCE)  # no Qt/SDL frontend
set(ENABLE_OGLRENDERER  OFF CACHE BOOL "" FORCE)  # we read the framebuffer array
set(ENABLE_GDBSTUB      OFF CACHE BOOL "" FORCE)  # our TCP protocol replaces it
set(ENABLE_JIT          OFF CACHE BOOL "" FORCE)  # deterministic interpreter
set(USE_VCPKG           OFF CACHE BOOL "" FORCE)
# gcc 15.2 ICEs under LTO on melonDS's DMA timing tables; the oracle's perf is
# irrelevant, so disable LTO outright.
set(ENABLE_LTO          OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO_RELEASE  OFF CACHE BOOL "" FORCE)

# Enable the oracle's SetIRQ event-counter hook in core (see ../patches/).
# Directory-scoped, so it applies to the melonDS subdirectory added below.
add_compile_definitions(MELONDS_ORACLE_HOOKS)

add_subdirectory("${MELONDS_DIR}" melonDS-build)

add_executable(nds_oracle
    nds_oracle.cpp
    platform_oracle.cpp)
target_include_directories(nds_oracle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nds_oracle PRIVATE core)

if(WIN32)
    target_link_libraries(nds_oracle PRIVATE ws2_32)
endif()
