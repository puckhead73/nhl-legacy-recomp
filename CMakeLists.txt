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
    # High-cut path C (P-1): glslang link/compile probe for the SPIR-V translator port.
    gpu/hooks/highcut_spirv_probe.cpp
    # High-cut path C (P-2): ported Xenia Xenos->SPIR-V translator (rex:: adaptation of the
    # source whose headers the SDK ships but whose .cc it does not). P-2a = the SpirvBuilder.
    gpu/spirv/spirv_builder.cc
    # P-2b = the SpirvShaderTranslator. NOTE: the SDK's spirv_translator.h is a ReXGlue-enhanced
    # kVersion-12 API (features forward-ported from Xenia's DXBC translator); these bodies are
    # upstream Xenia 95a5c3e (kVersion-6), reconciled to compile/link against the SDK header.
    # kVersion-12-only features are not implemented here. See docs/highcut-c-plume-renderer-plan.md.
    gpu/spirv/spirv_shader_translator.cc
    gpu/spirv/spirv_shader_translator_alu.cc
    gpu/spirv/spirv_shader_translator_fetch.cc
    gpu/spirv/spirv_shader_translator_memexport.cc
    gpu/spirv/spirv_shader_translator_rb.cc
    # P-3 = the SpirvShader (Shader subclass the translator emits into).
    gpu/spirv/spirv_shader.cc
    # Const data tables the SDK declares (ucode.h / util/draw.h) but doesn't export.
    gpu/spirv/spirv_translator_tables.cc
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

# SPIKE (docs/vulkan-rov-backend-spike-prompt.md): the SDK's Vulkan ROV backend is
# a COMPILE-TIME option (REXGLUE_USE_VULKAN, OFF in the stock win-amd64 zips). When
# CMAKE_PREFIX_PATH points at an SDK built with it ON, enable this to compile the
# NHL_VK_BACKEND env gate (src/nhllegacy_app.h) and link the Vulkan graphics system.
option(NHLLEGACY_VULKAN_BACKEND "Link the SDK's Vulkan ROV backend (needs an SDK built with REXGLUE_USE_VULKAN=ON)" OFF)
if(NHLLEGACY_VULKAN_BACKEND)
    target_compile_definitions(nhllegacy PRIVATE NHL_HAVE_VULKAN_BACKEND=1)
    # Compiled ONLY in the Vulkan build dir — links the SDK's Vulkan backend symbols
    # that the default (D3D12-only) SDK does not export.
    target_sources(nhllegacy PRIVATE renderer/core/nhl_vk_backend.cpp)
    # In-game enhancements/settings overlay (Guide-button toggle + perf HUD +
    # supersampling). VK-gated: it reads the Vulkan fps tap and is only created
    # under NHL_VK_BACKEND. Uses the SDK ImGui drawer (rexui/imgui, already linked).
    target_sources(nhllegacy PRIVATE renderer/core/nhl_overlay.cpp)
    # Guest controller-input block while the overlay is open (overrides the guest
    # XamInputGetState import). VK-build-only so the default D3D12 build's input
    # path is unchanged.
    target_sources(nhllegacy PRIVATE src/input_block.cpp)
    message(STATUS "nhllegacy: Vulkan backend gate ENABLED (NHL_HAVE_VULKAN_BACKEND)")
endif()

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
# C-3a: zero-input solid-color PS to pair with a translated Xenos VS for pipeline creation.
plume_compile_pixel_shader(nhllegacy
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/hooks/shaders/solid.hlsl" solidFrag PSMain)

# High-cut path C (P-1): vendor glslang for the Xenos->SPIR-V translator PORT. The SDK's
# spirv_builder.h / spirv_translator.h ARE Xenia's (they extend glslang's spv::Builder) but the
# implementation is not shipped, so P-2 ports Xenia's translator + spirv_builder .cc, which need
# glslang's SPIRV backend. P-1 proves glslang builds+links in this clang/MSVC env and that the
# SDK header compiles against it. Build only what we need (no GLSL binaries/tests/optimizer).
include(FetchContent)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)        # skip the SPIRV-Tools optimizer dependency for now
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG 14.3.0)
FetchContent_MakeAvailable(glslang)
# High-cut path C (P-3): glslang 14.3.0's Builder::createAccessChain derives its result type
# from the STATEFUL accessChain (getResultingAccessChainType), but Xenia's Xenos->SPIR-V
# translator calls createAccessChain directly with explicit base+offsets and never configures
# the stateful chain (it targets older, stateless glslang) -> NoResult assert at runtime. Patch
# createAccessChain to compute the type statelessly from base+offsets. glslang's own internal
# caller (collapseAccessChain) passes accessChain.base/indexChain, so it yields the identical
# type and is unaffected. Idempotent (guarded by the "nhl high-cut P-3" marker), so it is safe
# to re-run on every configure. See docs/highcut-c-plume-renderer-plan.md.
set(_glslang_spvbuilder "${glslang_SOURCE_DIR}/SPIRV/SpvBuilder.cpp")
file(READ "${_glslang_spvbuilder}" _spvbuilder_src)
string(FIND "${_spvbuilder_src}" "nhl high-cut P-3" _spvbuilder_patched)
if(_spvbuilder_patched EQUAL -1)
  string(REPLACE
    "    Id typeId = getResultingAccessChainType();"
[[    Id typeId = getTypeId(base); /* nhl high-cut P-3: stateless access-chain type */
    typeId = getContainedTypeId(typeId);
    for (int nhl_i = 0; nhl_i < (int)offsets.size(); ++nhl_i)
        typeId = isStructType(typeId) ? getContainedTypeId(typeId, getConstantScalar(offsets[nhl_i]))
                                      : getContainedTypeId(typeId, offsets[nhl_i]);]]
    _spvbuilder_src "${_spvbuilder_src}")
  file(WRITE "${_glslang_spvbuilder}" "${_spvbuilder_src}")
  message(STATUS "high-cut P-3: patched glslang createAccessChain (stateless)")
else()
  message(STATUS "high-cut P-3: glslang createAccessChain already patched")
endif()
# The SPIRV target provides spv::Builder (glslang's SPIR-V backend). Adding the glslang source
# root to the include path makes the SDK's `#include <SPIRV/SpvBuilder.h>` resolve.
target_link_libraries(nhllegacy PRIVATE SPIRV)
target_include_directories(nhllegacy PRIVATE "${glslang_SOURCE_DIR}")

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
