// nhllegacy - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/flags.h>
#include <rex/ui/presenter.h>

#include "renderer/core/nhl_graphics_system.h"
#include "tools/replay/src/image_dump.h"
#include "tools/replay/src/xtr_player.h"
#include "overall_weights_dump.h"
#include "union_device.h"

// Runtime cvar (defined in rexruntime): when true, guest page 0 is
// no-access. NHL Legacy's file-registration code (sub_82705510) walks an
// empty record table as [0 .. 0] step 24 and reads guest address 8 — legal
// on real hardware where low memory is mapped. Unprotect the zero page to
// match console behavior.
REXCVAR_DECLARE(bool, protect_zero);
REXCVAR_DECLARE(bool, scribble_heap);
REXCVAR_DECLARE(bool, vsync);
REXCVAR_DECLARE(bool, gpu_allow_invalid_fetch_constants);
// D3D12 render-target path: "rtv" (fast host path) vs "rov" (rasterizer-
// ordered-views, accurate per-sample EDRAM — handles the in-game scene's
// predicated tiling/overlapping resolves the rtv path renders black).
REXCVAR_DECLARE(std::string, render_target_path_d3d12);
// TEMP black-screen diagnosis: dump the guest PM4 command stream so we can
// count draw packets (DRAW_INDX / DRAW_INDX_2) vs swap-only frames.
REXCVAR_DECLARE(bool, trace_gpu_stream);
REXCVAR_DECLARE(std::string, trace_gpu_prefix);
// Phase 2 enhancement: internal render-resolution (supersampling) scale.
// Xenia's draw_resolution_scale_x/y; 2 => 2x2 the native 1280x720 internal
// buffers for a sharper image. GPU/VRAM cost; auto-clamped to GPU max.
REXCVAR_DECLARE(int32_t, draw_resolution_scale_x);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_y);
// Host window logical size (Xenia heritage cvars). The SDK's SetupPresentation
// creates the window at a hardcoded 1280x720 and has no resize API yet, so
// these may be ignored by 0.8.0 - verified at runtime.
REXCVAR_DECLARE(int32_t, window_width);
REXCVAR_DECLARE(int32_t, window_height);

// Win32 process-exit primitives (declared directly to avoid pulling <windows.h>
// into this widely-included header). Used only by the replay-mode fast-exit.
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess();
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void* handle, unsigned int code);

class NhllegacyApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NhllegacyApp>(new NhllegacyApp(ctx, "nhllegacy",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Substitute our renderer for the SDK's default D3D12 backend. The base
    // SetupPresentation() set config.graphics to a stock D3D12GraphicsSystem
    // just before calling this hook; replace it with our subclass, which reuses
    // the SDK guest-GPU front-end and (for now) logs-and-delegates every draw.
    config.graphics = std::make_unique<nhl::graphics::NhlD3D12GraphicsSystem>();
    REXCVAR_SET(protect_zero, false);  // see comment at REXCVAR_DECLARE above
    // NHL Legacy reads heap fields it never wrote (sub_82705510 record walk);
    // it relies on allocations being zero-filled like Xenia/Windows provide.
    REXCVAR_SET(scribble_heap, false);
    // Session-7 experiment: the FE script/Flash timeline runs ~5-6x slow with
    // the main loop unthrottled at ~125fps. Pace like a console (60Hz vsync)
    // to test whether the dt divergence is pacing-coupled.
    REXCVAR_SET(vsync, true);
    // Xenia warns "Texture fetch constant has 'invalid' type" on the boot
    // screens and renders anyway with this enabled; testing whether the
    // green flash/overlay artifacts come from skipped fetches.
    REXCVAR_SET(gpu_allow_invalid_fetch_constants, true);
    // Accurate EDRAM path (in-game scene renders black under the rtv path).
    REXCVAR_SET(render_target_path_d3d12, std::string("rov"));
    // Black-screen diagnosis: flip to true to dump the guest PM4 stream to
    // gpu_trace/<title>_stream.xtr (~5MB / 25s boot). Parse with
    // tools/parse_gpu_trace.py / trace_resolve_analysis.py. Xenia accepts the
    // same flags and produces the same format (v1) for side-by-side diffs.
    REXCVAR_SET(trace_gpu_stream, false);
    REXCVAR_SET(trace_gpu_prefix, std::string("gpu_trace/"));
    // --- Resolution: native 1080p, lighter GPU load ---
    // The 360 renders at 1280x720. draw_resolution_scale is an integer
    // multiplier (1 => native 720p, 2 => 1440p supersampled), so there is no
    // exact 1080p internal render. Use native 720p internal (much lower GPU
    // cost than the prior 2x2) and target a 1920x1080 window.
    REXCVAR_SET(draw_resolution_scale_x, 1);
    REXCVAR_SET(draw_resolution_scale_y, 1);
    REXCVAR_SET(window_width, 1920);
    REXCVAR_SET(window_height, 1080);
    // Opt-in D3D12 debug layer (NHL_BETA_D3D12_DEBUG): must be set before the
    // provider creates the device. Used to get the exact device-removal reason
    // while debugging the beta owned-draw path.
    if (std::getenv("NHL_BETA_D3D12_DEBUG")) {
      REXCVAR_SET(d3d12_debug, true);
      REXLOG_INFO("[nhl-beta] D3D12 debug layer enabled via NHL_BETA_D3D12_DEBUG");
    }
    // Opt-in verbose SDK logging (NHL_LOG_LEVEL=debug|trace): surfaces the texture
    // cache's per-texture create/load actions (TextureKey::LogAction) for diagnosing
    // textures that bind but sample zero.
    if (const char* lvl = std::getenv("NHL_LOG_LEVEL")) {
      REXCVAR_SET(log_level, std::string(lvl));
      // The logger tree is already initialized by the time OnPreSetup runs, so the
      // cvar alone is too late — apply the level to all live categories directly.
      rex::SetAllLevels(spdlog::level::from_str(lvl));
      REXLOG_INFO("[nhl-beta] log_level={} via NHL_LOG_LEVEL", lvl);
    }
    // Force the BINDFUL descriptor path under the beta backend. The base
    // D3D12CommandProcessor (which owns root-signature creation) otherwise picks
    // bindless on capable HW (RTX 4080), so ConfigurePipeline hands our owned
    // draw a BINDLESS root signature whose param[5] is a CBV (SystemConstants),
    // not the SharedMemoryAndEdram descriptor table our bindful binding code
    // assumes — SetGraphicsRootDescriptorTable(5) then faults the draw. Bindful
    // makes the base CP, our PipelineCache, and our hand-written binding code
    // agree. Gated on beta so default gameplay keeps the faster bindless path.
    if (const char* be = std::getenv("NHL_BACKEND"); be && std::strcmp(be, "beta") == 0) {
      REXCVAR_SET(d3d12_bindless, false);
      REXLOG_INFO("[nhl-beta] forcing bindful descriptor path (d3d12_bindless=false) for beta");
      // Synchronous pipeline creation (0 creation threads) for beta: the owned-draw
      // takeover otherwise hit an async PSO-creation race (device-removal crash, and
      // root-sig/PSO id=201 mismatches) because the async thread read register state
      // after our per-draw MSAA override was restored. Synchronous = built inline,
      // deterministic. Override with NHL_BETA_PSO_SYNC=<n> for experiments.
      int32_t pso_threads = 0;
      if (const char* v = std::getenv("NHL_BETA_PSO_SYNC")) {
        pso_threads = int32_t(std::strtol(v, nullptr, 10));
      }
      REXCVAR_SET(d3d12_pipeline_creation_threads, pso_threads);
      REXLOG_INFO("[nhl-beta] d3d12_pipeline_creation_threads = {}", pso_threads);
      // Disable sparse/tiled shared memory for beta (full 512 MB buffer). Under
      // RenderDoc (which forces this off) the textured root sig was built correctly
      // (8 params) and the id=708 race disappeared — the sparse-buffer path appears
      // to perturb the shader binding-population timing. NHL_BETA_TILED=1 to re-enable.
      if (!std::getenv("NHL_BETA_TILED")) {
        REXCVAR_SET(d3d12_tiled_shared_memory, false);
        REXLOG_INFO("[nhl-beta] d3d12_tiled_shared_memory = false (full buffer)");
      }
    }
  }

  // End-user installs assembled by tools/packager ship game data in a
  // "game" directory next to the exe and launch with no arguments. Fill the
  // default only when no --game_data_root was provided (cvar empty) so the
  // dev workflow — explicit flag, tools/drive.ps1 — keeps winning.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.game_data_root.empty()) {
      const std::filesystem::path candidate =
          rex::filesystem::GetExecutableFolder() / "game";
      std::error_code ec;
      if (std::filesystem::is_directory(candidate, ec)) {
        paths.game_data_root = candidate;
      }
    }
  }

  // NHL Legacy probes cache:\ during boot; rexruntime 0.8.0 only mounts
  // game: (and update: when present), and the guest boot path crashes in
  // file/volume bookkeeping (sub_82705510) right after the failed cache
  // opens. Mirror Xenia: back cache: with a writable host directory.
  //
  // Loose-file overlay: NHL Legacy's resource loader probes cache:\<logical
  // path> before falling back to the .big archive under game:\. When the
  // extracted loose-asset tree (game_data_root/_compiled) is present, mount it
  // read-only *beneath* the writable scratch dir via a UnionDevice, so the
  // guest serves loose files (easy replacement) while its own cache writes stay
  // isolated in cache_dir. (The X360 .big files are EA "EB\0\3" — un-reversed,
  // LZX-compressed — so overlaying loose files is how we avoid repacking them.)
  void OnPostSetup() override {
    namespace fs = rex::filesystem;
    const std::filesystem::path cache_dir = cache_root() / "guest";
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);

    const std::filesystem::path loose_root = game_data_root() / "_compiled";
    // Loose .dds texture overrides (converted to .rx2 at load via tdb-rx2-ffi).
    const std::filesystem::path tex_override_root = game_data_root() / "_loose_tex";
    const bool overlaid = std::filesystem::is_directory(loose_root, ec);

    nhllegacy::UnionDevice* union_dev = nullptr;
    std::unique_ptr<fs::Device> device;
    if (overlaid) {
      auto ud = std::make_unique<nhllegacy::UnionDevice>(
          "\\CACHE", cache_dir, loose_root, tex_override_root);
      union_dev = ud.get();  // stays valid after the move into the VFS
      device = std::move(ud);
    } else {
      device = std::make_unique<fs::HostPathDevice>("\\CACHE", cache_dir,
          /*read_only=*/false);
    }

    auto* vfs = runtime()->file_system();
    if (device->Initialize() && vfs->RegisterDevice(std::move(device))) {
      vfs->RegisterSymbolicLink("cache:", "\\CACHE");
      if (overlaid) {
        // The union mount succeeds on the writable upper layer alone, so only claim
        // the loose overlay is active when the read-only lower layer actually
        // initialized — otherwise loose-file overrides silently do nothing.
        if (union_dev && union_dev->lower_active()) {
          // Verified end-to-end: the guest's per-subfile cache:\<path> lookups
          // (rendering/*.rx2, attribdb/*.vlt, fe/*.bin, ...) now resolve to loose
          // files in _compiled via this union, while whole-archive probes
          // (cache:\*.big) miss and fall back to game:\ as before. Texture .rx2
          // with loose .dds overrides under _loose_tex are converted at load.
          REXLOG_INFO("[loose-overlay] mounted {} read-only beneath cache: {}",
                      loose_root.string(), cache_dir.string());
        } else {
          REXLOG_WARN("[loose-overlay] loose layer UNAVAILABLE ({}); cache: is "
                      "writable-only and loose-file overrides will NOT apply",
                      loose_root.string());
        }
      }
    }
  }

  // Replay-and-screenshot mode. With NHL_REPLAY_XTR=<frame.xtr>, replay the
  // captured frame through the live backend + presenter (instead of launching
  // the guest), read back the presented image, and write replay_frame.png next
  // to the exe. Reuses the host's fully set-up window/presenter/command
  // processor, so the resolves + scaler complete — unlike the headless tool,
  // where IssueSwap had no presenter to flush into (black output).
  void LaunchModule() override {
    // Stage B of the Overall-formula hunt (overall_weights_dump.h). With
    // NHL_DUMP_OVERALL_WEIGHTS set, the image is already mapped into guest
    // memory by Setup, so scan it for the gCardOverallWeights_* anchors +
    // weight arrays and fast-exit WITHOUT launching the guest (no window, no
    // game loop). Reuses the NHL_REPLAY_XTR fast-exit discipline below.
    // Runtime variant: let the guest boot so its tweak registration runs, then
    // scan committed guest memory for the now-materialised name->value links
    // and dump the weight floats. Spawn the watcher, then fall through to the
    // normal LaunchModule so the guest actually starts registering.
    if (const char* rt = std::getenv("NHL_DUMP_OVERALL_RUNTIME"); rt && *rt) {
      auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
      const uint8_t* vbase =
          (gs && gs->memory()) ? gs->memory()->TranslateVirtual<const uint8_t*>(0u)
                               : nullptr;
      unsigned delay_ms = 12000;
      if (const char* d = std::getenv("NHL_DUMP_DELAY_MS"); d && *d) {
        delay_ms = static_cast<unsigned>(strtoul(d, nullptr, 0));
      }
      const std::string out =
          (rex::filesystem::GetExecutableFolder() / "overall_weights_runtime.txt")
              .string();
      if (vbase) {
        std::thread([vbase, out, delay_ms]() {
          std::fprintf(stderr, "[ovr-rt] waiting %u ms for tweak registration...\n",
                       delay_ms);
          ::Sleep(delay_ms);
          nhllegacy::DumpOverallWeightsRuntime(vbase, out.c_str());
          rex::ShutdownLogging();
          ::TerminateProcess(::GetCurrentProcess(), 0u);
        }).detach();
      } else {
        std::fprintf(stderr, "[ovr-rt] no memory base; cannot scan\n");
      }
      rex::ReXApp::LaunchModule();
      return;
    }

    if (const char* dump = std::getenv("NHL_DUMP_OVERALL_WEIGHTS"); dump && *dump) {
      auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
      if (gs && gs->memory()) {
        constexpr uint32_t kImageBase = 0x82000000u;
        constexpr uint32_t kImageSize = 0x1EA0000u;
        const uint8_t* img =
            gs->memory()->TranslateVirtual<const uint8_t*>(kImageBase);
        const std::string out =
            (rex::filesystem::GetExecutableFolder() / "overall_weights_dump.txt")
                .string();
        nhllegacy::DumpOverallWeights(img, kImageBase, kImageSize, out.c_str());
      } else {
        std::fprintf(stderr, "[ovr-dump] no graphics/memory system available\n");
      }
      rex::ShutdownLogging();
      ::TerminateProcess(::GetCurrentProcess(), 0u);
    }

    const char* xtr = std::getenv("NHL_REPLAY_XTR");
    if (!xtr || !*xtr) {
      rex::ReXApp::LaunchModule();
      return;
    }
    const std::string xtr_path = xtr;
    // Defer to the UI thread (presenter fully live), then replay on a background
    // thread (it blocks awaiting the GPU worker), keeping the UI message loop
    // pumping meanwhile.
    app_context().CallInUIThreadDeferred([this, xtr_path]() {
      replay_thread_ = std::thread([this, xtr_path]() {
        const bool ok = ReplayAndCapture(xtr_path);
        // Replay mode never launched the guest module, so the normal
        // app/kernel/GPU teardown path — which assumes a running guest lifecycle
        // — double-frees and corrupts the heap on exit (0xC0000374 in ntdll,
        // after the artifact is already written). The PNG is on disk; flush the
        // log and terminate the process directly, bypassing the broken teardown.
        // Exit code reflects capture success so automation can detect a failure.
        // (Normal game runs are unaffected: this branch is only taken under
        // NHL_REPLAY_XTR.)
        rex::ShutdownLogging();
        ::TerminateProcess(::GetCurrentProcess(), ok ? 0u : 1u);
      });
    });
  }

  void OnShutdown() override {
    // Only reached on the normal (non-replay) path; replay mode fast-exits above.
    if (replay_thread_.joinable()) {
      replay_thread_.join();
    }
  }

  // Other available hooks:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnCreateDialogs(ui::ImGuiDrawer* drawer) override {}

 private:
  // Returns true only if the replay produced and wrote the PNG, so the caller can
  // surface a nonzero exit code on failure (CI/automation otherwise sees success).
  bool ReplayAndCapture(const std::string& xtr_path) {
    auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
    if (!gs) {
      REXLOG_ERROR("[nhl-replay] no graphics system");
      return false;
    }
    const nhl::replay::ReplayStats stats = nhl::replay::ReplayTrace(*gs, xtr_path);
    REXLOG_INFO(
        "[nhl-replay] {}: {} draw packets, {} leaf packets, {} resolves, {} frames "
        "(skipped {} wait_reg_mem, {} empty)",
        xtr_path, stats.draw_packets, stats.primary_packets, stats.memory_ranges, stats.frames,
        stats.skipped_wait_reg_mem, stats.skipped_empty_leaves);

    auto* presenter = gs->presenter();
    if (!presenter) {
      REXLOG_ERROR("[nhl-replay] no presenter to capture from");
      return false;
    }
    rex::ui::RawImage img;
    if (!presenter->CaptureGuestOutput(img) || img.width == 0 || img.height == 0) {
      REXLOG_ERROR("[nhl-replay] CaptureGuestOutput failed/empty ({}x{})", img.width, img.height);
      return false;
    }
    // RawImage is R8G8B8X8 with a row stride; repack tight RGBA (opaque).
    std::vector<uint8_t> rgba(static_cast<size_t>(img.width) * img.height * 4);
    for (uint32_t y = 0; y < img.height; ++y) {
      for (uint32_t x = 0; x < img.width; ++x) {
        const uint8_t* s = img.data.data() + static_cast<size_t>(y) * img.stride + x * 4;
        uint8_t* d = rgba.data() + (static_cast<size_t>(y) * img.width + x) * 4;
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = 255;
      }
    }
    const std::string out =
        (rex::filesystem::GetExecutableFolder() / "replay_frame.png").string();
    if (nhl::replay::WritePng(out, img.width, img.height, rgba.data())) {
      REXLOG_INFO("[nhl-replay] wrote {} ({}x{})", out, img.width, img.height);
      return true;
    }
    REXLOG_ERROR("[nhl-replay] failed to write {}", out);
    return false;
  }

  std::thread replay_thread_;
};
