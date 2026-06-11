// D3D9 entry-point TAP (high-cut engine M1, runtime correlation).
//
// Overrides candidate guest D3D9/graphics-lib functions (from the Phase-0
// labeling, docs/phase0-d3d9-entry-points.txt) with call-counting PASS-THROUGH
// hooks: each increments a counter then calls through to the real recompiled
// body (__imp__sub_X). Rendering stays 100% on rexglue — NO behavior change.
// The per-call frequencies confirm identities (Present ~1/frame, Draw
// ~tens/frame, Release ~lots, CreateDevice ~once).
//
// Mechanism: DEFINE_REX_FUNC makes `sub_X` a WEAK alias to `__imp__sub_X`; a
// strong `sub_X` here overrides it (clang/lld). Gated by env NHL_D3D9_TAP.
//
// The per-frame DUMP is driven by the swap hook (sub_827F1C88 -> VdSwap), which
// is guaranteed once per present. Each hook also logs its FIRST call so we can
// see exactly which hooks the linker actually wired in. Output via REXLOG ->
// logs/nhllegacy_*.log (the WIN32 app has no console).

#include "generated/default/nhllegacy_init.h"

#include <rex/logging.h>

#include <atomic>
#include <cstdlib>

namespace {

const bool g_enabled = std::getenv("NHL_D3D9_TAP") != nullptr;

struct Tap {
    const char* name;
    const char* guess;
    std::atomic<uint64_t> calls{0};
};

Tap t_present {"sub_827FA878", "Present(public)?"};
Tap t_swap    {"sub_827F1C88", "swap/submit(VdSwap)"};
Tap t_draw    {"sub_827EF8E0", "Draw/state-flush?"};
Tap t_release {"sub_827EB558", "COM Release?"};
Tap t_refc2   {"sub_827EB4E0", "COM refcount?"};
Tap t_devinit {"sub_827E3140", "device-init/alloc?"};
Tap t_dp20    {"sub_827ED9A8", "data-plane(20 callers)?"};
Tap t_dp13a   {"sub_827E2140", "data-plane(13)?"};
Tap t_dp13b   {"sub_827E5938", "data-plane(13)?"};
Tap t_dp10    {"sub_827EE438", "data-plane(10,large)?"};

Tap* const kAll[] = {&t_present, &t_swap, &t_draw, &t_release, &t_refc2,
                     &t_devinit, &t_dp20, &t_dp13a, &t_dp13b, &t_dp10};

std::atomic<uint64_t> g_frames{0};

// Increment + log first-ever call. Returns the new count.
inline uint64_t Hit(Tap& t) {
    uint64_t n = t.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) REXLOG_INFO("[d3d9-tap] FIRST-CALL {}  ({})", t.name, t.guess);
    return n;
}

void Dump(const char* tag) {
    uint64_t f = g_frames.load();
    for (Tap* t : kAll) {
        uint64_t n = t->calls.load();
        REXLOG_INFO("[d3d9-tap] {} {} calls={} per_frame={:.2f}  {}", tag, t->name, n,
                    f ? double(n) / double(f) : 0.0, t->guess);
    }
}

}  // namespace

// --- swap (VdSwap) drives the per-frame dump: guaranteed once per present ---
REX_HOOK_RAW(sub_827F1C88) {
    if (g_enabled) {
        Hit(t_swap);
        uint64_t f = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (f % 120 == 0) Dump("tick");
    }
    __imp__sub_827F1C88(ctx, base);
}

#define TAP_HOOK(SYM, COUNTER)                  \
    REX_HOOK_RAW(SYM) {                         \
        if (g_enabled) Hit(COUNTER);            \
        __imp__##SYM(ctx, base);                \
    }

TAP_HOOK(sub_827FA878, t_present)
TAP_HOOK(sub_827EF8E0, t_draw)
TAP_HOOK(sub_827EB558, t_release)
TAP_HOOK(sub_827EB4E0, t_refc2)
TAP_HOOK(sub_827E3140, t_devinit)
TAP_HOOK(sub_827ED9A8, t_dp20)
TAP_HOOK(sub_827E2140, t_dp13a)
TAP_HOOK(sub_827E5938, t_dp13b)
TAP_HOOK(sub_827EE438, t_dp10)
