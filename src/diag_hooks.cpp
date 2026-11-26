// Diagnostic + debugging infrastructure for the NHL Legacy recomp.
//
// This file holds ONLY diagnostics/tooling — it contains no behavioral fixes.
// Every real divergence fix lives elsewhere:
//   - nhllegacy_manifest.toml   (setjmp_address / longjmp_address)
//   - nhllegacy_functions.toml  (scanner-missed indirect/thunk functions)
//   - nhllegacy_app.h           (cvars: render_target_path_d3d12=rov, vsync,
//                                gpu_allow_invalid_fetch_constants, protect_zero,
//                                scribble_heap; cache: VFS mount)
// Earlier sessions carried strong-override hooks here (IGA/job-system band-aids,
// setjmp-era guards); all were root-caused and removed. See docs/rexglue-spike.md
// for the full investigation history.
//
// Kept here: a SIGABRT crash-context tap (always on), and two debugging tools
// gated off by default (a guest call-stack sampler and a single-frame GPU
// trace dumper). Flip their g_* flags + rebuild to use them.

#include "generated/default/nhllegacy_init.h"

#include "vp6_bridge.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "platform/host_compat.h"  // VirtualQuery, GetTickCount, _byteswap_* (portable)

#include <rex/graphics/graphics_system.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

// Captured once from any guest function's `base` (the host base of guest
// address space). Used by the guarded-read helpers below.
static std::atomic<uint8_t*> g_guest_base{nullptr};

// --- Crash-context tap (always on, zero cost until a crash) ------------------
// REX_FATAL (e.g. "Call to invalid or unregistered function") ends in abort();
// UCRT raises SIGABRT before fail-fast, so this handler snapshots the dying
// thread's guest context to nullcall_ctx.txt. lr = guest return address of the
// offending bctrl/branch; last_indirect_target = the unregistered callee.
// (lldb can't usefully attach to the 124MB exe, so this is the crash tool.)
static void AbortContextTap(int) {
  auto* ts = rex::runtime::ThreadState::Get();
  if (ts && ts->context()) {
    const PPCContext* c = ts->context();
    FILE* f = std::fopen("nullcall_ctx.txt", "w");
    if (f) {
      std::fprintf(f,
                   "lr=%08llX last_indirect_target=%08X r3=%08X r4=%08X "
                   "r29=%08X r30=%08X r31=%08X\n",
                   static_cast<unsigned long long>(c->lr),
                   c->last_indirect_target, c->r3.u32, c->r4.u32, c->r29.u32,
                   c->r30.u32, c->r31.u32);
      std::fclose(f);
    }
  }
  _exit(3);
}

static const int g_abort_tap_installed = [] {
  std::signal(SIGABRT, AbortContextTap);
  return 0;
}();

// Guarded guest read: a sampled r1 may be garbage or point at uncommitted
// guest pages (host AV on touch). VirtualQuery the host address first.
static bool SafeGuestLoadU32(uint8_t* base, uint32_t guest_addr,
                             uint32_t* out) {
  uint8_t* host = base + guest_addr + REX_PHYS_HOST_OFFSET(guest_addr);
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(host, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  uint32_t raw;
  std::memcpy(&raw, host, 4);
  *out = _byteswap_ulong(raw);
  return true;
}

// Capture g_guest_base from the generic thread-create wrapper (runs early,
// pure passthrough — no behavioral change).
REX_EXTERN(__imp__sub_8306B6A0);
extern "C" REX_FUNC(sub_8306B6A0) {
  g_guest_base.store(base);
  __imp__sub_8306B6A0(ctx, base);
}

// --- Diagnostic probe: team-color material binder (gated on) -----------------
// Equipment renders tint-BLACK in a full game but correct in test-on-ice
// (no team -> default color). sub_829BCC18 is the ONLY code referencing the
// HOME_TEAM_COLOUR/AWAY_TEAM_COLOUR/TEAM_COLOUR material-param strings; the
// recolor shader (player_equipment_recolor_cz.fxo) carries the team color in
// its recolor_template1/2 float4 constants. Hook the binder and dump its args
// plus the material object it operates on (r3) as floats -> a recolor_template
// of (0,0,0,*) means the value is already black before it reaches the GPU.
// (The earlier reader sub_82AE8D40 never fired in-game -> wrong path.)
static constexpr bool g_teamcolor_probe = false;  // binder never fires in-game (dead end)
static std::atomic<int> g_tc_calls{0};

static float GuestF32(uint8_t* base, uint32_t addr) {
  uint32_t w = 0;
  if (!SafeGuestLoadU32(base, addr, &w)) return -999.0f;
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

REX_EXTERN(__imp__sub_829BCC18);
extern "C" REX_FUNC(sub_829BCC18) {
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  int n = g_teamcolor_probe ? g_tc_calls.fetch_add(1) : 1000;
  __imp__sub_829BCC18(ctx, base);
  if (n < 32) {
    FILE* f = std::fopen("teamcolor_probe.txt", "a");
    if (f) {
      std::fprintf(f, "bind#%d r3=%08X r4=%08X r5=%08X r6=%08X\n", n, r3, r4, r5,
                   r6);
      // Dump the material object (r3) as hex + floats: scan for a color vec4.
      std::fprintf(f, "  mtl hex :");
      for (int k = 0; k < 24; ++k) {
        uint32_t w = 0;
        if (SafeGuestLoadU32(base, r3 + k * 4, &w))
          std::fprintf(f, " %08X", w);
        else
          std::fprintf(f, " --------");
      }
      std::fprintf(f, "\n  mtl flt :");
      for (int k = 0; k < 24; ++k)
        std::fprintf(f, " %.3f", GuestF32(base, r3 + k * 4));
      std::fprintf(f, "\n");
      std::fclose(f);
    }
  }
}

[[maybe_unused]] static void LogGuestStackHere(const char* tag, PPCContext& ctx,
                                               uint8_t* base);

// --- VP6 codec RE: capture the indirect transform target (gated on) ----------
// The EA boot movie (ealogo.vp6) shows 8x8-block-aligned, high-AC-only green
// corruption -> a recompiled arithmetic-precision bug in the VP6 dequant/inverse
// transform, reached through a VIRTUAL dispatch from the per-block driver
// sub_8276AC70: it calls vtable[0] of the singleton at *(0x83B3AA10) (matching the
// address the code below reads; re-verify via disasm if this probe is ever revived —
// an earlier draft of this comment read 0x83B7AA10). That
// target is the next codec layer (or the transform). Resolve the chain live and
// dump the vtable + the per-block args (r3..r6 -> coefficient/block pointers we
// will need for the differential I/O harness) -> vp6_probe.txt.
static constexpr bool g_vp6_probe = false;  // chain mapped 2026-07-06 - see docs/vp6-fork-investigation.md
static std::atomic<int> g_vp6_n{0};

// --- VP6 differential harness (Path A, iteration 1: layout reconnaissance) ---
// Dump r3..r10 plus guest memory around the pointer-like args BEFORE and AFTER
// each selected per-block call -> vp6_harness.txt. Goal: identify which region
// holds the int16 coefficient block (nonzero small BE values before the call)
// and which region the call writes (output pixels), so iteration 2 can diff
// them against a host FFmpeg reference decode of the same movie.
static constexpr bool g_vp6_harness = false;  // flip on + rebuild to re-run captures
static bool GuestPtrLike(uint32_t a) { return a >= 0x10000u && a < 0xFFF00000u; }

// Copy guest bytes into buf (VirtualQuery-guarded per page). Returns bytes valid.
static size_t ReadGuestBytes(uint8_t* base, uint32_t addr, uint8_t* buf,
                             size_t len) {
  size_t got = 0;
  while (got < len) {
    uint32_t a = addr + static_cast<uint32_t>(got);
    uint8_t* host = base + a + REX_PHYS_HOST_OFFSET(a);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(host, &mbi, sizeof(mbi))) break;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
      break;
    size_t page_left =
        reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - host;
    size_t chunk = page_left < (len - got) ? page_left : (len - got);
    std::memcpy(buf + got, host, chunk);
    got += chunk;
  }
  return got;
}

static void PrintHexBlock(FILE* f, const char* tag, uint32_t addr,
                          const uint8_t* buf, size_t len) {
  std::fprintf(f, "  %s @%08X (%zu bytes):\n", tag, addr, len);
  for (size_t i = 0; i < len; i += 16) {
    std::fprintf(f, "    +%03zX:", i);
    for (size_t k = i; k < i + 16 && k < len; k += 2) {
      // print as big-endian u16 (PPC byte order; DCT coeffs are int16)
      std::fprintf(f, " %04X", (buf[k] << 8) | buf[k + 1]);
    }
    std::fprintf(f, "\n");
  }
}

// --- VP6 IDCT differential taps (Path A, iteration 2) ------------------------
// sub_827D2FE8 / sub_82898660 are the two scalar VP3-family IDCTs in the image
// (identified by their fixed-point constants 46341/54491/60547/64277/36410).
// Contract: r3 -> int16[64] coefficient block, transformed IN PLACE (row pass
// to stack, column pass sth's back). Dump pre/post blocks for the first calls
// so the math can be verified against a host reference -> vp6_idct_io.txt.
static std::atomic<int> g_idct_a_n{0}, g_idct_b_n{0};

static void DumpIdctIo(const char* which, int n, PPCContext& ctx, uint8_t* base,
                       void (*impl)(PPCContext&, uint8_t*)) {
  constexpr size_t kBlk = 128;  // int16[64]
  uint8_t pre[kBlk]{}, post[kBlk]{};
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  size_t np = GuestPtrLike(r3) ? ReadGuestBytes(base, r3, pre, kBlk) : 0;
  impl(ctx, base);
  if (np != kBlk) return;
  ReadGuestBytes(base, r3, post, kBlk);
  FILE* f = std::fopen("vp6_idct_io.txt", "a");
  if (!f) return;
  std::fprintf(f, "=== %s#%d r3=%08X r4=%08X r5=%08X\nin :", which, n, r3, r4,
               r5);
  for (size_t k = 0; k < kBlk; k += 2)
    std::fprintf(f, " %d", int16_t((pre[k] << 8) | pre[k + 1]));
  std::fprintf(f, "\nout:");
  for (size_t k = 0; k < kBlk; k += 2)
    std::fprintf(f, " %d", int16_t((post[k] << 8) | post[k + 1]));
  std::fprintf(f, "\n");
  std::fclose(f);
}

// Runtime-gated variant (env NHL_VP6_IDCT, no rebuild needed): count every
// call (periodic timestamped totals -> vp6_idct_calls.txt, to establish
// whether these transforms fire during the EA-logo movie window at all), and
// dump pre/post only for HIGH-AC input blocks (>=6 nonzero AC coeffs or any
// |AC| >= 300) - the row-IDCT column-crush bug (docs) only manifests on those;
// low-AC cloud-movie blocks diff clean and would waste the dump budget.
static bool IdctEnvOn() {
  static const bool on = std::getenv("NHL_VP6_IDCT") != nullptr;
  return on;
}

static bool IdctBlockInteresting(PPCContext& ctx, uint8_t* base) {
  uint8_t buf[128];
  if (!GuestPtrLike(ctx.r3.u32) ||
      ReadGuestBytes(base, ctx.r3.u32, buf, sizeof(buf)) != sizeof(buf)) {
    return false;
  }
  int nnz = 0, maxa = 0;
  for (int i = 1; i < 64; ++i) {
    int v = int16_t((buf[2 * i] << 8) | buf[2 * i + 1]);
    if (v) ++nnz;
    int a = v < 0 ? -v : v;
    if (a > maxa) maxa = a;
  }
  return nnz >= 6 || maxa >= 300;
}

static void IdctCallTick(const char* which, std::atomic<int>& calls) {
  int c = calls.fetch_add(1) + 1;
  if ((c & 0x3FF) == 1) {
    FILE* f = std::fopen("vp6_idct_calls.txt", "a");
    if (f) {
      std::fprintf(f, "%s calls=%d tick=%lu\n", which, c,
                   (unsigned long)GetTickCount());
      std::fclose(f);
    }
  }
}

REX_EXTERN(__imp__sub_827D2FE8);
static std::atomic<int> g_idct_a_calls{0};
extern "C" REX_FUNC(sub_827D2FE8) {
  if (IdctEnvOn()) {
    IdctCallTick("idctA_827D2FE8", g_idct_a_calls);
    if (g_idct_a_n.load(std::memory_order_relaxed) < 64 &&
        IdctBlockInteresting(ctx, base)) {
      int n = g_idct_a_n.fetch_add(1);
      if (n < 64) {
        DumpIdctIo("idctA_827D2FE8", n, ctx, base, __imp__sub_827D2FE8);
        return;
      }
    }
  }
  __imp__sub_827D2FE8(ctx, base);
}

REX_EXTERN(__imp__sub_82898660);
static std::atomic<int> g_idct_b_calls{0};
extern "C" REX_FUNC(sub_82898660) {
  if (IdctEnvOn()) {
    IdctCallTick("idctB_82898660", g_idct_b_calls);
    if (g_idct_b_n.load(std::memory_order_relaxed) < 64 &&
        IdctBlockInteresting(ctx, base)) {
      int n = g_idct_b_n.fetch_add(1);
      if (n < 64) {
        DumpIdctIo("idctB_82898660", n, ctx, base, __imp__sub_82898660);
        return;
      }
    }
  }
  __imp__sub_82898660(ctx, base);
}

// Sweep: the frame driver sub_8277ABB8's direct callees (plus the block
// driver's sibling sub_8276ADB0). One of these is/reaches the per-block
// dequant+IDCT. Same pre/post dump as the recon harness -> vp6_sweep.txt.
static void SweepDump(const char* which, std::atomic<int>& counter,
                      PPCContext& ctx, uint8_t* base,
                      void (*impl)(PPCContext&, uint8_t*)) {
  int n = g_vp6_harness ? counter.fetch_add(1) : 1000;
  if (n >= 12) {
    impl(ctx, base);
    return;
  }
  constexpr size_t kW = 192;
  uint8_t pre4[kW]{}, pre5[kW]{}, post4[kW]{}, post5[kW]{};
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32,
           r7 = ctx.r7.u32;
  size_t n4 = GuestPtrLike(r4) ? ReadGuestBytes(base, r4, pre4, kW) : 0;
  size_t n5 = GuestPtrLike(r5) ? ReadGuestBytes(base, r5, pre5, kW) : 0;
  impl(ctx, base);
  if (n4) ReadGuestBytes(base, r4, post4, kW);
  if (n5) ReadGuestBytes(base, r5, post5, kW);
  FILE* f = std::fopen("vp6_sweep.txt", "a");
  if (!f) return;
  std::fprintf(f, "=== %s#%d r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X\n", which,
               n, r3, r4, r5, r6, r7);
  if (n4) {
    PrintHexBlock(f, "r4.pre ", r4, pre4, n4 < 64 ? n4 : 64);
    if (std::memcmp(pre4, post4, n4))
      PrintHexBlock(f, "r4.POST", r4, post4, n4 < 64 ? n4 : 64);
  }
  if (n5) {
    PrintHexBlock(f, "r5.pre ", r5, pre5, n5 < 64 ? n5 : 64);
    if (std::memcmp(pre5, post5, n5))
      PrintHexBlock(f, "r5.POST", r5, post5, n5 < 64 ? n5 : 64);
  }
  std::fclose(f);
}

#define NHL_VP6_SWEEP_HOOK(addr)                                     \
  REX_EXTERN(__imp__sub_##addr);                                     \
  static std::atomic<int> g_sw_##addr{0};                            \
  extern "C" REX_FUNC(sub_##addr) {                                  \
    SweepDump("sub_" #addr, g_sw_##addr, ctx, base,                  \
              __imp__sub_##addr);                                    \
  }

NHL_VP6_SWEEP_HOOK(82670768)
NHL_VP6_SWEEP_HOOK(82671568)
NHL_VP6_SWEEP_HOOK(8276ADB0)
NHL_VP6_SWEEP_HOOK(8277C350)
NHL_VP6_SWEEP_HOOK(8277A248)
NHL_VP6_SWEEP_HOOK(82779550)
NHL_VP6_SWEEP_HOOK(82778BE8)
NHL_VP6_SWEEP_HOOK(82776BD8)
NHL_VP6_SWEEP_HOOK(82776AE0)

// RGBA frame dumper (env NHL_VP6_RGBA=<hex guest addr>, from the SDK-side
// NHL_VP6_TAP log, e.g. 1CF32000): after each frame-driver call, dump the
// guest-converted 1280x720 RGBA movie frame to vp6_rgba_NNN.raw for diffing
// against the ffmpeg reference decode. ~3.7MB per frame; first 12 frames.
static uint32_t Vp6RgbaAddr() {
  static uint32_t addr = [] {
    const char* e = std::getenv("NHL_VP6_RGBA");
    return e ? uint32_t(std::strtoull(e, nullptr, 16)) : 0u;
  }();
  return addr;
}

static void DumpVp6Rgba(int frame_n, uint8_t* base) {
  uint32_t addr = Vp6RgbaAddr();
  if (!addr || frame_n >= 12) return;
  constexpr size_t kBytes = 1280u * 720u * 4u;
  // addr is a raw PHYSICAL address (texture-key base): translate via the
  // kernel memory system, not the virtual-window offset.
  auto* ks = rex::runtime::current_kernel_state();
  if (!ks || !ks->memory()) return;
  uint8_t* host = ks->memory()->TranslatePhysical<uint8_t*>(addr);
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(host, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return;
  uint8_t* buf = host;
  size_t got = kBytes;
  char name[64];
  std::snprintf(name, sizeof(name), "vp6_rgba_%03d.raw", frame_n);
  FILE* f = std::fopen(name, "wb");
  if (f) {
    std::fwrite(buf, 1, got, f);
    std::fclose(f);
  }
}

// Sampler thread: the frame driver runs ONCE per movie (setup; decode is
// async on workers), so per-call dumps see an empty buffer. Instead poll the
// RGBA buffer during playback and dump distinct frames (dedup by checksum).
static const int g_vp6_rgba_sampler = [] {
  if (!Vp6RgbaAddr()) return 0;
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(25));
    auto* ks = rex::runtime::current_kernel_state();
    if (!ks || !ks->memory()) return;
    constexpr size_t kBytes = 1280u * 720u * 4u;
    uint8_t* host = ks->memory()->TranslatePhysical<uint8_t*>(Vp6RgbaAddr());
    uint64_t last_sum = 0;
    int dumped = 0;
    for (int iter = 0; iter < 600 && dumped < 16; ++iter) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(host, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        continue;
      uint64_t sum = 0;
      for (size_t k = 0; k < kBytes; k += 4096) sum = sum * 31 + host[k];
      if (sum == last_sum || sum == 0) continue;
      last_sum = sum;
      char name[64];
      std::snprintf(name, sizeof(name), "vp6_rgba_%03d.raw", dumped++);
      FILE* f = std::fopen(name, "wb");
      if (f) {
        std::fwrite(host, 1, kBytes, f);
        std::fclose(f);
      }
    }
  }).detach();
  return 0;
}();

// --- VP6 bridge M1: publish-seam recon (env NHL_VP6_PUB) ----------------------
// sub_83067DF8 is the guest's generic optimized memcpy (r3=dst r4=src r5=len).
// The movie's internal-plane -> display-ring publish goes through it row by
// row (internal stride 0x560 vs ring pitch 0x500 forbids one whole-plane
// copy). Log movie-sized row copies (len 0x500 luma / 0x280 chroma) with the
// guest lr = the rect-copy loop that is the bridge's injection seam
// -> vp6_pub.txt.
static std::atomic<int> g_pub_n{0};

static void PubLog(const char* which, PPCContext& ctx) {
  static const bool on = std::getenv("NHL_VP6_PUB") != nullptr;
  if (!on) return;
  uint32_t len = ctx.r5.u32;
  if ((len != 0x500 && len != 0x280) ||
      g_pub_n.load(std::memory_order_relaxed) >= 96) {
    return;
  }
  int n = g_pub_n.fetch_add(1);
  if (n >= 96) return;
  FILE* f = std::fopen("vp6_pub.txt", "a");
  if (f) {
    std::fprintf(f, "%s#%d lr=%08llX dst=%08X src=%08X len=%X tick=%lu\n",
                 which, n, (unsigned long long)ctx.lr, ctx.r3.u32, ctx.r4.u32,
                 len, (unsigned long)GetTickCount());
    std::fclose(f);
  }
}

REX_EXTERN(__imp__sub_83067DF8);
extern "C" REX_FUNC(sub_83067DF8) {
  PubLog("df8", ctx);
  __imp__sub_83067DF8(ctx, base);
}

REX_EXTERN(__imp__sub_83067EA8);
extern "C" REX_FUNC(sub_83067EA8) {
  PubLog("ea8", ctx);
  __imp__sub_83067EA8(ctx, base);
}

REX_EXTERN(__imp__sub_83067F58);
extern "C" REX_FUNC(sub_83067F58) {
  PubLog("f58", ctx);
  __imp__sub_83067F58(ctx, base);
}

// --- VP6 bridge M1b/M2: plane-publish function sub_8277CC98 ------------------
// The movie player's plane publish (row loop -> sub_83067F58 per row,
// lr=8277CD80). r6=width r7=height; r9 = output descriptor with plane
// pointers at +16/+20/+24 (Y/U/V) and dims at +64/+68; YUV420 sizing math
// (w*h*3/2) at entry. NHL_VP6_PUB2 logs args+descriptor -> vp6_pub2.txt.
// NHL_VP6_TESTPAT additionally overwrites the published planes with a
// gradient test pattern AFTER the copy - proving the injection seam (M2).
static std::atomic<int> g_pub2_n{0};

REX_EXTERN(__imp__sub_8277CC98);
extern "C" REX_FUNC(sub_8277CC98) {
  static const bool log_on = std::getenv("NHL_VP6_PUB2") != nullptr;
  static const bool pat_on = std::getenv("NHL_VP6_TESTPAT") != nullptr;
  static const bool bridge_on = [] {
    const char* e = std::getenv("NHL_VP6_BRIDGE");
    return !e || !*e || *e != '0';  // default ON (NHL_VP6_BRIDGE=0 opts out)
  }();
  if (!log_on && !pat_on && !bridge_on) {
    __imp__sub_8277CC98(ctx, base);
    return;
  }
  const uint32_t r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32,
                 r7 = ctx.r7.u32, r8 = ctx.r8.u32, r9 = ctx.r9.u32;
  __imp__sub_8277CC98(ctx, base);
  int n = g_pub2_n.fetch_add(1);
  uint32_t desc[24] = {};
  for (int k = 0; k < 24; ++k) SafeGuestLoadU32(base, r9 + 4 * k, &desc[k]);
  if (log_on && n < 40) {
    FILE* f = std::fopen("vp6_pub2.txt", "a");
    if (f) {
      std::fprintf(f,
                   "pub#%d r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X\n",
                   n, r4, r5, r6, r7, r8, r9);
      std::fprintf(f, "  desc:");
      for (int k = 0; k < 24; ++k) std::fprintf(f, " %08X", desc[k]);
      std::fprintf(f, "\n");
      std::fclose(f);
    }
  }
  // Host-decode bridge: overwrite the published planes with the next
  // ffmpeg-decoded frame (no-op unless NHL_VP6_BRIDGE=1).
  if (r6 >= 320 && r7 >= 180) {
    uint32_t yp = desc[12] ? desc[12] : r6;      // +48 Y pitch
    uint32_t cp = desc[13] ? desc[13] : r6 / 2;  // +52 chroma pitch
    auto host = [&](uint32_t ga) -> uint8_t* {
      return ga ? base + ga + REX_PHYS_HOST_OFFSET(ga) : nullptr;
    };
    Vp6BridgePublish(host(desc[4]), host(desc[5]), host(desc[6]), r6, r7, yp,
                     cp);
  }
  if (pat_on && r6 >= 320 && r7 >= 180) {
    // Overwrite the published Y plane (desc[4] per +16) with a gradient and
    // flatten chroma (desc[5]/desc[6]) to neutral - if the movie shows this
    // pattern, the seam is proven.
    uint32_t w = r6, h = r7;
    uint8_t* y = desc[4] ? base + desc[4] + REX_PHYS_HOST_OFFSET(desc[4]) : nullptr;
    uint8_t* u = desc[5] ? base + desc[5] + REX_PHYS_HOST_OFFSET(desc[5]) : nullptr;
    uint8_t* v = desc[6] ? base + desc[6] + REX_PHYS_HOST_OFFSET(desc[6]) : nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    if (y && VirtualQuery(y, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
      for (uint32_t row = 0; row < h; ++row) {
        std::memset(y + row * w, uint8_t((row * 256) / h), w);
      }
    }
    // Ring chroma pitch is 768 (desc +52/+56), not w/2: cover all rows.
    if (u && VirtualQuery(u, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
      std::memset(u, 128, 768 * (h / 2));
    }
    if (v && VirtualQuery(v, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
      std::memset(v, 128, 768 * (h / 2));
    }
  }
}

// --- VP6 block-recon I/O dump (env NHL_VP6_RECON) -----------------------------
// sub_827C2848 (file 30) is the vectorized (VMX128 float-IDCT) per-block
// reconstruction - located via the plane write trap below: it stores decoded
// pixels (vpkshus128 saturating pack) into the decoder's internal plane,
// which sub_83067DF8 later memcpys into the display ring. Dump r3..r10 and
// 160B of guest memory around each pointer-like register PRE and POST for the
// first 24 calls -> vp6_recon_io.txt, to identify the coefficient block and
// output and diff the math against a host VP3/VP6 reference.
static std::atomic<int> g_recon_n{0};

REX_EXTERN(__imp__sub_827C2848);
extern "C" REX_FUNC(sub_827C2848) {
  static const bool on = std::getenv("NHL_VP6_RECON") != nullptr;
  if (!on || g_recon_n.load(std::memory_order_relaxed) >= 48) {
    __imp__sub_827C2848(ctx, base);
    return;
  }
  constexpr size_t kW = 160;
  const uint32_t regs[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                            ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  uint8_t pre[8][kW], post[8][kW];
  size_t got[8]{};
  for (int i = 0; i < 8; ++i) {
    if (GuestPtrLike(regs[i]))
      got[i] = ReadGuestBytes(base, regs[i], pre[i], kW);
  }
  // Content gate: only dump calls whose inputs carry real AC energy
  // (dequantized coefficients, |v| in [300,8191]) - skips boot/cloud-movie
  // low-AC traffic so the budget lands on the corrupting EA-logo blocks.
  int big = 0;
  for (int i = 5; i < 8; ++i) {
    for (size_t k = 0; k + 1 < got[i]; k += 2) {
      int v = int16_t((pre[i][k] << 8) | pre[i][k + 1]);
      int a = v < 0 ? -v : v;
      if (a >= 300 && a < 8192) ++big;
    }
  }
  int n = big >= 4 ? g_recon_n.fetch_add(1) : 1000;
  if (n >= 48) {
    __imp__sub_827C2848(ctx, base);
    return;
  }
  __imp__sub_827C2848(ctx, base);
  FILE* f = std::fopen("vp6_recon_io.txt", "a");
  if (!f) return;
  std::fprintf(f, "=== recon#%d", n);
  for (int i = 0; i < 8; ++i) std::fprintf(f, " r%d=%08X", i + 3, regs[i]);
  std::fprintf(f, " tick=%lu\n", (unsigned long)GetTickCount());
  for (int i = 0; i < 8; ++i) {
    if (!got[i]) continue;
    ReadGuestBytes(base, regs[i], post[i], kW);
    std::fprintf(f, "r%d.pre :", i + 3);
    for (size_t k = 0; k < got[i]; k += 2)
      std::fprintf(f, " %04X", (pre[i][k] << 8) | pre[i][k + 1]);
    std::fprintf(f, "\n");
    if (std::memcmp(pre[i], post[i], got[i])) {
      std::fprintf(f, "r%d.POST:", i + 3);
      for (size_t k = 0; k < got[i]; k += 2)
        std::fprintf(f, " %04X", (post[i][k] << 8) | post[i][k + 1]);
      std::fprintf(f, "\n");
    }
  }
  std::fclose(f);
}

#if defined(_WIN32)
// Windows-only: relies on a vectored exception handler + VirtualProtect + the
// x64 CONTEXT (Rip). NHL_VP6_TRAP is a one-off RE aid; not ported.
// --- Plane write-origin trap (env NHL_VP6_TRAP=<hex phys addr>) --------------
// Locates WHICH recompiled function writes the VP6 luma plane ring (the two
// known VP3-family IDCTs sub_827D2FE8/sub_82898660 do NOT run during the
// EA-logo movie - verified with NHL_VP6_IDCT counters). Protect one page in
// the middle of the plane, catch the first-write AV in a vectored handler,
// log the host RIP exe-relative (resolve against nhllegacy.pdb offline),
// unprotect and re-arm on a timer -> vp6_trap.txt. Trap runs may show extra
// visual glitches (we bypass the runtime's own watch invalidation) - fine.
static uint32_t Vp6TrapAddr() {
  static uint32_t addr = [] {
    const char* e = std::getenv("NHL_VP6_TRAP");
    return e ? uint32_t(std::strtoull(e, nullptr, 16)) : 0u;
  }();
  return addr;
}
static volatile LONG g_trap_armed = 0;
static uint8_t* g_trap_pages[8] = {};  // host aliases of the same plane page
static int g_trap_page_count = 0;
static std::atomic<int> g_trap_hits{0};

static LONG CALLBACK Vp6TrapHandler(PEXCEPTION_POINTERS xp) {
  if (xp->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  if (!g_trap_armed) return EXCEPTION_CONTINUE_SEARCH;
  uint8_t* fault = (uint8_t*)xp->ExceptionRecord->ExceptionInformation[1];
  int match = -1;
  for (int i = 0; i < g_trap_page_count; ++i) {
    if (fault >= g_trap_pages[i] && fault < g_trap_pages[i] + 4096) {
      match = i;
      break;
    }
  }
  if (match < 0) return EXCEPTION_CONTINUE_SEARCH;
  InterlockedExchange(&g_trap_armed, 0);
  DWORD old;
  for (int i = 0; i < g_trap_page_count; ++i) {
    VirtualProtect(g_trap_pages[i], 4096, PAGE_READWRITE, &old);
  }
  void* rip = (void*)xp->ContextRecord->Rip;
  void* mod = nullptr;
  RtlPcToFileHeader(rip, &mod);
  // Guest-side context of the faulting thread: lr = guest caller of the
  // writing routine (memcpy etc), r3/r4/r5 = its dst/src/len-ish args.
  unsigned long long lr = 0;
  unsigned r3 = 0, r4 = 0, r5 = 0;
  if (auto* ts = rex::runtime::ThreadState::Get(); ts && ts->context()) {
    const PPCContext* c = ts->context();
    lr = (unsigned long long)c->lr;
    r3 = c->r3.u32; r4 = c->r4.u32; r5 = c->r5.u32;
  }
  FILE* f = std::fopen("vp6_trap.txt", "a");
  if (f) {
    std::fprintf(f,
                 "hit#%d rip=%p rel=+0x%llX view=%d lr=%08llX r3=%08X r4=%08X "
                 "r5=%08X fault=%p tick=%lu\n",
                 g_trap_hits.load(), rip,
                 (unsigned long long)((uint8_t*)rip - (uint8_t*)mod), match, lr,
                 r3, r4, r5, fault, (unsigned long)GetTickCount());
    std::fclose(f);
  }
  g_trap_hits.fetch_add(1);
  return EXCEPTION_CONTINUE_EXECUTION;
}

static const int g_vp6_trap_init = [] {
  if (!Vp6TrapAddr()) return 0;
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(20));
    // Install NOW (not at static init): the runtime registers its own
    // first-position vectored handler (physical write-watch) during memory
    // init, which would otherwise sit in front of ours and consume the fault
    // (unprotect + continue) before we ever see it.
    AddVectoredExceptionHandler(1, Vp6TrapHandler);
    auto* ks = rex::runtime::current_kernel_state();
    if (!ks || !ks->memory()) return;
    auto* mem = ks->memory();
    // +0x40000 = mid-plane rows, guaranteed image content. The recompiled
    // code writes through guest-VIRTUAL host views (virtual_membase +
    // heap offsets), not the physical view - protect every alias we can
    // find: scan the 64KB-page virtual space for mappings onto the target
    // physical page, plus the physical view itself as a fallback.
    const uint32_t phys_target = Vp6TrapAddr() + 0x40000;
    FILE* f0 = std::fopen("vp6_trap.txt", "a");
    g_trap_pages[g_trap_page_count++] =
        mem->TranslatePhysical<uint8_t*>(phys_target);
    auto add_page = [&](uint8_t* host, const char* what, uint32_t virt) {
      if (!host || g_trap_page_count >= 8) return;
      for (int i = 0; i < g_trap_page_count; ++i)
        if (g_trap_pages[i] == host) return;
      g_trap_pages[g_trap_page_count++] = host;
      if (f0) std::fprintf(f0, "alias(%s): virt=%08X host=%p\n", what, virt, host);
    };
    // Raw guest-window aliases (recompiled stores compute
    // virtual_membase + guest_addr directly). The 0xE0000000 window is the
    // one the movie player's memcpy actually writes through.
    add_page(mem->virtual_membase() + (0x80000000u | phys_target), "raw80",
             0x80000000u | phys_target);
    add_page(mem->virtual_membase() + (0xA0000000u + phys_target), "rawA0",
             0xA0000000u + phys_target);
    add_page(mem->virtual_membase() + (0xC0000000u + phys_target), "rawC0",
             0xC0000000u + phys_target);
    add_page(mem->virtual_membase() + (0xE0000000u + phys_target), "rawE0",
             0xE0000000u + phys_target);
    if (f0) {
      std::fprintf(f0, "views=%d phys_host=%p\n", g_trap_page_count,
                   g_trap_pages[0]);
      std::fclose(f0);
    }
    for (int i = 0; i < 2400 && g_trap_hits.load() < 200; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      // Re-protect unconditionally: the runtime's own watch machinery
      // (invalidate -> upload -> re-arm, every frame on these pages) resets
      // page protection underneath us, so a one-shot arm goes stale silently.
      bool ok = false;
      DWORD old;
      static int prot_fail_logged = 0;
      for (int k = 0; k < g_trap_page_count; ++k) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(g_trap_pages[k], &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT) {
          continue;
        }
        if (VirtualProtect(g_trap_pages[k], 4096, PAGE_READONLY, &old)) {
          ok = true;
        } else if (prot_fail_logged < 4) {
          ++prot_fail_logged;
          FILE* f = std::fopen("vp6_trap.txt", "a");
          if (f) {
            std::fprintf(f, "protect FAIL view=%d err=%lu state=%lx prot=%lx\n",
                         k, GetLastError(), mbi.State, mbi.Protect);
            std::fclose(f);
          }
        }
      }
      if (ok) {
        InterlockedExchange(&g_trap_armed, 1);
        // One-shot plumbing self-test: write the page from THIS thread; the
        // handler must fire and log a hit with a diag_hooks RIP.
        static bool selftested = false;
        if (!selftested) {
          selftested = true;
          volatile uint8_t* p = g_trap_pages[0];
          *p = *p;
        }
      }
    }
    InterlockedExchange(&g_trap_armed, 0);
    DWORD old;
    for (int k = 0; k < g_trap_page_count; ++k) {
      VirtualProtect(g_trap_pages[k], 4096, PAGE_READWRITE, &old);
    }
  }).detach();
  return 0;
}();

#endif  // _WIN32 (NHL_VP6_TRAP)

// Frame driver tap: dump the codec object (r3) at entry, find plane-candidate
// pointers inside it (physical-alloc range), and dump a slice of each plane
// PRE and POST frame decode -> vp6_frame.txt. Plane slices that change every
// frame with pixel-like bytes = the decoded YUV output (Path A/B pivot data).
// --- VP6 bridge M3: frame-driver chunk tap (env NHL_VP6_CHUNK) ----------------
// Counts sub_8277ABB8 calls and dumps the r4 video-chunk descriptor (256B) +
// r3..r7 per call (cap 12 dumps, all calls counted) -> vp6_chunk.txt. Settles
// per-frame vs per-movie cadence and pins the compressed-payload location for
// the host-decode bridge input.
static std::atomic<int> g_chunk_calls{0}, g_chunk_dumps{0};

static void ChunkTap(PPCContext& ctx, uint8_t* base) {
  int c = g_chunk_calls.fetch_add(1) + 1;
  FILE* f = std::fopen("vp6_chunk.txt", "a");
  if (!f) return;
  std::fprintf(f, "call#%d r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X tick=%lu\n",
               c, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
               (unsigned long)GetTickCount());
  if (g_chunk_dumps.fetch_add(1) < 12 && GuestPtrLike(ctx.r4.u32)) {
    uint8_t buf[256];
    size_t got = ReadGuestBytes(base, ctx.r4.u32, buf, sizeof(buf));
    std::fprintf(f, "  r4:");
    for (size_t k = 0; k < got; k += 4) {
      uint32_t w;
      std::memcpy(&w, buf + k, 4);
      std::fprintf(f, " %08X", _byteswap_ulong(w));
    }
    std::fprintf(f, "\n");
  }
  std::fclose(f);
}

REX_EXTERN(__imp__sub_8277ABB8);
static std::atomic<int> g_vp6_frame_n{0};
extern "C" REX_FUNC(sub_8277ABB8) {
  static const bool chunk_on = std::getenv("NHL_VP6_CHUNK") != nullptr;
  if (chunk_on) ChunkTap(ctx, base);
  Vp6BridgeOnMovieSetup();
  if (Vp6RgbaAddr()) {
    int fn = g_vp6_frame_n.fetch_add(1);
    __imp__sub_8277ABB8(ctx, base);
    DumpVp6Rgba(fn, base);
    return;
  }
  int n = g_vp6_harness ? g_vp6_frame_n.fetch_add(1) : 1000;
  if (n >= 4) {
    __imp__sub_8277ABB8(ctx, base);
    return;
  }
  constexpr size_t kObj = 768;
  uint8_t obj[kObj]{};
  uint32_t r3 = ctx.r3.u32;
  size_t no = GuestPtrLike(r3) ? ReadGuestBytes(base, r3, obj, kObj) : 0;
  // Collect up to 8 distinct plane-candidate pointers from the object: words
  // in [0x70000000, 0x80000000) or [0xBD000000, 0xC0000000).
  uint32_t cand[8]{};
  int nc = 0;
  for (size_t k = 0; k + 4 <= no && nc < 8; k += 4) {
    uint32_t w = (obj[k] << 24) | (obj[k + 1] << 16) | (obj[k + 2] << 8) |
                 obj[k + 3];
    bool planeish = (w >= 0x70000000u && w < 0x80000000u);
    if (!planeish) continue;
    bool dup = false;
    for (int i = 0; i < nc; ++i)
      if (cand[i] == w) dup = true;
    if (!dup) cand[nc++] = w;
  }
  constexpr size_t kPl = 64;
  uint8_t pre[8][kPl]{}, post[8][kPl]{};
  for (int i = 0; i < nc; ++i) ReadGuestBytes(base, cand[i], pre[i], kPl);

  __imp__sub_8277ABB8(ctx, base);

  for (int i = 0; i < nc; ++i) ReadGuestBytes(base, cand[i], post[i], kPl);
  FILE* f = std::fopen("vp6_frame.txt", "a");
  if (!f) return;
  std::fprintf(f, "=== frame#%d r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X\n", n,
               r3, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  if (no) PrintHexBlock(f, "obj", r3, obj, no > 256 ? 256 : no);
  uint8_t d4[96]{}, d5[96]{};
  size_t nd4 = GuestPtrLike(ctx.r4.u32) ? ReadGuestBytes(base, ctx.r4.u32, d4, 96) : 0;
  size_t nd5 = GuestPtrLike(ctx.r5.u32) ? ReadGuestBytes(base, ctx.r5.u32, d5, 96) : 0;
  if (nd4) PrintHexBlock(f, "r4desc", ctx.r4.u32, d4, nd4);
  if (nd5) PrintHexBlock(f, "r5desc", ctx.r5.u32, d5, nd5);
  for (int i = 0; i < nc; ++i) {
    std::fprintf(f, "  plane_cand[%d]=%08X %s\n", i, cand[i],
                 std::memcmp(pre[i], post[i], kPl) ? "CHANGED" : "same");
    PrintHexBlock(f, "  pre ", cand[i], pre[i], 32);
    PrintHexBlock(f, "  post", cand[i], post[i], 32);
  }
  std::fclose(f);
}

REX_EXTERN(__imp__sub_8276AC70);
extern "C" REX_FUNC(sub_8276AC70) {
  if (g_vp6_harness) {
    int hn = g_vp6_n.fetch_add(1);
    if (hn < 24) {
      constexpr size_t kR3 = 96, kR4 = 256, kR5 = 256, kR6 = 128;
      uint8_t pre3[kR3]{}, pre4[kR4]{}, pre5[kR5]{}, pre6[kR6]{};
      uint8_t post4[kR4]{}, post5[kR5]{}, post6[kR6]{};
      uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32,
               r6 = ctx.r6.u32;
      size_t n3 = GuestPtrLike(r3) ? ReadGuestBytes(base, r3, pre3, kR3) : 0;
      size_t n4 = GuestPtrLike(r4) ? ReadGuestBytes(base, r4, pre4, kR4) : 0;
      size_t n5 = GuestPtrLike(r5) ? ReadGuestBytes(base, r5, pre5, kR5) : 0;
      size_t n6 = GuestPtrLike(r6) ? ReadGuestBytes(base, r6, pre6, kR6) : 0;

      __imp__sub_8276AC70(ctx, base);

      if (n4) ReadGuestBytes(base, r4, post4, kR4);
      if (n5) ReadGuestBytes(base, r5, post5, kR5);
      if (n6) ReadGuestBytes(base, r6, post6, kR6);
      FILE* f = std::fopen("vp6_harness.txt", "a");
      if (f) {
        std::fprintf(f,
                     "=== call#%d r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X "
                     "r8=%08X r9=%08X r10=%08X\n",
                     hn, r3, r4, r5, r6, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                     ctx.r10.u32);
        if (n3) PrintHexBlock(f, "r3.pre ", r3, pre3, n3);
        if (n4) {
          PrintHexBlock(f, "r4.pre ", r4, pre4, n4);
          if (std::memcmp(pre4, post4, n4))
            PrintHexBlock(f, "r4.POST", r4, post4, n4);
          else
            std::fprintf(f, "  r4 unchanged\n");
        }
        if (n5) {
          PrintHexBlock(f, "r5.pre ", r5, pre5, n5);
          if (std::memcmp(pre5, post5, n5))
            PrintHexBlock(f, "r5.POST", r5, post5, n5);
          else
            std::fprintf(f, "  r5 unchanged\n");
        }
        if (n6) {
          PrintHexBlock(f, "r6.pre ", r6, pre6, n6);
          if (std::memcmp(pre6, post6, n6))
            PrintHexBlock(f, "r6.POST", r6, post6, n6);
          else
            std::fprintf(f, "  r6 unchanged\n");
        }
        std::fclose(f);
      }
      return;
    }
    __imp__sub_8276AC70(ctx, base);
    return;
  }
  int n = g_vp6_probe ? g_vp6_n.fetch_add(1) : 1000;
  if (n < 6) {
    uint32_t obj = 0, vt = 0, vt0 = 0;
    bool ok = SafeGuestLoadU32(base, 0x83B3AA10u, &obj);  // singleton ptr
    if (obj) SafeGuestLoadU32(base, obj, &vt);            // vtable = *obj
    if (vt) SafeGuestLoadU32(base, vt, &vt0);             // vtable[0] = target
    LogGuestStackHere("vp6_blockdrv", ctx, base);
    FILE* f = std::fopen("vp6_probe.txt", "a");
    if (f) {
      std::fprintf(f,
                   "call#%d read_ok=%d obj=%08X vtable=%08X vtable[0]=%08X | "
                   "r3=%08X r4=%08X r5=%08X r6=%08X\n",
                   n, ok, obj, vt, vt0, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                   ctx.r6.u32);
      std::fprintf(f, "  vtable[0..11]:");
      for (int k = 0; k < 12 && vt; ++k) {
        uint32_t fn = 0;
        if (SafeGuestLoadU32(base, vt + k * 4, &fn))
          std::fprintf(f, " %08X", fn);
        else
          std::fprintf(f, " --------");
      }
      std::fprintf(f, "\n");
      std::fclose(f);
    }
  }
  __imp__sub_8276AC70(ctx, base);
}

// --- Debugging tool 1: in-process guest call-stack sampler (gated off) -------
// Enumerate XThreads via the kernel object table and walk each thread's PPC
// back-chain (old r1 at 0(r1), caller LR saved at -8(old r1) by __savegprlr).
// Two snapshots 10s apart distinguish "blocked" (identical lr) from "running".
// Annotate the output with tools/annotate_guest_stacks.py.
static constexpr bool g_stacks_on = false;

[[maybe_unused]] static void DumpGuestStacksOnce(FILE* f) {
  auto* ks = rex::runtime::current_kernel_state();
  uint8_t* base = g_guest_base.load();
  if (!ks || !base) {
    std::fprintf(f, "(kernel state or guest base unavailable)\n");
    return;
  }
  auto threads = ks->object_table()->GetObjectsByType<rex::system::XThread>();
  std::fprintf(f, "=== %zu guest threads ===\n", threads.size());
  for (auto& t : threads) {
    auto* ts = t->thread_state();
    const PPCContext* c = ts ? ts->context() : nullptr;
    if (!c) continue;
    std::fprintf(f, "tid=%04X name='%s' lr=%08X r1=%08X\n  stack:",
                 t->thread_id(), t->name().c_str(),
                 static_cast<uint32_t>(c->lr), c->r1.u32);
    uint32_t sp = c->r1.u32;
    std::fprintf(f, " %08X", static_cast<uint32_t>(c->lr));
    for (int i = 0; i < 24 && sp >= 0x10000 && sp < 0xC0000000; ++i) {
      uint32_t old_sp = 0, saved_lr = 0;
      if (!SafeGuestLoadU32(base, sp, &old_sp)) break;
      if (old_sp <= sp || old_sp - sp > 0x400000) break;
      if (!SafeGuestLoadU32(base, old_sp - 8, &saved_lr)) break;
      std::fprintf(f, " <-%08X", saved_lr);
      sp = old_sp;
    }
    std::fprintf(f, "\n");
  }
  std::fflush(f);
}

// Walk the CURRENT thread's guest stack from inside a hook (precise: ctx is
// live). Drop a call to this into any REX_FUNC hook to print its call chain.
[[maybe_unused]] static void LogGuestStackHere(const char* tag,
                                               PPCContext& ctx, uint8_t* base) {
  char buf[640];
  int len = std::snprintf(buf, sizeof(buf), "%08X",
                          static_cast<uint32_t>(ctx.lr));
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 16 && sp >= 0x10000 && sp < 0xC0000000; ++i) {
    uint32_t old_sp = 0, saved_lr = 0;
    if (!SafeGuestLoadU32(base, sp, &old_sp)) break;
    if (old_sp <= sp || old_sp - sp > 0x400000) break;
    if (!SafeGuestLoadU32(base, old_sp - 8, &saved_lr)) break;
    len += std::snprintf(buf + len, sizeof(buf) - len, " <-%08X", saved_lr);
    if (len >= (int)sizeof(buf) - 16) break;
    sp = old_sp;
  }
  REXKRNL_WARN("[diag] {} stack: {}", tag, buf);
}

static const int g_stack_sampler_started = [] {
  if (!g_stacks_on) return 0;
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    FILE* f = std::fopen("guest_stacks.txt", "w");
    if (!f) return;
    std::fprintf(f, "--- snapshot A (t=30s) ---\n");
    DumpGuestStacksOnce(f);
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::fprintf(f, "--- snapshot B (t=40s) ---\n");
    DumpGuestStacksOnce(f);
    std::fclose(f);
  }).detach();
  return 0;
}();

// --- Debugging tool 2: single-frame GPU trace dumper (gated off) -------------
// Continuous trace_gpu_stream corrupts past a 2GB total (boot+menus fill it
// before reaching in-game). RequestFrameTrace() dumps ONE frame to a tiny .xtr
// instead. To capture in-game frames: set g_ft_on=true + rebuild — the timer
// dumps one frame every 3s for ~4 min starting 45s in, so holding on a target
// view (e.g. black equipment) catches it. Needs trace_gpu_prefix in
// nhllegacy_app.h. Each dump = ~1-37MB and parses standalone; no 2GB issue.
static constexpr bool g_ft_on = false;
static const int g_frame_trace_timer = [] {
  if (!g_ft_on) return 0;
  std::thread([] {
    // VP6 boot movie plays in the first ~15s — capture early + frequently.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* gs =
        static_cast<rex::graphics::GraphicsSystem*>(rt->graphics_system());
    if (!gs) return;
    for (int i = 0; i < 40; ++i) {
      gs->RequestFrameTrace();
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }
  }).detach();
  return 0;
}();

// --- Diagnostic tool 3: team-color palette dump (Phase A, gated on) ----------
// The tint-black equipment is fed by a packed team-color palette in guest
// memory near 0x1d2d0000 (4-byte RGBA entries): most white (no-tint), ~9 come
// out BLACK (0,0,0,{0,1,4}) = the broken team colors. This scans the region in
// a full game to (a) confirm the address is stable this run and (b) capture the
// exact addresses of the black entries, so the Phase-B write-watch can be
// narrowed to those pages. Dumps every "interesting" word (not pure-0, not
// 0xFFFFFFFF) with its address -> palette_dump.txt at three in-game samples.
static constexpr bool g_palette_dump = false;

[[maybe_unused]] static void DumpPaletteRegion(FILE* f, uint32_t lo,
                                               uint32_t hi) {
  auto* ks = rex::runtime::current_kernel_state();
  if (!ks || !ks->memory()) {
    std::fprintf(f, "  (no memory)\n");
    return;
  }
  auto* mem = ks->memory();
  int white = 0, zero = 0, black = 0, color = 0, listed = 0, committed_pages = 0;
  for (uint32_t pa = lo; pa < hi; pa += 0x1000) {
    uint8_t* phost = mem->TranslatePhysical<uint8_t*>(pa);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(phost, &mbi, sizeof(mbi))) continue;
    if (mbi.State != MEM_COMMIT) continue;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
    committed_pages++;
    for (uint32_t off = 0; off < 0x1000; off += 4) {
      uint32_t raw;
      std::memcpy(&raw, phost + off, 4);
      uint32_t v = _byteswap_ulong(raw);
      if (v == 0xFFFFFFFF) { white++; continue; }
      if (v == 0) { zero++; continue; }
      bool blk = ((v & 0x00FFFFFFu) == 0) || ((v & 0xFFFFFF00u) == 0);
      if (blk) black++; else color++;
      if (listed < 500) {
        std::fprintf(f, "  @%08X = %08X %s\n", pa + off, v,
                     blk ? "BLACK" : "color");
        listed++;
      }
    }
  }
  std::fprintf(f,
               "  [%08X..%08X committed_pages=%d white=%d zero=%d black=%d "
               "color=%d]\n",
               lo, hi, committed_pages, white, zero, black, color);
  std::fflush(f);
}

static const int g_palette_dump_timer = [] {
  if (!g_palette_dump) return 0;
  std::thread([] {
    const int when[] = {90, 130, 170};
    for (int idx = 0; idx < 3; ++idx) {
      std::this_thread::sleep_for(std::chrono::seconds(
          idx == 0 ? when[0] : when[idx] - when[idx - 1]));
      FILE* f = std::fopen("palette_dump.txt", "a");
      if (!f) continue;
      std::fprintf(f, "=== sample %d (t=%ds) ===\n", idx, when[idx]);
      // Scan a wide physical window around the prior-session palette base.
      DumpPaletteRegion(f, 0x1d000000u, 0x1d400000u);
      std::fclose(f);
    }
  }).detach();
  return 0;
}();
