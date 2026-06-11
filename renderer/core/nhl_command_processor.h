// nhllegacy custom renderer — command processor
//
// Sprint-1 "log-and-delegate" command processor. Every override forwards to the
// base D3D12CommandProcessor, so rendering is bit-identical to the stock backend.
// The point is observation: we count the per-draw / per-copy / per-shader-load
// callbacks the SDK front-end hands us and emit a one-line summary per frame
// (at IssueSwap), proving the injection seam works against the live game and
// giving us the real NHL Legacy draw-stream shape before any rendering code is
// written.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <memory>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d12/d3d12_api.h>

// Forward declarations for the Tier-1 "beta" backend's own cache instances. The
// concrete definitions (heavy D3D12 headers) are pulled in only by the .cpp, so
// the unique_ptr members below require a user-declared destructor defined there.
namespace rex::graphics {
class DxbcShaderTranslator;
namespace d3d12 {
class D3D12SharedMemory;
class D3D12RenderTargetCache;
class D3D12TextureCache;
class D3D12PrimitiveProcessor;
class PipelineCache;
class D3D12Shader;
}  // namespace d3d12
}  // namespace rex::graphics

namespace rex::ui {
class RenderDocAPI;
}  // namespace rex::ui

namespace nhl::graphics {

class NhlD3D12CommandProcessor : public rex::graphics::d3d12::D3D12CommandProcessor {
 public:
  // Inherit (D3D12GraphicsSystem*, KernelState*) constructor.
  using rex::graphics::d3d12::D3D12CommandProcessor::D3D12CommandProcessor;

  // Defined in the .cpp (where the beta cache types are complete) so the
  // unique_ptr<incomplete> members can be destroyed.
  ~NhlD3D12CommandProcessor() override;

  // Lifetime IssueDraw count. Used by the offline replay tool to confirm that a
  // captured frame's draws actually executed through our backend (it has no
  // window/swap, so the per-frame summary may not fire).
  uint64_t draws_total() const { return draws_total_; }

  // RB_COPY_DEST_BASE captured at each IssueCopy (resolve) — lets the offline
  // replay tool dump every resolve target, not just the last one in the regs.
  const std::vector<uint32_t>& resolve_dest_bases() const { return resolve_dest_bases_; }

 protected:
  // SetupContext/ShutdownContext are the GPU-device lifecycle seam the abstract
  // CommandProcessor calls around the backend. In the default (alpha / "delegate")
  // path they forward straight to the base D3D12 backend, so live rendering is
  // unchanged. Under NHL_BACKEND=beta they additionally run the Tier-1 "owned
  // backend" feasibility probe (see nhl_command_processor.cpp / Q1 in
  // docs/tier1-backend-build-order.md): construct, Initialize, Shutdown and
  // destruct our OWN D3D12SharedMemory instance to confirm the SDK cache
  // ctors/Initialize/Shutdown symbols are exported and usable from this consumer
  // TU — the load-bearing assumption of the beta ("own CP, reuse the final D3D12
  // caches") architecture. The probe is transient and leaves live rendering fully
  // on the base path; it does NOT yet take over the draw path.
  bool SetupContext() override;
  void ShutdownContext() override;

  bool IssueDraw(rex::graphics::xenos::PrimitiveType primitive_type, uint32_t index_count,
                 rex::graphics::CommandProcessor::IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;
  bool IssueCopy() override;
  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType shader_type,
                                    uint32_t guest_address, const uint32_t* host_address,
                                    uint32_t dword_count) override;
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;
  // The trace player calls this on every replayed guest-memory write. The base
  // invalidates ITS OWN shared memory + primitive processor here (disasm of
  // rexruntimerd.dll TracePlaybackWroteMemory @0x176850), so on the next draw it
  // re-uploads the freshly written guest RAM. Our beta caches are SEPARATE
  // instances that never receive these invalidations, so their pages stay "valid"
  // (never re-uploaded) and texture untiling reads an empty buffer -> black
  // textures. Override to mirror the base: invalidate the beta shared memory +
  // primitive processor with the identical (start, length, exact_range=true) call.
  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  // The trace player restores the captured EDRAM contents once at stream start
  // (TraceCommandType::kEdramSnapshot). The base routes it to ITS render_target_cache_,
  // but our beta cache is a SEPARATE instance whose EDRAM buffer therefore stays
  // uninitialized: host RTs load garbage on the first transfer -> red/uncleared color
  // and depth==0, so the LESS depth-test culls every 3D (z-enabled) draw while the 2D
  // menu (no depth) survives. That is exactly the "menu renders, player model missing,
  // red background" EDRAM symptom. Override to mirror the snapshot onto the beta cache
  // and cache the bytes so we can re-seed each beta frame (avoids cross-frame drift in
  // the single-frame replay loop). Takeover+EDRAM only; otherwise just the base call.
  void RestoreEdramSnapshot(const void* snapshot) override;

 private:
  // NHL_BACKEND=beta: Tier-1 owned-backend bring-up. Detected once (env read) at
  // SetupContext time, before any draw.
  bool beta_enabled_ = false;

  // NHL_BETA_TAKEOVER: full-frame takeover. IssueDraw stops delegating to the base
  // and renders our own work into the already-open submission; with no base draws
  // there is no shared-command-list contamination. The first draw is rendered by
  // our backend; the rest no-op. Output captured via the offscreen RT readback.
  bool beta_takeover_ = false;
  bool beta_takeover_draw_done_ = false;
  // Multi-draw takeover: clear the offscreen RT on the first draw, then accumulate
  // every draw into it; the copy-to-readback is recorded once per frame in
  // FinalizeBetaTakeoverCapture() at IssueSwap. Textured draws are skipped for now
  // (their texture/sampler root params aren't bound yet — binding them is the next
  // step) so untextured geometry renders without unbound-descriptor faults.
  bool beta_takeover_cleared_ = false;
  // Counts the trace player's replayed memory writes that we mirrored onto the beta
  // caches (TracePlaybackWroteMemory override). Logged at the first owned draw to
  // confirm the beta shared memory actually received the trace's guest-RAM writes
  // (the fix for empty/black textures). Zero here = the override never fired =>
  // textures will still be black.
  uint64_t beta_trace_writes_seen_ = 0;
  uint64_t beta_trace_write_bytes_ = 0;
  uint32_t beta_takeover_rendered_ = 0;
  uint32_t beta_takeover_skipped_textured_ = 0;
  uint32_t beta_takeover_draw_seen_ = 0;  // total takeover draws seen (for NHL_BETA_MAX_DRAW)
  bool beta_ucode_dumped_ = false;        // dump textured VS/PS ucode disasm once
  void FinalizeBetaTakeoverCapture();
  // Renders (or, in bring-up, clears) the owned draw into our offscreen RT.
  void RenderBetaOwnedDraw(rex::graphics::xenos::PrimitiveType primitive_type, uint32_t index_count,
                           rex::graphics::CommandProcessor::IndexBufferInfo* index_buffer_info);
  // Loose-asset texture injection (replay only): some textures are never written
  // by the GPU trace (static assets cached before capture), so guest RAM is zero
  // at their fetch-constant base and they render black. NHL_BETA_INJECT supplies
  // "<guestAddr>=<_compiled-relpath.rx2>[:slot];..." entries; on the first owned
  // draw we decode each .rx2 slot's raw tiled payload (rx2ffi_decode_slot) and
  // write it into guest RAM at the address, then invalidate it like a trace write
  // so the untile re-uploads. Takeover/replay only; no-op when the env is unset.
  void MaybeInjectBetaTextures();
  bool beta_inject_parsed_ = false;        // parse NHL_BETA_INJECT once
  bool beta_inject_done_ = false;          // inject once (first owned draw)
  struct BetaInjectEntry {
    uint32_t addr = 0;
    std::string relpath;
    uint32_t slot = 0;
  };
  std::vector<BetaInjectEntry> beta_inject_entries_;

  // Stage-1 auto-mapping (NHL_INJECT_CORRELATE): during a capture/replay frame where
  // guest RAM IS populated, hash each texture fetch-constant's guest RAM and look it
  // up in the injection registry (built from loose .rx2 assets), recording
  // {addr -> relpath, slot}. Emitted as <trace>.inject.json by WriteInjectSidecar at
  // capture/replay finalize, then loaded by MaybeInjectBetaTextures on a later replay.
  // Env-gated; the default/live path never calls this.
  void CorrelateTexturesForInjection();
  void WriteInjectSidecar();
  bool inject_correlate_enabled_ = false;   // NHL_INJECT_CORRELATE seen (cached)
  bool inject_correlate_checked_ = false;   // env checked once
  bool inject_registry_scanned_ = false;    // ScanDirectory done once (replay side)
  bool inject_sidecar_written_ = false;     // emit once
  std::unordered_set<uint32_t> inject_correlate_seen_;  // bases already hashed
  // If the device was removed, log the reason + DRED page-fault VA / allocation
  // name (identifies which resource was accessed out of bounds).
  void LogBetaDeviceRemoval();

  // Phase 2 (docs/tier1-backend-build-order.md): construct our OWN instances of
  // the SDK's five concrete D3D12 caches and hold them alive alongside the base's
  // (which still drives live rendering, since draws delegate). Built in dependency
  // order in SetupContext; torn down (reverse order) in ShutdownContext BEFORE the
  // base tears down the device they reference. Inert for now — not yet driving any
  // draw. Returns false (and logs) on the first cache that fails to initialize.
  bool BuildBetaCaches();
  void ShutdownBetaCaches();

  // Declared in dependency order; destroyed in reverse (member order) — but we
  // reset them explicitly in ShutdownContext so they die before the base device.
  std::unique_ptr<rex::graphics::d3d12::D3D12SharedMemory> beta_shared_memory_;
  std::unique_ptr<rex::graphics::d3d12::D3D12RenderTargetCache> beta_render_target_cache_;
  std::unique_ptr<rex::graphics::d3d12::D3D12TextureCache> beta_texture_cache_;
  std::unique_ptr<rex::graphics::d3d12::D3D12PrimitiveProcessor> beta_primitive_processor_;
  std::unique_ptr<rex::graphics::d3d12::PipelineCache> beta_pipeline_cache_;

  // Phase 3 (docs/tier1-backend-build-order.md): validate that every real NHL
  // shader translates to DXBC through our own pipeline. In our LoadShader we load
  // the ucode into our pipeline cache, analyze it, and translate a default
  // modification with our own DxbcShaderTranslator — logging coverage. This
  // exercises the hardest REUSED piece (the public ucode->DXBC translator) on real
  // draws without touching the live command list, so it can run alongside the base
  // (which still renders). Translator is created lazily (needs the provider's
  // vendor id + the RT cache's chosen EDRAM path / MSAA support).
  void ValidateBetaShaderTranslation(rex::graphics::xenos::ShaderType type,
                                     const uint32_t* host_address, uint32_t dword_count);
  std::unique_ptr<rex::graphics::DxbcShaderTranslator> beta_translator_;
  uint32_t beta_xlat_ok_ = 0;
  uint32_t beta_xlat_fail_ = 0;

  // Current shaders for the next draw: LoadShader fires per-bind, so the last VS
  // and last PS loaded before a draw are the ones it uses. Tracked in
  // ValidateBetaShaderTranslation (they're our pipeline cache's analyzed shaders).
  rex::graphics::d3d12::D3D12Shader* beta_current_vs_ = nullptr;
  rex::graphics::d3d12::D3D12Shader* beta_current_ps_ = nullptr;

  // 4b (docs/tier1-backend-build-order.md): our OWN offscreen render target +
  // readback, the beta render-output pipeline. Independent of the EDRAM/RT cache so
  // we can drive draws to it and read pixels back to compare against the alpha
  // oracle. Created lazily; the clear-test proves create -> command-list drive ->
  // barrier -> copy -> CPU readback -> PNG end to end (no base contamination — a
  // clear via the RTV does not rebind the command list's render targets).
  bool CreateBetaOffscreenTarget(uint32_t width, uint32_t height);
  void DumpBetaOffscreenTarget(const char* path);  // awaits, maps, writes PNG
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_offscreen_rt_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> beta_rtv_heap_;
  // D3D12 debug-layer message queue (when NHL_BETA_D3D12_DEBUG); drained to the log
  // to capture the exact validation error.
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> beta_info_queue_;
  void DrainBetaInfoQueue(const char* when);
  // Shader-visible SAMPLER heap for the owned draw's sampler descriptor table. The
  // CBV/SRV/UAV side reuses the CP's global view heap (see RenderBetaOwnedDraw), not
  // a beta-owned heap.
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> beta_sampler_heap_;
  // NHL_BETA_RENDERDOC: programmatic RenderDoc capture bracketing the takeover
  // frame (StartFrameCapture before the first owned draw, EndFrameCapture after the
  // swap) so the failing textured DrawInstanced can be inspected in the RenderDoc
  // UI (actual bound root signature, descriptor tables, command order). Only works
  // when the process is launched through RenderDoc (CreateIfConnected succeeds).
  std::unique_ptr<rex::ui::RenderDocAPI> beta_renderdoc_;
  bool beta_rdoc_capturing_ = false;
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_readback_;
  // NHL_BETA_DEPTH: depth-stencil target for 3D scenes. The menu/intro are 2D
  // (painter order), but gameplay/arena geometry must depth-sort or far surfaces
  // overdraw near ones. Created lazily on the first owned draw from the guest
  // RB_DEPTH_INFO.depth_format (so the DXGI format matches the PSO ConfigurePipeline
  // builds); bound in OMSetRenderTargets and cleared once per capture. Per-draw depth
  // test/write still follow the guest RB_DEPTHCONTROL (UI draws with z_enable=0 just
  // don't test), so binding it for every draw is correct. Env-gated; off => unchanged.
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_depth_rt_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> beta_dsv_heap_;
  bool beta_depth_enabled_ = false;    // NHL_BETA_DEPTH (cached once)
  bool beta_depth_checked_ = false;    // env checked
  bool beta_depth_ready_ = false;      // resource + DSV created
  bool beta_depth_cleared_ = false;    // cleared this capture
  bool beta_rtcache_ticked_ = false;   // RT-cache submission lifecycle ticked (depth path)
  uint64_t beta_rtcache_submission_ = UINT64_MAX;  // submission the RT cache was last begun for
  uint32_t beta_depth_xenos_fmt_ = 1;  // xenos::DepthRenderTargetFormat (kD24FS8 default)

  // Phase 5 (docs/tier1-backend-build-order.md): NHL_BETA_EDRAM — route the owned-draw
  // path through the SDK's D3D12RenderTargetCache so EDRAM render-target binding,
  // resolve-to-texture, and per-pass restore happen per the guest stream, instead of
  // bypassing it with a single hand-rolled offscreen RTV. Required for MULTI-PASS 3D
  // scenes (scene_04): early reflection/shadow passes resolve to guest textures that a
  // late composite samples; without resolves those textures are absent and the frame is
  // a crushed band + leaked opaque planes. Set => Update() owns color+depth binding (no
  // offscreen RTV, no MSAA-1X force, standard viewport), IssueCopy honors the guest
  // resolves (RTCache::Resolve), and the final frame is captured from the resolved
  // frontbuffer guest texture (RequestSwapTexture). The single-pass menu/intro paths do
  // NOT set this, so they stay byte-identical. Takeover-replay only; cached at SetupContext.
  bool beta_edram_enabled_ = false;
  // Cached EDRAM snapshot (RestoreEdramSnapshot) so the beta cache can be re-seeded at
  // the start of each beta submission/frame — the replay loops a single frame, and
  // without a per-frame re-seed the beta EDRAM accumulates each frame's depth/color
  // store-backs and the model self-occludes on frame 2+. Sized to xenos::kEdramSizeBytes.
  std::vector<uint8_t> beta_edram_snapshot_;
  bool beta_edram_snapshot_valid_ = false;
  // Honors a guest resolve (a kCopy draw) on the beta caches: RTCache::Resolve dumps the
  // current pass's host RT into the beta shared-memory guest texture so later passes /
  // the composite sample it. Driven from IssueDraw (kCopy draws are intercepted there).
  void BetaResolveEdram();
  uint32_t beta_resolves_seen_ = 0;
  // Final-frame capture for the EDRAM path: the visible image is the frontbuffer guest
  // texture the last resolve wrote into beta_shared_memory_; RequestSwapTexture untiles it
  // to a host texture which we copy to this readback buffer and write as beta_owned_draw.png.
  void CaptureBetaEdramSwap();             // records swap-tex copy into the open submission
  void DumpBetaEdramSwap(const char* path);  // maps the readback + writes the PNG
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_swap_readback_;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT beta_swap_footprint_{};
  uint64_t beta_swap_total_bytes_ = 0;
  uint32_t beta_swap_w_ = 0;
  uint32_t beta_swap_h_ = 0;
  DXGI_FORMAT beta_swap_fmt_ = DXGI_FORMAT_UNKNOWN;
  // RequestSwapTexture returns the untiled frontbuffer as a host R8G8B8A8 resource whose
  // RAW bytes follow the GUEST swizzle (k_8_8_8_8 frontbuffer => BGRA in memory); the base
  // present applies srv_desc.Shader4ComponentMapping when sampling, but our readback copies
  // raw bytes and must apply the R<->B swap itself. Decoded from the SRV swizzle at capture.
  bool beta_swap_rb_ = false;
  float beta_depth_clear_ = 1.0f;      // far value (NHL_BETA_DEPTH_CLEAR; 0 for reversed-Z)
  bool EnsureBetaDepthTarget();        // create depth resource + DSV (once)
  void DumpBetaDepthStats();           // NHL_BETA_DEPTH_DIAG: read back depth min/max/spread
  // Decisive shared-memory upload probe (NHL_BETA_BIND_DIAG): after RequestRange,
  // copy bytes straight out of our beta shared-memory GPU buffer at a known-
  // populated guest address and compare to the guest RAM there. Non-zero & matching
  // => the upload delivered the data (so any remaining black is an untile/binding
  // issue); zero => RequestRange did NOT populate our buffer (the upload is the bug).
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_shmem_probe_buf_;
  uint32_t beta_shmem_probe_addr_ = 0;
  bool beta_shmem_probe_pending_ = false;
  void DumpBetaSharedMemoryProbe();
  // NHL_BETA_GPUDUMP="0xADDR,0xADDR,...": copy these guest addresses straight out of
  // OUR beta shared-memory GPU buffer (what beta's pipeline ACTUALLY produced — unlike
  // NHL_DUMP_ADDRS which reads CPU guest RAM = the trace's correct-by-construction data)
  // and dump each as a 1280x720 PNG. Recorded into the capture submission, read back
  // after the GPU idles. Localizes which RTT stage breaks in beta.
  std::vector<std::pair<uint32_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> beta_gpudump_bufs_;
  // Tiled 32bpp surfaces pad height to a multiple of 32 (720 -> 736); copy/alloc the
  // padded size (+margin) so the untile read never runs past the readback buffer.
  static constexpr uint32_t kBetaGpuDumpBytes = 1280u * 768u * 4u;
  void RecordBetaGpuDumps();
  void WriteBetaGpuDumps();
  // NHL_BETA_EDRAMDUMP="base:pitch:W:H,...": read beta's LIVE EDRAM buffer (the SDK's
  // 10 MiB edram_buffer_ — what ROV/host-RT actually render INTO, before any resolve) back
  // to the CPU via a self-contained compute copy (EDRAM raw SRV -> dst UAV) on a post-frame
  // GPU-idle direct list, then untile each region (EDRAM 80x16 tiles, pitch in tiles) and
  // write edramdump_b<base>.png. This is the direct EDRAM observability the green-fold
  // investigation needed: it shows EDRAM tile content the resolve can't (resolve dumps only
  // the guest's resolve region). Lazy-created compute objects below; diagnostic-only.
  void ReadbackBetaEdramRegion();
  Microsoft::WRL::ComPtr<ID3D12RootSignature> beta_edramdump_rootsig_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> beta_edramdump_pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> beta_edramdump_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_edramdump_dst_;       // default-heap UAV target
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_edramdump_readback_;  // CPU-mappable copy
  // NHL_BETA_FAKETEX isolation: a known solid-red texture bound in place of the real
  // untiled texture. If textured draws turn red, the bind/sample/blend/geometry path
  // is correct and only the untiled CONTENT is the issue; if still black, the problem
  // is upstream (sample/UV/shader).
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_fake_tex_;
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_fake_tex_upload_;
  bool beta_fake_tex_ready_ = false;
  void EnsureBetaFakeTexture();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT beta_rt_footprint_{};
  uint64_t beta_rt_total_bytes_ = 0;
  uint32_t beta_rt_width_ = 0;   // internal (supersampled) RT width
  uint32_t beta_rt_height_ = 0;  // internal (supersampled) RT height
  uint32_t beta_out_width_ = 0;  // output (downsampled) width
  uint32_t beta_out_height_ = 0;
  uint32_t beta_ss_factor_ = 1;  // NHL_BETA_SSAA internal-resolution scale [1,4]
  // NHL_BETA_MSAA: true multisample anti-aliasing to bit-match the oracle's 4X-MSAA
  // edge AA (the sole remaining Tier-1 parity residual). When >1, beta_offscreen_rt_
  // is a multisample RT (the draws' PSOs are forced to the same sample count) and is
  // RESOLVED to beta_resolve_rt_ (1X) on a one-shot command list at dump time (the
  // DeferredCommandList has no ResolveSubresource + the base CP's raw list is private,
  // so the resolve runs post-frame on our own list). Sample count [1|2|4]; standalone
  // (forces SSAA off — supersampling shades per-subsample and would shift texture LOD).
  uint32_t beta_msaa_ = 1;
  Microsoft::WRL::ComPtr<ID3D12Resource> beta_resolve_rt_;  // 1X resolve dest (MSAA only)
  void ResolveBetaMsaaToReadback();  // one-shot MSAA->1X resolve + copy to readback
  bool beta_capture_pending_ = false;
  bool beta_capture_done_ = false;
  uint64_t beta_capture_submission_ = 0;  // submission the clear+copy was recorded into
  // One-shot single-frame capture. Armed by the NHL_CAPTURE_FRAME env var
  // (target guest frame index); on reaching it we call the inherited
  // RequestFrameTrace(), which the base D3D12 CP fulfills by writing the *next*
  // full frame to an .xtr under the trace prefix dir. Replayed offline by the
  // nhl-trace-inspect / TracePlayer tooling — the game need not be running.
  void InitCaptureArmingOnce();

  // Hotkey-driven streaming capture (NHL_HOTKEY_CAPTURE=1; F9 toggles). Each
  // press-pair brackets BeginTracing/EndTracing and writes one scene .xtr to its
  // own gpu_trace/scene_NN/ subdir, so interactive in-game captures (gameplay,
  // arena, replay, edit-player) need no known frame indices and never collide
  // (BeginTracing names the file internally, so a shared root would overwrite).
  // Polled once per presented frame from IssueSwap via GetAsyncKeyState.
  void PollHotkeyCapture();

  // NHL_CAPTURE_FULL=1: at the start of a streaming capture, mark all guest RAM
  // dirty via TracePlaybackWroteMemory so every cached GPU resource (textures,
  // vertex/index buffers) re-uploads from guest RAM during the window and the
  // trace records its source data. Without it, resources uploaded BEFORE the
  // capture started (e.g. a player model's textures, loaded on screen entry) stay
  // resident, are never re-read, and so are absent from the trace -> render black
  // on replay. Makes the .xtr self-contained at the cost of a larger trace.
  void InvalidateAllForCapture();
  bool full_capture_ = false;

  bool hotkey_enabled_ = false;
  bool hotkey_capturing_ = false;
  bool hotkey_prev_down_ = false;
  uint32_t hotkey_capture_index_ = 0;

  // Frame-feature inventory (NHL_DRAW_INVENTORY=1): at each IssueDraw we sample
  // the *live* per-draw render state via the base CP's authoritative
  // ReadRegisterValue (the snapshot-only python trace tools can't see how state
  // evolves across draws). Aggregated over the run and dumped periodically at
  // IssueSwap. This scopes what NHL actually exercises (prim types, MSAA, depth,
  // blend, cull, alpha-test, # render targets) so the Tier-1 backend only builds
  // features the game uses. Plan §3 / §15 Q3.
  void PrintInventory();
  bool inventory_enabled_ = false;
  std::map<uint32_t, uint64_t> inv_prim_;        // PrimitiveType -> draw count
  std::map<uint32_t, uint64_t> inv_msaa_;        // msaa level (0=1x,1=2x,2=4x) -> draws
  std::map<uint32_t, uint64_t> inv_edram_mode_;  // RB_MODECONTROL.edram_mode -> draws
  std::map<uint32_t, uint64_t> inv_cull_;        // cull bits (0..3) -> draws
  std::set<uint32_t> inv_blend_vals_;            // distinct RB_BLENDCONTROL0
  std::set<uint32_t> inv_color_info_;            // distinct RB_COLOR_INFO (RT base/format)
  uint64_t inv_total_draws_ = 0;
  uint64_t inv_indexed_ = 0;    // index_buffer_info != null
  uint64_t inv_auto_ = 0;       // auto-index (null)
  uint64_t inv_zenable_ = 0;    // RB_DEPTHCONTROL.z_enable
  uint64_t inv_zwrite_ = 0;     // RB_DEPTHCONTROL.z_write_enable
  uint64_t inv_alphatest_ = 0;  // RB_COLORCONTROL.alpha_test_enable
  uint64_t inv_mrt_ = 0;        // draws with color RT1/2/3 bound (multi render target)
  uint32_t inv_min_idx_ = 0xFFFFFFFFu;
  uint32_t inv_max_idx_ = 0;

  bool capture_armed_ = false;
  bool capture_initialized_ = false;
  bool capture_done_ = false;
  uint64_t capture_target_frame_ = 0;
  std::filesystem::path capture_root_ = "gpu_trace";

  // Multi-frame streaming capture (NHL_CAPTURE_STREAM=<start>:<end>): brackets
  // BeginTracing/EndTracing so frames (start..end] land in one streaming .xtr.
  // Replaying the whole stream warms the texture cache across frames, so a
  // resource first uploaded by an early frame (e.g. the font glyph atlas) is
  // resident by the final frame — closing the single-frame "missing text" gap.
  bool stream_armed_ = false;
  bool stream_active_ = false;
  bool stream_done_ = false;
  uint64_t stream_start_frame_ = 0;
  uint64_t stream_end_frame_ = 0;

  // Lifetime IssueDraw count (never reset).
  uint64_t draws_total_ = 0;
  std::vector<uint32_t> resolve_dest_bases_;
  // Per-frame counters, reset on each IssueSwap.
  uint32_t draws_this_frame_ = 0;
  uint32_t draws_ok_this_frame_ = 0;   // base IssueDraw returned true (rendered)
  uint32_t copies_this_frame_ = 0;
  uint32_t copies_ok_this_frame_ = 0;  // base IssueCopy returned true
  uint32_t shader_loads_this_frame_ = 0;
  // Distinct shader microcode blobs loaded since startup (guest addresses seen).
  uint32_t shader_loads_total_ = 0;
  uint64_t frame_index_ = 0;
};

}  // namespace nhl::graphics
