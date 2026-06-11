# nhl-trace-replay - headless offline replay of a captured .xtr through our
# renderer backend (renderer plan, Phase 4).
#
# Self-written trace player: parses the .xtr (TraceReader is not exported) and
# drives the captured frame through the exported CommandProcessor API on a
# headless Runtime + our NhlD3D12GraphicsSystem. Links rex::runtime plus snappy
# (trace memory/EDRAM payloads are Snappy-compressed; snappy is statically
# linked into rexruntime.dll and not re-exported, so we link it ourselves).

add_executable(nhl-trace-replay
    src/trace_replay.cpp
    src/xtr_player.cpp
    src/image_dump.cpp
    ${CMAKE_SOURCE_DIR}/renderer/core/nhl_graphics_system.cpp
    ${CMAKE_SOURCE_DIR}/renderer/core/nhl_command_processor.cpp
)
# Repo root on the include path so "renderer/core/..." resolves (the host gets
# this from rexglue_setup_target; tool targets must add it themselves).
target_include_directories(nhl-trace-replay PRIVATE ${CMAKE_SOURCE_DIR})

# SDK lib dir, derived from the package config location (.../lib/cmake/rexglue).
get_filename_component(_rexsdk_lib "${rexglue_DIR}/../.." ABSOLUTE)
target_link_libraries(nhl-trace-replay PRIVATE
    rex::runtime
    $<$<CONFIG:Debug>:${_rexsdk_lib}/snappyd.lib>
    $<$<CONFIG:RelWithDebInfo>:${_rexsdk_lib}/snappyrd.lib>
    $<$<CONFIG:Release>:${_rexsdk_lib}/snappy.lib>
    $<$<CONFIG:MinSizeRel>:${_rexsdk_lib}/snappy.lib>
)
target_link_libraries(nhl-trace-replay PRIVATE dbghelp)  # crash stack symbolization
rexglue_apply_target_settings(nhl-trace-replay)

if(WIN32)
    # Stage rexruntime/TracyClient DLLs next to the tool (same pattern as the
    # packager; rexglue_configure_target is for windowed hosts only).
    add_custom_command(TARGET nhl-trace-replay POST_BUILD
        COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:nhl-trace-replay>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:nhl-trace-replay>;$<TARGET_FILE_DIR:nhl-trace-replay>>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()
