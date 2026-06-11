// Stage-1 texture-injection registry + sidecar (NHL Legacy Tier-1 beta backend).
//
// The owned "beta" backend renders captured GPU traces (.xtr). Textures whose
// guest RAM was resident BEFORE the capture window are never snapshotted into the
// .xtr, so on replay they sample zero-filled RAM and render black (see
// docs / memory "gameplay trace missing textures"). Stage 0 proved that writing a
// loose `.rx2` asset's decoded slot payload into guest RAM at the texture's
// fetch-constant base fixes it (NHL_BETA_INJECT). Stage 1 builds the address->asset
// map automatically instead of by hand.
//
// How it works (all env-gated; the default/live path is untouched):
//   1. REGISTRY: hash the decoded base-mip payload of every loaded/scanned `.rx2`
//      slot -> {hash -> relpath, slot}. The decoded payload is exactly the
//      X360-tiled bytes the GPU samples out of guest RAM (rx2ffi_decode_slot), so
//      it is byte-identical to what lands in guest RAM at load time.
//   2. CORRELATE (live capture, where guest RAM IS populated): at draw time hash
//      the guest RAM at each texture fetch-constant base and look it up in the
//      registry; a hit records {addr -> relpath, slot}. Addresses are the live
//      session's real addresses, aligned with the co-captured .xtr by construction.
//   3. SIDECAR: emit `<trace>.inject.json` next to the .xtr.
//   4. REPLAY: MaybeInjectBetaTextures() loads the sidecar and injects, reusing the
//      Stage-0 mechanism.
//
// Only ARGB8888 (linear) slots are a known hazard (guest wants tiled) — flagged at
// inject time, not here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nhllegacy {

// One resolved address->asset mapping (a sidecar entry).
struct InjectResolved {
  uint32_t addr = 0;      // guest fetch-constant base (tf.base_address << 12)
  std::string relpath;    // _compiled-relative path to the .rx2
  uint32_t slot = 0;      // texlib slot within the .rx2
};

// Process-wide registry. Thread-safe: RegisterRx2 runs on the file-IO/loader path
// while CorrelateRecord runs on the GPU thread.
class InjectionRegistry {
 public:
  static InjectionRegistry& Get();

  // Number of bytes hashed from the start of a payload / guest-RAM region. A fixed
  // prefix makes the hash independent of exact total/mip size on both sides; a slot
  // whose payload is shorter than this is skipped (too small to be a target).
  static constexpr size_t kHashPrefix = 4096;

  // Decode each slot of the served `.rx2` bytes and register its prefix hash. Safe
  // to call repeatedly with the same file (deduped by hash). No-op without RX2FFI.
  void RegisterRx2(const std::string& relpath, const uint8_t* rx2, size_t len);

  // Walk `root` recursively and RegisterRx2 every *.rx2 (for the replay side, where
  // the loose-file device never opens assets). Paths are stored relative to
  // `relative_root` when provided, otherwise relative to `root`.
  size_t ScanDirectory(const std::filesystem::path& root,
                       const std::filesystem::path& relative_root = {});

  // Hash `kHashPrefix` bytes at `data` and look it up. On a hit fills out_relpath /
  // out_slot and returns true. `len` must be >= kHashPrefix.
  bool LookupBytes(const uint8_t* data, size_t len, std::string& out_relpath,
                   uint32_t& out_slot) const;

  // Record a resolved address->asset mapping (deduped by addr). Thread-safe.
  void RecordResolved(uint32_t addr, const std::string& relpath, uint32_t slot);

  // Convenience: hash+lookup `data` and, on a hit, RecordResolved(addr,...). Returns
  // true if a mapping was recorded (or already present for this addr).
  bool CorrelateRecord(uint32_t addr, const uint8_t* data, size_t len);

  size_t registry_size() const;
  size_t resolved_count() const;

  // Write the resolved map as `<path>` JSON: {"version":1,"captures":[{"addr":"0x..",
  // "rx2":"rel/path.rx2","slot":N}, ...]}. Returns false on IO error.
  bool WriteSidecar(const std::filesystem::path& path) const;

  // Parse a sidecar written by WriteSidecar. Returns the entries (empty on error).
  static std::vector<InjectResolved> ParseSidecar(const std::filesystem::path& path);

 private:
  InjectionRegistry() = default;

  static uint64_t HashPrefix(const uint8_t* data, size_t len);

  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, InjectResolved> by_hash_;   // hash -> asset
  std::unordered_map<uint32_t, InjectResolved> resolved_;  // addr -> asset
};

}  // namespace nhllegacy
