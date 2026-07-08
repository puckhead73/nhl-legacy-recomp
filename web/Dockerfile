# WASM/Emscripten build environment for NHL Legacy Recomp.
#
# Entry: ISO=/path/to/game.iso ./web/build.sh
#
# Prerequisites: Docker, Emscripten SDK (emsdk installed at /opt/emsdk)
FROM emscripten/emsdk:3.1.74

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build patch python3 python3-venv \
    && rm -rf /var/lib/apt/lists/*

# Pre-baked SDK patches for emscripten compatibility
COPY web/patches/ /opt/nhl-patches/

WORKDIR /src
CMD ["/src/web/lib/compile.sh"]
