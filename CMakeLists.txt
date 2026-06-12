# nhllegacy - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
# C is enabled for the plume shader-embed (.c blobs from plume_file_to_c); CXX is the project.
project(nhllegacy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NHLLEGACY_SOURCES
    src/main.cpp
    src/kernel_stubs.cpp
    src/diag_hooks.cpp
    # High-cut engine (H-1): D3D9 logical resource graph — pass-through hooks that
    # build the EDRAM-free RT/texture/resolve graph. Env-gated (NHL_HIGHCUT); no
    # behavior change. Supersedes the M1 correlation tap (gpu/hooks/d3d9_tap.cpp,
    # kept in-tree for reference) — both override the same symbols, so only one may
    # be compiled into the target at a time.
    gpu/hooks/d3d9_resources.cpp
    # High-cut engine (H-2): first plume-driven frame — an in-process plume D3D12
    # swapchain driven by the guest Present hook. Env-gated (NHL_HIGHCUT_PRESENT).
    gpu/hooks/plume_present.cpp
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

# High-cut engine (H-2): vendored plume RHI (MIT, third_party/plume). Built as a
# static lib in its own directory scope (C++17, its own backends) and linked into
# nhllegacy so gpu/hooks/plume_present.cpp can drive an in-process plume swapchain
# from the intercepted guest Present. Same add_subdirectory pattern proven by the
# standalone gpu/smoke build.
add_subdirectory(third_party/plume ${CMAKE_BINARY_DIR}/plume_build)
target_link_libraries(nhllegacy PRIVATE plume d3d12 dxgi dxguid)

# High-cut C-1: compile the bring-up triangle shader to SPIR-V (+DXIL) via plume's own
# dxc-fetch + embed cmake. Generates ${CMAKE_BINARY_DIR}/shaders/triangle{Vert,Frag}.hlsl.
# spirv.h (blobs triangle{Vert,Frag}BlobSPIRV) and adds the embedded .c to nhllegacy +
# ${CMAKE_BINARY_DIR} to its include path, so gpu/hooks/plume_present.cpp can render real
# geometry (proves plume's pipeline/vertex-buffer/draw path beyond the H-2 clears). The
# REAL path-C engine generates SPIR-V at runtime from Xenos ucode via the SDK
# SpirvShaderTranslator; this hardcoded triangle is bring-up only.
include(third_party/plume/examples/cmake/PlumeShaders.cmake)
plume_shaders_init()
plume_compile_vertex_shader(nhllegacy
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/hooks/shaders/triangle.hlsl" triangleVert VSMain)
plume_compile_pixel_shader(nhllegacy
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/hooks/shaders/triangle.hlsl" triangleFrag PSMain)

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
