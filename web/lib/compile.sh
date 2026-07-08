#!/usr/bin/env bash
# Compile all NHL Legacy generated code + SDK core under Emscripten.
# Invoked by web/build.sh inside the Docker image.
set -euo pipefail

WEB_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "${WEB_DIR}/.." && pwd)"
cd "${ROOT}"

PATCH_DIR="${WEB_DIR}/patches"
GENERATED="${ROOT}/generated/default"
OUT="${ROOT}/out/web"
mkdir -p "${OUT}"

echo "==> Patching SDK headers for Emscripten"
SDK_DIR="${ROOT}/out/linux/deps/rexglue-sdk"
if [ ! -d "${SDK_DIR}/include" ]; then
  echo "ERROR: SDK not found at ${SDK_DIR}"
  echo "  Run linux/build.sh first to clone the SDK"
  exit 1
fi

# Copy patches into the SDK include tree (overrides existing headers)
cp -v "${PATCH_DIR}/rex/platform.h"           "${SDK_DIR}/include/rex/platform.h"
cp -v "${PATCH_DIR}/rex/platform/fpscr.h"     "${SDK_DIR}/include/rex/platform/fpscr.h"
cp -v "${PATCH_DIR}/rex/chrono/chrono.h"      "${SDK_DIR}/include/rex/chrono/chrono.h"
cp -v "${PATCH_DIR}/rex/ppc/intrinsics.h"     "${SDK_DIR}/include/rex/ppc/intrinsics.h"

# Include paths
SDK_INC="${SDK_DIR}/include"
TP="${SDK_DIR}/thirdparty"
INCLUDES="-I${GENERATED} -I${SDK_INC} -I${TP}/simde -I${TP}/fmt/include -I${TP}/spdlog/include -I${TP}/tomlplusplus/include -I${TP}/spirv-headers/include"

CXXFLAGS="-std=c++23 -pthread -O0 -sWASM_BIGINT ${INCLUDES} -Wno-deprecated-pragma"

echo "==> Compiling generated recomp TUs (this will take several minutes)"
OBJ_DIR="${OUT}/obj"
rm -rf "${OBJ_DIR}"
mkdir -p "${OBJ_DIR}"

# Compile all generated C++ files in parallel
count=0
for f in "${GENERATED}"/nhllegacy_recomp.*.cpp "${GENERATED}"/nhllegacy_init.cpp "${GENERATED}"/nhllegacy_register.cpp; do
  [ ! -f "$f" ] && continue
  count=$((count + 1))
  base=$(basename "$f" .cpp)
  emcc -c ${CXXFLAGS} "$f" -o "${OBJ_DIR}/${base}.o" &
  
  # Limit parallelism to avoid OOM
  if [ $((count % 8)) -eq 0 ]; then
    wait
  fi
done
wait

total=$(ls "${OBJ_DIR}"/*.o 2>/dev/null | wc -l)
echo "  Compiled ${total}/${count} objects"
du -sh "${OBJ_DIR}"

# Verify we got all of them
expected=$(ls "${GENERATED}"/nhllegacy_recomp.*.cpp | wc -l)
expected=$((expected + 2))  # + init.cpp + register.cpp
if [ "${total}" -ne "${expected}" ]; then
  echo "ERROR: missing objects (${total}/${expected})"
  for f in "${GENERATED}"/nhllegacy_recomp.*.cpp; do
    base=$(basename "$f" .cpp)
    [ ! -f "${OBJ_DIR}/${base}.o" ] && echo "  MISSING: ${base}"
  done
  [ ! -f "${OBJ_DIR}/nhllegacy_init.o" ] && echo "  MISSING: nhllegacy_init"
  [ ! -f "${OBJ_DIR}/nhllegacy_register.o" ] && echo "  MISSING: nhllegacy_register"
  exit 1
fi

echo ""
echo "==> All ${total} objects compiled successfully!"
echo "  Output: ${OBJ_DIR}/"
echo "  Size: $(du -sh "${OBJ_DIR}" | awk '{print $1}')"
