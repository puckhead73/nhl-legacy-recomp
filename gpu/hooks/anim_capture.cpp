// Animation capture — hook the runtime DctAnimationAsset decode chain and record
// the decoded poses (the "extract animations" final step).
//
// Env-gated by NHL_ANIM_CAPTURE; when unset every hook is a byte-for-byte
// pass-through and the build/render is unchanged. When set, each hook logs its
// inputs and dumps decoded output to logs/ (REXLOG) and, if NHL_ANIM_CAPTURE
// names a path, appends raw records to that file for offline analysis.
//
// Decode chain identified by static RE (see docs/nhl12-decomp-reference/animation/
// cba-format.md §6). All three are real recompiled functions, so REX_HOOK_RAW
// overrides them directly — no nhllegacy_functions.toml entry needed.
//
//   sub_82CA9B20  clip-sample ENTRY: time -> tick/key (FPS/FPSScale math), then
//                 calls the per-track blend. Args: r3 = clip controller/"this",
//                 r4 = time (float in fpr1 on PPC; r4 may carry a frame index/ctx).
//   sub_82CA9618  per-track orchestration: fetches the 3 surrounding key-frames,
//                 picks the active one, calls the core decompressor x3 and blends.
//   sub_82CA7BD0  CORE decompress + dequantize + key-interpolate (VMX-heavy):
//                 r3 = decode context ([r3+0] -> DctAnimationAsset; NumKeys lives
//                 in the asset header), r4 / r5 = output pose pointers. Emits the
//                 int16 quaternion/vector stream (4xint16 per joint).
//
// This is a VALIDATION-FIRST pass: we do not yet assume exact runtime object
// offsets (the loaded asset layout differs from the on-disk .cba record). We log
// the argument pointers and dump memory windows around them so the layout — and
// the clip-name resolution — can be confirmed against a live run, then tightened.

#include "generated/default/nhllegacy_init.h"

#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

const char* g_capPath = std::getenv("NHL_ANIM_CAPTURE");
const bool  g_enabled = g_capPath != nullptr;

// Cap the volume of per-call logging/dumps so a few seconds of gameplay is
// readable rather than a multi-GB flood. Raw-file dumps (when a path is given)
// are uncapped.
constexpr uint32_t kLogCap = 256;
std::atomic<uint32_t> g_entryLogs{0};
std::atomic<uint32_t> g_coreLogs{0};

std::mutex g_fileMtx;
std::FILE* g_file = nullptr;

std::FILE* CaptureFile() {
    // Open lazily on first use. A bare "1"/"on" means log-only (no raw file).
    static std::once_flag once;
    std::call_once(once, [] {
        if (g_capPath && std::strcmp(g_capPath, "1") != 0 &&
            std::strcmp(g_capPath, "on") != 0) {
            g_file = std::fopen(g_capPath, "wb");
            if (!g_file) {
                REXLOG_INFO("[anim] could not open capture file '{}'", g_capPath);
            }
        }
    });
    return g_file;
}

inline bool PlausibleGuestPtr(uint32_t a) {
    return a >= 0x10000u && a < 0xFFFF0000u;
}

inline uint32_t G32(uint8_t* base, uint32_t addr) {
    uint32_t v = 0;
    std::memcpy(&v, base + addr, 4);
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

// Render up to `n` bytes of guest memory at `addr` as hex for the log.
std::string HexWindow(uint8_t* base, uint32_t addr, uint32_t n) {
    if (!PlausibleGuestPtr(addr)) return "<bad ptr>";
    static const char* hx = "0123456789abcdef";
    std::string s;
    s.reserve(n * 3);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t b = base[addr + i];
        s.push_back(hx[b >> 4]);
        s.push_back(hx[b & 0xf]);
        s.push_back(' ');
    }
    return s;
}

// Append a tagged raw record to the capture file: [tag u32][addr u32][len u32][bytes].
void DumpRaw(uint8_t* base, uint32_t tag, uint32_t addr, uint32_t len) {
    std::FILE* f = CaptureFile();
    if (!f || !PlausibleGuestPtr(addr)) return;
    std::lock_guard<std::mutex> lk(g_fileMtx);
    std::fwrite(&tag, 4, 1, f);
    std::fwrite(&addr, 4, 1, f);
    std::fwrite(&len, 4, 1, f);
    std::fwrite(base + addr, 1, len, f);
    std::fflush(f);
}

}  // namespace

// ===========================================================================
// Hooks (all pass-through). PPC integer arg regs: r3..r10; float args: fr1..fr13.
// ===========================================================================

// Clip-sample entry: time -> key. Capture the controller and a window around it
// (to later resolve the clip name/guid), plus the float time arg in fr1.
REX_HOOK_RAW(sub_82CA9B20) {
    if (g_enabled) {
        const uint32_t self = ctx.r3.u32;
        if (PlausibleGuestPtr(self)) {
            DumpRaw(base, /*tag=*/0x53504C43u /*'CLPS'*/, self, 128);
            if (g_entryLogs.fetch_add(1) < kLogCap) {
                REXLOG_INFO("[anim] sample(sub_82CA9B20) self={:08X} r4={:08X} "
                            "t={:.4f} ctrl[+0..32]={}",
                            self, ctx.r4.u32, ctx.f1.f64, HexWindow(base, self, 32));
            }
        }
    }
    __imp__sub_82CA9B20(ctx, base);
}

// Core decompressor: capture asset + NumKeys + the decoded output buffer. We dump
// the output AFTER the real function runs (so the buffer holds decoded poses).
REX_HOOK_RAW(sub_82CA7BD0) {
    uint32_t self = ctx.r3.u32;
    uint32_t outA = ctx.r4.u32;
    uint32_t outB = ctx.r5.u32;
    uint32_t asset = 0, numKeys = 0;
    if (g_enabled && PlausibleGuestPtr(self)) {
        asset = G32(base, self + 0);
        if (PlausibleGuestPtr(asset)) {
            numKeys = G32(base, asset + 12);  // header NumKeys (validate live)
            DumpRaw(base, /*tag=*/0x54455341u /*'ASET'*/, asset, 96);
        }
    }

    __imp__sub_82CA7BD0(ctx, base);

    if (g_enabled && PlausibleGuestPtr(self)) {
        // Output pose buffer(s): dump a window after decode for offline decode.
        if (PlausibleGuestPtr(outA)) DumpRaw(base, 0x30544F50u /*'POT0'*/, outA, 128);
        if (PlausibleGuestPtr(outB)) DumpRaw(base, 0x31544F50u /*'POT1'*/, outB, 128);
        if (g_coreLogs.fetch_add(1) < kLogCap) {
            REXLOG_INFO("[anim] core(sub_82CA7BD0) self={:08X} asset={:08X} "
                        "numKeys={} outA={:08X} outB={:08X} out[+0..32]={}",
                        self, asset, numKeys, outA, outB,
                        PlausibleGuestPtr(outA) ? HexWindow(base, outA, 32) : "");
        }
    }
}

// Per-track orchestrator: light touch — count invocations to confirm the chain.
REX_HOOK_RAW(sub_82CA9618) {
    if (g_enabled) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1) < 8) {
            REXLOG_INFO("[anim] track(sub_82CA9618) self={:08X} r4={:08X} r5={:08X}",
                        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
        }
    }
    __imp__sub_82CA9618(ctx, base);
}
