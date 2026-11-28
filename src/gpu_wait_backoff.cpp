// GPU-wait backoff: make the game's D3D "wait for the GPU" loops actually wait.
//
// sub_827E2BC8 is one iteration of the D3D runtime's shared GPU-wait helper
// (7 callers: ring-buffer space / fence / present waits). Each call lowers the
// hardware-thread priority (cctpl), runs a 4 x 8 `db16cyc` delay loop, checks
// whether the GPU's write-back read pointer moved (refreshing a progress
// timestamp), and after 5000 ticks without progress calls a hang handler
// (sub_827F72B8). Returns r3=1 = "keep waiting" (the caller re-checks its real
// condition and calls again), r3=0 = abort.
//
// On the Xbox 360, db16cyc/cctpl hand the core to the sibling hardware thread,
// so the spin is nearly free. Recompiled, they're no-ops: the loop spins flat
// out. A PGO profile of a Steam Deck match measured ~53M delay-loop iterations
// per second here - the game's render thread (~80-99% of a core) was mostly
// this spin, burning shared CPU/GPU power budget the GPU command processor and
// GPU need. Linux default: spin briefly (yield) for short waits, then sleep in
// small steps once a wait drags on. The opaque yield/sleep call also forces the
// caller to re-read the guest's GPU progress word each round.
//
// NHL_GPU_WAIT_SLEEP_US=<us>: sleep step once past the spin phase (default 50
// off Windows). 0 = yield only, -1 = off (stock spin). Windows defaults to off:
// its sleep granularity (>= ~1 ms) is far too coarse for this.

#include "generated/default/nhllegacy_init.h"

#include <rex/hook.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace {

int64_t SleepStepUs() {
  static const int64_t v = [] {
    if (const char* e = std::getenv("NHL_GPU_WAIT_SLEEP_US"); e && *e) {
      return int64_t(std::strtoll(e, nullptr, 10));
    }
#if defined(_WIN32)
    return int64_t(-1);
#else
    return int64_t(50);
#endif
  }();
  return v;
}

// Calls within this gap belong to the same wait episode.
constexpr auto kEpisodeGap = std::chrono::milliseconds(1);
// Iterations that only yield before sleeping (keeps short waits low-latency).
constexpr uint32_t kSpinIterations = 32;

}  // namespace

REX_HOOK_RAW(sub_827E2BC8) {
  __imp__sub_827E2BC8(ctx, base);
  if (ctx.r3.u32 != 1) return;  // wait aborted - return to the caller at once

  const int64_t step_us = SleepStepUs();
  if (step_us < 0) return;

  using clock = std::chrono::steady_clock;
  thread_local uint32_t iterations = 0;
  thread_local clock::time_point last_call{};
  const auto now = clock::now();
  if (now - last_call > kEpisodeGap) iterations = 0;  // new wait episode
  last_call = now;

  if (step_us == 0 || ++iterations <= kSpinIterations) {
    std::this_thread::yield();
  } else {
    std::this_thread::sleep_for(std::chrono::microseconds(step_us));
  }
}
