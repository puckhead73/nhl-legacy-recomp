// Abstract engine-tunable store the enhancements overlay talks to.
//
// Backend-free by design (mirrors PadState/PerfSnapshot): the overlay TU pulls
// in no guest-memory or Windows headers. The concrete implementation lives in
// src/tunable_runtime.h (TunableRuntimeStore), which enumerates the tweak
// registration pool from the live mapped image, scans committed guest memory
// for the materialised name->value records (value at record_va+8, the offset
// pinned by the runtime dump), and reads/writes those records live via the
// big-endian REX accessors.
//
// The index build is expensive (a ~1.7 GB committed-memory scan) and only valid
// AFTER the guest has registered its tweaks, so it runs lazily on a worker
// thread — the overlay polls GetState() and shows progress until kReady.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nhl::ui {

// Inferred scalar type of a tunable. Matches tr_infer_type() in the dumper:
// 0=float, 1=int, 2=bool. kUnknown is reserved for entries with no live value.
enum class TunableType : int { kFloat = 0, kInt = 1, kBool = 2, kUnknown = 3 };

// Lightweight display record. All string pointers are owned by the store and
// stable for its lifetime (the entry vector is built once and never resized).
struct TunableEntry {
  const char* name = "";    // gXxx programmer name (the stable join key)
  const char* group = "";   // normalized category, else name-prefix bucket
  const char* label = "";   // human label if the pool carried one (may be "")
  TunableType type = TunableType::kUnknown;
  bool scalar = false;       // live value decodes to a plausible non-pointer
  uint32_t captured_bits = 0;  // stock value at index-build time (reset target)
};

// The overlay edits tunables through this interface; the app owns the concrete
// instance and passes it in (null on non-Vulkan paths or when memory is absent).
class ITunableStore {
 public:
  enum class State { kIdle, kBuilding, kReady, kUnavailable };

  virtual ~ITunableStore() = default;

  // Current build state. kIdle until RequestBuild(); kBuilding while the worker
  // scans; kReady once entries are populated; kUnavailable if there is no guest
  // memory to scan.
  virtual State GetState() const = 0;

  // Kick the one-time background index build. Idempotent: safe to call every
  // frame the section is open; only the first call spawns the worker.
  virtual void RequestBuild() = 0;

  // Valid only once GetState() == kReady.
  virtual std::size_t Count() const = 0;
  virtual TunableEntry Info(std::size_t i) const = 0;
  virtual uint32_t Get(std::size_t i) const = 0;        // live value bits
  virtual void Set(std::size_t i, uint32_t bits) = 0;   // live write + override
  virtual void Reset(std::size_t i) = 0;                // back to captured stock
  virtual bool IsOverridden(std::size_t i) const = 0;

  // Group list for the filter combo (sorted, unique). Index 0 conventionally is
  // not special — "All" is synthesized by the overlay.
  virtual std::size_t GroupCount() const = 0;
  virtual const char* Group(std::size_t g) const = 0;

  // Persistence to nhl_tunables.json (name-keyed; applied at boot).
  virtual std::size_t OverrideCount() const = 0;
  virtual void Save() const = 0;     // write the override file
  virtual void ClearAll() = 0;       // drop every override (live-revert to stock)
};

}  // namespace nhl::ui
