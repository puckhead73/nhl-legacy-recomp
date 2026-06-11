// Stage B of the Overall-rating-formula hunt: locate EA's per-position
// card-overall weight arrays in the live guest image and dump their f32
// values.
//
// Architecture (confirmed in nhl-database-studio/docs/eboot/overall-formula.md
// against the PS3 EBOOT; the 360 default.xex shares the engine): the Overall
// is a per-position weighted sum, with three named tuning globals in .rodata —
// gCardOverallWeights_{Forwards,Defense,Goalie} under category "HUT/CardOverall".
// The PS3 VAs do NOT transfer to the 360 image, and a raw `strings` pass over
// the LZX-compressed default.xex finds nothing — but once the recomp host boots,
// the runtime maps the *decompressed* .rdata/.data into guest memory, so the
// names and their default weight floats are directly scannable here.
//
// This scanner runs entirely on the host pointer to the mapped guest image
// (no Memory type dependency). It does three things:
//   1. String search for the anchor names; reports each guest VA.
//   2. Name-pointer xref: finds the registration record that points at each
//      name string, and from the neighbouring dwords recovers the (value
//      pointer, count) so it can follow the pointer and dump the actual
//      weight floats — exactly the "extract (name -> array ptr, count)" step
//      the eboot doc leaves as future work.
//   3. A normalised-float-array fingerprint scan (port of tdb-eboot's
//      weights.rs) over the whole image: contiguous runs of big-endian f32
//      that are finite, in ~[0,0.6], >=3 distinct, summing to ~1.0 — the
//      shape of an EA card-overall weight array — reported with their VAs.
//
// PPC is big-endian, so all multi-byte image values are read byte-swapped.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

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

inline uint32_t be_u32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
         uint32_t(p[3]);
}

inline float be_f32(const uint8_t* p) {
  uint32_t v = be_u32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// First occurrence of `needle` (NUL-terminated) in [hay, hay+n); SIZE_MAX if
// absent. (memmem is POSIX-only; MSVC lacks it.)
inline size_t find_bytes(const uint8_t* hay, size_t n, const char* needle) {
  const size_t m = std::strlen(needle);
  if (m == 0 || m > n) return SIZE_MAX;
  for (size_t i = 0; i + m <= n; ++i) {
    if (hay[i] == uint8_t(needle[0]) && std::memcmp(hay + i, needle, m) == 0) {
      return i;
    }
  }
  return SIZE_MAX;
}

// Dump `count` big-endian floats starting at guest VA `va` (if it lies inside
// the mapped image) to `out`.
inline void dump_floats_at(std::FILE* out, const uint8_t* img, uint32_t base_va,
                           uint32_t img_size, uint32_t va, int count,
                           const char* tag) {
  if (va < base_va || va + uint32_t(count) * 4u > base_va + img_size) {
    std::fprintf(out, "    %s: VA %08X out of image range\n", tag, va);
    return;
  }
  const uint8_t* p = img + (va - base_va);
  std::fprintf(out, "    %s @ %08X [%d floats]:\n      ", tag, va, count);
  double sum = 0.0;
  for (int i = 0; i < count; ++i) {
    float f = be_f32(p + i * 4);
    sum += f;
    std::fprintf(out, "%.5f ", f);
    if ((i % 8) == 7) std::fprintf(out, "\n      ");
  }
  std::fprintf(out, "\n    %s: sum=%.5f\n", tag, sum);
}

// Main entry. `img` is the host pointer to guest VA `base_va`; `img_size` the
// number of mapped bytes. Writes a human-readable report to `out_path` and
// mirrors it to stderr.
inline void DumpOverallWeights(const uint8_t* img, uint32_t base_va,
                               uint32_t img_size, const char* out_path) {
  std::FILE* out = std::fopen(out_path, "w");
  if (!out) out = stderr;
  auto emit = [&](auto&&... a) {
    std::fprintf(out, a...);
    std::fprintf(stderr, a...);
  };

  emit("=== Overall-weight scan: image %08X .. %08X (%u bytes) ===\n", base_va,
       base_va + img_size, img_size);

  // 1) Anchor strings, longest/most-specific first.
  static const char* kAnchors[] = {
      "gCardOverallWeights_Forwards", "gCardOverallWeights_Defense",
      "gCardOverallWeights_Goalie",   "gCardOverallWeights",
      "HUT/CardOverall",              "GetProjectedOverall",
      "CardOverall",                  "CardWeight",
  };
  std::vector<std::pair<std::string, uint32_t>> found;  // (name, VA)
  for (const char* a : kAnchors) {
    // Find every occurrence (names + their category prefixes can recur).
    size_t from = 0;
    bool any = false;
    while (from < img_size) {
      size_t off = find_bytes(img + from, img_size - from, a);
      if (off == SIZE_MAX) break;
      uint32_t va = base_va + uint32_t(from + off);
      emit("[string] %-30s @ %08X\n", a, va);
      found.emplace_back(a, va);
      any = true;
      from += off + 1;
    }
    if (!any) emit("[string] %-30s : NOT FOUND\n", a);
  }

  if (found.empty()) {
    emit("\nNo anchor strings present in the mapped image. Either the image\n"
         "data is not mapped at this stage, or the names differ on 360. The\n"
         "fingerprint scan below still runs.\n");
  }

  // 2) Name-pointer xref -> registration record -> (value ptr, count) -> floats.
  // Walk every 4-byte-aligned dword; when it equals a found name VA, the
  // surrounding dwords are the tweak's registration record. EA's record packs
  // the name pointer with a pointer to the float array and an element count;
  // we scan a small window around the hit for a plausible image-VA dword
  // (the value pointer) and a small integer (the count, typically 20/25/26).
  emit("\n=== name-pointer xrefs (registration records) ===\n");
  for (const auto& [name, name_va] : found) {
    bool xref_any = false;
    for (uint32_t off = 0; off + 4 <= img_size; off += 4) {
      if (be_u32(img + off) != name_va) continue;
      uint32_t rec_va = base_va + off;
      xref_any = true;
      emit("  ptr->\"%s\" (%08X) at record VA %08X; window:\n", name.c_str(),
           name_va, rec_va);
      // Dump 6 dwords before .. 6 dwords after as (hex / asVA? / asInt / asF32).
      for (int d = -6; d <= 6; ++d) {
        long w_off = long(off) + d * 4;
        if (w_off < 0 || uint32_t(w_off) + 4 > img_size) continue;
        uint32_t v = be_u32(img + w_off);
        bool is_va = (v >= base_va && v < base_va + img_size);
        emit("    [%+d] %08X = %08X  %s int=%-6u f32=%.5f\n", d * 4,
             uint32_t(base_va + w_off), v, is_va ? "VA " : "   ", v, be_f32(img + w_off));
      }
      // Heuristic recovery: nearest preceding/following dword that is a valid
      // image VA is the candidate value pointer; pair with a nearby small int
      // as the count and dump that many floats.
      for (int d = -6; d <= 6; ++d) {
        long w_off = long(off) + d * 4;
        if (d == 0 || w_off < 0 || uint32_t(w_off) + 4 > img_size) continue;
        uint32_t cand_ptr = be_u32(img + w_off);
        if (cand_ptr < base_va || cand_ptr >= base_va + img_size) continue;
        // Look for a plausible count (8..40) in the same window.
        for (int e = -6; e <= 6; ++e) {
          long c_off = long(off) + e * 4;
          if (c_off < 0 || uint32_t(c_off) + 4 > img_size) continue;
          uint32_t cnt = be_u32(img + c_off);
          if (cnt < 8 || cnt > 40) continue;
          dump_floats_at(out, img, base_va, img_size, cand_ptr, int(cnt),
                         "candidate-array");
          std::fprintf(stderr, "    candidate-array @ %08X [%u] (via record %08X)\n",
                       cand_ptr, cnt, rec_va);
          break;
        }
        break;  // first VA candidate only
      }
    }
    if (!xref_any) {
      emit("  \"%s\": no 32-bit pointer to it found (registered via static\n"
           "       ctor / computed pointer — fall back to fingerprint scan)\n",
           name.c_str());
    }
  }

  // 3) Normalised-float-array fingerprint scan over the whole image.
  // EA card-overall arrays: finite, in ~[0,0.6], >=3 distinct, sum ~ 1.0.
  emit("\n=== normalised f32-array fingerprint (sum~1.0, vals in [-0.02,0.6]) ===\n");
  const int kLens[] = {20, 24, 25, 26};
  const double kMin = -0.02, kMax = 0.60, kTargetSum = 1.0, kSumTol = 0.05;
  int reported = 0;
  for (int len : kLens) {
    for (uint32_t off = 0; off + uint32_t(len) * 4 <= img_size; off += 4) {
      const uint8_t* p = img + off;
      double sum = 0.0;
      bool ok = true;
      float first = be_f32(p);
      bool all_same = true;
      int distinct_guard = 0;
      float seen[4] = {first, 0, 0, 0};
      int nseen = 1;
      for (int i = 0; i < len; ++i) {
        float f = be_f32(p + i * 4);
        if (!std::isfinite(f) || f < kMin || f > kMax) { ok = false; break; }
        sum += f;
        if (f != first) all_same = false;
        bool known = false;
        for (int s = 0; s < nseen; ++s) if (seen[s] == f) { known = true; break; }
        if (!known && nseen < 4) seen[nseen++] = f;
        (void)distinct_guard;
      }
      if (!ok || all_same || nseen < 3) continue;
      if (std::fabs(sum - kTargetSum) > kSumTol) continue;
      uint32_t va = base_va + off;
      std::fprintf(out, "  [len %d] @ %08X sum=%.4f:\n    ", len, va, sum);
      std::fprintf(stderr, "  fingerprint [len %d] @ %08X sum=%.4f\n", len, va, sum);
      for (int i = 0; i < len; ++i) {
        std::fprintf(out, "%.5f ", be_f32(p + i * 4));
        if ((i % 8) == 7) std::fprintf(out, "\n    ");
      }
      std::fprintf(out, "\n");
      if (++reported >= 200) {
        emit("  ...(capped at 200 fingerprint hits)\n");
        goto done;
      }
    }
  }
done:
  emit("\n=== scan complete: %zu anchor hit(s), %d fingerprint hit(s) ===\n",
       found.size(), reported);
  if (out != stderr) std::fclose(out);
}

// ---------------------------------------------------------------------------
// Runtime (post-registration) scan.
//
// The static scan above proves the name strings exist but can't reach the
// float arrays: EA's tuning system has no static defaults table — the
// name->value link is built at runtime by the tweak registration (confirmed
// by nhl-database-studio/docs/eboot/tuning-registration-pool.md). So once the
// guest has booted far enough to register its tweaks, a 32-bit pointer to each
// name string materialises in the runtime registry; the neighbouring record
// fields carry the value pointer + element count. This scanner runs after that
// registration, walks every COMMITTED guest page (page-safe via VirtualQuery),
// finds those name pointers, and follows them to the weight floats.
//
// `vbase` is the host pointer to guest VA 0 (Memory::virtual_membase). Guest
// VA `a` -> host `vbase + a`; host `h` -> guest VA `(uint32_t)(h - vbase)`.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
// True if [vbase+gva, +len) is committed and readable.
inline bool rt_readable(const uint8_t* vbase, uint32_t gva, size_t len) {
  const uint8_t* p = vbase + gva;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const uint8_t* region_end =
      static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  return p + len <= region_end;
}

inline bool rt_looks_like_weights(const uint8_t* vbase, uint32_t gva, int probe) {
  if (!rt_readable(vbase, gva, size_t(probe) * 4)) return false;
  const uint8_t* p = vbase + gva;
  bool any_nonzero = false;
  for (int i = 0; i < probe; ++i) {
    float f = be_f32(p + i * 4);
    if (!std::isfinite(f) || f < -1.0f || f > 100.0f) return false;
    if (f != 0.0f) any_nonzero = true;
  }
  return any_nonzero;
}

inline void DumpOverallWeightsRuntime(const uint8_t* vbase, const char* out_path) {
  std::FILE* out = std::fopen(out_path, "w");
  if (!out) out = stderr;
  auto emit = [&](auto&&... a) {
    std::fprintf(out, a...);
    std::fprintf(stderr, a...);
  };

  constexpr uint32_t IMG_BASE = 0x82000000u;
  constexpr uint32_t IMG_SIZE = 0x1EA0000u;
  emit("=== Overall-weight RUNTIME scan (post-registration) ===\n");

  (void)IMG_SIZE;
  // The image is fixed-mapped at the same VAs every boot (static recomp), so
  // target the anchor name VAs proven by the static scan directly — verifying
  // each with a page-safe string compare rather than gating on the whole image
  // being one committed region (it isn't: .text/.rodata/.data differ).
  struct Anchor { const char* name; uint32_t va; };
  static const Anchor kAnchors[] = {
      {"gCardOverallWeights_Goalie", 0x82104FA8u},
      {"gCardOverallWeights_Forwards", 0x82104FC4u},
      {"gCardOverallWeights_Defense", 0x82104FE4u},
      {"HUT/CardOverall", 0x82104F98u},
  };
  std::vector<std::pair<std::string, uint32_t>> names;  // (name, VA)
  for (const auto& a : kAnchors) {
    const size_t len = std::strlen(a.name);
    const bool verified = rt_readable(vbase, a.va, len + 1) &&
                          std::memcmp(vbase + a.va, a.name, len) == 0;
    emit("[name] %-30s @ %08X %s\n", a.name, a.va,
         verified ? "(verified)" : "(UNVERIFIED at scan time)");
    names.emplace_back(a.name, a.va);
  }

  // Walk every committed guest page, scanning 4-aligned dwords for a big-endian
  // pointer to any name VA. (Single pass; checks each dword against all names.)
  emit("\n=== runtime pointers to the name strings ===\n");
  const uint8_t* p = vbase;
  const uint8_t* guest_end = vbase + (size_t)0x100000000ull;  // 4 GB guest space
  size_t scanned_bytes = 0;
  int total_hits = 0;
  while (p < guest_end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
    auto* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
    size_t rsize = mbi.RegionSize;
    const bool scannable = mbi.State == MEM_COMMIT &&
                           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (scannable && rbase >= vbase) {
      scanned_bytes += rsize;
      for (size_t off = 0; off + 4 <= rsize; off += 4) {
        uint32_t v = be_u32(rbase + off);
        for (const auto& [nm, nva] : names) {
          if (v != nva) continue;
          uint32_t rec_va = uint32_t((rbase + off) - vbase);
          emit("\n  ptr->\"%s\" (%08X) at guest VA %08X:\n", nm.c_str(), nva,
               rec_va);
          if (++total_hits > 64) { emit("  ...(hit cap)\n"); goto after_scan; }
          // Dump the surrounding record (16 dwords each side).
          for (int d = -16; d <= 16; ++d) {
            long w = long(off) + d * 4;
            if (w < 0 || size_t(w) + 4 > rsize) continue;
            uint32_t wv = be_u32(rbase + w);
            uint32_t wva = uint32_t((rbase + w) - vbase);
            bool is_ptr = rt_readable(vbase, wv, 4);
            emit("    [%+4d] gva=%08X = %08X %s int=%-8u f32=%.5f\n", d * 4, wva,
                 wv, is_ptr ? "PTR" : "   ", wv, be_f32(rbase + w));
          }
          // For the three weight records (skip the HUT category node), dump
          // EVERY readable pointer field's target as 20- and 25-float arrays so
          // the actual weight storage is visible regardless of which field of
          // the EA tweak-node holds the value pointer. A normalised weight array
          // is finite, in ~[0,0.7], and sums to ~1.0 (flagged WEIGHTS?).
          const bool is_weight_rec = nm.rfind("gCardOverallWeights_", 0) == 0;
          if (is_weight_rec) {
            for (int d = -12; d <= 44; ++d) {
              long w = long(off) + d * 4;
              if (w < 0 || size_t(w) + 4 > rsize) continue;
              uint32_t cand = be_u32(rbase + w);
              if (!rt_readable(vbase, cand, 25 * 4)) continue;
              const uint8_t* vp = vbase + cand;
              for (int len : {20, 25}) {
                double sum = 0.0;
                bool finite_norm = true;
                int distinct = 0;
                float prev = 1e30f;
                for (int i = 0; i < len; ++i) {
                  float f = be_f32(vp + i * 4);
                  sum += f;
                  if (!std::isfinite(f) || f < -0.05f || f > 0.7f) finite_norm = false;
                  if (f != prev) { ++distinct; prev = f; }
                }
                const bool looks = finite_norm && distinct >= 3 &&
                                   sum > 0.85 && sum < 1.15;
                emit("    [field %+d] -> %08X len=%d sum=%.5f %s:\n      ", d * 4,
                     cand, len, sum, looks ? "<== WEIGHTS?" : "");
                for (int i = 0; i < len; ++i) {
                  emit("%.5f ", be_f32(vp + i * 4));
                  if ((i % 8) == 7) emit("\n      ");
                }
                emit("\n");
              }
            }
          }
        }
      }
    }
    p = rbase + rsize;
    if (p <= rbase) break;
  }
after_scan:
  emit("\n=== runtime scan complete: %d name-pointer hit(s), %.1f MB committed "
       "scanned ===\n",
       total_hits, double(scanned_bytes) / (1024.0 * 1024.0));
  if (out != stderr) std::fclose(out);
}
#endif  // _WIN32

}  // namespace nhllegacy
