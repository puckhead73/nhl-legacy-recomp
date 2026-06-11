# nhl-legacy-builder - end-user "disc dump -> playable install" CLI.
#
# Console app linking rex::runtime in tool mode (XDVDFS reader + sha256 only;
# no window, no GPU, no generated guest code).

set(PACKAGER_VERSION "0.1.0")

add_executable(nhl-legacy-builder
    src/cli.cpp
    src/errors.cpp
    src/folder_source.cpp
    src/installer.cpp
    src/iso_source.cpp
    src/main.cpp
    src/manifest.cpp
)
target_compile_definitions(nhl-legacy-builder PRIVATE
    PACKAGER_VERSION="${PACKAGER_VERSION}")
target_link_libraries(nhl-legacy-builder PRIVATE rex::runtime)
rexglue_apply_target_settings(nhl-legacy-builder)

if(WIN32)
    # Stage rexruntime/TracyClient DLLs next to the builder - same pattern as
    # rexglue_configure_target, which we don't use (it's for windowed hosts).
    add_custom_command(TARGET nhl-legacy-builder POST_BUILD
        COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:nhl-legacy-builder>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:nhl-legacy-builder>;$<TARGET_FILE_DIR:nhl-legacy-builder>>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()

# Dev payload manifest, generated at configure time from the in-repo
# reference XEX. Lets 'verify' (and the gated tests below) run before any
# release zip exists: --payload <build>/tools/packager/dev_payload
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/default.xex")
    file(SHA256 "${CMAKE_SOURCE_DIR}/assets/default.xex" NHLLEGACY_XEX_SHA256)
    file(SIZE "${CMAKE_SOURCE_DIR}/assets/default.xex" NHLLEGACY_XEX_SIZE)
    configure_file(manifest.toml.in
        "${CMAKE_CURRENT_BINARY_DIR}/dev_payload/manifest.toml" @ONLY)
endif()

# Gated integration tests - they need real game data, so they only register
# when the cache vars point at it (e.g. -DNHLLEGACY_TEST_ISO=H:/dumps/nhl.iso).
set(NHLLEGACY_TEST_ISO "" CACHE FILEPATH
    "Real NHL Legacy .iso used by the gated packager tests")
set(NHLLEGACY_TEST_GAME_DIR "" CACHE PATH
    "Extracted NHL Legacy game folder used by the gated packager tests")
if(NHLLEGACY_TEST_ISO)
    add_test(NAME packager_verify_iso
        COMMAND nhl-legacy-builder verify --iso "${NHLLEGACY_TEST_ISO}"
                --payload "${CMAKE_CURRENT_BINARY_DIR}/dev_payload")
endif()
if(NHLLEGACY_TEST_GAME_DIR)
    add_test(NAME packager_verify_folder
        COMMAND nhl-legacy-builder verify --from "${NHLLEGACY_TEST_GAME_DIR}"
                --payload "${CMAKE_CURRENT_BINARY_DIR}/dev_payload")
endif()
