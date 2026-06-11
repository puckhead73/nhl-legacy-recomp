# nhllegacy - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nhllegacy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NHLLEGACY_SOURCES
    src/main.cpp
    src/kernel_stubs.cpp
    src/diag_hooks.cpp
    # High-cut engine (M1): D3D9 entry-point TAP — call-counting pass-through
    # hooks for runtime correlation. Env-gated (NHL_D3D9_TAP); no behavior change.
    gpu/hooks/d3d9_tap.cpp
    # Loose-file overlay: union-mount _compiled over the cache: scratch dir,
    # with per-file .dds -> .rx2 texture synthesis in the lower tree.
    src/union_device.cpp
    src/loose_tree_device.cpp
    # Stage-1 texture-injection registry + sidecar (auto address->asset mapping).
    src/injection_registry.cpp
    # Custom renderer backend (Sprint 1: log-and-delegate over the SDK front-end)
    renderer/core/nhl_graphics_system.cpp
    renderer/core/nhl_command_processor.cpp
    # Trace replay + image dump, shared with tools/replay. Enables the in-host
    # replay-and-screenshot mode (NHL_REPLAY_XTR=<frame.xtr>): replays a captured
    # frame through the live backend + presenter and writes replay_frame.png.
    tools/replay/src/xtr_player.cpp
    tools/replay/src/image_dump.cpp
)

if(WIN32)
    add_executable(nhllegacy WIN32 ${NHLLEGACY_SOURCES})
else()
    add_executable(nhllegacy ${NHLLEGACY_SOURCES})
endif()

rexglue_setup_target(nhllegacy)

# snappy (for .xtr trace decompression in the in-host replay mode). Statically
# linked into rexruntime.dll but not re-exported, so link it directly; lib name
# is config-specific. SDK lib dir derived from the package config location.
get_filename_component(_rexsdk_lib "${rexglue_DIR}/../.." ABSOLUTE)
target_link_libraries(nhllegacy PRIVATE
    $<$<CONFIG:Debug>:${_rexsdk_lib}/snappyd.lib>
    $<$<CONFIG:RelWithDebInfo>:${_rexsdk_lib}/snappyrd.lib>
    $<$<CONFIG:Release>:${_rexsdk_lib}/snappy.lib>
    $<$<CONFIG:MinSizeRel>:${_rexsdk_lib}/snappy.lib>
)

# rex/ui/renderdoc_api.h does `#include <renderdoc_app.h>`, which lives in the
# SDK's include/renderdoc subdir (not on the default -isystem include path). Add
# it so the beta backend's programmatic RenderDoc capture (NHL_BETA_RENDERDOC) can
# bracket the takeover frame.
target_include_directories(nhllegacy PRIVATE "${_rexsdk_lib}/../include/renderdoc")

# Runtime .dds -> .rx2 texture conversion: link the Rust static lib built from
# nhl-database-studio/crates/tdb-rx2-ffi (reuses the proven tdb-rx2 format code
# rather than re-porting the tiling/byte-swap/descriptor math into C++).
# Build it with: cargo build -p tdb-rx2-ffi --release   (in nhl-database-studio)
set(RX2FFI_LIB "E:/Repositories/nhl-database-studio/target/release/tdb_rx2_ffi.lib"
    CACHE FILEPATH "Path to the tdb-rx2-ffi static library")
if(WIN32 AND EXISTS "${RX2FFI_LIB}")
    target_compile_definitions(nhllegacy PRIVATE NHLLEGACY_HAVE_RX2FFI=1)
    # Rust staticlib pulls in these Windows system libs.
    target_link_libraries(nhllegacy PRIVATE "${RX2FFI_LIB}"
        ntdll userenv ws2_32 dbghelp bcrypt advapi32)
else()
    message(WARNING "tdb-rx2-ffi not found at ${RX2FFI_LIB}; loose .dds texture "
                    "override disabled (build it: cargo build -p tdb-rx2-ffi --release)")
endif()

# End-user packager CLI (disc dump -> playable install). See tools/packager/.
# Windows-only for now: it ships the Windows port binaries and uses a wide
# (wmain) entry point.
option(NHLLEGACY_BUILD_PACKAGER "Build the nhl-legacy-builder packager CLI" ON)
if(NHLLEGACY_BUILD_PACKAGER AND WIN32)
    enable_testing()
    add_subdirectory(tools/packager)
endif()

# Offline trace replay tooling (renderer plan, Phase 4).
# Self-written .xtr player: the SDK's TraceDump/TracePlayer/TraceReader are NOT
# exported from rexruntime.dll, so tools/replay parses the trace itself and
# drives it through the *exported* CommandProcessor API (ExecutePacket /
# RestoreRegisters / RestoreEdramSnapshot) on a headless Runtime + our backend.
option(NHLLEGACY_BUILD_TRACE_TOOLS "Build the offline .xtr replay tool (nhl-trace-replay)" ON)
if(NHLLEGACY_BUILD_TRACE_TOOLS AND WIN32)
    add_subdirectory(tools/replay)
endif()
