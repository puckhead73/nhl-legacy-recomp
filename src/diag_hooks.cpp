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

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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
static constexpr bool g_vp6_probe = false;  // VP6 RE paused (SDK-codegen bug)
static std::atomic<int> g_vp6_n{0};

REX_EXTERN(__imp__sub_8276AC70);
extern "C" REX_FUNC(sub_8276AC70) {
  int n = g_vp6_probe ? g_vp6_n.fetch_add(1) : 1000;
  if (n < 6) {
    uint32_t obj = 0, vt = 0, vt0 = 0;
    bool ok = SafeGuestLoadU32(base, 0x83B3AA10u, &obj);  // singleton ptr
    if (obj) SafeGuestLoadU32(base, obj, &vt);            // vtable = *obj
    if (vt) SafeGuestLoadU32(base, vt, &vt0);             // vtable[0] = target
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
