// Dev tool: locate the Create/Edit-Player stick-picker's item list in live guest
// memory.
//
// Findings to date (docs/nhl12-decomp-reference/editplayer-equipment-enumeration.md):
// the picker shows a FIXED set of 23 stick model IDs out of 106 on-disk assets,
// and that list is NOT a static byte array anywhere on disk (the 32 MB image,
// the create-player .big files, equipatrib/renddb/nhlng.db were all searched —
// negative). So the list is built at runtime. This scanner runs WHILE the stick
// picker is on-screen (triggered from the overlay's Developer Tools section) and
// walks committed guest pages for a compact span that is dense in *distinct*
// shown stick IDs — the materialised list. The distinct-count filter is what
// kills the constant-run / ascending-counter noise that defeated the static scan.
//
// Mirrors DumpOverallWeightsRuntime's committed-page walk. PPC is big-endian, so
// u16/u32 interpretations are byte-swapped.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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

// The 23 stick model IDs the picker currently shows (capture nhllegacy_055.log,
// user-confirmed count). Any other selectable-list would still be dense in this
// set, so it serves as the fingerprint.
inline const std::vector<int>& sls_shown_ids() {
  static const std::vector<int> ids = {35, 36, 82,  84,  85,  86,  87,  93,
                                       94, 95, 98,  99,  102, 103, 104, 105,
                                       106, 107, 108, 109, 110, 111, 112};
  return ids;
}

inline bool sls_in_set(const bool (&member)[256], int v) {
  return v >= 0 && v < 256 && member[v];
}

// Scan one committed region under `interp` element width (1/2/4 bytes, big-endian)
// for windows of `win` consecutive elements containing >= `min_distinct` distinct
// shown IDs. Emits each hit (VA, distinct count, contaminants, values) to `out`.
inline int sls_scan_region(std::FILE* out, const uint8_t* vbase,
                           const uint8_t* rbase, size_t rsize, int width,
                           const bool (&member)[256], int win, int min_distinct,
                           int& budget) {
  int hits = 0;
  const size_t elems = rsize / size_t(width);
  if (elems < size_t(win)) return 0;
  auto read = [&](size_t e) -> int {
    const uint8_t* p = rbase + e * size_t(width);
    uint32_t v = 0;
    for (int b = 0; b < width; ++b) v = (v << 8) | p[b];  // big-endian
    return int(v);
  };
  // Slide a window; cheap recompute (win is small, ~32).
  size_t e = 0;
  while (e + size_t(win) <= elems) {
    bool seen[256] = {false};
    int distinct = 0, contaminants = 0;
    for (int k = 0; k < win; ++k) {
      int v = read(e + size_t(k));
      if (sls_in_set(member, v)) {
        if (!seen[v]) { seen[v] = true; ++distinct; }
      } else if (v >= 1 && v <= 127) {
        ++contaminants;  // a valid-looking stick id that's NOT in the shown set
      }
    }
    if (distinct >= min_distinct) {
      uint32_t va = uint32_t((rbase + e * size_t(width)) - vbase);
      std::fprintf(out,
                   "  [u%d win=%d] VA %08X  distinct=%d/23 contaminants=%d:\n   ",
                   width * 8, win, va, distinct, contaminants);
      for (int k = 0; k < win; ++k) std::fprintf(out, " %d", read(e + size_t(k)));
      std::fprintf(out, "\n");
      ++hits;
      if (--budget <= 0) return hits;
      e += size_t(win);  // skip past this window to avoid flooding on overlap
    } else {
      ++e;
    }
  }
  return hits;
}

// Walk every committed guest page and report dense-distinct windows in u8/u16/u32.
// `vbase` is the host pointer to guest VA 0 (Memory::virtual_membase).
inline void ScanStickList(const uint8_t* vbase, const char* out_path) {
  std::FILE* out = std::fopen(out_path, "w");
  if (!out) out = stderr;
  auto emit = [&](auto&&... a) {
    std::fprintf(out, a...);
    std::fprintf(stderr, a...);
  };

  bool member[256] = {false};
  for (int id : sls_shown_ids()) member[id] = true;

  emit("=== stick-list scan: dense windows of the 23 shown IDs ===\n");
  emit("=== shown set:");
  for (int id : sls_shown_ids()) emit(" %d", id);
  emit("\n");

  // A real materialised list of 23 entries should show >=16 distinct in a 32-wide
  // window even if interleaved with a brand/field byte. Lower min_distinct catches
  // sparser layouts; the contaminants count helps rank true hits.
  constexpr int kWin = 32;
  constexpr int kMinDistinct = 16;

  const uint8_t* p = vbase;
  const uint8_t* guest_end = vbase + (size_t)0x100000000ull;  // 4 GB guest space
  size_t scanned = 0;
  int total = 0;
  int budget = 200;  // hard cap on reported windows
  while (p < guest_end && budget > 0) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
    auto* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
    size_t rsize = mbi.RegionSize;
    const bool scannable = mbi.State == MEM_COMMIT &&
                           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (scannable && rbase >= vbase) {
      scanned += rsize;
      for (int width : {1, 2, 4}) {
        total += sls_scan_region(out, vbase, rbase, rsize, width, member, kWin,
                                 kMinDistinct, budget);
        if (budget <= 0) break;
      }
    }
    p = rbase + rsize;
    if (p <= rbase) break;
  }
  emit("\n=== stick-list scan complete: %d dense window(s), %.1f MB scanned ===\n",
       total, double(scanned) / (1024.0 * 1024.0));
  if (out != stderr) std::fclose(out);
}

#endif  // _WIN32

}  // namespace nhllegacy
