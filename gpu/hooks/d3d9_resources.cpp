// High-cut engine — Milestone H-1: logical resource graph from D3D9 hooks.
//
// Pure OBSERVATION pass (no plume, no rendering change). Env-gated by NHL_HIGHCUT;
// when the var is unset every hook is a byte-for-byte pass-through and the default
// build/render is unchanged. When set, each hook records into a process-global,
// mutex-guarded registry, then calls through to the real recompiled body
// (__imp__sub_X). Output via REXLOG -> logs/nhllegacy_*.log (WIN32 app, no console).
//
// Goal: reconstruct the *logical* render-target graph entirely at the D3D9 level —
// every render target / texture / surface with its LOGICAL width/height/format/guest
// address, the current bindings, and each resolve's src->dest — proving we have the
// EDRAM-free information the hybrid renders from. The decisive proof is that the
// bound render target reports its LOGICAL size (1280x720), NOT an EDRAM-pitch-sized
// surface — i.e. the fold can never form once we own RT allocation at this level.
//
// Hooked guest functions (identities from docs/highcut-m1-tap-correlation.md):
//   sub_827E3140  device / resource allocator (startup-only)
//   sub_827E6480  SetRenderTarget-class surface bind: copies a descriptor block into
//                 device slot (index+120)*16; args (device, index, surface*, count)
//   sub_827E5938  SetTexture/SetVertexBuffer fetch-constant builder (137/frame);
//                 args (device, slot, resource*) — builds device fetch slot (slot+48)*24
//   sub_827E2140  2nd fetch-constant builder (48/frame); args (device, slot, resource*)
//   sub_827EF8E0  Resolve (~2/frame); builds the k_32_32_FLOAT resolve rect; a dest
//                 texture is an argument
//   sub_827F1C88  Present/Swap (1/frame, VdSwap); args (device, surface*, _, w, h) —
//                 drives the per-frame graph dump
//
// Object layout (decoded from the builder sub_827E5938, which re-packs a D3D resource
// header at object+28 into the hardware fetch constant): the resource object carries a
// GPUTEXTURE_FETCH_CONSTANT-style header whose width/height live at object+36 with the
// xenos.h xe_gpu_texture_fetch_t dword_2 bit layout (width:13|height:13|stack:6, each
// stored minus 1), and whose format/base live at object+32 (format:6 | ... | base:20).
// We decode those plus dump a raw window for empirical cross-check.

#include "generated/default/nhllegacy_init.h"

#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace {

const bool g_enabled = std::getenv("NHL_HIGHCUT") != nullptr;

}  // namespace

// H-2: drive the in-process plume swapchain once per guest Present. Implemented in
// gpu/hooks/plume_present.cpp; self-gating on NHL_HIGHCUT_PRESENT (no-op otherwise).
extern "C" void HighcutPlumeTick();

namespace {

// ---------------------------------------------------------------------------
// Guest memory read (big-endian). `base` is the virtual membase; the recomp's own
// loads use base+addr directly (REX_RAW_ADDR), so we match that — the descriptor
// pointers the game passes live in the main heap (host_address_offset == 0).
// ---------------------------------------------------------------------------
inline bool PlausibleGuestPtr(uint32_t a) {
    // Guest objects seen live sit in the 0x80000000..0xFFFFFFFF range; reject low
    // null-ish / obviously-bogus pointers before dereferencing.
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

inline float GF(uint8_t* base, uint32_t addr) {
    const uint32_t u = G32(base, addr);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// ---------------------------------------------------------------------------
// Logical resource registry
// ---------------------------------------------------------------------------
enum class Kind { Unknown, RenderTarget, Texture, Buffer, Present };

struct Resource {
    Kind kind = Kind::Unknown;
    uint32_t guestPtr = 0;   // object pointer
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t baseAddr = 0;   // decoded GPU base address (bytes), if any
    uint32_t pitch = 0;      // fetch-constant pitch field (pixels), 0 if n/a
    uint32_t tiled = 0;
    uint64_t firstFrame = 0;
};

struct Resolve {
    uint32_t dest = 0;        // dest texture (hook arg) — the resolved RT at logical size
    uint32_t destW = 0;
    uint32_t destH = 0;
    uint32_t destFmt = 0;
    uint64_t frame = 0;
};

std::mutex g_mtx;
std::map<uint32_t, Resource> g_registry;     // guestPtr -> Resource
uint32_t g_currentVport = 0;                 // viewport/transform object (sub_827E6480)
uint32_t g_vportW = 0, g_vportH = 0;         // decoded logical render extent
uint32_t g_presentSurf = 0;                  // present backbuffer
uint32_t g_presentW = 0, g_presentH = 0;     // present logical size (= frame size)
uint32_t g_currentTex[16] = {0};
std::vector<Resolve> g_frameResolves;        // resolves seen since last frame dump
uint64_t g_resolvesTotal = 0;
uint64_t g_frames = 0;

// C-5d frame delimiter. Bumped on EVERY guest VdSwap (sub_827F1C88), UNCONDITIONALLY — independent
// of the NHL_HIGHCUT/NHL_HIGHCUT_PRESENT gates AND of the CP's NhlD3D12CommandProcessor::IssueSwap
// (which live-3D takeover never reaches: it presents via PresentBetaLiveFrame, leaving frame_index_
// frozen, so a multi-frame capture stacks). The guest's high-level present call is upstream of all
// our takeover plumbing and still executes every game-loop iteration, so this is the reliable
// per-frame signal the C-5 capture uses to delimit one frame. Read via HighcutGuestPresentCount().
std::atomic<uint64_t> g_guestPresentCount{0};
extern "C" uint64_t HighcutGuestPresentCount() {
    return g_guestPresentCount.load(std::memory_order_relaxed);
}

// Decode the GPUTEXTURE_FETCH_CONSTANT-style header embedded in a D3D resource
// object (header base at object+28; see file header). Returns false if the pointer
// is implausible. Reads dword_1 (object+32) and dword_2 (object+36).
bool DecodeDims(uint8_t* base, uint32_t obj, Resource& r) {
    if (!PlausibleGuestPtr(obj)) return false;
    const uint32_t d1 = G32(base, obj + 32);  // fetch dword_1: format:6 | ... | base:20
    const uint32_t d2 = G32(base, obj + 36);  // fetch dword_2: width:13 | height:13 | stack:6
    const uint32_t d0 = G32(base, obj + 28);  // fetch dword_0: ... | pitch:9@22 | tiled:1@31
    r.format   = d1 & 0x3Fu;
    r.baseAddr = ((d1 >> 12) & 0xFFFFFu) << 12;
    r.width    = (d2 & 0x1FFFu) + 1u;
    r.height   = ((d2 >> 13) & 0x1FFFu) + 1u;
    r.pitch    = ((d0 >> 22) & 0x1FFu) << 5;  // field is pixels >> 5
    r.tiled    = (d0 >> 31) & 1u;
    return true;
}

// Insert/refresh a resource in the registry and return its (now-known) dims.
Resource& Track(uint8_t* base, uint32_t obj, Kind kind) {
    auto it = g_registry.find(obj);
    if (it == g_registry.end()) {
        Resource r;
        r.guestPtr = obj;
        r.kind = kind;
        r.firstFrame = g_frames;
        DecodeDims(base, obj, r);
        it = g_registry.emplace(obj, r).first;
    } else if (it->second.kind == Kind::Unknown && kind != Kind::Unknown) {
        it->second.kind = kind;
    }
    return it->second;
}

// One-time raw window dump for empirical layout verification (first few objects).
void DumpRawWindow(uint8_t* base, uint32_t obj, const char* tag) {
    if (!PlausibleGuestPtr(obj)) return;
    REXLOG_INFO("[highcut] RAW {} obj={:08X} +28..+52: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                tag, obj, G32(base, obj + 28), G32(base, obj + 32), G32(base, obj + 36),
                G32(base, obj + 40), G32(base, obj + 44), G32(base, obj + 48), G32(base, obj + 52));
}

// Wide raw dump (+0..+124, 32 dwords) for locating dims in a layout we don't yet
// decode (e.g. the render-target surface descriptor).
void DumpRawWide(uint8_t* base, uint32_t obj, const char* tag) {
    if (!PlausibleGuestPtr(obj)) return;
    for (uint32_t off = 0; off < 128; off += 32) {
        REXLOG_INFO("[highcut] WIDE {} obj={:08X} +{:>3}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                    tag, obj, off,
                    G32(base, obj + off + 0),  G32(base, obj + off + 4),
                    G32(base, obj + off + 8),  G32(base, obj + off + 12),
                    G32(base, obj + off + 16), G32(base, obj + off + 20),
                    G32(base, obj + off + 24), G32(base, obj + off + 28));
    }
}

std::atomic<int> g_rawRT{0}, g_rawTex{0};

void DumpFrameGraph() {
    REXLOG_INFO("[highcut] ===== frame {} graph: {} resources, {} resolves(total {}) =====",
                g_frames, g_registry.size(), g_frameResolves.size(), g_resolvesTotal);
    // The LOGICAL render extent — THE fold-killer proof. Every D3D9-level size signal
    // is the true logical size (1280x720), NEVER an EDRAM-pitch-sized surface (640):
    //   - present surface  = the swapchain/frame logical size
    //   - viewport extent  = the render extent draws map into (from sub_827E6480)
    //   - resolve dest tex  = the rendered RT copied out at logical size (decoded)
    if (g_presentSurf)
        REXLOG_INFO("[highcut]   present surf={:08X} LOGICAL {}x{}", g_presentSurf, g_presentW, g_presentH);
    if (g_currentVport)
        REXLOG_INFO("[highcut]   viewport obj={:08X} extent {}x{}", g_currentVport, g_vportW, g_vportH);
    // Bound textures (decoded via the validated fetch-constant decoder).
    for (int s = 0; s < 16; ++s) {
        if (!g_currentTex[s]) continue;
        auto it = g_registry.find(g_currentTex[s]);
        if (it != g_registry.end()) {
            const Resource& r = it->second;
            REXLOG_INFO("[highcut]   tex[{}] obj={:08X} {}x{} fmt={} base={:08X}",
                        s, r.guestPtr, r.width, r.height, r.format, r.baseAddr);
        }
    }
    // Resolves this frame: the rendered RT -> dest texture, dest decoded to its
    // LOGICAL size (this is the resolved render target's true size).
    for (const Resolve& rv : g_frameResolves) {
        REXLOG_INFO("[highcut]   resolve -> dest={:08X} LOGICAL {}x{} fmt={}",
                    rv.dest, rv.destW, rv.destH, rv.destFmt);
    }
    g_frameResolves.clear();
}

}  // namespace

// ===========================================================================
// Hooks (all pass-through). PPC arg regs: r3..r10.
// ===========================================================================

// Device / resource allocator (startup-only). Pure log of allocation events.
REX_HOOK_RAW(sub_827E3140) {
    if (g_enabled) {
        REXLOG_INFO("[highcut] alloc sub_827E3140 r3={:08X} r4={:08X} r5={:08X} r6={:08X}",
                    ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    }
    __imp__sub_827E3140(ctx, base);
}

// Viewport / 2D-transform setter (NOT SetRenderTarget — corrects M1's tentative
// label): copies a transform block into device slot (index+120)*16. The block is a
// scale/offset transform (e.g. +0=1.0, +20=-1.0 Y-flip, +48=-640, +52=360, +56=1280)
// with NO GPU surface base address — i.e. it is a viewport, not a pixel surface.
// It nonetheless carries the LOGICAL render extent (1280x720), which the high cut
// uses to size flat RTs. Args (device r3, index r4, block* r5, count r6).
REX_HOOK_RAW(sub_827E6480) {
    if (g_enabled) {
        const uint32_t index = ctx.r4.u32;
        const uint32_t blk   = ctx.r5.u32;
        if (index == 0 && PlausibleGuestPtr(blk)) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_currentVport = blk;
            // Logical extent: width at block+56, half-height at block+52 (empirical,
            // cross-checked against present 1280x720 and the resolve-dest decode).
            const float fw = GF(base, blk + 56);
            const float fhh = GF(base, blk + 52);
            if (fw > 0.0f && fw < 16384.0f) g_vportW = (uint32_t)(fw + 0.5f);
            if (fhh > 0.0f && fhh < 8192.0f) g_vportH = (uint32_t)(2.0f * fhh + 0.5f);
            if (g_rawRT.fetch_add(1) < 4) {
                DumpRawWide(base, blk, "VPORT");
                REXLOG_INFO("[highcut] SetViewport(sub_827E6480) index={} blk={:08X} -> extent {}x{}",
                            index, blk, g_vportW, g_vportH);
            }
        }
    }
    __imp__sub_827E6480(ctx, base);
}

// Fetch-constant builder (SetTexture/SetVertexBuffer class): args (device r3, slot r4,
// resource* r5). r5==0 is a clear/unbind. 137/frame.
REX_HOOK_RAW(sub_827E5938) {
    if (g_enabled) {
        const uint32_t slot = ctx.r4.u32;
        const uint32_t res  = ctx.r5.u32;
        if (res) {
            std::lock_guard<std::mutex> lk(g_mtx);
            Resource& r = Track(base, res, Kind::Texture);
            if (slot < 16) g_currentTex[slot] = res;
            if (g_rawTex.fetch_add(1) < 4) {
                DumpRawWindow(base, res, "TEX");
                REXLOG_INFO("[highcut] SetTexture slot={} res={:08X} -> {}x{} fmt={} base={:08X}",
                            slot, res, r.width, r.height, r.format, r.baseAddr);
            }
        }
    }
    __imp__sub_827E5938(ctx, base);
}

// 2nd fetch-constant builder (48/frame): args (device r3, slot r4, resource* r5).
REX_HOOK_RAW(sub_827E2140) {
    if (g_enabled) {
        const uint32_t res = ctx.r5.u32;
        if (res) {
            std::lock_guard<std::mutex> lk(g_mtx);
            Track(base, res, Kind::Unknown);  // VB/IB/tex — classify later
        }
    }
    __imp__sub_827E2140(ctx, base);
}

// Resolve (~2/frame): src = currently-bound color RT, dest texture = an argument.
// We log r3..r8 (first calls) to pin which reg carries the dest, and record the
// resolve with the best-known dest candidate (a registered or plausible resource).
REX_HOOK_RAW(sub_827EF8E0) {
    if (g_enabled) {
        std::lock_guard<std::mutex> lk(g_mtx);
        const uint32_t cands[4] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
        uint32_t dest = 0;
        // Prefer a candidate already in the registry; else the first plausible ptr.
        for (uint32_t c : cands) {
            if (g_registry.count(c)) { dest = c; break; }
        }
        if (!dest) {
            for (uint32_t c : cands) {
                if (PlausibleGuestPtr(c)) { dest = c; break; }
            }
        }
        Resolve rv;
        rv.dest = dest;
        rv.frame = g_frames;
        if (dest) {
            Resource& r = Track(base, dest, Kind::Texture);
            rv.destW = r.width;
            rv.destH = r.height;
            rv.destFmt = r.format;
        }
        g_frameResolves.push_back(rv);
        ++g_resolvesTotal;
        if (g_resolvesTotal <= 6) {
            REXLOG_INFO("[highcut] Resolve r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} -> dest={:08X} LOGICAL {}x{} fmt={}",
                        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                        dest, rv.destW, rv.destH, rv.destFmt);
        }
    }
    __imp__sub_827EF8E0(ctx, base);
}

// ---------------------------------------------------------------------------
// NHL_DRAW_TAP: frequency counters for the DRAW_INDX_2-EMITTING functions found
// by static scan (`ori ...,13824` = PM4 0x3600 header low half) — the candidates
// the M1 "per-draw path is inlined" verdict never measured. sub_827FFEC8 was
// labeled "game code writing packets inline" purely from its TU index, but TUs
// are address chunks, not library boundaries. If any of these ticks at ~draw
// rate on a live boot, draws ARE interceptable out-of-line and the inlined-draw
// determination (docs/highcut-m1-tap-correlation.md) is overturned.
// ---------------------------------------------------------------------------
const bool g_drawtap = std::getenv("NHL_DRAW_TAP") != nullptr;
struct DrawTap { const char* name; std::atomic<uint64_t> calls; };
DrawTap g_drawTaps[] = {
    {"sub_827FFEC8", {}}, {"sub_82800798", {}}, {"sub_83739050", {}},
    {"sub_827E5DB8", {}}, {"sub_827EB7D0", {}}, {"sub_827EB928", {}},
};
std::atomic<uint64_t> g_drawTapFrames{0};
inline void DrawTapHit(int slot, const PPCContext& ctx) {
    if (!g_drawtap) return;
    const uint64_t n = g_drawTaps[slot].calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4) {
        REXLOG_INFO("[draw-tap] FIRST {} #{} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X}",
                    g_drawTaps[slot].name, n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                    ctx.r7.u32);
    }
}
inline void DrawTapFrame() {
    if (!g_drawtap) return;
    const uint64_t f = g_drawTapFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (f % 120 == 0) {
        for (auto& t : g_drawTaps) {
            const uint64_t n = t.calls.load(std::memory_order_relaxed);
            REXLOG_INFO("[draw-tap] tick {} calls={} per_frame={:.2f}", t.name, n,
                        double(n) / double(f));
        }
    }
}

// Present/Swap (1/frame): args (device r3, surface* r4, _, w r6, h r7). Drives the
// per-frame graph dump (H-1) and the in-process plume swapchain (H-2).
REX_HOOK_RAW(sub_827F1C88) {
    // C-5d: bump the unconditional guest-present counter FIRST (before any gate) — this is the CP's
    // C-5 frame delimiter, and it must advance even when nothing else high-cut is enabled.
    g_guestPresentCount.fetch_add(1, std::memory_order_relaxed);
    // H-2: advance the plume frame in lock-step with the guest present (self-gating).
    HighcutPlumeTick();
    DrawTapFrame();
    if (g_enabled) {
        std::lock_guard<std::mutex> lk(g_mtx);
        const uint32_t surf = ctx.r4.u32;
        const uint32_t w = ctx.r6.u32, h = ctx.r7.u32;
        if (PlausibleGuestPtr(surf)) {
            Resource& r = Track(base, surf, Kind::Present);
            // The present surface's logical size is carried in the args directly.
            if (w && h) { r.width = w; r.height = h; }
            g_presentSurf = surf;
            g_presentW = r.width;
            g_presentH = r.height;
        }
        ++g_frames;
        if (g_frames == 1 || g_frames % 120 == 0) {
            REXLOG_INFO("[highcut] Present surf={:08X} args_w={} args_h={}", surf, w, h);
            DumpFrameGraph();
        } else {
            g_frameResolves.clear();  // keep only the dumped frames' resolve lists
        }
    }
    __imp__sub_827F1C88(ctx, base);
}

// NHL_DRAW_TAP pass-through counters for the DRAW_INDX_2 emitters (see block above).
REX_HOOK_RAW(sub_827FFEC8) { DrawTapHit(0, ctx); __imp__sub_827FFEC8(ctx, base); }
REX_HOOK_RAW(sub_82800798) { DrawTapHit(1, ctx); __imp__sub_82800798(ctx, base); }
REX_HOOK_RAW(sub_83739050) { DrawTapHit(2, ctx); __imp__sub_83739050(ctx, base); }
REX_HOOK_RAW(sub_827E5DB8) { DrawTapHit(3, ctx); __imp__sub_827E5DB8(ctx, base); }
REX_HOOK_RAW(sub_827EB7D0) { DrawTapHit(4, ctx); __imp__sub_827EB7D0(ctx, base); }
REX_HOOK_RAW(sub_827EB928) { DrawTapHit(5, ctx); __imp__sub_827EB928(ctx, base); }
