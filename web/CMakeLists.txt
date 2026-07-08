# WASM build — compiles all generated recomp TUs into a static library.
# Usage: source /tmp/emsdk/emsdk_env.sh && emcmake cmake -S web -B out/web/build -G Ninja
cmake_minimum_required(VERSION 3.25)
project(nhllegacy-wasm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GENERATED_DIR "${CMAKE_SOURCE_DIR}/../generated/default")
set(SDK_DIR "${CMAKE_SOURCE_DIR}/../out/linux/deps/rexglue-sdk")
set(PATCH_DIR "${CMAKE_SOURCE_DIR}/patches")

# Include paths (with patched headers taking precedence)
set(NHL_WASM_INCLUDES
  "${PATCH_DIR}"                    # patched headers first
  "${GENERATED_DIR}"                # generated init.h
  "${SDK_DIR}/include"              # SDK headers
  "${SDK_DIR}/thirdparty/simde"     # SIMDE
  "${SDK_DIR}/thirdparty/fmt/include" # fmt
  "${SDK_DIR}/thirdparty/spdlog/include" # spdlog
  "${SDK_DIR}/thirdparty/tomlplusplus/include" # toml++
)

file(GLOB GENERATED_SOURCES "${GENERATED_DIR}/*.cpp")

add_library(nhllegacy_recomp STATIC ${GENERATED_SOURCES})
target_include_directories(nhllegacy_recomp PRIVATE ${NHL_WASM_INCLUDES})
target_compile_options(nhllegacy_recomp PRIVATE
  -pthread
  -Wno-deprecated-pragma
)
target_link_options(nhllegacy_recomp PRIVATE
  -pthread
)
