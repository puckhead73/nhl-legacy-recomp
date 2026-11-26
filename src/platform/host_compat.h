// host_compat.h - the handful of Win32 APIs the host code uses, made portable.
//
// On Windows this just pulls in <windows.h> (lean) so existing call sites are
// unchanged. Elsewhere (Linux / ARM64 today, Horizon later) it provides
// source-compatible stand-ins with the same names and semantics that matter here:
//
//   VirtualQuery + MEMORY_BASIC_INFORMATION + MEM_* / PAGE_*  - page-safe guest
//       memory probing used by the diagnostic scanners. Linux answers from
//       /proc/self/maps; the Horizon port maps it onto svcQueryMemory.
//   _byteswap_ushort/_ulong/_uint64, _stricmp/_strnicmp, Sleep, GetTickCount
//   SetEnvironmentVariableA (setenv), plus nhl_host::GetProcessEnv to read it back
//   GetAsyncKeyState - debug hotkeys (F9/F10/F11). Returns "up" off Windows
//       until an SDL-backed implementation is wired in.
//
// plus nhl_host::HardExit(code), the TerminateProcess(GetCurrentProcess()) idiom.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdlib>

#include <string>

namespace nhl_host {
// Process environment (what SetEnvironmentVariableA writes; the CRT's getenv copy
// does not see those updates on Windows). Empty string when unset.
inline std::string GetProcessEnv(const char* name) {
  char buf[1024];
  const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
  return (n && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}
[[noreturn]] inline void HardExit(unsigned code) {
  ::TerminateProcess(::GetCurrentProcess(), code);
  std::abort();  // unreachable
}
}  // namespace nhl_host

#else  // !_WIN32

#include <strings.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

using DWORD = std::uint32_t;
using SIZE_T = std::size_t;
using PVOID = void*;
using LPCVOID = const void*;

// Values match winnt.h so code that logs/compares them stays meaningful.
constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD MEM_RESERVE = 0x2000;
constexpr DWORD MEM_FREE = 0x10000;
constexpr DWORD MEM_PRIVATE = 0x20000;
constexpr DWORD MEM_MAPPED = 0x40000;
constexpr DWORD MEM_IMAGE = 0x1000000;

constexpr DWORD PAGE_NOACCESS = 0x01;
constexpr DWORD PAGE_READONLY = 0x02;
constexpr DWORD PAGE_READWRITE = 0x04;
constexpr DWORD PAGE_EXECUTE = 0x10;
constexpr DWORD PAGE_EXECUTE_READ = 0x20;
constexpr DWORD PAGE_EXECUTE_READWRITE = 0x40;
constexpr DWORD PAGE_GUARD = 0x100;

struct MEMORY_BASIC_INFORMATION {
  PVOID BaseAddress;
  PVOID AllocationBase;
  DWORD AllocationProtect;
  SIZE_T RegionSize;
  DWORD State;
  DWORD Protect;
  DWORD Type;
};

// Describes the region containing `address`, like Win32 VirtualQuery: a mapped
// range reports MEM_COMMIT (or MEM_RESERVE + PAGE_NOACCESS when mapped PROT_NONE,
// which is how the runtime reserves the guest address space); an unmapped gap
// reports MEM_FREE up to the next mapping. Returns sizeof(*info), or 0 on failure.
SIZE_T VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION* info, SIZE_T length);

inline std::uint16_t _byteswap_ushort(std::uint16_t v) { return __builtin_bswap16(v); }
inline std::uint32_t _byteswap_ulong(std::uint32_t v) { return __builtin_bswap32(v); }
inline std::uint64_t _byteswap_uint64(std::uint64_t v) { return __builtin_bswap64(v); }

inline int _stricmp(const char* a, const char* b) { return ::strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, std::size_t n) {
  return ::strncasecmp(a, b, n);
}

// Milliseconds since an arbitrary epoch (wraps at 2^32 like Win32's). Log stamps only.
inline DWORD GetTickCount() {
  return static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

inline void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Debug hotkeys only. TODO(switch-port): back with SDL_GetKeyboardState / HID.
inline short GetAsyncKeyState(int /*virtual_key*/) { return 0; }

// Same truthiness as Win32: nonzero on success. value == nullptr unsets.
inline int SetEnvironmentVariableA(const char* name, const char* value) {
  return (value ? ::setenv(name, value, 1) : ::unsetenv(name)) == 0;
}

namespace nhl_host {
inline std::string GetProcessEnv(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}
[[noreturn]] inline void HardExit(unsigned code) {
  // Same contract as TerminateProcess: no atexit/static destructors, no flush of
  // C++ streams. Callers that need the PGO profile flushed do it beforehand.
  ::_exit(static_cast<int>(code));
}
}  // namespace nhl_host

#endif  // _WIN32
