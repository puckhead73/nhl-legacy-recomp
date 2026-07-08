#!/usr/bin/env bash
# Build nhllegacy for WebAssembly (Emscripten).
#
# Prerequisites: Docker, SDK already built (run linux/build.sh first to clone the SDK)
#
#   ./web/build.sh
set -euo pipefail

WEB_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${WEB_DIR}/.." && pwd)"
cd "${ROOT}"

IMAGE="${NHL_WEB_IMAGE:-nhl-legacy-recomp-web}"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0"
  echo "Compiles all generated recomp C++ to WebAssembly."
  echo "Requires Docker. Builds the emscripten-based image and compiles in it."
  exit 0
fi

echo "==> Docker build ${IMAGE}"
docker build -f web/Dockerfile -t "${IMAGE}" .

echo "==> Compile in Docker"
docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v "${ROOT}:/src" \
  "${IMAGE}" \
  /src/web/lib/compile.sh

echo ""
echo "Build complete."
echo "  Objects: out/web/obj/"
echo "  $(ls out/web/obj/*.o 2>/dev/null | wc -l) object files"
