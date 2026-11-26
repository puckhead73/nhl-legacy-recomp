// host_compat.cpp - non-Windows implementations for host_compat.h.
#include "platform/host_compat.h"

#include <cstdlib>

namespace nhl_host {
void HardExitProcess(unsigned code) { HardExit(code); }
void SetProcessEnv(const char* name, const char* value) {
#if defined(_WIN32)
  ::_putenv_s(name, value);
#else
  ::setenv(name, value, 1);
#endif
}
}  // namespace nhl_host

#if !defined(_WIN32)

#include <cinttypes>
#include <cstdio>
#include <cstring>

#if defined(__linux__)
// Linux: answer from /proc/self/maps. Re-read on every call (no cache: the guest
// heap commits/decommits pages at runtime). The callers are env-gated diagnostic
// scanners that step region-by-region, so the cost is a few thousand parses per
// full 4 GB walk - acceptable, and correct while other threads remap.
SIZE_T VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION* info, SIZE_T length) {
  if (!info || length < sizeof(*info)) return 0;
  const auto addr = reinterpret_cast<std::uintptr_t>(address);
  const std::uintptr_t page = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
  const std::uintptr_t page_addr = addr & ~(page - 1);

  std::FILE* f = std::fopen("/proc/self/maps", "re");
  if (!f) return 0;

  // maps is sorted by address: the first mapping that ends above the queried page
  // either contains it, or starts above it (the page is in a hole -> MEM_FREE).
  bool found = false;
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    std::uintptr_t lo = 0, hi = 0;
    char perms[8] = {};
    if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %7s", &lo, &hi, perms) != 3) continue;
    if (page_addr < lo) {
      // Hole before this mapping.
      *info = {};
      info->BaseAddress = reinterpret_cast<PVOID>(page_addr);
      info->RegionSize = lo - page_addr;
      info->State = MEM_FREE;
      info->Protect = PAGE_NOACCESS;
      found = true;
      break;
    }
    if (page_addr < hi) {
      const bool r = perms[0] == 'r', w = perms[1] == 'w', x = perms[2] == 'x';
      *info = {};
      // Report from the queried page to the mapping end (Win32 reports the run of
      // pages with identical attributes starting at the queried page).
      info->BaseAddress = reinterpret_cast<PVOID>(page_addr);
      info->AllocationBase = reinterpret_cast<PVOID>(lo);
      info->RegionSize = hi - page_addr;
      info->Type = perms[3] == 's' ? MEM_MAPPED : MEM_PRIVATE;
      if (!r && !w && !x) {
        info->State = MEM_RESERVE;
        info->Protect = PAGE_NOACCESS;
      } else {
        info->State = MEM_COMMIT;
        info->Protect = x ? (w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
                          : (w ? PAGE_READWRITE : PAGE_READONLY);
      }
      info->AllocationProtect = info->Protect;
      found = true;
      break;
    }
  }
  std::fclose(f);
  if (!found) {
    // Above the last mapping: free to the top of the user address space.
    *info = {};
    info->BaseAddress = reinterpret_cast<PVOID>(page_addr);
    info->RegionSize = (std::uintptr_t(1) << 47) > page_addr
                           ? (std::uintptr_t(1) << 47) - page_addr
                           : page;
    info->State = MEM_FREE;
    info->Protect = PAGE_NOACCESS;
  }
  return sizeof(*info);
}
#else
#error "VirtualQuery: no implementation for this platform yet (Horizon: svcQueryMemory)"
#endif

#endif  // !_WIN32
