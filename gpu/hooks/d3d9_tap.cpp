// D3D9 entry-point TAP (high-cut engine M1, runtime correlation).
//
// Overrides candidate guest D3D9/graphics-lib functions (Phase-0 labeling,
// docs/phase0-d3d9-entry-points.txt) with call-counting PASS-THROUGH hooks:
// each increments a counter then calls through to the real recompiled body
// (__imp__sub_X). Rendering stays 100% on rexglue — NO behavior change. The
// per-frame frequencies pin identities by matching the swap log's ground truth
// (draws=~61/frame, resolves=~2/frame, shader_loads=~16/frame).
//
// Mechanism: DEFINE_REX_FUNC makes `sub_X` a WEAK alias to `__imp__sub_X`; a
// strong `sub_X` here overrides it (clang/lld). Gated by env NHL_D3D9_TAP.
// Dump is driven by the swap hook (sub_827F1C88 -> VdSwap, 1x/frame). Output via
// REXLOG -> logs/nhllegacy_*.log (the WIN32 app has no console).

#include "generated/default/nhllegacy_init.h"

#include <rex/logging.h>

#include <atomic>
#include <cstdlib>

namespace {

const bool g_enabled = std::getenv("NHL_D3D9_TAP") != nullptr;

// All tapped guest functions. The swap fn (sub_827F1C88) MUST stay first — Hit()
// uses it to drive the per-frame dump.
#define TAP_LIST(X)                                                         \
    X(sub_827F1C88) /* swap/present (VdSwap) — drives frame dump */          \
    X(sub_827FA878) X(sub_827EF8E0) X(sub_827EB558) X(sub_827EB4E0)          \
    X(sub_827E3140) X(sub_827ED9A8) X(sub_827E2140) X(sub_827E5938)          \
    X(sub_827EE438) X(sub_827E2260) X(sub_827EED20) X(sub_827E6FB0)          \
    X(sub_827E6D58) X(sub_827E6978) X(sub_827E6480) X(sub_827E4B58)          \
    X(sub_827EE908) X(sub_827EDEC0) X(sub_827ECD58) X(sub_827EC318)          \
    X(sub_827EB650) X(sub_827EDAB0) X(sub_827EB6E0) X(sub_827E7138)          \
    X(sub_827E57F8) X(sub_827E12B8) X(sub_827F97E0) X(sub_827E8C20)          \
    X(sub_827E5570) X(sub_827E52E0) X(sub_827E4D68) X(sub_827E44D8)          \
    X(sub_827E39D8) X(sub_827E38B8)                                          \
    /* packet-writing VERB candidates (in-lib callers of reserve-space) */   \
    X(sub_827F1588) X(sub_827F24C8) X(sub_827F2B60) X(sub_827F4488)          \
    X(sub_827F51B8) X(sub_827F52A8) X(sub_827F8B10)

enum {
#define X(sym) S_##sym,
    TAP_LIST(X)
#undef X
    S_COUNT
};

struct Tap { const char* name; std::atomic<uint64_t> calls; };

Tap g_taps[S_COUNT] = {
#define X(sym) { #sym, {} },
    TAP_LIST(X)
#undef X
};

std::atomic<uint64_t> g_frames{0};

void Dump(const char* tag) {
    uint64_t f = g_frames.load();
    for (int i = 0; i < S_COUNT; ++i) {
        uint64_t n = g_taps[i].calls.load();
        REXLOG_INFO("[d3d9-tap] {} {} calls={} per_frame={:.2f}", tag, g_taps[i].name, n,
                    f ? double(n) / double(f) : 0.0);
    }
}

inline uint64_t Hit(int slot) {
    uint64_t n = g_taps[slot].calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) REXLOG_INFO("[d3d9-tap] FIRST-CALL {}", g_taps[slot].name);
    if (slot == S_sub_827F1C88) {  // swap drives the per-frame dump
        uint64_t f = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (f % 120 == 0) Dump("tick");
    }
    return n;
}

}  // namespace

// Generate a pass-through hook for every tapped function. Logs the PPC args
// (r3=this, r4..r7) on the first 2 calls — SetRenderTarget shows (index, surf*)
// (small r4 + heap r5); resource setters show a resource pointer.
#define TAP_HOOK(sym)                                                         \
    REX_HOOK_RAW(sym) {                                                       \
        if (g_enabled) {                                                      \
            uint64_t n = Hit(S_##sym);                                        \
            if (n <= 2)                                                       \
                REXLOG_INFO("[d3d9-tap] ARG {} r3={:08X} r4={:08X} r5={:08X} " \
                            "r6={:08X} r7={:08X}", #sym, ctx.r3.u32,          \
                            ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);  \
        }                                                                     \
        __imp__##sym(ctx, base);                                             \
    }
#define X(sym) TAP_HOOK(sym)
TAP_LIST(X)
#undef X
