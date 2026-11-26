# Cross-compile for Linux aarch64 (Switch L4T / any ARM64 Linux) with clang + lld.
#
# Host requirements (Debian/Ubuntu multiarch; see docs/switch-port/linux-arm64-build.md):
#   dpkg --add-architecture arm64   (+ an arm64 ports apt source)
#   apt install clang-20 lld-20 llvm-20 libc6-dev:arm64 libstdc++-13-dev:arm64 libgcc-13-dev:arm64 \
#               binutils-aarch64-linux-gnu  (+ the SDL3 -dev:arm64 headers)
#
# A native aarch64 host needs no toolchain file at all.
#
# Override the sysroot with -DNHL_AARCH64_SYSROOT=<path> (default "/", i.e. the
# multiarch libs under /usr/lib/aarch64-linux-gnu). A Switch L4T rootfs works too.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(NHL_AARCH64_TRIPLE "aarch64-linux-gnu" CACHE STRING "aarch64 target triple")
set(NHL_AARCH64_SYSROOT "/" CACHE PATH "aarch64 sysroot")

# Clang >= 19 is required in practice: with libstdc++ (<= 14), <expected> is
# hidden unless __cpp_concepts >= 202002L, which clang 18 does not report, and the
# SDK uses std::expected. Prefer clang-20 (what CMakePresets.json names).
find_program(NHL_CLANG NAMES clang-20 clang-19 clang REQUIRED)
find_program(NHL_CLANGXX NAMES clang++-20 clang++-19 clang++ REQUIRED)
set(CMAKE_C_COMPILER ${NHL_CLANG})
set(CMAKE_CXX_COMPILER ${NHL_CLANGXX})
set(CMAKE_C_COMPILER_TARGET ${NHL_AARCH64_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${NHL_AARCH64_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${NHL_AARCH64_TRIPLE})
if(NOT NHL_AARCH64_SYSROOT STREQUAL "/")
  set(CMAKE_SYSROOT ${NHL_AARCH64_SYSROOT})
endif()

# lld understands every target; binutils-aarch64-linux-gnu supplies ar/ranlib/strip.
set(CMAKE_LINKER_TYPE LLD)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
find_program(CMAKE_AR NAMES llvm-ar-20 llvm-ar-19 llvm-ar ${NHL_AARCH64_TRIPLE}-ar)
find_program(CMAKE_RANLIB NAMES llvm-ranlib-20 llvm-ranlib-19 llvm-ranlib ${NHL_AARCH64_TRIPLE}-ranlib)
find_program(CMAKE_STRIP NAMES llvm-strip-20 llvm-strip-19 llvm-strip ${NHL_AARCH64_TRIPLE}-strip)

# Cortex-A57 is the Tegra X1 (Switch) core; the generic armv8-a baseline runs on
# every ARM64 board. Keep it portable (no -mcpu=native, same lesson as x86 #UD).
set(NHL_AARCH64_CPU "cortex-a57" CACHE STRING "-mcpu for the aarch64 build (empty = armv8-a)")
if(NHL_AARCH64_CPU)
  set(CMAKE_C_FLAGS_INIT "-mcpu=${NHL_AARCH64_CPU}")
  set(CMAKE_CXX_FLAGS_INIT "-mcpu=${NHL_AARCH64_CPU}")
else()
  set(CMAKE_C_FLAGS_INIT "-march=armv8-a")
  set(CMAKE_CXX_FLAGS_INIT "-march=armv8-a")
endif()

# pkg-config must look at the target's .pc files, not the host's.
set(ENV{PKG_CONFIG_LIBDIR} "${NHL_AARCH64_SYSROOT}/usr/lib/${NHL_AARCH64_TRIPLE}/pkgconfig:${NHL_AARCH64_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")

# Run aarch64 helpers / tests under qemu-user when cross-compiling from x86_64.
find_program(NHL_QEMU_AARCH64 NAMES qemu-aarch64-static qemu-aarch64)
if(NHL_QEMU_AARCH64 AND NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(CMAKE_CROSSCOMPILING_EMULATOR ${NHL_QEMU_AARCH64} -L ${NHL_AARCH64_SYSROOT})
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH ${NHL_AARCH64_SYSROOT})
set(CMAKE_LIBRARY_ARCHITECTURE ${NHL_AARCH64_TRIPLE})
