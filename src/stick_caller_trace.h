// Dev tool: identify the recomp function that enumerates the Create/Edit-Player
// stick picker.
//
// The 23-stick set is computed, not stored (static + runtime scans negative —
// docs/nhl12-decomp-reference/editplayer-equipment-enumeration.md), so the only
// way left to the producing logic is the call site. The recomp compiles each
// guest PPC function into a real host C++ function that calls its callees
// directly, so the HOST call stack at a `stickN.big` open mirrors the guest call
// chain. We capture that stack (x64 unwinds via .pdata, frame-pointer-independent,
// so it works at -O2) and log each frame as an RVA into nhllegacy.exe. Offline,
// llvm-symbolizer maps the RVAs to `sub_XXXXXXXX`; reading that function's
// generated C++ reveals the enumeration loop + the test that gates the 23 shown.
//
// Gated by NHL_TRACE_STICK_OPEN; logs the first few stick opens then stops.

#pragma once

#include <cstdio>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nhllegacy {

#if defined(_WIN32)

// Append the host call stack (as RVAs into the main module) for one stick-open to
// `out_path`. Capped via a static counter so we record a handful of opens, not all.
inline void CaptureStickCaller(const char* stick_name, const char* out_path) {
  static int remaining = 8;       // record at most this many opens
  if (remaining <= 0) return;
  --remaining;

  void* frames[40] = {0};
  const USHORT n = RtlCaptureStackBackTrace(/*FramesToSkip=*/1, /*Capture=*/40,
                                            frames, /*BackTraceHash=*/nullptr);
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

  std::FILE* f = std::fopen(out_path, "a");
  if (!f) f = stderr;
  std::fprintf(f, "\n=== stick open: %s  (module base %p, %u frames) ===\n",
               stick_name, reinterpret_cast<void*>(base), unsigned(n));
  for (USHORT i = 0; i < n; ++i) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
    const uintptr_t rva = (a >= base) ? (a - base) : 0;
    // RVA is what llvm-symbolizer wants (addresses are non-ASLR-stable only as
    // RVAs); also print the absolute for cross-checking against the live base.
    std::fprintf(f, "  [%2u] rva=0x%08llX  abs=0x%016llX\n", unsigned(i),
                 static_cast<unsigned long long>(rva),
                 static_cast<unsigned long long>(a));
  }
  std::fprintf(f, "  (symbolize: llvm-symbolizer.exe --obj=nhllegacy.exe <rva...>)\n");
  if (f != stderr) std::fclose(f);
}

#endif  // _WIN32

}  // namespace nhllegacy
