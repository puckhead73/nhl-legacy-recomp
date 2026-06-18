// Live engine-tunable store — the concrete ITunableStore the overlay edits.
//
// Phase 1 of the engine-settings editor (docs/engine-settings-editor-plan.md).
// Reuses the dumper's pool enumeration + memory scan (tunable_registry_dump.h)
// but, instead of writing files, keeps an in-memory index of every located
// tunable and its live patch site so the overlay can read/write values live and
// persist chosen ones to nhl_tunables.json.
//
// Record layout (pinned by the runtime dump): the runtime tweak registry is a
// flat fixed-stride array — record_va+0 is a big-endian pointer to the gXxx name
// string, record_va+8 is the live 32-bit value. We read/write +8 in big-endian
// (PPC), exactly like REX_LOAD_U32/REX_STORE_U32.
//
// Threading: Build() runs on a worker thread (the scan is ~1.7 GB and only valid
// after the guest registers its tweaks). The render thread only touches entries
// once state_ == kReady (acquire/release on the atomic). Set/Reset/Save/ClearAll
// are render-thread only and never overlap the worker (the UI shows "building"
// until ready, and boot re-apply runs inside the worker before kReady).

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/filesystem.h>

#include "renderer/core/nhl_tunable_store.h"
#include "tunable_registry_dump.h"  // EnumerateTunables, tr_infer_type, tr_group
#include "overall_weights_dump.h"   // be_u32, rt_readable (+ <windows.h>)

namespace nhllegacy {

inline std::filesystem::path TunablesOverridePath() {
  return rex::filesystem::GetExecutableFolder() / "nhl_tunables.json";
}

class TunableRuntimeStore final : public nhl::ui::ITunableStore {
 public:
  // `vbase_provider` returns the host pointer to guest VA 0
  // (Memory::virtual_membase). It is invoked lazily ON THE WORKER THREAD at
  // build time — NOT at construction — because the guest memory subsystem is not
  // yet initialized when the overlay's dialogs are created (OnCreateDialogs runs
  // before "Runtime initialized"). Returning null → the store reports
  // kUnavailable.
  explicit TunableRuntimeStore(std::function<const uint8_t*()> vbase_provider)
      : vbase_provider_(std::move(vbase_provider)) {}
  ~TunableRuntimeStore() override {
    if (worker_.joinable()) worker_.join();
  }

  State GetState() const override { return state_.load(std::memory_order_acquire); }

  void RequestBuild() override {
    State expected = State::kIdle;
    if (!state_.compare_exchange_strong(expected, State::kBuilding)) return;
    worker_ = std::thread([this] {
      vbase_ = vbase_provider_ ? vbase_provider_() : nullptr;
      if (!vbase_) {
        state_.store(State::kUnavailable, std::memory_order_release);
        return;
      }
      Build();
      ApplyPersistedOverrides();  // captured_bits already holds stock values
      state_.store(State::kReady, std::memory_order_release);
    });
  }

  std::size_t Count() const override { return entries_.size(); }

  nhl::ui::TunableEntry Info(std::size_t i) const override {
    const Entry& e = entries_[i];
    return {e.name.c_str(),
            e.group.c_str(),
            e.label.c_str(),
            static_cast<nhl::ui::TunableType>(e.type),
            e.scalar,
            e.captured_bits};
  }

  uint32_t Get(std::size_t i) const override { return ReadBits(entries_[i].value_va); }

  void Set(std::size_t i, uint32_t bits) override {
    Entry& e = entries_[i];
    WriteBits(e.value_va, bits);
    e.overridden = true;
    overrides_[e.name] = bits;
  }

  void Reset(std::size_t i) override {
    Entry& e = entries_[i];
    WriteBits(e.value_va, e.captured_bits);
    e.overridden = false;
    overrides_.erase(e.name);
  }

  bool IsOverridden(std::size_t i) const override { return entries_[i].overridden; }

  std::size_t GroupCount() const override { return groups_.size(); }
  const char* Group(std::size_t g) const override { return groups_[g].c_str(); }

  std::size_t OverrideCount() const override { return overrides_.size(); }

  void Save() const override {
    std::FILE* f = std::fopen(TunablesOverridePath().string().c_str(), "w");
    if (!f) return;
    std::fprintf(f, "[\n");
    std::size_t written = 0;
    for (const Entry& e : entries_) {
      if (!e.overridden) continue;
      const uint32_t bits = ReadBits(e.value_va);
      float fv;
      std::memcpy(&fv, &bits, 4);
      std::fprintf(f, "  {\"name\":\"");
      tr_json_escape(f, e.name);
      std::fprintf(f, "\",\"bits\":%u,\"value\":%.6g,\"type\":\"%s\"}%s\n", bits,
                   fv, tr_type_name(e.type),
                   (++written < overrides_.size()) ? "," : "");
    }
    std::fprintf(f, "]\n");
    std::fclose(f);
  }

  void ClearAll() override {
    for (Entry& e : entries_) {
      if (!e.overridden) continue;
      WriteBits(e.value_va, e.captured_bits);
      e.overridden = false;
    }
    overrides_.clear();
  }

 private:
  struct Entry {
    std::string name;
    std::string group;
    std::string label;
    int type = 0;             // 0=float 1=int 2=bool 3=unknown
    bool scalar = false;
    bool overridden = false;
    uint32_t captured_bits = 0;  // stock value at build time (reset target)
    uint32_t value_va = 0;       // guest VA of the live value (record_va+8)
  };

  // Big-endian (PPC) value access at a guest VA, mirroring REX_LOAD/STORE_U32.
  uint32_t ReadBits(uint32_t va) const {
    if (!va) return 0;
    return be_u32(vbase_ + va);
  }
  void WriteBits(uint32_t va, uint32_t bits) {
    if (!va) return;
    uint8_t* p = const_cast<uint8_t*>(vbase_) + va;
    p[0] = uint8_t(bits >> 24);
    p[1] = uint8_t(bits >> 16);
    p[2] = uint8_t(bits >> 8);
    p[3] = uint8_t(bits);
  }

  // Worker: enumerate the pool, scan committed memory for each name pointer, and
  // record record_va+8 as the live value. captured_bits = the stock value now.
  void Build() {
    constexpr uint32_t IMG_BASE = 0x82000000u;
    constexpr uint32_t IMG_SIZE = 0x1EA0000u;
    constexpr int kValueOff = 8;
    const uint8_t* img = vbase_ + IMG_BASE;

    std::vector<TunableRec> recs = EnumerateTunables(img, IMG_BASE, IMG_SIZE);
    entries_.reserve(recs.size());

    // name VA -> entry index.
    std::unordered_map<uint32_t, std::size_t> by_va;
    by_va.reserve(recs.size() * 2);
    for (const TunableRec& r : recs) {
      Entry e;
      e.name = r.name;
      e.group = tr_group(r.category, r.name);
      e.label = r.label;
      by_va[r.name_va] = entries_.size();
      entries_.push_back(std::move(e));
    }
    // Track which entries still need a (more confident) value, mirroring the
    // dumper's "scalar upgrade" rule.
    std::vector<bool> got(entries_.size(), false);
    std::vector<bool> scalar_hit(entries_.size(), false);

    const uint8_t* p = vbase_;
    const uint8_t* guest_end = vbase_ + (size_t)0x100000000ull;
    while (p < guest_end) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
      auto* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
      size_t rsize = mbi.RegionSize;
      const bool scannable = mbi.State == MEM_COMMIT &&
                             !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
      if (scannable && rbase >= vbase_) {
        for (size_t o = 0; o + 4 <= rsize; o += 4) {
          uint32_t v = be_u32(rbase + o);
          auto it = by_va.find(v);
          if (it == by_va.end()) continue;
          const size_t idx = it->second;
          if (got[idx] && scalar_hit[idx]) continue;
          if (o + kValueOff + 4 > rsize) continue;
          const uint32_t rec_va = uint32_t((rbase + o) - vbase_);
          const uint32_t value_va = rec_va + kValueOff;
          const uint32_t bits = be_u32(rbase + o + kValueOff);
          const bool looks_ptr = rt_readable(vbase_, bits, 4) && bits >= IMG_BASE;
          float fb;
          std::memcpy(&fb, &bits, 4);
          const bool scalar = !looks_ptr && std::isfinite(fb);
          if (!got[idx] || (scalar && !scalar_hit[idx])) {
            Entry& e = entries_[idx];
            e.value_va = value_va;
            e.captured_bits = bits;
            e.scalar = scalar;
            e.type = tr_infer_type(e.name, true, bits);
            got[idx] = true;
            scalar_hit[idx] = scalar;
          }
        }
      }
      p = rbase + rsize;
      if (p <= rbase) break;
    }

    // Entries with no live value (located name but +8 unreadable / never hit):
    // infer type by name only and leave value_va = 0 (read/write are no-ops).
    for (Entry& e : entries_)
      if (e.value_va == 0) e.type = tr_infer_type(e.name, false, 0);

    BuildGroups();
  }

  void BuildGroups() {
    std::unordered_map<std::string, int> seen;
    for (const Entry& e : entries_) {
      if (seen.emplace(e.group, 1).second) groups_.push_back(e.group);
    }
    std::sort(groups_.begin(), groups_.end());
  }

  // Apply nhl_tunables.json over the freshly-built stock index: write each saved
  // value live and mark the entry overridden. captured_bits stays the stock
  // value so "reset" restores it. Tolerant hand parser for our own schema (no
  // JSON lib in-tree); keyed on "name" + "bits".
  void ApplyPersistedOverrides() {
    std::ifstream f(TunablesOverridePath());
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    std::unordered_map<std::string, std::size_t> by_name;
    by_name.reserve(entries_.size() * 2);
    for (std::size_t i = 0; i < entries_.size(); ++i) by_name[entries_[i].name] = i;

    size_t pos = 0;
    while (true) {
      const size_t nk = text.find("\"name\"", pos);
      if (nk == std::string::npos) break;
      // value of "name": next quoted string after the colon.
      size_t q1 = text.find('"', text.find(':', nk) + 1);
      if (q1 == std::string::npos) break;
      size_t q2 = text.find('"', q1 + 1);
      if (q2 == std::string::npos) break;
      const std::string name = text.substr(q1 + 1, q2 - q1 - 1);
      // "bits": integer, searched only within this object (up to next '}').
      const size_t obj_end = text.find('}', q2);
      const size_t bk = text.find("\"bits\"", q2);
      pos = (obj_end == std::string::npos) ? q2 + 1 : obj_end + 1;
      if (bk == std::string::npos || (obj_end != std::string::npos && bk > obj_end))
        continue;
      const size_t colon = text.find(':', bk);
      if (colon == std::string::npos) continue;
      const uint32_t bits =
          uint32_t(std::strtoul(text.c_str() + colon + 1, nullptr, 10));
      auto it = by_name.find(name);
      if (it == by_name.end()) continue;
      Entry& e = entries_[it->second];
      if (!e.value_va) continue;
      WriteBits(e.value_va, bits);
      e.overridden = true;
      overrides_[e.name] = bits;
    }
  }

  std::function<const uint8_t*()> vbase_provider_;
  const uint8_t* vbase_ = nullptr;  // resolved on the worker at build time
  std::atomic<State> state_{State::kIdle};
  std::thread worker_;
  std::vector<Entry> entries_;             // built once; never resized after
  std::vector<std::string> groups_;        // sorted unique group keys
  std::unordered_map<std::string, uint32_t> overrides_;  // name -> bits
};

}  // namespace nhllegacy
