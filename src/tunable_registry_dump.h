// Engine-tunable registry enumerator.
//
// Goal: produce the *complete* inventory of every `gXxx` engine tunable the
// game exposes — physics, skating, collision, animation, AI, fighting,
// goalie, rules — by name, category path, and (best-effort) current value.
//
// Background (nhl-database-studio/docs/eboot/tuning-registration-pool.md):
// EA's in-engine "tweak editor" registers its schema from a packed,
// variable-length record stream baked into `.rodata`. Each record is:
//
//   record   ::= [category]? name [default|label]? terminator
//   category ::= ASCII string containing '/'      ("AI/Shooting/Desperation")
//   name     ::= ASCII matching g[A-Z]\w*          ("gGoalieMassMultiplier")
//   default  ::= one 4-byte word (IEEE-754 BE float OR i32 BE)  -- sparse
//   label    ::= human-readable string             ("Num HIK Lookahead frames")
//   terminator ::= 4 zero bytes at a 4-byte boundary
//
// All strings NUL-terminated and 4-byte aligned. Category propagates
// statefully to subsequent names until replaced. This is a faithful C++ port
// of nhl-database-studio/crates/tdb-eboot/src/tuning_pool.rs, with one crucial
// difference for the recomp: that Rust parser ran on the *static* PS3 EBOOT at
// a known file offset (0x01c06780). We don't know our 360 default.xex offset
// and the names are LZX-compressed in the file — but once the recomp boots,
// the decompressed `.rodata`/`.data` is mapped into guest memory, so we locate
// the pool(s) here by record DENSITY rather than a hard-coded address, and can
// additionally read *live* values from the runtime tweak registry (which the
// static EBOOT scan could not).
//
// PPC is big-endian: all multi-byte image reads go through be_u32/be_f32.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "overall_weights_dump.h"  // be_u32, be_f32, rt_readable (+ <windows.h>)

namespace nhllegacy {

// ---------------------------------------------------------------------------
// Token classification (port of tuning_pool.rs).
// ---------------------------------------------------------------------------

inline bool tr_is_printable(uint8_t b) { return b >= 0x20 && b <= 0x7e; }

// EA convention: lowercase 'g', then an uppercase letter, then identifier
// chars (alnum / '_').
inline bool tr_looks_like_name(const char* s, size_t n) {
  if (n < 2) return false;
  if (s[0] != 'g') return false;
  if (!(s[1] >= 'A' && s[1] <= 'Z')) return false;
  for (size_t i = 2; i < n; ++i) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

inline bool tr_contains_slash(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (s[i] == '/') return true;
  return false;
}

struct TunableRec {
  std::string category;            // empty if none seen yet
  std::string name;                // the gXxx programmer name
  std::string label;               // empty if none
  bool has_default = false;        // an inline 4-byte default was present
  uint32_t default_bits = 0;       // raw big-endian-decoded value
  uint32_t name_va = 0;            // guest VA of the name string
  uint32_t default_va = 0;         // guest VA of the 4 default bytes (patch site)
  float default_as_f32() const {
    float f;
    std::memcpy(&f, &default_bits, 4);
    return f;
  }
  int32_t default_as_i32() const { return int32_t(default_bits); }
};

// ---------------------------------------------------------------------------
// Pool enumeration with density-gated run detection.
//
// We walk the whole mapped image word-by-word. A "run" is a contiguous stretch
// obeying the pool grammar (strings + occasional value words + short zero
// terminators). A run is broken by a stretch of non-grammar bytes (binary that
// is neither a >=2-char printable string nor a short zero gap). Only runs with
// >= kMinNamesPerRun named records AND at least one category are emitted —
// this isolates the registration pool(s) (incl. the separate HUT pool) and
// rejects scattered code-literal strings elsewhere in .rodata.
// ---------------------------------------------------------------------------

inline std::vector<TunableRec> EnumerateTunables(const uint8_t* img,
                                                 uint32_t base_va,
                                                 uint32_t img_size) {
  constexpr int kMinNamesPerRun = 8;  // a real pool has hundreds
  constexpr int kMaxGapWords = 8;     // tolerate sparse value words
  constexpr int kMaxZeroWords = 16;   // tolerate padding, break on big zero fill
  constexpr uint32_t kMaxStr = 512;   // defensive per-string scan cap

  std::vector<TunableRec> out;
  std::vector<TunableRec> run;
  std::optional<TunableRec> pending;
  std::string current_category;
  int run_names = 0;
  bool run_has_cat = false;
  int gap = 0, zero_run = 0;

  auto flush_pending = [&]() {
    if (pending) {
      run.push_back(*pending);
      pending.reset();
    }
  };
  auto end_run = [&]() {
    flush_pending();
    if (run_names >= kMinNamesPerRun && run_has_cat) {
      for (auto& r : run) out.push_back(std::move(r));
    }
    run.clear();
    run_names = 0;
    run_has_cat = false;
    current_category.clear();
    gap = 0;
    zero_run = 0;
  };

  uint32_t off = 0;
  while (off + 4 <= img_size) {
    uint32_t raw = be_u32(img + off);

    if (raw == 0) {
      flush_pending();  // terminator ends a record
      if (++zero_run > kMaxZeroWords) {
        end_run();
      } else {
        gap = 0;
      }
      off += 4;
      continue;
    }
    zero_run = 0;

    uint8_t first = img[off];
    if (tr_is_printable(first)) {
      // Try to consume a NUL-terminated, all-printable string of len >= 2.
      uint32_t scan_end = off + kMaxStr;
      if (scan_end > img_size) scan_end = img_size;
      uint32_t nul = 0;
      bool found_nul = false, all_printable = true;
      for (uint32_t j = off; j < scan_end; ++j) {
        uint8_t b = img[j];
        if (b == 0) {
          nul = j;
          found_nul = true;
          break;
        }
        if (!tr_is_printable(b)) {
          all_printable = false;
          break;
        }
      }
      if (found_nul && all_printable && (nul - off) >= 2) {
        const char* text = reinterpret_cast<const char*>(img + off);
        size_t len = nul - off;
        uint32_t str_va = base_va + off;
        gap = 0;
        if (tr_contains_slash(text, len)) {
          flush_pending();
          current_category.assign(text, len);
          run_has_cat = true;
        } else if (tr_looks_like_name(text, len)) {
          flush_pending();
          TunableRec r;
          r.category = current_category;
          r.name.assign(text, len);
          r.name_va = str_va;
          pending = std::move(r);
          ++run_names;
        } else {  // label
          if (pending && pending->label.empty()) pending->label.assign(text, len);
        }
        off = (nul + 1 + 3) & ~3u;  // advance past NUL, 4-byte align
        continue;
      }
      // Single-char "string" or non-printable mid-word: fall through as value.
    }

    // Generic 4-byte value word — first non-empty becomes the record default.
    if (pending && !pending->has_default) {
      pending->has_default = true;
      pending->default_bits = raw;
      pending->default_va = base_va + off;
    }
    if (++gap > kMaxGapWords) end_run();
    off += 4;
  }
  end_run();
  return out;
}

// ---------------------------------------------------------------------------
// JSON string escape (categories/names are identifiers; labels may have
// spaces and the odd punctuation).
// ---------------------------------------------------------------------------
inline void tr_json_escape(std::FILE* f, const std::string& s) {
  for (char c : s) {
    switch (c) {
      case '"': std::fputs("\\\"", f); break;
      case '\\': std::fputs("\\\\", f); break;
      case '\n': std::fputs("\\n", f); break;
      case '\t': std::fputs("\\t", f); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20)
          std::fprintf(f, "\\u%04x", static_cast<unsigned char>(c));
        else
          std::fputc(c, f);
    }
  }
}

// Heuristic: does the inline default decode to a sane float? (Used only to
// annotate the report; the pool's defaults are sparse and ambiguous — the
// runtime pass below gives authoritative live values.)
inline bool tr_plausible_float(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return std::isfinite(f) && std::fabs(f) >= 1e-6f && std::fabs(f) < 1e9f;
}

// ---------------------------------------------------------------------------
// Type inference (Phase 0). The dump carries no type — every slot is a raw
// 32-bit word. We infer float/int/bool from the gXxx name (EA's camelCase
// conventions) and, when a live value is known, its bit pattern. This is a
// HINT the editors surface with a raw-hex override; it is not authoritative.
// ---------------------------------------------------------------------------
inline const char* tr_type_name(int t) {
  switch (t) {
    case 0: return "float";
    case 1: return "int";
    case 2: return "bool";
    default: return "unknown";
  }
}

// Does `name` begin with `pfx` at a camelCase boundary (next char uppercase or
// end)? Rejects e.g. "gIslandSize" matching the "gIs" bool prefix.
inline bool tr_camel_prefix(const std::string& name, const char* pfx) {
  const size_t n = std::strlen(pfx);
  if (name.size() < n || name.compare(0, n, pfx) != 0) return false;
  if (name.size() == n) return true;
  const char c = name[n];
  return c >= 'A' && c <= 'Z';
}

// Whole-word camelCase token present in `name` (e.g. "Num", "Frames").
inline bool tr_camel_word(const std::string& name, const char* word) {
  const size_t m = std::strlen(word);
  for (size_t i = 0; i + m <= name.size(); ++i) {
    if (name.compare(i, m, word) != 0) continue;
    const bool left_ok = (i == 0) || (name[i - 1] >= 'a' && name[i - 1] <= 'z') ||
                         (name[i - 1] >= 'A' && name[i - 1] <= 'Z');
    (void)left_ok;
    const size_t j = i + m;
    const bool right_ok = (j == name.size()) || (name[j] >= 'A' && name[j] <= 'Z') ||
                          (name[j] >= '0' && name[j] <= '9');
    if (right_ok) return true;
  }
  return false;
}

// 0=float, 1=int, 2=bool. `have_value`/`bits` are the live value when known.
inline int tr_infer_type(const std::string& name, bool have_value, uint32_t bits) {
  static const char* kBoolPrefixes[] = {
      "gIs",   "gHas",  "gEnable", "gEnabled", "gUse",     "gAllow",  "gCan",
      "gShould", "gDoes", "gForce",  "gShow",    "gDisable", "gWants",
  };
  for (const char* p : kBoolPrefixes)
    if (tr_camel_prefix(name, p)) return 2;

  static const char* kIntWords[] = {
      "Num", "Count", "Frames", "Ticks", "Index", "Iterations", "Steps",
  };
  for (const char* w : kIntWords)
    if (tr_camel_word(name, w)) return 1;

  // Value-bit disambiguation: a clean small magnitude that is implausible as a
  // float but sane as an int → int. (e.g. 0x00000005). Pure {0,1} stays float
  // unless the name already said bool above.
  if (have_value && bits != 0) {
    float f;
    std::memcpy(&f, &bits, 4);
    const bool plausible_f =
        std::isfinite(f) && std::fabs(f) >= 1e-4f && std::fabs(f) < 1e7f;
    const int32_t iv = int32_t(bits);
    const bool small_int = iv > -1000000 && iv < 1000000;
    if (!plausible_f && small_int) return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Category normalization (Phase 0). The pool parser attributes the nearest
// preceding slash-containing string as a record's category, which mislabels
// asset/shader paths (".fsh", ".big", "rendering/watermark/...") as tunable
// categories and smears one real category across many records. We can't repair
// the smear, but we CAN reject the obviously-bogus path-like ones and give the
// editors a usable grouping key that falls back to the name prefix.
// ---------------------------------------------------------------------------

// A path-like string is an asset path, not a tweak category: any segment ends
// in a 2–4 char file extension, or it starts with a known asset root.
inline bool tr_pathlike_category(const std::string& c) {
  for (size_t i = 0; i + 1 < c.size(); ++i) {
    if (c[i] != '.') continue;
    size_t j = i + 1;
    int k = 0;
    while (j < c.size() && c[j] != '/' &&
           ((c[j] >= 'a' && c[j] <= 'z') || (c[j] >= 'A' && c[j] <= 'Z') ||
            (c[j] >= '0' && c[j] <= '9'))) {
      ++j;
      ++k;
    }
    if (k >= 2 && k <= 4 && (j == c.size() || c[j] == '/')) return true;
  }
  static const char* kAssetRoots[] = {"rendering/", "watermark", "shaders/"};
  for (const char* r : kAssetRoots)
    if (c.rfind(r, 0) == 0) return true;
  return false;
}

// True if the category looks like a genuine engine tweak path (has a '/' and is
// not asset/path-like).
inline bool tr_category_ok(const std::string& c) {
  return !c.empty() && tr_contains_slash(c.c_str(), c.size()) &&
         !tr_pathlike_category(c);
}

// First two camelCase tokens of a gXxx name, used as the grouping fallback when
// the captured category is bogus. "gSpeedBoostMaxAutoBoost" -> "SpeedBoost".
inline std::string tr_name_prefix(const std::string& name) {
  size_t i = (!name.empty() && name[0] == 'g') ? 1 : 0;
  std::string out;
  for (int tokens = 0; i < name.size() && tokens < 2; ++tokens) {
    if (!(name[i] >= 'A' && name[i] <= 'Z')) break;
    const size_t start = i++;
    while (i < name.size() && !(name[i] >= 'A' && name[i] <= 'Z')) ++i;
    out.append(name, start, i - start);
  }
  return out.empty() ? name : out;
}

// The grouping key both editors build their tree from: the real category if it
// survived normalization, otherwise the name prefix.
inline std::string tr_group(const std::string& category, const std::string& name) {
  return tr_category_ok(category) ? category : tr_name_prefix(name);
}

// ---------------------------------------------------------------------------
// Static enumeration entry point: writes a human-readable report and a JSON
// catalog. `img` is the host pointer to guest VA `base_va`.
// ---------------------------------------------------------------------------
inline void DumpTunableRegistry(const uint8_t* img, uint32_t base_va,
                                uint32_t img_size, const char* txt_path,
                                const char* json_path) {
  std::vector<TunableRec> recs = EnumerateTunables(img, base_va, img_size);

  std::FILE* out = std::fopen(txt_path, "w");
  if (!out) out = stderr;
  auto emit = [&](auto&&... a) {
    std::fprintf(out, a...);
    std::fprintf(stderr, a...);
  };

  emit("=== Engine tunable registry enumeration ===\n");
  emit("image %08X .. %08X (%u bytes); %zu tunables found\n\n", base_va,
       base_va + img_size, img_size, recs.size());

  // Category histogram (top-level prefix before the first '/').
  std::unordered_map<std::string, int> top;
  for (const auto& r : recs) {
    std::string c = r.category;
    auto slash = c.find('/');
    top[slash == std::string::npos ? c : c.substr(0, slash)]++;
  }
  emit("--- top-level category histogram ---\n");
  for (const auto& [k, v] : top) emit("  %-24s %5d\n", k.c_str(), v);
  emit("\n--- full catalog (category | name | label | inline-default) ---\n");
  for (const auto& r : recs) {
    std::fprintf(out, "  %-32s %-44s", r.category.c_str(), r.name.c_str());
    if (!r.label.empty()) std::fprintf(out, " \"%s\"", r.label.c_str());
    if (r.has_default) {
      std::fprintf(out, "  =0x%08X", r.default_bits);
      if (tr_plausible_float(r.default_bits))
        std::fprintf(out, " (f32=%.5f)", r.default_as_f32());
      std::fprintf(out, " (i32=%d) @%08X", r.default_as_i32(), r.default_va);
    }
    std::fprintf(out, "\n");
  }
  emit("\n=== %zu tunables across %zu top-level categories ===\n", recs.size(),
       top.size());
  if (out != stderr) std::fclose(out);

  // JSON catalog for the studio / overlay editors.
  std::FILE* j = std::fopen(json_path, "w");
  if (j) {
    std::fprintf(j, "[\n");
    for (size_t i = 0; i < recs.size(); ++i) {
      const auto& r = recs[i];
      std::fprintf(j, "  {\"category\":\"");
      tr_json_escape(j, r.category);
      std::fprintf(j, "\",\"name\":\"");
      tr_json_escape(j, r.name);
      std::fprintf(j, "\",\"label\":\"");
      tr_json_escape(j, r.label);
      std::fprintf(j, "\",\"group\":\"");
      tr_json_escape(j, tr_group(r.category, r.name));
      std::fprintf(j, "\",\"type\":\"%s\",\"name_va\":%u",
                   tr_type_name(tr_infer_type(r.name, false, 0)), r.name_va);
      if (r.has_default) {
        std::fprintf(j, ",\"default_bits\":%u,\"default_va\":%u", r.default_bits,
                     r.default_va);
        if (tr_plausible_float(r.default_bits))
          std::fprintf(j, ",\"default_f32\":%.6g", r.default_as_f32());
      }
      std::fprintf(j, "}%s\n", i + 1 < recs.size() ? "," : "");
    }
    std::fprintf(j, "]\n");
    std::fclose(j);
    std::fprintf(stderr, "[tunables] wrote JSON catalog: %s\n", json_path);
  }
}

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// Runtime value resolution.
//
// The pool carries names but defaults are sparse (most resolve at runtime via
// per-tunable registration call sites). After the guest boots and registers
// its tweaks, a big-endian pointer to each name string materialises inside the
// runtime registry; an adjacent field holds the live value. We enumerate the
// catalog from the mapped image, build a name-VA -> index set, then make a
// single page-safe pass over committed guest memory looking for pointers to
// those names, and record the neighbouring words as candidate live values.
//
// The runtime registry is a flat fixed-stride record array: the matched word is
// a big-endian pointer to the name string at `record_va+0`, and the live 32-bit
// value sits at `record_va+8` (pinned from the dump: 99.7% of values resolved
// there; the prior multi-offset walk only drifted when +8 was zero/pointer).
// We therefore read +8 unconditionally — recording the value even when it is 0
// (a legitimate value, not "unresolved") — and flag whether it decodes to a
// plausible scalar so the editors can gauge confidence.
//
// `vbase` is the host pointer to guest VA 0 (Memory::virtual_membase).
// `catalog_path` (optional) receives the merged schema both editors consume:
// name/name_va/category/group/type + the live value and its patch site.
// ---------------------------------------------------------------------------
inline void DumpTunableValuesRuntime(const uint8_t* vbase, const char* txt_path,
                                     const char* json_path,
                                     const char* catalog_path = nullptr) {
  constexpr uint32_t IMG_BASE = 0x82000000u;
  constexpr uint32_t IMG_SIZE = 0x1EA0000u;
  constexpr int kValueOff = 8;  // pinned value-field offset within a record
  const uint8_t* img = vbase + IMG_BASE;

  std::vector<TunableRec> recs = EnumerateTunables(img, IMG_BASE, IMG_SIZE);

  std::FILE* out = std::fopen(txt_path, "w");
  if (!out) out = stderr;
  auto emit = [&](auto&&... a) {
    std::fprintf(out, a...);
    std::fprintf(stderr, a...);
  };
  emit("=== Engine tunable RUNTIME value scan (post tweak-registration) ===\n");
  emit("%zu tunables enumerated from mapped image\n", recs.size());

  // name VA -> record index.
  std::unordered_map<uint32_t, size_t> by_va;
  by_va.reserve(recs.size() * 2);
  for (size_t i = 0; i < recs.size(); ++i) by_va[recs[i].name_va] = i;

  // Resolved live value per record. The value is always read from record_va+8.
  struct Resolved {
    bool got = false;        // a registry pointer to the name was found
    bool has_value = false;  // the +8 slot was in-bounds and read
    bool scalar = false;     // +8 decodes to a plausible non-pointer scalar
    uint32_t bits = 0;       // raw BE value at record_va+8
    uint32_t record_va = 0;  // VA of the name-pointer slot
    uint32_t value_va = 0;   // record_va + 8 (the patch site)
  };
  std::vector<Resolved> resolved(recs.size());

  // Single page-walk over committed guest memory.
  const uint8_t* p = vbase;
  const uint8_t* guest_end = vbase + (size_t)0x100000000ull;  // 4 GB guest space
  size_t scanned = 0;
  int hits = 0;
  while (p < guest_end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
    auto* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
    size_t rsize = mbi.RegionSize;
    bool scannable = mbi.State == MEM_COMMIT &&
                     !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (scannable && rbase >= vbase) {
      scanned += rsize;
      for (size_t o = 0; o + 4 <= rsize; o += 4) {
        uint32_t v = be_u32(rbase + o);
        auto it = by_va.find(v);
        if (it == by_va.end()) continue;
        size_t idx = it->second;
        // Already have a confident (scalar) hit for this name → keep it. A
        // spurious low-address match whose +8 is garbage can still be upgraded
        // by a later hit that decodes to a real scalar.
        if (resolved[idx].got && resolved[idx].scalar) continue;

        Resolved cand;
        cand.got = true;
        cand.record_va = uint32_t((rbase + o) - vbase);
        if (o + kValueOff + 4 <= rsize) {
          uint32_t wv = be_u32(rbase + o + kValueOff);
          cand.has_value = true;
          cand.bits = wv;
          cand.value_va = cand.record_va + kValueOff;
          bool looks_ptr = rt_readable(vbase, wv, 4) && wv >= IMG_BASE;
          float f;
          std::memcpy(&f, &wv, 4);
          cand.scalar = !looks_ptr && std::isfinite(f);
        }
        if (!resolved[idx].got) ++hits;  // count distinct names located once
        if (!resolved[idx].got || (cand.scalar && !resolved[idx].scalar))
          resolved[idx] = cand;
      }
    }
    p = rbase + rsize;
    if (p <= rbase) break;
  }

  int located = 0, valued = 0, scalars = 0;
  for (auto& r : resolved) {
    if (r.got) ++located;
    if (r.has_value && r.bits) ++valued;
    if (r.scalar) ++scalars;
  }
  emit("registry pointer hits: %d; names located: %d; non-zero values: %d; "
       "plausible scalars: %d; %.1f MB committed scanned\n\n",
       hits, located, valued, scalars, double(scanned) / (1024.0 * 1024.0));

  emit("--- category | name | live-value @record_va+8 ---\n");
  for (size_t i = 0; i < recs.size(); ++i) {
    const auto& r = recs[i];
    const auto& rv = resolved[i];
    std::fprintf(out, "  %-32s %-44s", r.category.c_str(), r.name.c_str());
    if (rv.has_value) {
      float f;
      std::memcpy(&f, &rv.bits, 4);
      std::fprintf(out, " = 0x%08X (f32=%.5f i32=%d) [%s @%08X]", rv.bits, f,
                   int32_t(rv.bits), rv.scalar ? "scalar" : "non-scalar",
                   rv.value_va);
    } else if (rv.got) {
      std::fprintf(out, " [located @%08X, +8 out of bounds]", rv.record_va);
    } else {
      std::fprintf(out, " [no registry pointer found]");
    }
    std::fprintf(out, "\n");
  }
  if (out != stderr) std::fclose(out);

  std::FILE* jf = std::fopen(json_path, "w");
  if (jf) {
    std::fprintf(jf, "[\n");
    for (size_t i = 0; i < recs.size(); ++i) {
      const auto& r = recs[i];
      const auto& rv = resolved[i];
      std::fprintf(jf, "  {\"category\":\"");
      tr_json_escape(jf, r.category);
      std::fprintf(jf, "\",\"name\":\"");
      tr_json_escape(jf, r.name);
      std::fprintf(jf, "\",\"label\":\"");
      tr_json_escape(jf, r.label);
      std::fprintf(jf, "\",\"name_va\":%u,\"located\":%s", r.name_va,
                   rv.got ? "true" : "false");
      if (rv.has_value) {
        float f;
        std::memcpy(&f, &rv.bits, 4);
        std::fprintf(jf,
                     ",\"value_bits\":%u,\"value_f32\":%.6g,\"value_i32\":%d,"
                     "\"value_va\":%u,\"record_va\":%u,\"scalar\":%s",
                     rv.bits, f, int32_t(rv.bits), rv.value_va, rv.record_va,
                     rv.scalar ? "true" : "false");
      }
      std::fprintf(jf, "}%s\n", i + 1 < recs.size() ? "," : "");
    }
    std::fprintf(jf, "]\n");
    std::fclose(jf);
    std::fprintf(stderr, "[tunables-rt] wrote JSON: %s\n", json_path);
  }

  // Merged catalog — the single source of truth both editors build from.
  if (catalog_path) {
    std::FILE* cf = std::fopen(catalog_path, "w");
    if (cf) {
      std::fprintf(cf, "[\n");
      for (size_t i = 0; i < recs.size(); ++i) {
        const auto& r = recs[i];
        const auto& rv = resolved[i];
        const int type = tr_infer_type(r.name, rv.has_value, rv.bits);
        std::fprintf(cf, "  {\"name\":\"");
        tr_json_escape(cf, r.name);
        std::fprintf(cf, "\",\"name_va\":%u,\"category\":\"", r.name_va);
        tr_json_escape(cf, r.category);
        std::fprintf(cf, "\",\"category_ok\":%s,\"group\":\"",
                     tr_category_ok(r.category) ? "true" : "false");
        tr_json_escape(cf, tr_group(r.category, r.name));
        std::fprintf(cf, "\",\"label\":\"");
        tr_json_escape(cf, r.label);
        std::fprintf(cf, "\",\"type\":\"%s\",\"located\":%s", tr_type_name(type),
                     rv.got ? "true" : "false");
        if (rv.has_value) {
          float f;
          std::memcpy(&f, &rv.bits, 4);
          std::fprintf(cf,
                       ",\"value_bits\":%u,\"value_f32\":%.6g,\"value_i32\":%d,"
                       "\"value_va\":%u,\"record_va\":%u,\"scalar\":%s",
                       rv.bits, f, int32_t(rv.bits), rv.value_va, rv.record_va,
                       rv.scalar ? "true" : "false");
        }
        std::fprintf(cf, "}%s\n", i + 1 < recs.size() ? "," : "");
      }
      std::fprintf(cf, "]\n");
      std::fclose(cf);
      std::fprintf(stderr, "[tunables-rt] wrote merged catalog: %s\n",
                   catalog_path);
    }
  }
}
#endif  // _WIN32

}  // namespace nhllegacy
