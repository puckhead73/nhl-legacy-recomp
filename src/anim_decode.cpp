// See anim_decode.h. Milestone 0: host->guest call-harness validation.
// Milestone 1: boot-then-scan recon (RunAnimScan).

#include "anim_decode.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

// Pulls in the recompiled function declarations (DECLARE_REX_FUNC -> plain
// linkable `void sub_XXXX(PPCContext&, uint8_t* base)` symbols) and, transitively,
// rex/ppc/context.h (PPCContext). Same include the anim_capture hook uses.
#include "generated/default/nhllegacy_init.h"

namespace nhllegacy {
namespace {

// Guest memory is big-endian PPC; the recomp's REX_LOAD_U32 byteswaps on read.
// So to seed a field the guest will read as `v`, store `v` byteswapped.
inline void Poke32(uint8_t* base, uint32_t ga, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + ga, &v, 4);
}
inline uint32_t Peek32(uint8_t* base, uint32_t ga) {
  uint32_t v = 0;
  std::memcpy(&v, base + ga, 4);
  return __builtin_bswap32(v);
}

// Call a recompiled leaf function with up to three integer args (r3..r5) and
// return its result (r3). The two functions we exercise are pure leaves (no
// stwu / no bl), so r1 is unused — but we set a valid guest stack top anyway so
// this same helper extends to the non-leaf pipeline later.
using GuestFn = void (*)(PPCContext&, uint8_t*);
uint32_t CallGuest(uint8_t* base, uint32_t stack_top, GuestFn fn, uint32_t r3,
                   uint32_t r4 = 0, uint32_t r5 = 0) {
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  ctx.r1.u32 = stack_top;
  ctx.r3.u32 = r3;
  ctx.r4.u32 = r4;
  ctx.r5.u32 = r5;
  fn(ctx, base);
  return ctx.r3.u32;
}

struct Report {
  std::FILE* f = nullptr;
  int checks = 0;
  int failures = 0;

  void Line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (f) {
      std::fputs(buf, f);
      std::fputc('\n', f);
    }
    REXLOG_INFO("[anim-decode] {}", buf);
  }
  void Check(const char* name, uint32_t got, uint32_t want) {
    ++checks;
    const bool ok = (got == want);
    if (!ok) ++failures;
    Line("  %-10s got=%-6u want=%-6u  %s", name, got, want, ok ? "OK" : "**FAIL**");
  }
};

}  // namespace

bool RunAnimDecode(rex::memory::Memory* mem, const char* out_path) {
  if (!mem) {
    REXLOG_ERROR("[anim-decode] no Memory; cannot run");
    return false;
  }
  uint8_t* base = mem->virtual_membase();
  if (!base) {
    REXLOG_ERROR("[anim-decode] no membase; cannot run");
    return false;
  }

  Report rep;
  rep.f = (out_path && std::strcmp(out_path, "1") != 0 && std::strcmp(out_path, "on") != 0)
              ? std::fopen(out_path, "w")
              : nullptr;
  rep.Line("=== NHL_ANIM_DECODE — Milestone 0: host->guest call-harness proof ===");

  // --- Guest scratch (all in guest VA space so the recomp's relative loads work) ---
  const uint32_t stack_va = mem->SystemHeapAlloc(64 * 1024, 0x1000);
  const uint32_t asset_va = mem->SystemHeapAlloc(256, 0x20);
  const uint32_t dof_va = mem->SystemHeapAlloc(64, 0x20);   // DofTableDescBytes
  const uint32_t d_va = mem->SystemHeapAlloc(128, 0x20);    // descriptor D out
  if (!stack_va || !asset_va || !dof_va || !d_va) {
    rep.Line("SystemHeapAlloc failed (stack=%08X asset=%08X dof=%08X d=%08X)", stack_va,
             asset_va, dof_va, d_va);
    if (rep.f) std::fclose(rep.f);
    return false;
  }
  const uint32_t stack_top = (stack_va + 64 * 1024 - 0x100) & ~0xFu;
  rep.Line("guest scratch: stack=%08X(top %08X) asset=%08X dof=%08X D=%08X", stack_va,
           stack_top, asset_va, dof_va, d_va);

  // --- Build a synthetic DctAnimationAsset (runtime layout; see §6) ---
  // Zero the whole asset, then set the fields the leaf fns read. Values chosen so
  // both functions' outputs are trivially hand-predictable.
  std::memset(base + asset_va, 0, 256);
  Poke32(base, asset_va + 4, 4);    // count (NumQuats-ish)
  Poke32(base, asset_va + 16, 8);   // count
  Poke32(base, asset_va + 28, 10);  // count (also a 7330 term)
  Poke32(base, asset_va + 40, 2);   // 7330 term
  Poke32(base, asset_va + 44, 43);  // primary count (NumKeys-ish) — drives 3750
  Poke32(base, asset_va + 52, 1);   // 7330 term
  Poke32(base, asset_va + 76, dof_va);   // -> DofTableDescBytes buffer
  Poke32(base, asset_va + 88, 100);      // 7330 base size

  // DofTableDescBytes: N = [40]+[52]+[28] = 2+1+10 = 13 bytes, each 0x10
  // (high nibble = 1). 7330 accumulates ((hi_nibble + 4) * 2) per byte = 10 each.
  std::memset(base + dof_va, 0x10, 13);

  // --- Test 1: sub_82CE3750(D, asset) — pure descriptor-D builder ---
  rep.Line("[1] sub_82CE3750(D=%08X, asset=%08X) — descriptor builder", d_va, asset_va);
  std::memset(base + d_va, 0xCC, 128);  // poison so we see real writes
  CallGuest(base, stack_top, &sub_82CE3750, d_va, asset_va);
  // Hand-derived from recomp.73.cpp:18027 (see §6 derivation):
  rep.Check("D+0", Peek32(base, d_va + 0), 43);    // [asset+44]
  rep.Check("D+4", Peek32(base, d_va + 4), 5);     // 43>>3
  rep.Check("D+8", Peek32(base, d_va + 8), 3);     // 43&7
  rep.Check("D+12", Peek32(base, d_va + 12), 6);   // (43+7)>>3
  rep.Check("D+16", Peek32(base, d_va + 16), 4);   // [asset+4]
  rep.Check("D+20", Peek32(base, d_va + 20), 192); // 48*[asset+4]
  rep.Check("D+24", Peek32(base, d_va + 24), 4);
  rep.Check("D+28", Peek32(base, d_va + 28), 8);   // [asset+16]
  rep.Check("D+32", Peek32(base, d_va + 32), 384); // 48*[asset+16]
  rep.Check("D+36", Peek32(base, d_va + 36), 4);
  rep.Check("D+40", Peek32(base, d_va + 40), 3);   // (10+3)>>2
  rep.Check("D+44", Peek32(base, d_va + 44), 144); // 48*3
  rep.Check("D+48", Peek32(base, d_va + 48), 4);
  rep.Check("D+52", Peek32(base, d_va + 52), 10);  // max([asset+16],[asset+28])
  rep.Check("D+56", Peek32(base, d_va + 56), 480); // 10*48
  rep.Check("D+60", Peek32(base, d_va + 60), 4);

  // --- Test 2: sub_82CA7330(asset) — unpacked-size calculator (returns r3) ---
  rep.Line("[2] sub_82CA7330(asset=%08X) — size calc", asset_va);
  const uint32_t size = CallGuest(base, stack_top, &sub_82CA7330, asset_va);
  // [asset+88] + 13*((1+4)*2) + parity(N+12=25) + (N+12=25) = 100 + 130 + 1 + 25
  rep.Check("size", size, 256);

  rep.Line("=== %d checks, %d failures -> %s ===", rep.checks, rep.failures,
           rep.failures == 0 ? "PASS" : "FAIL");
  if (rep.f) std::fclose(rep.f);

  // Free scratch (best-effort; process exits right after anyway).
  mem->SystemHeapFree(d_va);
  mem->SystemHeapFree(dof_va);
  mem->SystemHeapFree(asset_va);
  mem->SystemHeapFree(stack_va);
  return rep.failures == 0;
}

// ===========================================================================
// Milestone 1 recon: boot-then-scan for resident loaded GD records.
// ===========================================================================
namespace {

// Guest image occupies [0x82000000, 0x83EA0000) (base + 0x1EA0000), plus a 0x92…
// mirror at +0x10000000. A clip-name string found OUTSIDE both ranges is a loaded
// anim.cba copy (heap), not the compiled-in string constant.
constexpr uint32_t kImageLo = 0x82000000u;
constexpr uint32_t kImageHi = 0x83EA0000u;
inline bool InImage(uint32_t va) {
  return (va >= kImageLo && va < kImageHi) ||
         (va >= kImageLo + 0x10000000u && va < kImageHi + 0x10000000u);
}

constexpr uint32_t kGdMagic = 0x030e6205u;     // marks every GD record/sub-record
constexpr uint32_t kHashDct = 0xf8ff03e3u;     // DctAnimationAsset
constexpr uint32_t kHashClip = 0x75e9ea49u;    // ClipControllerAsset
constexpr uint32_t kHashActor = 0xe08752b2u;   // ActorControllerAsset
constexpr uint32_t kHashSeq = 0x90ff09e7u;     // SequenceContainerAsset

inline uint32_t RdBE(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint32_t RdLE(const uint8_t* p) {
  return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}

const char* HashName(uint32_t h) {
  switch (h) {
    case kHashDct: return "DctAnimationAsset";
    case kHashClip: return "ClipControllerAsset";
    case kHashActor: return "ActorControllerAsset";
    case kHashSeq: return "SequenceContainerAsset";
    default: return "";
  }
}

void HexDump(std::FILE* f, const uint8_t* p, uint32_t guest_va, int n) {
  for (int i = 0; i < n; i += 16) {
    std::fprintf(f, "    %08X:", guest_va + i);
    for (int j = 0; j < 16 && i + j < n; ++j) std::fprintf(f, " %02X", p[i + j]);
    std::fprintf(f, "  ");
    for (int j = 0; j < 16 && i + j < n; ++j) {
      uint8_t c = p[i + j];
      std::fputc((c >= 0x20 && c < 0x7f) ? c : '.', f);
    }
    std::fputc('\n', f);
  }
}

}  // namespace

void RunAnimScan(const unsigned char* vbase, const char* out_path) {
  std::FILE* f = std::fopen(out_path, "w");
  if (!f) f = stderr;
  auto emit = [&](auto&&... a) {
    std::fprintf(f, a...);
    std::fprintf(stderr, a...);
  };
  emit("=== NHL_ANIM_DECODE=scan — Milestone 1 recon: resident GD records ===\n");

  // Histograms of type_hash, split by which byte order the magic matched (tells us
  // the loaded endianness). Also collect example addresses for the anim types.
  std::map<uint32_t, uint32_t> hist_be, hist_le;
  uint64_t magic_be = 0, magic_le = 0;
  std::vector<uint32_t> dct_be, dct_le, clip_be;  // example guest VAs
  std::vector<std::pair<uint32_t, size_t>> big_regions;  // (va, size) non-image >=1MB
  size_t scanned = 0;

  const uint8_t* p = vbase;
  const uint8_t* guest_end = vbase + (size_t)0x100000000ull;  // 4 GB guest space
  while (p < guest_end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
    auto* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
    size_t rsize = mbi.RegionSize;
    const bool scannable = mbi.State == MEM_COMMIT &&
                           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && rbase >= vbase;
    if (scannable && rsize >= 20) {
      scanned += rsize;
      // Format-agnostic anchor: record large non-image allocations. The ~28 MB
      // anim.cba bundle (or its sub-buffers) stands out even if its records keep
      // neither the GD magic nor plaintext names.
      const uint32_t rva = uint32_t(rbase - vbase);
      if (rsize >= (1u << 20) && !InImage(rva) && !InImage(rva + uint32_t(rsize) - 1)) {
        big_regions.emplace_back(rva, rsize);
      }
      // Scan for the 4-byte magic at every offset; the GD header is 4-aligned in
      // practice but byte-scan to be safe. type_hash lives at magic+16.
      const uint8_t* end = rbase + rsize - 20;  // need +16..+20 readable
      for (const uint8_t* q = rbase; q < end; ++q) {
        if (RdBE(q) == kGdMagic) {
          ++magic_be;
          uint32_t h = RdBE(q + 16);
          ++hist_be[h];
          uint32_t va = uint32_t(q - vbase);
          if (h == kHashDct && dct_be.size() < 8) dct_be.push_back(va);
          if (h == kHashClip && clip_be.size() < 8) clip_be.push_back(va);
        } else if (RdLE(q) == kGdMagic) {
          ++magic_le;
          uint32_t h = RdLE(q + 16);
          ++hist_le[h];
          uint32_t va = uint32_t(q - vbase);
          if (h == kHashDct && dct_le.size() < 8) dct_le.push_back(va);
        }
      }
    }
    const uint8_t* next = rbase + rsize;
    if (next <= p) break;
    p = next;
  }

  emit("\nscanned %.1f MB committed; magic hits: BE-order=%llu LE-order=%llu\n",
       double(scanned) / (1024.0 * 1024.0), (unsigned long long)magic_be,
       (unsigned long long)magic_le);
  emit("(BE-order = loader byteswapped records to native PPC; LE-order = loaded "
       "verbatim from the little-endian .cba)\n");

  // Large non-image allocations — where a 28 MB bundle would land regardless of
  // its internal format. Sort by size desc, show top 25.
  std::sort(big_regions.begin(), big_regions.end(),
            [](auto& a, auto& b) { return a.second > b.second; });
  emit("\n--- large non-image committed regions (>=1MB), top 25 of %zu ---\n",
       big_regions.size());
  for (size_t i = 0; i < big_regions.size() && i < 25; ++i) {
    emit("  %08X  %7.2f MB\n", big_regions[i].first,
         double(big_regions[i].second) / (1024.0 * 1024.0));
  }

  auto dump_hist = [&](const char* tag, std::map<uint32_t, uint32_t>& h) {
    if (h.empty()) return;
    // Sort by count desc.
    std::vector<std::pair<uint32_t, uint32_t>> v(h.begin(), h.end());
    std::sort(v.begin(), v.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    emit("\n--- top type_hashes (%s-order magic), %zu distinct ---\n", tag, v.size());
    int n = 0;
    for (auto& [hash, cnt] : v) {
      if (n++ >= 30) break;
      emit("  %08X  x%-6u %s\n", hash, cnt, HashName(hash));
    }
  };
  dump_hist("BE", hist_be);
  dump_hist("LE", hist_le);

  // Hexdump example DctAnimationAsset records so we can see the runtime layout
  // (the on-disk +16=type_hash vs the decode fns reading +16 as a count).
  auto dump_examples = [&](const char* tag, std::vector<uint32_t>& vas) {
    for (uint32_t va : vas) {
      emit("\n[%s] DctAnimationAsset record @ guest %08X (record start = magic):\n", tag, va);
      HexDump(f, vbase + va, va, 96);
    }
  };
  dump_examples("BE", dct_be);
  dump_examples("LE", dct_le);
  for (uint32_t va : clip_be) {
    emit("\n[BE] ClipControllerAsset record @ guest %08X:\n", va);
    HexDump(f, vbase + va, va, 96);
  }

  // Locate clip-name strings (authored ASCII). The loaded form keeps the name in
  // plaintext but drops the GD magic (deserialized into a runtime object) — so the
  // object that OWNS the name is what we need (its fields lead Anims ->
  // DctAnimationAsset). Collect HEAP hits of the full name for the follow-up below.
  static const char* kNames[] = {"CHKP04_SP_SHFBF_3a", "CHKP04", "_SHFBF"};
  emit("\n--- clip-name string search ---\n");
  uint32_t first_heap_name = 0;
  for (const char* name : kNames) {
    const size_t nlen = std::strlen(name);
    const bool is_full = (std::strcmp(name, "CHKP04_SP_SHFBF_3a") == 0);
    int hits = 0;
    const uint8_t* q = vbase;
    while (q < guest_end && hits < 8) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(q, &mbi, sizeof(mbi))) break;
      auto* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
      size_t rsize = mbi.RegionSize;
      if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
          rbase >= vbase && rsize >= nlen) {
        const uint8_t* e = rbase + rsize - nlen;
        for (const uint8_t* r = rbase; r < e && hits < 8; ++r) {
          if (std::memcmp(r, name, nlen) == 0) {
            uint32_t va = uint32_t(r - vbase);
            const bool heap = !InImage(va);
            emit("  \"%s\" @ guest %08X  [%s]\n", name, va, heap ? "HEAP" : "IMG");
            if (is_full && heap && !first_heap_name) first_heap_name = va;
            ++hits;
          }
        }
      }
      const uint8_t* next = rbase + rsize;
      if (next <= q) break;
      q = next;
    }
    if (hits == 0) emit("  \"%s\" — NOT FOUND\n", name);
  }

  // Follow-up: dump the runtime object around the loaded name, and find what points
  // to it (the owning ClipControllerAsset). Guest RAM is mirror-aliased across the
  // 8 high-nibble views (step 0x20000000); a pointer to the name could use any
  // alias, so search for all 8 forms of the same physical offset.
  if (first_heap_name) {
    emit("\n--- runtime object around loaded name @ %08X ---\n", first_heap_name);
    uint32_t lo = first_heap_name >= 128 ? first_heap_name - 128 : 0;
    HexDump(f, vbase + lo, lo, 128 + 256);  // -128 .. +256 around the name

    const uint32_t phys = first_heap_name & 0x1FFFFFFFu;
    uint32_t aliases[8];
    for (int k = 0; k < 8; ++k) aliases[k] = phys | (uint32_t(k) << 29);
    emit("\n--- pointers to the name (any mirror alias of phys %08X) ---\n", phys);
    int ptr_hits = 0;
    const uint8_t* q = vbase;
    while (q < guest_end && ptr_hits < 24) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(q, &mbi, sizeof(mbi))) break;
      auto* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
      size_t rsize = mbi.RegionSize;
      if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
          rbase >= vbase && rsize >= 4) {
        const uint8_t* e = rbase + rsize - 4;
        for (const uint8_t* r = rbase; r < e && ptr_hits < 24; r += 4) {
          const uint32_t val = RdBE(r);
          bool match = false;
          for (uint32_t a : aliases)
            if (val == a) { match = true; break; }
          if (match) {
            uint32_t va = uint32_t(r - vbase);
            emit("\n  ptr@ %08X [%s] -> name; surrounding object:\n", va,
                 InImage(va) ? "IMG" : "HEAP");
            uint32_t olo = va >= 64 ? va - 64 : 0;
            HexDump(f, vbase + olo, olo, 64 + 128);  // -64 .. +128 around the pointer
            ++ptr_hits;
          }
        }
      }
      const uint8_t* next = rbase + rsize;
      if (next <= q) break;
      q = next;
    }
    if (ptr_hits == 0) emit("  (no pointer to the name found — name may be inline in "
                            "its object; inspect the -128..+256 dump above)\n");
  }

  emit("\n=== scan complete ===\n");
  if (f != stderr) std::fclose(f);
}

namespace {
// First HEAP (non-image) occurrence of the probe clip name, or 0 if none. anim.cba
// being loaded in-place puts the authored name string in heap; the compiled-in copy
// lives in the image and is ignored.
uint32_t FindClipNameHeap(const uint8_t* vbase) {
  static const char kProbe[] = "CHKP04_SP_SHFBF_3a";
  constexpr size_t kLen = sizeof(kProbe) - 1;
  const uint8_t* guest_end = vbase + (size_t)0x100000000ull;
  const uint8_t* q = vbase;
  while (q < guest_end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(q, &mbi, sizeof(mbi))) break;
    auto* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
    size_t rsize = mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
        rbase >= vbase && rsize >= kLen) {
      const uint8_t* e = rbase + rsize - kLen;
      for (const uint8_t* r = rbase; r < e; ++r) {
        if (r[0] == 'C' && std::memcmp(r, kProbe, kLen) == 0) {
          uint32_t va = uint32_t(r - vbase);
          if (!InImage(va)) return va;
        }
      }
    }
    const uint8_t* next = rbase + rsize;
    if (next <= q) break;
    q = next;
  }
  return 0;
}
}  // namespace

bool WaitForAnimData(const unsigned char* vbase, unsigned timeout_ms) {
  const unsigned step = 3000;
  for (unsigned waited = 0; waited < timeout_ms; waited += step) {
    if (uint32_t va = FindClipNameHeap(vbase)) {
      std::fprintf(stderr, "[anim-scan] anim.cba resident (clip name in heap @ %08X)\n", va);
      return true;
    }
    ::Sleep(step);
  }
  return false;
}

}  // namespace nhllegacy
