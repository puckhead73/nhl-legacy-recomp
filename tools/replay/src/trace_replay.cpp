// nhl-trace-replay - headless offline replay of a captured .xtr through our
// renderer backend (renderer plan, Phase 4).
//
// The SDK's TraceDump/TracePlayer/TraceReader are not exported from
// rexruntime.dll, so this stands up a headless Runtime + our
// NhlD3D12GraphicsSystem directly and drives the trace through the *exported*
// CommandProcessor API (see xtr_player.cpp). Success criterion: the captured
// frame's draws execute through our backend (NhlD3D12CommandProcessor::IssueDraw)
// with no game running.
//
// Wide entry point (wmain): rexruntime's filesystem helpers call _get_wpgmptr,
// which fail-fasts unless the CRT was wide-initialized.

#define _CRT_SECURE_NO_WARNINGS  // std::getenv

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX  // rex/thread.h uses std::chrono::milliseconds::max()
#include <windows.h>

#include <dbghelp.h>
#include <cstdio>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>

#include "renderer/core/nhl_command_processor.h"
#include "renderer/core/nhl_graphics_system.h"
#include "image_dump.h"
#include "xtr_player.h"

namespace {
namespace fs = std::filesystem;

// Vectored exception handler: on the divide-by-zero (or AV) deep in the D3D12
// draw path, walk and symbolize the faulting stack with dbghelp. We have our own
// PDB; rexruntime has none, so its frames resolve to the nearest exported
// function — enough to pinpoint which GPU routine faults.
LONG CALLBACK CrashStackHandler(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_ACCESS_VIOLATION &&
      code != EXCEPTION_FLT_DIVIDE_BY_ZERO) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  HANDLE proc = GetCurrentProcess();
  HANDLE thr = GetCurrentThread();
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
  SymInitialize(proc, nullptr, TRUE);

  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 sf{};
  sf.AddrPC.Offset = ctx.Rip;
  sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = ctx.Rbp;
  sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = ctx.Rsp;
  sf.AddrStack.Mode = AddrModeFlat;

  fprintf(stderr, "\n*** EXCEPTION 0x%08lX at %016llX — faulting stack ***\n", code,
          static_cast<unsigned long long>(ep->ContextRecord->Rip));
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thr, &sf, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
      break;
    }
    if (sf.AddrPC.Offset == 0) break;

    const DWORD64 modbase = SymGetModuleBase64(proc, sf.AddrPC.Offset);
    char modname[64] = "?";
    IMAGEHLP_MODULE64 mi{};
    mi.SizeOfStruct = sizeof(mi);
    if (modbase && SymGetModuleInfo64(proc, modbase, &mi)) {
      strncpy(modname, mi.ModuleName, sizeof(modname) - 1);
    }
    alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym)) {
      fprintf(stderr, "  %2d  %s!%s + 0x%llX\n", i, modname, sym->Name,
              static_cast<unsigned long long>(disp));
    } else {
      fprintf(stderr, "  %2d  %s + 0x%llX\n", i, modname,
              static_cast<unsigned long long>(sf.AddrPC.Offset - modbase));
    }
  }
  fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;  // let it terminate
}

std::string WideToUtf8(const wchar_t* w) {
  if (!w || !*w) {
    return {};
  }
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) {
    return {};
  }
  std::string s(static_cast<size_t>(n - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  AddVectoredExceptionHandler(1, CrashStackHandler);
  char prog[] = "nhl-trace-replay";
  char* fake_argv[] = {prog, nullptr};
  rex::cvar::Init(1, fake_argv);
  rex::cvar::ApplyEnvironment();
  // File logging (not InitLoggingEarly, which only routes to OutputDebugString)
  // so the runtime's own setup diagnostics land somewhere we can read.
  rex::InitLogging("nhl-trace-replay.log");

  if (argc < 2) {
    fprintf(stderr, "usage: nhl-trace-replay <trace.xtr>\n");
    return 2;
  }
  const std::string trace_path = WideToUtf8(argv[1]);

  // Runtime::SetupVfs requires game_data_root to exist; replay supplies all
  // guest memory from the trace, so any existing dir works. Use the exe folder
  // (guaranteed present/writable — the staged DLLs live there) and put the
  // scratch user/cache trees beside it.
  std::error_code ec;
  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  const fs::path scratch = exe_dir / "replay_scratch";
  fs::create_directories(scratch / "user", ec);
  fs::create_directories(scratch / "cache", ec);

  fprintf(stderr, "[replay] trace=%s\n[replay] game_root=%s\n", trace_path.c_str(),
          exe_dir.string().c_str());
  fflush(stderr);

  rex::Runtime runtime(exe_dir, scratch / "user", {}, scratch / "cache");

  rex::RuntimeConfig config;
  config.graphics = std::make_unique<nhl::graphics::NhlD3D12GraphicsSystem>();
  config.kernel_init = rex::kernel::InitializeKernel;
  // Headless: no audio/input factories, no presentation window. SetupGuestGpu
  // creates the provider with_presentation=false.

  fprintf(stderr, "[replay] calling Runtime::Setup (headless, GPU)...\n");
  fflush(stderr);
  const auto status = runtime.Setup(std::move(config));
  fprintf(stderr, "[replay] Runtime::Setup returned 0x%08X\n", static_cast<uint32_t>(status));
  fflush(stderr);
  if (status != 0) {
    return 1;
  }

  auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime.graphics_system());
  fprintf(stderr, "[replay] graphics_system=%p command_processor=%p\n", static_cast<void*>(gs),
          static_cast<void*>(gs ? gs->command_processor() : nullptr));
  fflush(stderr);
  if (!gs || !gs->command_processor()) {
    fprintf(stderr, "[replay] no command processor — headless GPU did not initialize\n");
    return 1;
  }
  REXLOG_INFO("[replay] backend '{}' live; replaying {}", gs->name(), trace_path);
  fprintf(stderr, "[replay] backend live; replaying...\n");
  fflush(stderr);

  const nhl::replay::ReplayStats stats = nhl::replay::ReplayTrace(*gs, trace_path);
  if (!stats.ok) {
    REXLOG_ERROR("[replay] failed: {}", stats.error);
    return 1;
  }

  auto* ncp = static_cast<nhl::graphics::NhlD3D12CommandProcessor*>(gs->command_processor());
  REXLOG_INFO(
      "[replay] done: frames={} primary_packets={} draw_packets_in_stream={} mem_ranges={} "
      "reg_blocks={} edram={} | backend IssueDraw count={}",
      stats.frames, stats.primary_packets, stats.draw_packets, stats.memory_ranges,
      stats.register_blocks, stats.edram_snapshots, ncp->draws_total());
  fprintf(stdout,
          "replay: %u draw packets in stream, backend executed %llu IssueDraw calls\n",
          stats.draw_packets, static_cast<unsigned long long>(ncp->draws_total()));

  // The display frontbuffer (0x1CF32000) stays black headless (the scaler/present
  // step is skipped). The rendered scene is what the frame's resolves (IssueCopy)
  // wrote to guest memory — RB_COPY_DEST_BASE/PITCH/INFO. Read those and untile
  // that buffer to a PNG. Env overrides: NHL_REPLAY_ADDR=<hex> base, NHL_REPLAY_W/H,
  // NHL_REPLAY_LINEAR=1 linear read.
  {
    auto* rf = gs->register_file();
    const uint32_t dest_base = (*rf)[0x2319];   // RB_COPY_DEST_BASE
    const uint32_t dest_pitch = (*rf)[0x231A];  // RB_COPY_DEST_PITCH
    const uint32_t dest_info = (*rf)[0x231B];   // RB_COPY_DEST_INFO
    const uint32_t pitch_px = dest_pitch & 0x3FFF;
    const uint32_t height_px = (dest_pitch >> 16) & 0x3FFF;
    fprintf(stderr, "[replay] RB_COPY_DEST_BASE=%08X PITCH=%08X (%ux%u) INFO=%08X\n", dest_base,
            dest_pitch, pitch_px, height_px, dest_info);

    auto env_u32 = [](const char* k, uint32_t dflt) -> uint32_t {
      const char* v = std::getenv(k);
      return v ? static_cast<uint32_t>(strtoul(v, nullptr, 0)) : dflt;
    };
    const uint32_t W = env_u32("NHL_REPLAY_W", pitch_px ? pitch_px : 1280u);
    const uint32_t H = env_u32("NHL_REPLAY_H", height_px ? height_px : 720u);
    const bool linear = std::getenv("NHL_REPLAY_LINEAR") != nullptr;

    auto dump = [&](uint32_t base, const std::string& outpath) {
      const uint8_t* fb = gs->memory()->TranslatePhysical<const uint8_t*>(base);
      std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
      size_t nonzero_rgb = 0;
      for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
          // GetTiledOffset2D returns a byte offset for 4bpp (log2=2), no extra *4.
          uint32_t src = linear ? (y * W + x) * 4u
                                : static_cast<uint32_t>(rex::graphics::texture_util::GetTiledOffset2D(
                                      static_cast<int32_t>(x), static_cast<int32_t>(y), W, 2));
          const uint8_t* p = fb + src;
          uint8_t* o = rgba.data() + (static_cast<size_t>(y) * W + x) * 4;
          o[0] = p[1];  // ARGB stored big-endian -> R,G,B,opaque
          o[1] = p[2];
          o[2] = p[3];
          o[3] = 255;
          if (p[1] || p[2] || p[3]) ++nonzero_rgb;
        }
      }
      const bool ok = nhl::replay::WritePng(outpath, W, H, rgba.data());
      fprintf(stderr, "[replay] %s %s (%ux%u @ %08X, %s) nonzero_rgb=%zu/%u\n",
              ok ? "wrote" : "FAILED", outpath.c_str(), W, H, base, linear ? "linear" : "tiled",
              nonzero_rgb, W * H);
    };

    const auto& resolves = ncp->resolve_dest_bases();
    fprintf(stderr, "[replay] %zu resolve(s)\n", resolves.size());
    const char* addr_override = std::getenv("NHL_REPLAY_ADDR");
    if (addr_override) {
      dump(static_cast<uint32_t>(strtoul(addr_override, nullptr, 0)),
           (exe_dir / "replay_frame.png").string());
    } else {
      for (size_t i = 0; i < resolves.size(); ++i) {
        dump(resolves[i], (exe_dir / ("replay_resolve_" + std::to_string(i) + ".png")).string());
      }
      dump(dest_base ? dest_base : 0x1CF32000u, (exe_dir / "replay_frame.png").string());
    }
  }
  return 0;
}
