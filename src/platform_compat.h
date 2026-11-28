// Minimal Win32 surface for the host-side NHL code when building on Linux.
//
// On Windows this just includes <windows.h>. Elsewhere it provides the small set
// of Win32 calls the port's host code actually uses, implemented on POSIX:
//   - VirtualQuery + MEMORY_BASIC_INFORMATION (BaseAddress/RegionSize/State/
//     Protect only), backed by /proc/self/maps. Used by the diagnostic dumpers to
//     check a host address is mapped + readable before touching it.
//   - Sleep, GetTickCount, GetCurrentProcess/TerminateProcess (hard exit),
//     _putenv_s / SetEnvironmentVariableA / GetEnvironmentVariableA, fopen_s,
//     GetAsyncKeyState (always "not pressed" - hotkeys are Windows-only).
// Anything heavier (VEH page traps, D3D12, CreateProcess pipes) is either ported
// properly in its own TU or compiled out with #ifdef _WIN32.
#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#else  // !_WIN32

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using DWORD = uint32_t;
using SIZE_T = size_t;
using BOOL = int;
using HANDLE = void*;

inline constexpr DWORD MEM_COMMIT = 0x00001000;
inline constexpr DWORD MEM_RESERVE = 0x00002000;
inline constexpr DWORD MEM_FREE = 0x00010000;
inline constexpr DWORD PAGE_NOACCESS = 0x01;
inline constexpr DWORD PAGE_READONLY = 0x02;
inline constexpr DWORD PAGE_READWRITE = 0x04;
inline constexpr DWORD PAGE_EXECUTE_READ = 0x20;
inline constexpr DWORD PAGE_EXECUTE_READWRITE = 0x40;
inline constexpr DWORD PAGE_GUARD = 0x100;

struct MEMORY_BASIC_INFORMATION {
  void* BaseAddress;
  void* AllocationBase;
  DWORD AllocationProtect;
  SIZE_T RegionSize;
  DWORD State;
  DWORD Protect;
  DWORD Type;
};

// Describe the region containing `addr` from /proc/self/maps. Mapped ranges
// report MEM_COMMIT with a PAGE_* mirroring r/w/x ("---" -> PAGE_NOACCESS, which
// is how reserved-but-uncommitted guest memory is mapped). A gap reports
// MEM_FREE with RegionSize running to the next mapping, so callers that walk
// memory by `p += RegionSize` still make progress. Returns sizeof(*info) on
// success, 0 on failure - same contract as Win32.
inline SIZE_T VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* info,
                           SIZE_T len) {
  if (!info || len < sizeof(*info)) return 0;
  const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
  const uintptr_t a = uintptr_t(addr) & ~(page - 1);
  FILE* f = std::fopen("/proc/self/maps", "r");
  if (!f) return 0;
  char line[512];
  uintptr_t next_start = UINTPTR_MAX;
  bool found = false;
  while (std::fgets(line, sizeof(line), f)) {
    unsigned long long lo = 0, hi = 0;
    char perms[5] = {};
    if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) != 3) continue;
    if (a >= lo && a < hi) {
      DWORD prot;
      const bool r = perms[0] == 'r', w = perms[1] == 'w', x = perms[2] == 'x';
      if (!r && !w && !x) prot = PAGE_NOACCESS;
      else if (x) prot = w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
      else prot = w ? PAGE_READWRITE : PAGE_READONLY;
      *info = {reinterpret_cast<void*>(a), reinterpret_cast<void*>(uintptr_t(lo)),
               prot, SIZE_T(hi - a), MEM_COMMIT, prot, 0};
      found = true;
      break;
    }
    if (lo > a && lo < next_start) next_start = uintptr_t(lo);
  }
  std::fclose(f);
  if (!found) {
    const uintptr_t end = next_start == UINTPTR_MAX ? a + page : next_start;
    *info = {reinterpret_cast<void*>(a), nullptr, 0, SIZE_T(end - a), MEM_FREE,
             PAGE_NOACCESS, 0};
  }
  return sizeof(*info);
}

inline void Sleep(DWORD ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline DWORD GetTickCount() {
  using namespace std::chrono;
  return DWORD(duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                   .count());
}

inline HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(-1); }

// Only ever used on the current process as a hard exit (skips static dtors,
// like the Windows path).
inline BOOL TerminateProcess(HANDLE, unsigned int code) {
  std::fflush(nullptr);
  _exit(int(code));
}

inline int _putenv_s(const char* name, const char* value) {
  return setenv(name, value, 1) == 0 ? 0 : errno;
}

inline BOOL SetEnvironmentVariableA(const char* name, const char* value) {
  return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

// Win32 contract: returns the length copied (excluding NUL), the required size
// (including NUL) when the buffer is too small, or 0 when unset.
inline DWORD GetEnvironmentVariableA(const char* name, char* buf, DWORD size) {
  const char* v = std::getenv(name);
  if (!v) return 0;
  const size_t n = std::strlen(v);
  if (!buf || n + 1 > size) return DWORD(n + 1);
  std::memcpy(buf, v, n + 1);
  return DWORD(n);
}

inline int fopen_s(FILE** out, const char* path, const char* mode) {
  *out = std::fopen(path, mode);
  return *out ? 0 : errno;
}

inline short GetAsyncKeyState(int) { return 0; }

// MSVC byte-swap intrinsics (big-endian guest data reads).
inline uint16_t _byteswap_ushort(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t _byteswap_ulong(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t _byteswap_uint64(uint64_t v) { return __builtin_bswap64(v); }

#endif  // _WIN32
