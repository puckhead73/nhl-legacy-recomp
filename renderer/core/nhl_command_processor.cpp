#define _CRT_SECURE_NO_WARNINGS  // std::getenv below; no _dupenv_s dependency

#include "renderer/core/nhl_command_processor.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/primitive_processor.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
// High-cut path C, P-3: the ported Xenos->SPIR-V translator + its SpirvShader subclass.
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include "gpu/hooks/highcut_draw_packet.h"  // C-3b.2 draw-data bridge
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/pipeline/texture/info.h>  // C-4: FormatInfo (block dims / bytes-per-block)
#include <rex/string/buffer.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include <rex/ui/presenter.h>
#include <rex/ui/renderdoc_api.h>

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")

#include "tools/replay/src/image_dump.h"
#include "src/injection_registry.h"

#if NHLLEGACY_HAVE_RX2FFI
#include "rx2_ffi.h"
#endif

// Win32 keyboard poll for the F9 hotkey capture (declared directly to keep
// <windows.h> out of this TU; user32 is already linked by the windowed app).
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int v_key);
extern "C" __declspec(dllimport) void __stdcall Sleep(unsigned long ms);

namespace nhl::graphics {

namespace d3d12 = rex::graphics::d3d12;
namespace xenos = rex::graphics::xenos;
using nhllegacy::InjectionRegistry;  // Stage-1 texture-injection registry/sidecar

// --- Xenos PM4 register indices used by the beta backend ---------------------
// Mirror SDK 0.8.0 (graphics/registers.h + register_table.inc). The SDK exports
// only each register's singular register_index; the per-render-target index ARRAYS
// (RB_COLOR_INFO0..3, RB_BLENDCONTROL0..3 = the non-exported rt_register_indices)
// are pinned here. RE-VERIFY ON ANY SDK BUMP — the static_assert tripwire catches
// RT0 drift against the one index the SDK does export.
namespace beta_reg {
constexpr uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};      // RB_COLOR_INFO0..3
constexpr uint32_t kBlendControl[4] = {0x2201, 0x2209, 0x220A, 0x220B};   // RB_BLENDCONTROL0..3
static_assert(kColorInfo[0] == uint32_t(rex::graphics::reg::RB_COLOR_INFO::register_index),
              "RB_COLOR_INFO RT0 register index drifted from SDK 0.8.0; re-verify the RT array");
}  // namespace beta_reg

namespace {
// Crash-safe read of the base CP's private view_bindful_heap_current_
// (D3D12CommandProcessor + 0xCE8; from the rexruntimerd.dll disasm of
// RequestOneUseSingleViewDescriptors @0x178448, SDK 0.8.0 — RE-VERIFY ON ANY SDK
// BUMP). If an SDK layout change ever moves that member, the raw read would yield a
// garbage pointer and binding it as a descriptor heap = device loss. SEH-guard the
// access and sanity-check the descriptor, returning nullptr on any anomaly so the
// caller degrades (skips the sampler table this draw) instead of crashing. POD-only
// locals here so the __try has no C++ unwinding to contend with.
ID3D12DescriptorHeap* ReadCpViewHeapChecked(const void* base_cp) {
  __try {
    ID3D12DescriptorHeap* heap = *reinterpret_cast<ID3D12DescriptorHeap* const*>(
        reinterpret_cast<const char*>(base_cp) + 0xCE8);
    if (!heap) return nullptr;
    const D3D12_DESCRIPTOR_HEAP_DESC d = heap->GetDesc();
    if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        !(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) || d.NumDescriptors == 0) {
      return nullptr;  // not the shader-visible view heap we expect
    }
    return heap;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;  // the offset pointed at unreadable / non-object memory
  }
}
}  // namespace

// --- SDK private-member access bypass ---------------------------------------
// The SDK ships precompiled (headers + lib, no source), and D3D12CommandProcessor's
// submission lifecycle — BeginSubmission()/submission_open_ — is private. The base
// IssueDraw() calls BeginSubmission(true) to guarantee an open GPU submission before
// recording; our full-frame takeover overrides IssueDraw and so loses that guarantee.
// On a heavy 3D frame the first owned draw's shared-memory residency does sparse
// UpdateTileMappings, a direct-queue op that ends the current submission (the base then
// expects its own next IssueDraw to reopen it). Without a reopen the next owned draw's
// GetDeferredCommandList() asserts submission_open_ (command_processor.h:72) — the
// black-screen crash. Reopening needs BeginSubmission, which is only blocked by
// COMPILE-TIME access control; its symbol is in the lib. The explicit-template-
// instantiation idiom below (ISO C++: explicit instantiation ignores access) yields a
// pointer-to-member we can legally call, exactly mirroring what the base does.
namespace beta_access {
template <typename Tag, typename Tag::type Member>
struct Thief {
  friend typename Tag::type Steal(Tag) { return Member; }
};
struct BeginSubmissionTag {
  using type = bool (d3d12::D3D12CommandProcessor::*)(bool);
  friend type Steal(BeginSubmissionTag);
};
template struct Thief<BeginSubmissionTag, &d3d12::D3D12CommandProcessor::BeginSubmission>;
struct SubmissionOpenTag {
  using type = bool d3d12::D3D12CommandProcessor::*;
  friend type Steal(SubmissionOpenTag);
};
template struct Thief<SubmissionOpenTag, &d3d12::D3D12CommandProcessor::submission_open_>;
}  // namespace beta_access

// True if a GPU submission is currently open on `cp`.
static bool BetaSubmissionOpen(d3d12::D3D12CommandProcessor* cp) {
  // Steal() is a friend found via ADL on its tag argument (not via qualified lookup).
  return cp->*Steal(beta_access::SubmissionOpenTag{});
}
// Ensure a submission is open (idempotent mid-frame — a frame is already open, so this
// only reopens the submission and does NOT start a new frame). Returns false if the
// device was removed / the submission could not be opened.
static bool BetaEnsureSubmissionOpen(d3d12::D3D12CommandProcessor* cp) {
  if (BetaSubmissionOpen(cp)) {
    return true;
  }
  return (cp->*Steal(beta_access::BeginSubmissionTag{}))(/*is_guest_command=*/true);
}

void NhlD3D12CommandProcessor::InitCaptureArmingOnce() {
  if (capture_initialized_) {
    return;
  }
  capture_initialized_ = true;
  if (std::getenv("NHL_DRAW_INVENTORY")) {
    inventory_enabled_ = true;
    REXLOG_INFO("[nhl-inv] per-draw frame-feature inventory enabled");
  }
  if (std::getenv("NHL_CAPTURE_FULL")) {
    full_capture_ = true;
    REXLOG_INFO("[nhl-cap] full-capture enabled: will force texture/buffer re-record at trace start");
  }
  if (std::getenv("NHL_HOTKEY_CAPTURE")) {
    hotkey_enabled_ = true;
    // Resume the scene index past any existing gpu_trace/scene_NN/ dirs so a new
    // run never silently overwrites a prior capture (the index member resets to 0
    // each launch). Scan once at arm time.
    std::error_code ec;
    if (std::filesystem::exists(capture_root_, ec)) {
      for (const auto& entry : std::filesystem::directory_iterator(capture_root_, ec)) {
        if (!entry.is_directory()) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("scene_", 0) != 0) continue;
        const uint32_t n = std::strtoul(name.c_str() + 6, nullptr, 10);
        if (n + 1 > hotkey_capture_index_) hotkey_capture_index_ = n + 1;
      }
    }
    REXLOG_INFO("[nhl-cap] F9 hotkey streaming capture enabled "
                "(press F9 in-scene to start, F9 again to stop -> gpu_trace/scene_NN/); "
                "next index = scene_{:02}",
                hotkey_capture_index_);
  }
  if (const char* env = std::getenv("NHL_CAPTURE_FRAME")) {
    capture_target_frame_ = std::strtoull(env, nullptr, 10);
    capture_armed_ = true;
    REXLOG_INFO("[nhl-gpu] single-frame capture armed: will trace frame {} -> {}/",
                capture_target_frame_, capture_root_.string());
  }
  if (const char* env = std::getenv("NHL_CAPTURE_STREAM")) {
    const char* colon = std::strchr(env, ':');
    if (colon) {
      stream_start_frame_ = std::strtoull(env, nullptr, 10);
      stream_end_frame_ = std::strtoull(colon + 1, nullptr, 10);
      stream_armed_ = stream_end_frame_ > stream_start_frame_;
      REXLOG_INFO("[nhl-gpu] streaming capture armed: frames ({}..{}] -> {}/", stream_start_frame_,
                  stream_end_frame_, capture_root_.string());
    }
  }
}

namespace {
const char* PrimName(uint32_t p) {
  switch (p) {
    case 0x01: return "PointList";
    case 0x02: return "LineList";
    case 0x03: return "LineStrip";
    case 0x04: return "TriangleList";
    case 0x05: return "TriangleFan";
    case 0x06: return "TriangleStrip";
    case 0x08: return "RectangleList";
    case 0x0D: return "QuadList";
    default: return "other";
  }
}
const char* MsaaName(uint32_t m) {
  switch (m) {
    case 0: return "1X";
    case 1: return "2X";
    case 2: return "4X";
    default: return "?";
  }
}
}  // namespace

void NhlD3D12CommandProcessor::InvalidateAllForCapture() {
  // Mark all guest physical RAM dirty so caches re-upload from guest RAM (and the
  // active trace writer records the source) on the next frames. Chunked to avoid
  // any single oversized invalidation. 512 MB guest physical space.
  constexpr uint32_t kPhysEnd = 0x20000000u;
  constexpr uint32_t kChunk = 0x01000000u;  // 16 MB
  for (uint32_t base = 0; base < kPhysEnd; base += kChunk) {
    TracePlaybackWroteMemory(base, kChunk);
  }
  REXLOG_INFO("[nhl-cap] full-capture: invalidated guest RAM 0..{:08X} to force resource re-record",
              kPhysEnd);
}

void NhlD3D12CommandProcessor::PollHotkeyCapture() {
  constexpr int kVkF9 = 0x78;
  const bool down = (GetAsyncKeyState(kVkF9) & 0x8000) != 0;
  const bool rising = down && !hotkey_prev_down_;
  hotkey_prev_down_ = down;
  if (!rising) {
    return;
  }
  if (!hotkey_capturing_) {
    const std::filesystem::path dir =
        capture_root_ / fmt::format("scene_{:02}", hotkey_capture_index_);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    BeginTracing(dir);
    if (full_capture_) InvalidateAllForCapture();
    hotkey_capturing_ = true;
    REXLOG_INFO("[nhl-cap] F9 capture BEGIN -> {}/ (frame {})", dir.string(), frame_index_);
  } else {
    EndTracing();
    hotkey_capturing_ = false;
    // Stage-1: emit the address->asset sidecar built during this capture window.
    WriteInjectSidecar();
    REXLOG_INFO("[nhl-cap] F9 capture END (frame {}) -> {}/scene_{:02}/", frame_index_,
                capture_root_.string(), hotkey_capture_index_);
    ++hotkey_capture_index_;
  }
}

void NhlD3D12CommandProcessor::PrintInventory() {
  if (!inventory_enabled_ || inv_total_draws_ == 0) {
    return;
  }
  std::string prims;
  for (auto& [p, n] : inv_prim_) prims += fmt::format("{}={} ", PrimName(p), n);
  std::string msaa;
  for (auto& [m, n] : inv_msaa_) msaa += fmt::format("{}={} ", MsaaName(m), n);
  std::string modes;
  for (auto& [m, n] : inv_edram_mode_) modes += fmt::format("{}={} ", m, n);
  std::string cull;
  for (auto& [c, n] : inv_cull_) {
    const char* nm = c == 0 ? "none" : c == 1 ? "front" : c == 2 ? "back" : "both";
    cull += fmt::format("{}={} ", nm, n);
  }
  REXLOG_INFO(
      "[nhl-inv] frame {} | draws={} (indexed={} auto={}) idx_count=[{}..{}]\n"
      "  prim:   {}\n"
      "  msaa:   {}\n"
      "  edram_mode: {}\n"
      "  cull:   {}\n"
      "  depth:  z_enable={} z_write={} | alpha_test={} | mrt_draws={}\n"
      "  distinct RB_BLENDCONTROL0={} distinct RB_COLOR_INFO={}",
      frame_index_, inv_total_draws_, inv_indexed_, inv_auto_,
      inv_min_idx_ == 0xFFFFFFFFu ? 0 : inv_min_idx_, inv_max_idx_, prims, msaa, modes, cull,
      inv_zenable_, inv_zwrite_, inv_alphatest_, inv_mrt_, inv_blend_vals_.size(),
      inv_color_info_.size());
}

// Defined here (not =default in the header) so the unique_ptr<beta cache> members
// are destroyed where the cache types are complete. By this point ShutdownContext
// has already reset them, so this is normally a no-op.
NhlD3D12CommandProcessor::~NhlD3D12CommandProcessor() = default;

bool NhlD3D12CommandProcessor::BuildBetaCaches() {
  // Phase 2 of the Tier-1 owned backend (docs/tier1-backend-build-order.md):
  // construct our OWN instances of the SDK's five concrete D3D12 caches and keep
  // them alive. They are INERT for now — the base D3D12 backend still drives every
  // draw (IssueDraw delegates), using ITS OWN private caches. This step only
  // proves that all five construct, Initialize, coexist with the base set, and
  // tear down cleanly in the real runtime — the construction half of beta.
  //
  // All ctor inputs are reachable from our subclass: we ARE a D3D12CommandProcessor
  // (pass *this); memory_, register_file_ and trace_writer_ are protected members
  // of the abstract CommandProcessor base. Tier-1 = parity, so no resolution
  // scaling (1x1) and the simpler bindful descriptor path (bindless=false).
  if (!memory_ || !register_file_) {
    REXLOG_ERROR("[nhl-beta] BuildBetaCaches ABORT: memory_/register_file_ null at SetupContext");
    return false;
  }
  constexpr uint32_t kScaleX = 1;
  constexpr uint32_t kScaleY = 1;
  constexpr bool kBindless = false;

  // 1. Shared memory — the guest-RAM mirror; dependency of texture/RT/primitive.
  beta_shared_memory_ = std::make_unique<d3d12::D3D12SharedMemory>(*this, *memory_, trace_writer_);
  if (!beta_shared_memory_->Initialize()) {
    REXLOG_ERROR("[nhl-beta] D3D12SharedMemory::Initialize FAILED");
    return false;
  }
  REXLOG_INFO("[nhl-beta] 1/5 D3D12SharedMemory initialized (buffer={})",
              static_cast<const void*>(beta_shared_memory_->GetBuffer()));
  if (beta_shared_memory_->GetBuffer()) {
    beta_shared_memory_->GetBuffer()->SetName(L"beta_shared_memory");  // debug-layer identity
  }

  // 2. Render-target cache — the reused EDRAM emulation. Force the HOST render-
  //    target (RTV/DSV) path instead of the base's ROV path: on the host-RT path
  //    pixel shaders write SV_Target to a bound render target, which we can point
  //    at our own offscreen RTV and read back (the ROV path writes color to an
  //    EDRAM UAV with NO bound RT, so bound_bits==0 and an RTV captures nothing).
  //    Both paths resolve to the same image; host-RT is simpler for our single-RT
  //    Tier-1 case. The path is chosen from this cvar in Initialize(); set it only
  //    around our construction so the base (already built, on ROV) is unaffected.
  {
    const std::string saved_rt_path = REXCVAR_GET(render_target_path_d3d12);
    const char* beta_rt_path_env = std::getenv("NHL_BETA_RT_PATH");
    const std::string beta_rt_path =
        beta_rt_path_env && std::strcmp(beta_rt_path_env, "rov") == 0 ? "rov" : "rtv";
    REXCVAR_SET(render_target_path_d3d12, beta_rt_path);
    beta_render_target_cache_ = std::make_unique<d3d12::D3D12RenderTargetCache>(
        *register_file_, *memory_, trace_writer_, kScaleX, kScaleY, *this, kBindless);
    const bool rt_init = beta_render_target_cache_->Initialize();
    REXCVAR_SET(render_target_path_d3d12, saved_rt_path);
    if (!rt_init) {
      REXLOG_ERROR("[nhl-beta] D3D12RenderTargetCache::Initialize FAILED");
      return false;
    }
  }
  REXLOG_INFO("[nhl-beta] 2/5 D3D12RenderTargetCache initialized (path={} [0=host-RT,1=ROV], "
              "msaa2x={})",
              static_cast<int>(beta_render_target_cache_->GetPath()),
              beta_render_target_cache_->msaa_2x_supported());

  // 3. Texture cache — untile/convert; Create() runs Initialize() internally.
  beta_texture_cache_ = d3d12::D3D12TextureCache::Create(
      *register_file_, *beta_shared_memory_, kScaleX, kScaleY, *this, kBindless);
  if (!beta_texture_cache_) {
    REXLOG_ERROR("[nhl-beta] D3D12TextureCache::Create FAILED");
    return false;
  }
  REXLOG_INFO("[nhl-beta] 3/5 D3D12TextureCache created");

  // 4. Primitive processor — strip/fan/quad/rect expansion + index conversion.
  beta_primitive_processor_ = std::make_unique<d3d12::D3D12PrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *beta_shared_memory_, *this);
  if (!beta_primitive_processor_->Initialize()) {
    REXLOG_ERROR("[nhl-beta] D3D12PrimitiveProcessor::Initialize FAILED");
    return false;
  }
  REXLOG_INFO("[nhl-beta] 4/5 D3D12PrimitiveProcessor initialized");

  // 5. Pipeline cache — ucode->DXBC + PSO; needs the render-target cache.
  beta_pipeline_cache_ = std::make_unique<d3d12::PipelineCache>(
      *this, *register_file_, *beta_render_target_cache_, kBindless);
  if (!beta_pipeline_cache_->Initialize()) {
    REXLOG_ERROR("[nhl-beta] PipelineCache::Initialize FAILED");
    return false;
  }
  REXLOG_INFO("[nhl-beta] 5/5 PipelineCache initialized");

  REXLOG_INFO("[nhl-beta] Phase-2 cache bring-up COMPLETE: all 5 beta caches live alongside the "
              "base set; draws still delegate to the base (inert).");
  return true;
}

void NhlD3D12CommandProcessor::ShutdownBetaCaches() {
  // Reverse dependency order, BEFORE the base ShutdownContext tears down the
  // device/provider these caches reference. unique_ptr::reset() runs each dtor
  // (which calls the cache's own Shutdown).
  beta_pipeline_cache_.reset();
  beta_primitive_processor_.reset();
  beta_texture_cache_.reset();
  beta_render_target_cache_.reset();
  beta_shared_memory_.reset();
}

bool NhlD3D12CommandProcessor::CreateBetaOffscreenTarget(uint32_t width, uint32_t height) {
  if (beta_offscreen_rt_) {
    return true;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  // Internal-resolution SUPERSAMPLING (Tier-2 enhancement, the payoff of owning the
  // backend): render the whole frame at NHL_BETA_SSAA× the output size into this RT,
  // then box-downsample on readback. All draw setup keys off beta_rt_width_/height_
  // (viewport, guest-surface scale), so a larger RT upscales the native content to
  // fill it — true SSAA with zero per-draw changes. The PNG is downsampled back to
  // the output (guest) resolution. Clamped to [1,4].
  beta_ss_factor_ = 1;
  if (const char* ss = std::getenv("NHL_BETA_SSAA")) {
    beta_ss_factor_ = std::min(4u, std::max(1u, uint32_t(std::strtoul(ss, nullptr, 10))));
  }
  // True MSAA (NHL_BETA_MSAA=1|2|4): standalone, forces SSAA off (see header note).
  beta_msaa_ = 1;
  if (const char* ms = std::getenv("NHL_BETA_MSAA")) {
    const uint32_t v = uint32_t(std::strtoul(ms, nullptr, 10));
    beta_msaa_ = (v >= 4) ? 4u : (v >= 2) ? 2u : 1u;
    if (beta_msaa_ > 1) beta_ss_factor_ = 1;
  }
  beta_out_width_ = width;
  beta_out_height_ = height;
  width *= beta_ss_factor_;
  height *= beta_ss_factor_;
  beta_rt_width_ = width;
  beta_rt_height_ = height;

  D3D12_HEAP_PROPERTIES heap_default{};
  heap_default.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rt_desc{};
  rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rt_desc.Width = width;
  rt_desc.Height = height;
  rt_desc.DepthOrArraySize = 1;
  rt_desc.MipLevels = 1;
  rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rt_desc.SampleDesc.Count = beta_msaa_;
  rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear_value{};
  clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  if (FAILED(device->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                                             IID_PPV_ARGS(&beta_offscreen_rt_)))) {
    REXLOG_ERROR("[nhl-beta] 4b: CreateCommittedResource(offscreen RT) failed");
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = 1;
  if (FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&beta_rtv_heap_)))) {
    REXLOG_ERROR("[nhl-beta] 4b: CreateDescriptorHeap(RTV) failed");
    return false;
  }
  // Null RTV desc auto-selects TEXTURE2DMS for a multisample resource.
  device->CreateRenderTargetView(beta_offscreen_rt_.Get(), nullptr,
                                 beta_rtv_heap_->GetCPUDescriptorHandleForHeapStart());

  // For MSAA, the readback footprint must describe the resolved 1X texture, not the
  // multisample RT (which can't be copied to a buffer). Create a 1X resolve target
  // and base the footprint/readback on it.
  D3D12_RESOURCE_DESC footprint_desc = rt_desc;
  footprint_desc.SampleDesc.Count = 1;
  if (beta_msaa_ > 1) {
    if (FAILED(device->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &footprint_desc,
                                               D3D12_RESOURCE_STATE_COMMON, &clear_value,
                                               IID_PPV_ARGS(&beta_resolve_rt_)))) {
      REXLOG_ERROR("[nhl-beta] msaa: CreateCommittedResource(resolve RT) failed");
      return false;
    }
  }

  UINT num_rows = 0;
  UINT64 row_size = 0;
  device->GetCopyableFootprints(&footprint_desc, 0, 1, 0, &beta_rt_footprint_, &num_rows, &row_size,
                                &beta_rt_total_bytes_);

  D3D12_HEAP_PROPERTIES heap_readback{};
  heap_readback.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buf_desc{};
  buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buf_desc.Width = beta_rt_total_bytes_;
  buf_desc.Height = 1;
  buf_desc.DepthOrArraySize = 1;
  buf_desc.MipLevels = 1;
  buf_desc.Format = DXGI_FORMAT_UNKNOWN;
  buf_desc.SampleDesc.Count = 1;
  buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&heap_readback, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&beta_readback_)))) {
    REXLOG_ERROR("[nhl-beta] 4b: CreateCommittedResource(readback) failed");
    return false;
  }
  REXLOG_INFO("[nhl-beta] 4b: offscreen RT {}x{} created (msaa={}x ssaa={}x row_pitch={} "
              "total_bytes={})",
              width, height, beta_msaa_, beta_ss_factor_, beta_rt_footprint_.Footprint.RowPitch,
              beta_rt_total_bytes_);
  return true;
}

bool NhlD3D12CommandProcessor::EnsureBetaDepthTarget() {
  if (beta_depth_ready_) {
    return true;
  }
  if (!beta_depth_enabled_) {
    return false;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const auto fmt = xenos::DepthRenderTargetFormat(beta_depth_xenos_fmt_);
  // Match the DXGI formats the SDK PipelineCache derives for this guest depth format,
  // so the PSO's DSVFormat agrees with our resource/view (else CreateGraphicsPipeline
  // or OMSetRenderTargets mismatches).
  const DXGI_FORMAT res_fmt = d3d12::D3D12RenderTargetCache::GetDepthResourceDXGIFormat(fmt);
  const DXGI_FORMAT dsv_fmt = d3d12::D3D12RenderTargetCache::GetDepthDSVDXGIFormat(fmt);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = beta_rt_width_;
  d.Height = beta_rt_height_;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = res_fmt;
  d.SampleDesc.Count = beta_msaa_;  // must match the color RT's sample count
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE cv{};
  cv.Format = dsv_fmt;
  cv.DepthStencil.Depth = beta_depth_clear_;
  cv.DepthStencil.Stencil = 0;
  if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                             IID_PPV_ARGS(&beta_depth_rt_)))) {
    REXLOG_ERROR("[nhl-beta] depth: CreateCommittedResource(depth) failed (fmt={})",
                 beta_depth_xenos_fmt_);
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = 1;
  if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&beta_dsv_heap_)))) {
    REXLOG_ERROR("[nhl-beta] depth: CreateDescriptorHeap(DSV) failed");
    return false;
  }
  D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{};
  dsvd.Format = dsv_fmt;
  dsvd.ViewDimension =
      beta_msaa_ > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(beta_depth_rt_.Get(), &dsvd,
                                 beta_dsv_heap_->GetCPUDescriptorHandleForHeapStart());
  beta_depth_ready_ = true;
  REXLOG_INFO("[nhl-beta] depth: {}x{} depth target ready (xenos_fmt={} res=0x{:X} dsv=0x{:X} "
              "msaa={}x clear={})",
              beta_rt_width_, beta_rt_height_, beta_depth_xenos_fmt_, uint32_t(res_fmt),
              uint32_t(dsv_fmt), beta_msaa_, beta_depth_clear_);
  return true;
}

void NhlD3D12CommandProcessor::WriteBetaFlatResolveDumps() {
  // NHL_BETA_FLAT_DUMP: write each recorded per-resolve host-texture snapshot to
  // flatresolve_<seq>_<dest>.png. Called at capture finalize (GPU idle), so every
  // recorded readback copy has retired. Same layout as DumpBetaOffscreenTarget
  // (plain RGBA8, row-pitch from beta_rt_footprint_, no tiling / no channel swap).
  if (beta_flat_dumps_.empty()) return;
  const uint32_t w = beta_rt_width_, h = beta_rt_height_;
  const uint32_t row_pitch = beta_rt_footprint_.Footprint.RowPitch;
  for (auto& fd : beta_flat_dumps_) {
    void* mapped = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(beta_rt_total_bytes_)};
    if (!fd.readback || FAILED(fd.readback->Map(0, &rr, &mapped)) || !mapped) {
      REXLOG_ERROR("[nhl-beta] flat-dump: Map failed for dest=0x{:X}", fd.dest);
      continue;
    }
    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> tight(static_cast<size_t>(w) * h * 4);
    uint64_t nz = 0;
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* srow = base + static_cast<size_t>(y) * row_pitch;
      uint8_t* drow = tight.data() + static_cast<size_t>(y) * w * 4;
      std::memcpy(drow, srow, static_cast<size_t>(w) * 4);
      for (uint32_t x = 0; x < w; ++x) {
        if (drow[x * 4] | drow[x * 4 + 1] | drow[x * 4 + 2]) ++nz;
      }
    }
    D3D12_RANGE wr{0, 0};
    fd.readback->Unmap(0, &wr);
    char path[80];
    std::snprintf(path, sizeof(path), "flatresolve_%02u_%08X.png", fd.seq, fd.dest);
    nhl::replay::WritePng(path, w, h, tight.data());
    REXLOG_INFO("[nhl-beta] flat-dump: wrote {} (dest=0x{:X} nzRGB={}/{})", path, fd.dest, nz,
                uint64_t(w) * h);
  }
  beta_flat_dumps_.clear();
}

void NhlD3D12CommandProcessor::DumpBetaOffscreenTarget(const char* path) {
  // Caller gates on GetCompletedSubmission() >= the recorded submission, so the
  // GPU copy into the readback buffer has finished and the data is valid.
  void* mapped = nullptr;
  D3D12_RANGE read_range{0, static_cast<SIZE_T>(beta_rt_total_bytes_)};
  if (FAILED(beta_readback_->Map(0, &read_range, &mapped)) || !mapped) {
    REXLOG_ERROR("[nhl-beta] 4b: readback Map failed");
    return;
  }
  const uint32_t w = beta_rt_width_;
  const uint32_t h = beta_rt_height_;
  const uint32_t row_pitch = beta_rt_footprint_.Footprint.RowPitch;
  const uint8_t* base = static_cast<const uint8_t*>(mapped);
  const uint32_t ss = beta_ss_factor_ ? beta_ss_factor_ : 1;
  const uint32_t ow = w / ss, oh = h / ss;  // output (downsampled) dimensions
  std::vector<uint8_t> tight(static_cast<size_t>(ow) * oh * 4);
  if (ss == 1) {
    for (uint32_t y = 0; y < h; ++y) {
      std::memcpy(tight.data() + static_cast<size_t>(y) * w * 4,
                  base + static_cast<size_t>(y) * row_pitch, static_cast<size_t>(w) * 4);
    }
  } else {
    // Box-downsample each ss×ss block (averaged supersampling resolve).
    const uint32_t n = ss * ss;
    for (uint32_t oy = 0; oy < oh; ++oy) {
      for (uint32_t ox = 0; ox < ow; ++ox) {
        uint32_t acc[4] = {0, 0, 0, 0};
        for (uint32_t dy = 0; dy < ss; ++dy) {
          const uint8_t* row = base + size_t(oy * ss + dy) * row_pitch + size_t(ox * ss) * 4;
          for (uint32_t dx = 0; dx < ss; ++dx) {
            for (int c = 0; c < 4; ++c) acc[c] += row[dx * 4 + c];
          }
        }
        uint8_t* o = tight.data() + (size_t(oy) * ow + ox) * 4;
        for (int c = 0; c < 4; ++c) o[c] = uint8_t(acc[c] / n);
      }
    }
  }
  D3D12_RANGE write_range{0, 0};
  beta_readback_->Unmap(0, &write_range);
  nhl::replay::WritePng(path, ow, oh, tight.data());
  REXLOG_INFO("[nhl-beta] wrote {} ({}x{} from {}x{} internal, {}x SSAA); first px=({},{},{},{})",
              path, ow, oh, w, h, ss, tight[0], tight[1], tight[2], tight[3]);
}

void NhlD3D12CommandProcessor::DumpBetaSharedMemoryProbe() {
  if (!beta_shmem_probe_pending_ || !beta_shmem_probe_buf_) {
    return;
  }
  beta_shmem_probe_pending_ = false;
  void* mapped = nullptr;
  D3D12_RANGE rr{0, 4096};
  if (FAILED(beta_shmem_probe_buf_->Map(0, &rr, &mapped)) || !mapped) {
    REXLOG_ERROR("[nhl-beta] shmem-probe: Map failed");
    return;
  }
  const uint8_t* buf = static_cast<const uint8_t*>(mapped);
  uint32_t buf_nz = 0;
  for (int i = 0; i < 4096; ++i) {
    if (buf[i]) ++buf_nz;
  }
  // Compare to guest RAM at the same address.
  uint32_t ram_nz = 0;
  bool match = true;
  if (memory_) {
    const uint8_t* ram = memory_->TranslatePhysical<const uint8_t*>(beta_shmem_probe_addr_);
    if (ram) {
      for (int i = 0; i < 4096; ++i) {
        if (ram[i]) ++ram_nz;
        if (ram[i] != buf[i]) match = false;
      }
    }
  }
  // Decode the vertex attribute at +32 (FMT_32_32_32_32_FLOAT, k8in32 swap) from BOTH
  // our GPU buffer and guest RAM — the o2/TEXCOORD2 source. If our buffer has the blue
  // attrib, the vfetch SRV/addressing is the bug; if zero, residency missed it.
  auto rdf = [](const uint8_t* b) {
    uint8_t s[4] = {b[3], b[2], b[1], b[0]};
    float r;
    std::memcpy(&r, s, 4);
    return r;
  };
  REXLOG_INFO("[nhl-beta] shmem-probe attrib@+32 from OUR buf=({:.3f},{:.3f},{:.3f},{:.3f}) "
              "pos@+0=({:.3f},{:.3f},{:.3f})",
              rdf(buf + 32), rdf(buf + 36), rdf(buf + 40), rdf(buf + 44), rdf(buf + 0), rdf(buf + 4),
              rdf(buf + 8));
  D3D12_RANGE wr{0, 0};
  beta_shmem_probe_buf_->Unmap(0, &wr);
  REXLOG_INFO("[nhl-beta] shmem-probe @guest 0x{:X}: our GPU buffer non-zero bytes={}/4096, "
              "guest RAM non-zero bytes={}/4096, bytes_match={} => {}",
              beta_shmem_probe_addr_, buf_nz, ram_nz, match,
              buf_nz ? "UPLOAD DELIVERED the data (remaining black is untile/binding)"
                     : "UPLOAD did NOT populate our buffer (the upload is the bug)");
}

void NhlD3D12CommandProcessor::MaybeInjectBetaTextures() {
#if NHLLEGACY_HAVE_RX2FFI
  if (beta_inject_done_) {
    return;
  }
  beta_inject_done_ = true;

  // Parse NHL_BETA_INJECT once: "<addr>=<relpath.rx2>[:slot];<addr>=...;".
  // <addr> is the guest fetch-constant base (hex like 0x17193000). <relpath> is
  // resolved against NHL_BETA_INJECT_ROOT (default: the extracted _compiled tree)
  // unless absolute. The optional :slot suffix selects a texlib slot (default 0);
  // a "C:"-style drive colon is NOT mistaken for a slot (only a trailing all-digit
  // suffix counts).
  if (!beta_inject_parsed_) {
    beta_inject_parsed_ = true;
    if (const char* env = std::getenv("NHL_BETA_INJECT")) {
      std::string s(env);
      size_t pos = 0;
      while (pos < s.size()) {
        size_t semi = s.find(';', pos);
        std::string item = s.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? s.size() : semi + 1;
        if (item.empty()) {
          continue;
        }
        size_t eq = item.find('=');
        if (eq == std::string::npos) {
          continue;
        }
        BetaInjectEntry e;
        e.addr = uint32_t(std::strtoul(item.substr(0, eq).c_str(), nullptr, 0));
        std::string rest = item.substr(eq + 1);
        size_t colon = rest.rfind(':');
        if (colon != std::string::npos && colon + 1 < rest.size()) {
          const std::string tail = rest.substr(colon + 1);
          if (tail.find_first_not_of("0123456789") == std::string::npos) {
            e.slot = uint32_t(std::strtoul(tail.c_str(), nullptr, 10));
            rest = rest.substr(0, colon);
          }
        }
        e.relpath = rest;
        beta_inject_entries_.push_back(std::move(e));
      }
    }
    // Stage-1: also load a sidecar map (NHL_BETA_INJECT_SIDECAR=<json>, or auto-
    // discover "<NHL_REPLAY_XTR>.inject.json"). Each {addr,rx2,slot} becomes an
    // inject entry, injected by the same loop below as a manual NHL_BETA_INJECT.
    std::filesystem::path sidecar;
    if (const char* sc = std::getenv("NHL_BETA_INJECT_SIDECAR")) {
      sidecar = sc;
    } else if (const char* xtr = std::getenv("NHL_REPLAY_XTR")) {
      std::filesystem::path cand = std::filesystem::path(xtr).replace_extension(".inject.json");
      std::error_code ec;
      if (std::filesystem::exists(cand, ec)) sidecar = cand;
    }
    if (!sidecar.empty()) {
      const auto entries = InjectionRegistry::ParseSidecar(sidecar);
      for (const auto& r : entries) {
        beta_inject_entries_.push_back(BetaInjectEntry{r.addr, r.relpath, r.slot});
      }
      REXLOG_INFO("[nhl-beta] inject sidecar {} -> {} entries", sidecar.string(), entries.size());
    }
  }

  if (beta_inject_entries_.empty() || !memory_) {
    return;
  }

  std::filesystem::path root;
  if (const char* r = std::getenv("NHL_BETA_INJECT_ROOT")) {
    root = r;
  } else {
    root = "H:\\Emulators\\games\\XBOX\\NHL Legacy - Vanilla\\_compiled";
  }
  const bool diag =
      std::getenv("NHL_BETA_INJECT_DIAG") != nullptr || std::getenv("NHL_BETA_BIND_DIAG") != nullptr;

  for (const auto& e : beta_inject_entries_) {
    std::filesystem::path p(e.relpath);
    if (p.is_relative()) {
      p = root / p;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
      REXLOG_WARN("[nhl-beta] inject: cannot open {}", p.string());
      continue;
    }
    std::vector<uint8_t> rx2((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (rx2.empty()) {
      REXLOG_WARN("[nhl-beta] inject: empty file {}", p.string());
      continue;
    }
    Rx2SlotInfo info{};
    uint8_t* out = nullptr;
    size_t out_len = 0;
    const int32_t rc = rx2ffi_decode_slot(rx2.data(), rx2.size(), e.slot, &info, &out, &out_len);
    if (rc != kRx2FfiOk || !out || out_len == 0) {
      REXLOG_WARN("[nhl-beta] inject: rx2ffi_decode_slot rc={} for {} slot {}", rc, e.relpath,
                  e.slot);
      if (out) {
        rx2ffi_free(out, out_len);
      }
      continue;
    }
    // ARGB8888 (and other LINEAR) slots are stored un-tiled, but the guest fetch
    // constant samples them X360-tiled -> a verbatim memcpy would render scrambled.
    // Skip them with a warning (the Stage-1 8888 hazard) unless explicitly allowed
    // (NHL_BETA_INJECT_ALLOW_LINEAR, e.g. the Stage-0 size-matched marker probe).
    if (info.tiled == 0 && !std::getenv("NHL_BETA_INJECT_ALLOW_LINEAR")) {
      REXLOG_WARN("[nhl-beta] inject @0x{:X} <- {} slot {}: LINEAR slot (xenos_fmt={} tiled=0) needs "
                  "tiling for the tiled guest fetch; skipped (set NHL_BETA_INJECT_ALLOW_LINEAR to force)",
                  e.addr, e.relpath, e.slot, info.xenos_format);
      rx2ffi_free(out, out_len);
      continue;
    }
    // Diagnostic knob: NHL_BETA_INJECT_SWAP16 applies a 16-bit-pair byte swap to
    // the payload before writing — to test whether guest RAM wants the swapped
    // (on-disk) or unswapped form vs what the fetch constant's endian expects.
    if (std::getenv("NHL_BETA_INJECT_SWAP16")) {
      for (size_t i = 0; i + 1 < out_len; i += 2) {
        std::swap(out[i], out[i + 1]);
      }
    }
    uint8_t* dst = memory_->TranslatePhysical<uint8_t*>(e.addr);
    uint32_t nz = 0;
    if (dst) {
      std::memcpy(dst, out, out_len);
      for (size_t i = 0; i < out_len && i < 4096; ++i) {
        if (dst[i]) ++nz;
      }
    }
    rx2ffi_free(out, out_len);
    // Invalidate the injected range so the SDK caches re-upload + re-untile it,
    // exactly as a real trace write would (mirrors TracePlaybackWroteMemory's
    // beta-side invalidation). The first-draw full-range RequestRange that runs
    // right after this also covers it, but invalidating keeps injection correct
    // even if called after first draw (Stage 1).
    if (beta_shared_memory_) {
      beta_shared_memory_->MemoryInvalidationCallback(e.addr, uint32_t(out_len),
                                                      /*exact_range=*/true);
    }
    if (beta_primitive_processor_) {
      beta_primitive_processor_->MemoryInvalidationCallback(e.addr, uint32_t(out_len),
                                                            /*exact_range=*/true);
    }
    if (diag) {
      // Compare the .rx2's base-mip size to what the fetch constant at this address
      // expects (read at draw time via NHL_BETA_BIND_DIAG's tex# log) to catch a
      // wrong file/slot/dims pick.
      REXLOG_INFO("[nhl-beta] inject @0x{:X} <- {} slot {}: rc={} {}x{} fmt_byte=0x{:X} "
                  "xenos_fmt={} base_size={} buffer_capacity={} tiled={} wrote={} "
                  "guest-nz(first4k)={} dst={}",
                  e.addr, e.relpath, e.slot, rc, info.width, info.height, info.format_byte,
                  info.xenos_format, info.base_size, info.buffer_capacity, info.tiled, out_len, nz,
                  static_cast<const void*>(dst));
    }
  }
#endif  // NHLLEGACY_HAVE_RX2FFI
}

void NhlD3D12CommandProcessor::CorrelateTexturesForInjection() {
#if NHLLEGACY_HAVE_RX2FFI
  // Stage-1 live correlation: at draw time (guest RAM IS populated), hash each
  // texture fetch constant's guest RAM and look it up in the loose-.rx2 registry,
  // recording {addr -> relpath, slot}. Runs in the live-capture session (base path)
  // AND in replay (takeover) — in replay only PRESENT textures hash-match (missing
  // ones are zero), which validates the plumbing; live capture additionally maps the
  // textures that will be missing from the .xtr (their bytes are resident live).
  if (!inject_correlate_checked_) {
    inject_correlate_checked_ = true;
    inject_correlate_enabled_ = std::getenv("NHL_INJECT_CORRELATE") != nullptr;
    // Replay side: the loose-file device never opens assets, so populate the registry
    // by scanning a root once (NHL_INJECT_REGISTRY_ROOT, else the default _compiled).
    if (inject_correlate_enabled_ && !inject_registry_scanned_) {
      inject_registry_scanned_ = true;
      if (const char* root = std::getenv("NHL_INJECT_REGISTRY_ROOT")) {
        size_t n = 0;
        if (const char* categories = std::getenv("NHL_INJECT_REGISTRY_CATEGORIES")) {
          std::string category_list(categories);
          size_t pos = 0;
          while (pos < category_list.size()) {
            const size_t comma = category_list.find(',', pos);
            std::string category =
                category_list.substr(pos, comma == std::string::npos ? std::string::npos
                                                                    : comma - pos);
            pos = comma == std::string::npos ? category_list.size() : comma + 1;
            category.erase(0, category.find_first_not_of(" \t"));
            const size_t last = category.find_last_not_of(" \t");
            if (last == std::string::npos) continue;
            category.resize(last + 1);
            n += InjectionRegistry::Get().ScanDirectory(
                std::filesystem::path(root) / category, root);
          }
          REXLOG_INFO("[nhl-inject] registry category scan {} [{}] -> {} slots ({} total)",
                      root, categories, n, InjectionRegistry::Get().registry_size());
        } else {
          n = InjectionRegistry::Get().ScanDirectory(root);
          REXLOG_INFO("[nhl-inject] registry scan {} -> {} slots ({} total)", root, n,
                      InjectionRegistry::Get().registry_size());
        }
      }
    }
  }
  if (!inject_correlate_enabled_ || !memory_ || !register_file_) {
    return;
  }
  const uint32_t* regs = &(*register_file_)[0];
  for (uint32_t slot = 0; slot < xenos::kTextureFetchConstantCount; ++slot) {
    xenos::xe_gpu_texture_fetch_t tf{};
    std::memcpy(&tf, &regs[0x4800 + slot * 6], 6 * 4);
    if (tf.type != xenos::FetchConstantType::kTexture) {
      continue;
    }
    const uint32_t base = uint32_t(tf.base_address) << 12;
    if (!base || base >= 0x20000000u || base + InjectionRegistry::kHashPrefix > 0x20000000u) {
      continue;
    }
    if (!inject_correlate_seen_.insert(base).second) {
      continue;  // already hashed this base
    }
    const uint8_t* p = memory_->TranslatePhysical<const uint8_t*>(base);
    if (!p) {
      continue;
    }
    std::string relpath;
    uint32_t rx2_slot = 0;
    if (InjectionRegistry::Get().LookupBytes(p, InjectionRegistry::kHashPrefix, relpath, rx2_slot)) {
      InjectionRegistry::Get().RecordResolved(base, relpath, rx2_slot);
      REXLOG_INFO("[nhl-inject] correlate @0x{:X} -> {} slot {}", base, relpath, rx2_slot);
    }
  }
#endif  // NHLLEGACY_HAVE_RX2FFI
}

void NhlD3D12CommandProcessor::WriteInjectSidecar() {
#if NHLLEGACY_HAVE_RX2FFI
  if (inject_sidecar_written_ || !inject_correlate_enabled_) {
    return;
  }
  inject_sidecar_written_ = true;
  auto& reg = InjectionRegistry::Get();
  if (reg.resolved_count() == 0) {
    REXLOG_INFO("[nhl-inject] no correlations resolved; sidecar not written");
    return;
  }
  // Write next to the trace: "<NHL_REPLAY_XTR>.inject.json" in replay, else under the
  // capture root for a live capture.
  std::filesystem::path out;
  if (const char* xtr = std::getenv("NHL_REPLAY_XTR")) {
    out = std::filesystem::path(xtr).replace_extension(".inject.json");
  } else {
    out = capture_root_ / "capture.inject.json";
  }
  if (reg.WriteSidecar(out)) {
    REXLOG_INFO("[nhl-inject] wrote sidecar {} ({} mappings)", out.string(), reg.resolved_count());
  } else {
    REXLOG_WARN("[nhl-inject] FAILED to write sidecar {}", out.string());
  }
#endif  // NHLLEGACY_HAVE_RX2FFI
}

void NhlD3D12CommandProcessor::EnsureBetaFakeTexture() {
  if (beta_fake_tex_ready_) {
    return;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  constexpr uint32_t kW = 8, kH = 8;
  D3D12_HEAP_PROPERTIES hp_default{};
  hp_default.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = kW;
  td.Height = kH;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  if (FAILED(device->CreateCommittedResource(&hp_default, D3D12_HEAP_FLAG_NONE, &td,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&beta_fake_tex_)))) {
    REXLOG_ERROR("[nhl-beta] fake-tex: create texture failed");
    return;
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0;
  UINT64 row_size = 0, total = 0;
  device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_size, &total);
  D3D12_HEAP_PROPERTIES hp_upload{};
  hp_upload.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&hp_upload, D3D12_HEAP_FLAG_NONE, &bd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&beta_fake_tex_upload_)))) {
    REXLOG_ERROR("[nhl-beta] fake-tex: create upload failed");
    return;
  }
  uint8_t* mapped = nullptr;
  D3D12_RANGE none{0, 0};
  beta_fake_tex_upload_->Map(0, &none, reinterpret_cast<void**>(&mapped));
  for (uint32_t y = 0; y < kH; ++y) {
    uint8_t* row = mapped + fp.Footprint.RowPitch * y;
    for (uint32_t x = 0; x < kW; ++x) {
      row[x * 4 + 0] = 255;  // R
      row[x * 4 + 1] = 0;
      row[x * 4 + 2] = 0;
      row[x * 4 + 3] = 255;  // A (opaque so alpha-test + blend keep it)
    }
  }
  beta_fake_tex_upload_->Unmap(0, nullptr);
  d3d12::DeferredCommandList& dcl = GetDeferredCommandList();
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = beta_fake_tex_.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = beta_fake_tex_upload_.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = fp;
  dcl.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  PushTransitionBarrier(beta_fake_tex_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  SubmitBarriers();
  beta_fake_tex_ready_ = true;
  REXLOG_INFO("[nhl-beta] fake-tex: 8x8 solid-red texture created + uploaded (NHL_BETA_FAKETEX)");
}

NhlD3D12CommandProcessor::BetaVsTexEntry* NhlD3D12CommandProcessor::EnsureBetaVsTexSub(
    uint32_t guest_base, uint32_t width, uint32_t height) {
  if (!memory_ || !width || !height) {
    return nullptr;
  }
  BetaVsTexEntry& e = beta_vstex_subs_[guest_base];
  if (e.ready && !e.dirty) {
    return &e;
  }
  const uint8_t* guest = memory_->TranslatePhysical<const uint8_t*>(guest_base);
  if (!guest) {
    REXLOG_ERROR("[nhl-beta] vstex-sub: TranslatePhysical(0x{:X}) failed", guest_base);
    return nullptr;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = width;
  td.Height = height;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  td.SampleDesc.Count = 1;
  const bool fresh = !e.tex;
  if (fresh) {
    e.width = width;
    e.height = height;
    D3D12_HEAP_PROPERTIES hp_default{};
    hp_default.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&hp_default, D3D12_HEAP_FLAG_NONE, &td,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&e.tex)))) {
      REXLOG_ERROR("[nhl-beta] vstex-sub: create texture failed (0x{:X})", guest_base);
      beta_vstex_subs_.erase(guest_base);
      return nullptr;
    }
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0;
  UINT64 row_size = 0, total = 0;
  device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_size, &total);
  if (!e.upload) {
    D3D12_HEAP_PROPERTIES hp_upload{};
    hp_upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp_upload, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&e.upload)))) {
      REXLOG_ERROR("[nhl-beta] vstex-sub: create upload failed (0x{:X})", guest_base);
      beta_vstex_subs_.erase(guest_base);
      return nullptr;
    }
  }
  // NOTE: re-filling the upload buffer assumes any previously recorded copy from it
  // has executed (true for the one-capture-frame flow: the dirtying trace write lands
  // before the frame's first VS-textured draw).
  uint8_t* mapped = nullptr;
  D3D12_RANGE none{0, 0};
  e.upload->Map(0, &none, reinterpret_cast<void**>(&mapped));
  const uint32_t row_bytes = width * 16;  // float4 texels, linear guest layout
  uint64_t nz_words = 0;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t* s = reinterpret_cast<const uint32_t*>(guest + size_t(y) * row_bytes);
    uint32_t* d = reinterpret_cast<uint32_t*>(mapped + fp.Footprint.RowPitch * y);
    for (uint32_t w = 0; w < width * 4; ++w) {
      const uint32_t v = s[w];
      d[w] = _byteswap_ulong(v);  // guest endian=2 (k8in32): swap bytes within each dword
      if (v) ++nz_words;
    }
  }
  e.upload->Unmap(0, nullptr);
  d3d12::DeferredCommandList& dcl = GetDeferredCommandList();
  if (!fresh) {
    PushTransitionBarrier(e.tex.Get(),
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COPY_DEST);
    SubmitBarriers();
  }
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = e.tex.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = e.upload.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = fp;
  dcl.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  PushTransitionBarrier(e.tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  SubmitBarriers();
  e.ready = true;
  e.dirty = false;
  REXLOG_INFO("[nhl-beta] vstex-sub: {}x{} float4 texture built from guest 0x{:X} "
              "({} non-zero dwords) {}",
              width, height, guest_base, nz_words, fresh ? "created+uploaded" : "re-uploaded");
  return &e;
}

namespace {
D3D_PRIMITIVE_TOPOLOGY HostPrimToD3DTopology(uint32_t host_prim) {
  switch (host_prim) {
    case 0x01: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case 0x02: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case 0x03: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case 0x04: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 0x06: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case 0x08: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  // rect list: GS-expanded (triangle in)
    // Quad list (0x0D/kQuadList): the SDK pipeline cache attaches a quad geometry
    // shader (PipelineGeometryShader::kQuadList) whose input is `lineadj` — 4
    // vertices per primitive. The matching IA topology is LINELIST_WITH_ADJ, NOT a
    // triangle list. Feeding triangles (raw or index-expanded) desyncs the GS and
    // it emits nothing — the cause of the invisible "ENGLISH" language-select text.
    case 0x0D: return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
    default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  }
}
}  // namespace

// Route (a): on a guest resolve (kCopy) in the flat path, copy our flat offscreen RT
// (the just-rendered pass) into a host texture keyed by the resolve DEST address, then
// clear the RT for the next pass. A later draw sampling that dest binds this host texture
// (see write_tex_table) instead of a guest-RAM untile of the resolved surface — so the
// composite samples our flat host RT and the player lands correctly (no fold, no untile).
void NhlD3D12CommandProcessor::BetaFlatResolve() {
  if (!beta_offscreen_rt_ || !beta_rtv_heap_ || !memory_) return;
  // The guest resolve's physical dest address (what a later texture fetch will sample)
  // is NOT RB_COPY_DEST_BASE<<12 — Xenos uses tiled resolve addressing. GetResolveInfo
  // computes the real copy_dest_base, matching the sampling draw's fetch-constant base.
  namespace draw_util = rex::graphics::draw_util;
  draw_util::ResolveInfo ri{};
  if (!draw_util::GetResolveInfo(*register_file_, *memory_, trace_writer_, 1, 1, false, false, ri)) {
    return;
  }
  const uint32_t dest = ri.copy_dest_base;
  if (!dest) return;
  if (std::getenv("NHL_BETA_FLAT_DUMP") || std::getenv("NHL_BETA_FLAT_DIAG")) {
    REXLOG_INFO("[nhl-beta] flat-resolve src #{}: dest=0x{:X} is_depth={} color_orig_base={} "
                "depth_orig_base={} color_edram_base={} depth_edram_base={} src_wxh=({}x{}) "
                "edram_off_div8=({},{}) dest_off_div8=({},{}) dest_pitch={}",
                uint32_t(beta_flat_dumps_.size()), dest, uint32_t(ri.IsCopyingDepth()),
                ri.color_original_base, ri.depth_original_base,
                uint32_t(ri.color_edram_info.base_tiles), uint32_t(ri.depth_edram_info.base_tiles),
                uint32_t(ri.coordinate_info.width_div_8) * 8, uint32_t(ri.height_div_8) * 8,
                uint32_t(ri.coordinate_info.edram_offset_x_div_8),
                uint32_t(ri.coordinate_info.edram_offset_y_div_8),
                uint32_t(ri.copy_dest_coordinate_info.offset_x_div_8),
                uint32_t(ri.copy_dest_coordinate_info.offset_y_div_8),
                uint32_t(ri.copy_dest_coordinate_info.pitch_aligned_div_32) * 32);
  }
  // NHL_BETA_FLAT_KEEPFIRST: a dest can be resolved multiple times per frame; if a later
  // resolve's scratch is empty it would overwrite a good earlier capture. Keep the first.
  if (std::getenv("NHL_BETA_FLAT_KEEPFIRST")) {
    auto exist = beta_flat_resolves_.find(dest);
    if (exist != beta_flat_resolves_.end() && exist->second.tex) return;
  }
  // DEPTH resolves (shadow/depth-map captures, is_depth=1) must NOT snapshot the COLOR
  // RT: when the guest resolves depth to the SAME dest as a color image (the player
  // composite 0x1AF09000 is resolved twice), the depth-as-color copy OVERWRITES the
  // good color capture — the composite then shows the player as the green depth
  // encoding instead of its shaded color. Skip the color snapshot for depth resolves
  // (the dest keeps its latest COLOR capture); still treat it as a pass boundary for
  // the per-pass depth clear below. NHL_BETA_FLAT_DEPTH_COPY restores the old copy.
  if (ri.IsCopyingDepth() && !std::getenv("NHL_BETA_FLAT_DEPTH_COPY")) {
    if (beta_depth_enabled_ && beta_depth_ready_ && beta_dsv_heap_) {
      GetDeferredCommandList().D3DClearDepthStencilView(
          beta_dsv_heap_->GetCPUDescriptorHandleForHeapStart(),
          D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, beta_depth_clear_, 0, 0, nullptr);
    }
    return;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  auto& fr = beta_flat_resolves_[dest];
  if (!fr.tex) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = beta_rt_width_;
    d.Height = beta_rt_height_;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&fr.tex)))) {
      REXLOG_ERROR("[nhl-beta] flat-resolve: host tex create failed dest=0x{:X}", dest);
      beta_flat_resolves_.erase(dest);
      return;
    }
    fr.state = D3D12_RESOURCE_STATE_COPY_DEST;
  }
  d3d12::DeferredCommandList& dcl = GetDeferredCommandList();
  auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dcl.D3DResourceBarrier(1, &b);
  };
  if (fr.state != D3D12_RESOURCE_STATE_COPY_DEST) {
    barrier(fr.tex.Get(), fr.state, D3D12_RESOURCE_STATE_COPY_DEST);
    fr.state = D3D12_RESOURCE_STATE_COPY_DEST;
  }
  barrier(beta_offscreen_rt_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
  dcl.D3DCopyResource(fr.tex.Get(), beta_offscreen_rt_.Get());
  barrier(beta_offscreen_rt_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_RENDER_TARGET);
  // NHL_BETA_FLAT_DUMP: snapshot THIS resolve's captured host texture into a readback buffer
  // (recorded into the same submission; written at capture finalize once the GPU is idle).
  // Lets us see, per resolve event, what landed in the dest the composite samples — the
  // decisive "does the player rasterize / which surface holds it" test, free of shared-RT
  // overwrite ambiguity. The host tex is plain RGBA8 (offscreen RT copy), so the readback +
  // PNG write mirror DumpBetaOffscreenTarget exactly (no tiling, no channel swap).
  if (std::getenv("NHL_BETA_FLAT_DUMP")) {
    ID3D12Resource* rb = nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = beta_rt_total_bytes_;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&rb)))) {
      barrier(fr.tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION ddst{};
      ddst.pResource = rb;
      ddst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      ddst.PlacedFootprint = beta_rt_footprint_;
      D3D12_TEXTURE_COPY_LOCATION dsrc{};
      dsrc.pResource = fr.tex.Get();
      dsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dsrc.SubresourceIndex = 0;
      dcl.D3DCopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
      barrier(fr.tex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      FlatResolveDump fd;
      fd.dest = dest;
      fd.seq = uint32_t(beta_flat_dumps_.size());  // ascending resolve-event index
      fd.readback.Attach(rb);
      beta_flat_dumps_.push_back(std::move(fd));
    } else {
      barrier(fr.tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
  } else {
    barrier(fr.tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  fr.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  // NOTE: do NOT clear the scratch here — the final composite pass renders into it and is
  // what we dump. Clearing after the frontbuffer resolve wiped the final frame to black.
  // Per-pass separation is instead handled by the guest's own background/clear draws.
  // (NHL_BETA_FLAT_CLEAR re-enables a transparent clear for experimentation.)
  if (std::getenv("NHL_BETA_FLAT_CLEAR")) {
    const float clear0[4] = {0, 0, 0, 0};
    dcl.D3DClearRenderTargetView(beta_rtv_heap_->GetCPUDescriptorHandleForHeapStart(), clear0, 0,
                                 nullptr);
  }
  // Per-pass depth: clear the depth target at each pass boundary (resolve) so the next
  // pass's z-test starts from the far plane. The single depth target otherwise accumulates
  // across passes, so later passes (e.g. the player) fail LESS/LEQUAL and get culled. We do
  // NOT clear color here (the final composite renders into the scratch and is what we dump).
  if (beta_depth_enabled_ && beta_depth_ready_ && beta_dsv_heap_) {
    dcl.D3DClearDepthStencilView(beta_dsv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                 D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                 beta_depth_clear_, 0, 0, nullptr);
  }
  if (std::getenv("NHL_BETA_FLAT_DIAG"))
    REXLOG_INFO("[nhl-beta] flat-resolve #{}: offscreen RT -> host tex copy_dest_base=0x{:X} "
                "extent_start=0x{:X}",
                beta_resolves_seen_++, dest, ri.copy_dest_extent_start);
}

// Defined in gpu/hooks/plume_present.cpp — C-2 shader bridge: hand a translated Xenos VS's
// SPIR-V to the plume Vulkan thread for shader-module creation (no-op unless NHL_HIGHCUT_PRESENT).
extern "C" void HighcutPublishTranslatedVS(const uint8_t* data, size_t size);

void NhlD3D12CommandProcessor::RenderBetaOwnedDraw(
    xenos::PrimitiveType primitive_type, uint32_t index_count,
    rex::graphics::CommandProcessor::IndexBufferInfo* index_buffer_info) {
  // Step 2: the REAL owned draw, reconstructed from public pieces, in full-frame
  // takeover (no base draws => the open command list is ours). Attempt #1 of the
  // binding reconstruction — instrumented at each stage. Renders into our own
  // offscreen RTV (bypassing the RT cache for output), then copies to readback.
  (void)index_count;
  namespace draw_util = rex::graphics::draw_util;
  namespace reg = rex::graphics::reg;
  if (!beta_current_vs_ || !CreateBetaOffscreenTarget(1280, 720)) {
    REXLOG_ERROR("[nhl-beta] owned-draw: no current VS or RT create failed");
    return;
  }
  // Phase 5: EDRAM multi-pass mode. When set, the RT cache owns color+depth binding and
  // the per-pass resolve flow; we do NOT bind our offscreen RTV, force MSAA to 1X, or
  // hand-roll the sub-viewport. (The offscreen RT above is still created for its
  // beta_rt_width_/height_ side effects but goes unused for output here.)
  const bool edram = beta_edram_enabled_;
  const bool edram_rov =
      edram && beta_render_target_cache_->GetPath() ==
                   rex::graphics::RenderTargetCache::Path::kPixelShaderInterlock;

  const size_t vs_tex = beta_current_vs_->texture_bindings().size();
  const size_t ps_tex = beta_current_ps_ ? beta_current_ps_->texture_bindings().size() : 0;

  // Per-frame cache ticks: once, on the first rendered draw. Process() may request
  // the index buffer in shared memory, so the shared-memory/primitive-processor
  // submission must be open first. The texture cache is also ticked so
  // RequestTextures can untile/upload bound textures this frame.
  const bool first_draw = (beta_takeover_rendered_ == 0);
  if (first_draw) {
    beta_shared_memory_->CompletedSubmissionUpdated();
    beta_shared_memory_->BeginSubmission();
    beta_primitive_processor_->CompletedSubmissionUpdated();
    beta_primitive_processor_->BeginSubmission();
    beta_primitive_processor_->BeginFrame();
    beta_texture_cache_->CompletedSubmissionUpdated(GetCompletedSubmission());
    beta_texture_cache_->BeginSubmission(GetCurrentSubmission());
    beta_texture_cache_->BeginFrame();
  }

  rex::graphics::PrimitiveProcessor::ProcessingResult result{};
  if (!beta_primitive_processor_->Process(result)) {
    REXLOG_INFO("[nhl-beta] owned-draw: Process() false (nothing to draw)");
    return;
  }

  // Pipeline: drive the output to our own R8G8B8A8 RT0, no depth. The menu's draws
  // use color format k_8_8_8_8 (enum 0), matching our RT.
  reg::RB_DEPTHCONTROL ndc = draw_util::GetNormalizedDepthControl(*register_file_);
  // DIAG: force zfunc=NEVER on depth-testing draws. If ConfigurePipeline honors zfunc,
  // the frame goes black (nothing passes); if unchanged, zfunc is being ignored (the
  // DepthFunc=ALWAYS bug). Decides whether the compare is even read.
  if (std::getenv("NHL_BETA_DEPTH_FORCE_NEVER") && ndc.z_enable) {
    ndc.zfunc = xenos::CompareFunction::kNever;
  }
  // DIAG: force the depth compare to ALWAYS (keep z-write, never cull). If a 3D model
  // hidden in EDRAM mode reappears with this set, an UNCLEARED host depth buffer (near=0
  // failing the guest LESS test) is culling it — the fix is to far-clear the host depth.
  if (std::getenv("NHL_BETA_DEPTH_FORCE_ALWAYS") && ndc.z_enable) {
    ndc.zfunc = xenos::CompareFunction::kAlways;
  }
  // NHL_BETA_DEPTH: bring up the depth target lazily on the first owned draw, keyed to
  // the guest's depth format (so our DXGI resource/DSV match the PSO). Done once.
  if (!beta_depth_checked_) {
    beta_depth_checked_ = true;
    beta_depth_enabled_ = std::getenv("NHL_BETA_DEPTH") != nullptr;
    if (const char* dc = std::getenv("NHL_BETA_DEPTH_CLEAR")) {
      beta_depth_clear_ = float(std::atof(dc));
    }
  }
  if (beta_depth_enabled_ && !edram && !beta_depth_ready_) {
    beta_depth_xenos_fmt_ = uint32_t(register_file_->Get<reg::RB_DEPTH_INFO>().depth_format);
    EnsureBetaDepthTarget();
  }
  // Per the pipeline_cache contract, if rasterization isn't potentially done the
  // pixel shader MUST be treated as null for ConfigurePipeline (it builds a
  // vs-only root signature). Using the real PS there would make us bind texture
  // tables at root params the 6-param root signature doesn't have (error 708).
  const bool polygonal = draw_util::IsPrimitivePolygonal(*register_file_);
  const bool rast_done = draw_util::IsRasterizationPotentiallyDone(*register_file_, polygonal);
  d3d12::D3D12Shader* eff_ps = rast_done ? beta_current_ps_ : nullptr;
  const uint32_t writes_color = eff_ps ? eff_ps->writes_color_targets() : 0;
  const uint32_t ncm = draw_util::GetNormalizedColorMask(*register_file_, writes_color);
  uint32_t rt_formats[1 + 4] = {0, 0, 0, 0, 0};  // [0]=depth, [1]=color RT0 = k_8_8_8_8
  uint32_t bound_rt_bits = edram_rov ? 0 : uint32_t(1) << 1;
  // Depth path: manual bound bits make ConfigurePipeline honor the COLOR format but NOT
  // the depth COMPARE (zfunc) — it enables depth-write but leaves DepthFunc=ALWAYS
  // (verified: depth buffer writes varying z, yet clear=0 and clear=1 are byte-identical).
  // The SDK only emits the guest depth compare when the RT cache has bound a depth target
  // via Update() (docs Phase 5.1). So drive Update() to get the real bound bits/formats.
  // Update() also records its own OMSetRenderTargets/tile-transfers, but our
  // OMSetRenderTargets below re-binds our offscreen RTV/DSV so the render still goes to
  // our targets. Gated by NHL_BETA_DEPTH — the validated menu/intro paths never run this.
  // Drive the RT cache when either the manual depth path (NHL_BETA_DEPTH) or the EDRAM
  // multi-pass path (NHL_BETA_EDRAM) is active. EDRAM mode relies on Update() to BIND the
  // EDRAM-backed color+depth host render targets for this pass (no offscreen RTV below).
  const bool use_rtcache = edram || (beta_depth_enabled_ && beta_depth_ready_);
  if (use_rtcache) {
    // Begin the RT cache submission once, and re-begin whenever the submission index
    // changes (the first owned draw's sparse tile-mapping can close+reopen the swap
    // submission mid-frame; the RT cache's per-submission state must follow). Without
    // this the EDRAM mode's cached command-list RT binding would go stale on reopen.
    const uint64_t sub_now = GetCurrentSubmission();
    if (!beta_rtcache_ticked_ || beta_rtcache_submission_ != sub_now) {
      beta_rtcache_ticked_ = true;
      beta_rtcache_submission_ = sub_now;
      beta_render_target_cache_->CompletedSubmissionUpdated();
      beta_render_target_cache_->BeginSubmission();
      // Re-seed the beta EDRAM from the captured snapshot at the start of every beta
      // submission/frame. The replay loops a single frame; reseeding gives each frame the
      // same cleared-color + far-depth start the title's capture saw, instead of letting
      // the prior frame's depth/color store-backs accumulate (which self-occludes the
      // model and tints the background). EDRAM mode only — the depth-only path manages its
      // own clear. Guarded by snapshot-valid so non-snapshot traces are unaffected.
      // DIAG (NHL_BETA_NO_RESEED): skip the per-frame snapshot reseed. The snapshot is the
      // EDRAM image at capture instant; if its depth tile (e.g. player tile 736) holds NEAR
      // values, reseeding clobbers any in-frame far-clear and re-culls the LEQUAL player.
      // If the player reappears with reseed off, the reseed is the depth-cull source.
      if (edram && beta_edram_snapshot_valid_ && !std::getenv("NHL_BETA_NO_RESEED")) {
        beta_render_target_cache_->RestoreEdramSnapshot(beta_edram_snapshot_.data());
      }
    }
    // EDRAM mode: force Update() to re-bind the OM render targets on the CURRENT deferred
    // command list every draw (we never bind our own RTV below, and the binding is lost
    // across submission reopen). For the depth-only path we keep the original behavior.
    if (edram) {
      beta_render_target_cache_->InvalidateCommandListRenderTargets();
    }
    beta_render_target_cache_->Update(rast_done, ndc, ncm, *beta_current_vs_);
    uint32_t rt_fmts_rtc[1 + 4] = {};
    const uint32_t rtb = beta_render_target_cache_->GetLastUpdateBoundRenderTargets(rt_fmts_rtc);
    if (rtb) {
      bound_rt_bits = rtb;
      for (int i = 0; i < 1 + 4; ++i) rt_formats[i] = rt_fmts_rtc[i];
    } else if (edram_rov) {
      bound_rt_bits = 0;
    } else if (!edram) {
      // Fallback: RT cache reported nothing — advertise depth manually (write only).
      rt_formats[0] = beta_depth_xenos_fmt_;
      bound_rt_bits |= uint32_t(1) << 0;
    }
  }
  if (beta_depth_enabled_ && std::getenv("NHL_BETA_DEPTH_DIAG")) {
    const auto raw = register_file_->Get<reg::RB_DEPTHCONTROL>();
    REXLOG_INFO("[nhl-beta] depth-diag #{}: ndc(z_en={} z_wr={} zfunc={}) raw(z_en={} z_wr={} "
                "zfunc={}) bound_bits=0x{:X} rtfmt0={} rast_done={}",
                beta_takeover_rendered_, uint32_t(ndc.z_enable), uint32_t(ndc.z_write_enable),
                uint32_t(ndc.zfunc), uint32_t(raw.z_enable), uint32_t(raw.z_write_enable),
                uint32_t(raw.zfunc), bound_rt_bits, rt_formats[0], rast_done);
  }

  // Force a single-sample PSO. The menu draws at 4X MSAA, so ConfigurePipeline
  // (which reads RB_SURFACE_INFO.msaa_samples from the register file) would build
  // a 4-sample PSO that mismatches our single-sample offscreen RT
  // (DrawInstanced error 614). The DeferredCommandList has no ResolveSubresource,
  // so we can't resolve MSAA on this recording surface — for the Phase-4 mesh
  // proof, override MSAA to 1X around the modification + ConfigurePipeline calls
  // and restore immediately after. (Phase-5 parity will need a real MSAA RT +
  // resolve path once the recording surface supports it.)
  const uint32_t kSurfInfoIdx = reg::RB_SURFACE_INFO::register_index;
  const uint32_t saved_surface_info = (*register_file_)[kSurfInfoIdx];
  // EDRAM mode: the RT cache creates host render targets at the guest's real MSAA, so
  // leave RB_SURFACE_INFO untouched and let ConfigurePipeline build a matching-sample PSO
  // (the per-pass resolve handles the downsample). Only the offscreen-RTV path forces 1X.
  if (!edram) {
    // Force the PSO's sample count to match our offscreen RT. Without MSAA the RT is
    // single-sample (the menu draws natively at 4X, which would mismatch our 1X RT and
    // trip DrawInstanced error 614). With NHL_BETA_MSAA the RT is multisample, so force
    // the SAME count on every draw — guaranteeing PSO/RT agreement and bit-matching the
    // oracle's 4X-MSAA coverage AA.
    reg::RB_SURFACE_INFO si;
    si.value = saved_surface_info;
    si.msaa_samples = (beta_msaa_ >= 4)   ? xenos::MsaaSamples::k4X
                      : (beta_msaa_ >= 2) ? xenos::MsaaSamples::k2X
                                          : xenos::MsaaSamples::k1X;
    (*register_file_)[kSurfInfoIdx] = si.value;
  }
  // DIAG: NHL_BETA_NOBLEND forces opaque (1*src + 0*dst) on all RTs around the PSO
  // build, so the PS color is written verbatim regardless of output alpha. Isolates
  // whether the textured draws are invisible due to alpha blending (alpha~0 src) vs.
  // genuinely producing no pixels. Restored with the MSAA register after PSO build.
  const bool beta_noblend = std::getenv("NHL_BETA_NOBLEND") != nullptr;
  // RB_BLENDCONTROL0..3 live at 0x2201, 0x2209, 0x220A, 0x220B — NOT 0x2201+i.
  // (0x2202..0x2204 are RB_COLORCONTROL/RB_COLOR_MASK/RB_COLOR_RED; writing the
  // diagnostic blend value there corrupted unrelated color-control state for RT1-3.)
  uint32_t saved_blend[4] = {0, 0, 0, 0};
  if (beta_noblend) {
    for (uint32_t i = 0; i < 4; ++i) {
      saved_blend[i] = (*register_file_)[beta_reg::kBlendControl[i]];
      (*register_file_)[beta_reg::kBlendControl[i]] = 0x00010001;  // src*1 + dst*0 => disabled
    }
  }
  // NHL_BETA_NOCULL: force PA_SU_SC_MODE_CNTL cull_front/cull_back OFF around the PSO
  // build. Isolates whether 3D geometry is invisible because back-face culling (63% of
  // gameplay draws) is rejecting EVERY triangle — which happens if our RT's winding /
  // front_counter_clockwise convention is inverted relative to the guest.
  constexpr uint32_t kModeCntlIdx = 0x2205;  // PA_SU_SC_MODE_CNTL
  const bool beta_nocull = std::getenv("NHL_BETA_NOCULL") != nullptr;
  const uint32_t saved_mode_cntl = (*register_file_)[kModeCntlIdx];
  if (beta_nocull) {
    (*register_file_)[kModeCntlIdx] = saved_mode_cntl & ~0x3u;  // clear cull_front|cull_back
  }

  // VS and PS modifications MUST share ONE interpolator mask or their I/O
  // signatures mismatch (CreateGraphicsPipelineState linkage error 666/660). The
  // transferred set is what the VS writes AND the PS reads.
  uint32_t param_gen_pos = UINT32_MAX;
  const uint32_t vs_writes_interp = beta_current_vs_->writes_interpolators();
  uint32_t interpolator_mask = vs_writes_interp;
  uint32_t ps_input_mask = 0;
  if (eff_ps) {
    ps_input_mask = eff_ps->GetInterpolatorInputMask(
        register_file_->Get<reg::SQ_PROGRAM_CNTL>(), register_file_->Get<reg::SQ_CONTEXT_MISC>(),
        param_gen_pos);
    interpolator_mask &= ps_input_mask;
  }
  if (std::getenv("NHL_BETA_BIND_DIAG") && eff_ps) {
    REXLOG_INFO("[nhl-beta]   interp: vs_writes=0x{:X} ps_input=0x{:X} final_mask=0x{:X} "
                "param_gen_pos={} sq_prog_cntl=0x{:X} sq_ctx_misc=0x{:X}",
                vs_writes_interp, ps_input_mask, interpolator_mask, param_gen_pos,
                register_file_->Get<reg::SQ_PROGRAM_CNTL>().value,
                register_file_->Get<reg::SQ_CONTEXT_MISC>().value);
  }
  // High-cut path C, P-3 (gated NHL_HIGHCUT_XLAT_TEST, once per process): translate the
  // current VS's Xenos ucode to SPIR-V via the ported SpirvShaderTranslator and dump the
  // bytes for spirv-val. FULLY ISOLATED — a fresh SpirvShader built from the same ucode,
  // never touching beta_current_vs_ (the live DXBC D3D12Shader), so the validated beta
  // path is byte-identical. The fresh shader owns its own translation slot, so there is
  // no clobber risk (is_new is always true). See docs/highcut-c-plume-renderer-plan.md.
  // C-3b.2 draw survey: translate the VS of the first kP3MaxDraws owned draws, dump each to a
  // numbered file, and log each one's vertex-binding count (the vfetch indicator) + ucode size,
  // so a REAL geometry draw (one that fetches vertices) can be identified — the first owned draw
  // is procedurally trivial (no vfetch -> degenerate point). highcut_p3_vs.spv stays = draw 0 for
  // the existing C-2/C-3 path. Select which draw the plume bridge gets via NHL_HIGHCUT_XLAT_DRAW.
  bool p3_dump_data = false;  // C-3b.2: dump this draw's data packet (set when it's the selected draw)
  // C-4: the selected draw's translated VS+PS + the PS texture/sampler interface, captured in the
  // survey block and consumed by the packet dump below (after vpi). Empty => VS-only C-3 path.
  std::vector<uint8_t> p3_vs_spirv;  // C-5a: masked VS dumped INLINE per draw (frame capture)
  std::vector<uint8_t> p3_ps_spirv;
  std::vector<rex::graphics::SpirvShader::TextureBinding> p3_ps_texbinds;
  uint32_t p3_ps_sampler_count = 0;
  // C-5a: NHL_HIGHCUT_FRAME_CAPTURE dumps EVERY interesting owned draw of the frame (to
  // highcut_frame_<idx>.bin) for the multi-draw plume replay, not just the C-4 single selected draw.
  const bool frame_capture = std::getenv("NHL_HIGHCUT_FRAME_CAPTURE") != nullptr;
  static std::atomic<int> highcut_p3_count{0};
  constexpr int kP3MaxDraws = 32;
  if (std::getenv("NHL_HIGHCUT_XLAT_TEST") || frame_capture) {
    // C-4: only survey INTERESTING draws (a vfetch VS or a textured PS) — skip the many trivial
    // boot-overlay draws so textured menu draws are reached. (Bindings come from the SDK's shader
    // analysis, no translation needed for the pre-check.)
    const bool p3_interesting =
        (beta_current_vs_ && !beta_current_vs_->vertex_bindings().empty()) ||
        (beta_current_ps_ && !beta_current_ps_->texture_bindings().empty());
    const int p3_idx = p3_interesting ? highcut_p3_count.fetch_add(1) : -1;
    // Frame capture surveys EVERY interesting draw (no 32-draw cap); the C-4 single-draw survey caps.
    if (p3_interesting && (frame_capture || (p3_idx >= 0 && p3_idx < kP3MaxDraws))) {
    namespace rg = rex::graphics;
    rg::SpirvShader p3_vs(beta_current_vs_->type(), beta_current_vs_->ucode_data_hash(),
                          beta_current_vs_->ucode_dwords(),
                          beta_current_vs_->ucode_dword_count(), std::endian::native);
    rex::string::StringBuffer p3_disasm;
    p3_vs.AnalyzeUcode(p3_disasm);
    const uint32_t p3_num_reg = register_file_->Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg;
    const uint32_t p3_reg_count = p3_vs.GetDynamicAddressableRegisterCount(p3_num_reg);
    rg::SpirvShaderTranslator::Features p3_features(/*all=*/true);
    // C-3: plume's Vulkan device does not enable VK_KHR_shader_float_controls; the driver
    // crashes in vkCreateGraphicsPipelines compiling a shader that declares the denorm/rounding/
    // NaN-preserve execution modes. Disable those float-controls for now (a bring-up accuracy
    // simplification — re-enable once the plume device advertises float-controls support).
    p3_features.signed_zero_inf_nan_preserve_float32 = false;
    p3_features.denorm_flush_to_zero_float32 = false;
    p3_features.rounding_mode_rte_float32 = false;
    rg::SpirvShaderTranslator p3_xlat(p3_features, /*native_2x_msaa_with_attachments=*/false,
                                      /*native_2x_msaa_no_attachments=*/false,
                                      /*edram_fragment_shader_interlock=*/false);
    // C-3b.3: a Xenos RectangleList is left as host_prim=rect/hvst=kVertex by the beta path (the
    // SDK D3D12 backend expands rects natively). plume has no rect primitive, so translate the VS
    // as kRectangleListAsTriangleStrip — it synthesizes the 4th corner in-shader — and draw a
    // 4-vertex triangle strip on plume (full quad instead of the 3-corner half).
    auto p3_hvst = result.host_vertex_shader_type;
    if (result.guest_primitive_type == xenos::PrimitiveType::kRectangleList &&
        p3_hvst == rg::Shader::HostVertexShaderType::kVertex) {
      p3_hvst = rg::Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
    }
    const uint64_t p3_mod =
        p3_xlat.GetDefaultVertexShaderModification(p3_reg_count, p3_hvst);
    bool p3_is_new = false;
    rg::Shader::Translation* p3_tr = p3_vs.GetOrCreateTranslation(p3_mod, &p3_is_new);
    const bool p3_ok = p3_xlat.TranslateAnalyzedShader(*p3_tr);
    const auto& p3_bin = p3_tr->translated_binary();
    const size_t p3_vbinds = p3_vs.vertex_bindings().size();
    const size_t p3_vs_tex = p3_vs.texture_bindings().size();
    // C-4: textures live in the PIXEL shader — report its texture-binding count so a textured menu
    // draw can be found (the boot-overlay draws have none).
    const size_t p3_ps_tex = beta_current_ps_ ? beta_current_ps_->texture_bindings().size() : 0;
    REXLOG_INFO("[highcut-P3] draw#{}: ucode_dwords={} reg_count={} vfetch_bindings={} "
                "vs_tex={} ps_tex={} ok={} is_valid={} spirv_bytes={}",
                p3_idx, p3_vs.ucode_dword_count(), p3_reg_count, p3_vbinds, p3_vs_tex, p3_ps_tex,
                p3_ok, p3_tr->is_valid(), p3_bin.size());
    if (p3_tr->is_valid() && !p3_bin.empty()) {
      if (!frame_capture) {  // C-4 survey numbered dump (for selecting a draw); not needed per-frame
        char p3_path[64];
        std::snprintf(p3_path, sizeof(p3_path), "highcut_p3_vs_%03d.spv", p3_idx);
        if (std::FILE* p3_f = std::fopen(p3_path, "wb")) {
          std::fwrite(p3_bin.data(), 1, p3_bin.size(), p3_f);
          std::fclose(p3_f);
        }
      }
      // Selected draw (default 0) feeds the plume bridge. C-4: re-translate the VS AND the PS with
      // the SHARED interpolator mask (the AND of VS-writes & PS-reads computed above) so their
      // SPIR-V I/O signatures match — otherwise pipeline linking on plume fails. The numbered
      // survey dump above keeps the default-mask VS (unchanged); only the bridge gets the masked
      // pair. With no PS bound, the bridge still gets the masked VS (the solid-PS C-3 path).
      static const int p3_sel = []() {
        const char* s = std::getenv("NHL_HIGHCUT_XLAT_DRAW");
        return s ? int(std::strtol(s, nullptr, 10)) : 0;
      }();
      // C-5a: in frame-capture mode EVERY interesting draw is fully translated + dumped (the masked
      // VS goes INLINE in the per-draw packet); the C-4 path does this only for the selected draw.
      if (p3_idx == p3_sel || frame_capture) {
        // VS, re-translated with the shared interpolator mask (a distinct modification value ->
        // its own translation slot; reuses p3_xlat, which is stateless between translations).
        rg::SpirvShaderTranslator::Modification vs_modw(
            p3_xlat.GetDefaultVertexShaderModification(p3_reg_count, p3_hvst));
        vs_modw.vertex.interpolator_mask = interpolator_mask;
        bool vs_new = false;
        rg::Shader::Translation* vs_trw = p3_vs.GetOrCreateTranslation(vs_modw.value, &vs_new);
        const bool vs_okw = p3_xlat.TranslateAnalyzedShader(*vs_trw);
        const auto& vs_binw =
            (vs_okw && vs_trw->is_valid() && !vs_trw->translated_binary().empty())
                ? vs_trw->translated_binary()
                : p3_bin;  // fall back to the default-mask VS if the masked retranslate failed
        p3_vs_spirv.assign(vs_binw.begin(), vs_binw.end());  // inline VS for the packet
        if (!frame_capture) {  // C-4 single-draw bridge writes the shared .spv + in-memory publish
          if (std::FILE* p3_f = std::fopen("highcut_p3_vs.spv", "wb")) {
            std::fwrite(vs_binw.data(), 1, vs_binw.size(), p3_f);
            std::fclose(p3_f);
          }
          HighcutPublishTranslatedVS(vs_binw.data(), vs_binw.size());
          REXLOG_INFO("[highcut-P3] selected draw#{} -> highcut_p3_vs.spv (masked=0x{:X}) + bridge",
                      p3_idx, interpolator_mask);
        }
        p3_dump_data = true;  // also dump this draw's data packet (below, after vpi)
        // PS (C-4): translate the textured pixel shader from a FRESH SpirvShader so its SPIR-V and
        // its texture/sampler bindings come from one analysis, matching the dumped module exactly.
        if (eff_ps) {
          rg::SpirvShader p3_ps(eff_ps->type(), eff_ps->ucode_data_hash(), eff_ps->ucode_dwords(),
                                eff_ps->ucode_dword_count(), std::endian::native);
          rex::string::StringBuffer ps_disasm;
          p3_ps.AnalyzeUcode(ps_disasm);
          const uint32_t ps_num_reg = register_file_->Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg;
          const uint32_t ps_reg_count = p3_ps.GetDynamicAddressableRegisterCount(ps_num_reg);
          rg::SpirvShaderTranslator::Modification ps_modw(
              p3_xlat.GetDefaultPixelShaderModification(ps_reg_count));
          ps_modw.pixel.interpolator_mask = interpolator_mask;
          // Param-gen: if the PS consumes the generated pixel position/point input (param_gen_pos
          // valid), enable it at that interpolator slot so the I/O layout matches the VS pairing.
          if (param_gen_pos != UINT32_MAX) {
            ps_modw.pixel.param_gen_enable = 1;
            ps_modw.pixel.param_gen_interpolator = param_gen_pos & 0xF;
            ps_modw.pixel.param_gen_point =
                primitive_type == xenos::PrimitiveType::kPointList ? 1 : 0;
          }
          bool ps_new = false;
          rg::Shader::Translation* ps_trw = p3_ps.GetOrCreateTranslation(ps_modw.value, &ps_new);
          const bool ps_okw = p3_xlat.TranslateAnalyzedShader(*ps_trw);
          const auto& ps_bin = ps_trw->translated_binary();
          REXLOG_INFO("[highcut-P3] PS translate: ok={} valid={} bytes={} tex_binds={} samp_binds={}",
                      ps_okw, ps_trw->is_valid(), ps_bin.size(),
                      p3_ps.GetTextureBindingsAfterTranslation().size(),
                      p3_ps.GetSamplerBindingsAfterTranslation().size());
          if (ps_okw && ps_trw->is_valid() && !ps_bin.empty()) {
            p3_ps_spirv.assign(ps_bin.begin(), ps_bin.end());
            p3_ps_texbinds = p3_ps.GetTextureBindingsAfterTranslation();
            p3_ps_sampler_count = uint32_t(p3_ps.GetSamplerBindingsAfterTranslation().size());
            if (!frame_capture) {  // C-4 single-draw debug dump; the packet carries inline PS SPIR-V
              if (std::FILE* psf = std::fopen("highcut_p3_ps.spv", "wb")) {
                std::fwrite(ps_bin.data(), 1, ps_bin.size(), psf);
                std::fclose(psf);
              }
            }
          }
        }
      }
    }
    }
  }

  const uint64_t vs_mod = beta_pipeline_cache_
                              ->GetCurrentVertexShaderModification(
                                  *beta_current_vs_, result.host_vertex_shader_type,
                                  interpolator_mask)
                              .value;
  auto* vs_tr = static_cast<d3d12::D3D12Shader::D3D12Translation*>(
      beta_current_vs_->GetOrCreateTranslation(vs_mod));
  d3d12::D3D12Shader::D3D12Translation* ps_tr = nullptr;
  if (eff_ps) {
    const uint64_t ps_mod = beta_pipeline_cache_
                                ->GetCurrentPixelShaderModification(*eff_ps, interpolator_mask,
                                                                    param_gen_pos, ndc)
                                .value;
    ps_tr = static_cast<d3d12::D3D12Shader::D3D12Translation*>(
        eff_ps->GetOrCreateTranslation(ps_mod));
  }
  // Translate the shaders NOW so their texture/sampler bindings are populated
  // before ConfigurePipeline builds the root signature from them; otherwise a
  // shader's first use gets a 0-binding (6-param) root sig while we bind a texture
  // table (error 708). Safe now that the MSAA override is restored AFTER PSO
  // creation (the earlier white-image regression was the MSAA id=201 issue, fixed).
  if (beta_translator_) {
    if (!vs_tr->is_translated()) beta_translator_->TranslateAnalyzedShader(*vs_tr);
    if (ps_tr && !ps_tr->is_translated()) beta_translator_->TranslateAnalyzedShader(*ps_tr);
  }
  // The shader's texture/sampler bindings are populated by an ASYNC translation
  // that EndSubmission/IsCreatingPipelines don't synchronize; if it isn't done when
  // ConfigurePipeline reads the binding counts, the root sig is built with 0 texture
  // params (id=708, non-deterministic). Spin briefly until ps_tr reports translated
  // AND its binding vector settles, so the FIRST ConfigurePipeline sees real counts.
  if (ps_tr && beta_current_ps_) {
    int spin = 0;
    while (spin < 200 && !ps_tr->is_translated()) { Sleep(1); ++spin; }
    if (std::getenv("NHL_BETA_BIND_DIAG")) {
      REXLOG_INFO("[nhl-beta] bind-wait: spin={}ms ps_translated={} ps_texbind={}", spin,
                  ps_tr->is_translated(), eff_ps ? eff_ps->GetTextureBindingsAfterTranslation().size()
                                                 : size_t(0));
    }
  }
  if (std::getenv("NHL_BETA_BIND_DIAG") && eff_ps) {
    REXLOG_INFO("[nhl-beta] PRE-CONFIG: translator={} ps_translated={} ps_texbind={} ps_sampbind={}",
                beta_translator_ != nullptr, ps_tr && ps_tr->is_translated(),
                eff_ps->GetTextureBindingsAfterTranslation().size(),
                eff_ps->GetSamplerBindingsAfterTranslation().size());
  }
  void* pso_handle = nullptr;
  ID3D12RootSignature* root_sig = nullptr;
  // EndSubmission() flushes our pipeline cache's queued work (the base flushes only
  // its OWN cache); the spin waits until the PSO + the shader's async translation
  // are done. CRITICAL: keep the MSAA=1X override in effect until the PSO is fully
  // built (the async PSO thread reads register MSAA/topology at creation time; an
  // early restore builds the PSO at 4X while root_sig was computed at 1X -> id=201).
  auto flush_and_wait = [&]() {
    beta_pipeline_cache_->EndSubmission();
    for (int spin = 0; spin < 1000 && beta_pipeline_cache_->IsCreatingPipelines() &&
                       !beta_pipeline_cache_->GetD3D12PipelineByHandle(pso_handle);
         ++spin) {
      Sleep(1);
    }
  };
  bool configured = beta_pipeline_cache_->ConfigurePipeline(
      vs_tr, ps_tr, result, ndc, ncm, bound_rt_bits, rt_formats, &pso_handle, &root_sig);
  flush_and_wait();
  // ConfigurePipeline builds the root signature from the shader's texture/sampler
  // binding COUNTS, but the internal translation that POPULATES those bindings only
  // completes (asynchronously) during the flush above — so a textured shader's FIRST
  // use yields a 6-param root sig (no texture params), and our param-6 table bind
  // then faults at execute time (id=708; non-deterministic, masked only by
  // RenderDoc's serialization). Now that the bindings are populated, reconfigure:
  // the second call builds the correct N-param root sig (+ matching PSO), then flush.
  if (configured && eff_ps && !eff_ps->GetTextureBindingsAfterTranslation().empty()) {
    configured = beta_pipeline_cache_->ConfigurePipeline(
        vs_tr, ps_tr, result, ndc, ncm, bound_rt_bits, rt_formats, &pso_handle, &root_sig);
    flush_and_wait();
  }
  ID3D12PipelineState* pso_ready = beta_pipeline_cache_->GetD3D12PipelineByHandle(pso_handle);
  (*register_file_)[kSurfInfoIdx] = saved_surface_info;  // restore AFTER PSO is built
  if (beta_noblend) {
    for (uint32_t i = 0; i < 4; ++i) (*register_file_)[beta_reg::kBlendControl[i]] = saved_blend[i];
  }
  if (beta_nocull) (*register_file_)[kModeCntlIdx] = saved_mode_cntl;
  if (!configured || !pso_handle || !root_sig || !pso_ready) {
    REXLOG_ERROR("[nhl-beta] owned-draw: ConfigurePipeline/PSO not ready (configured={} pso={})",
                 configured, pso_handle);
    return;
  }
  if (std::getenv("NHL_BETA_BIND_DIAG") && eff_ps) {
    ID3D12RootSignature* rs_withps = GetRootSignature(beta_current_vs_, eff_ps, false);
    ID3D12RootSignature* rs_nops = GetRootSignature(beta_current_vs_, nullptr, false);
    REXLOG_INFO("[nhl-beta] ROOTSIG cmp: configpipe={} withPS={} noPS={} -> {}",
                static_cast<void*>(root_sig), static_cast<void*>(rs_withps),
                static_cast<void*>(rs_nops),
                root_sig == rs_withps ? "8-param(withPS)"
                                      : (root_sig == rs_nops ? "6-param(noPS!)" : "other"));
  }

  // Viewport + scissor. GetHostViewportInfo derives ndc_scale from the x_max/y_max
  // we pass when the guest viewport transform is disabled (vte vport_*_ena=0, as in
  // 2D menu draws). Passing our RT size (1280x720) there gives a half ndc_scale vs
  // the guest's 640-wide surface -> geometry squished into the top-left quadrant.
  // Feed the GUEST surface size instead: width = RB_SURFACE_INFO.surface_pitch,
  // height scaled to our RT's aspect (the surface and frontbuffer share 16:9). Then
  // render to the FULL RT viewport so the native-res content upscales to fill it.
  uint32_t guest_w = register_file_->Get<reg::RB_SURFACE_INFO>().surface_pitch;
  if (!guest_w) guest_w = beta_rt_width_;
  const uint32_t guest_h = guest_w * beta_rt_height_ / beta_rt_width_;
  // x_max/y_max bound the viewport extent inside GetHostViewportInfo. For 2D draws (guest
  // viewport transform DISABLED) the viewport IS x_max/y_max, so they must equal the guest
  // surface size — this is the menu/intro calibration. But for 3D draws (viewport transform
  // ENABLED) the guest programs its own PA_CL_VPORT scale/offset and may legitimately use a
  // viewport WIDER than the surface pitch (the create-player 1280-wide player into a 640-pitch
  // surface; the scene_04 arena). Base Xenia passes D3D12_VIEWPORT_BOUNDS_MAX there (NOT the
  // pitch) so the viewport keeps full width and ndc_scale stays ~1, then clamps the SCISSOR to
  // surface_pitch instead. Passing the pitch as x_max (the old behavior) forced the clamp-and-
  // rescale path => ndc_scale bumped to 2, 3D geometry squeezed/clipped out of the resolved
  // region (blank player). Match base: big x_max for vport-enabled draws, scissor clamp below.
  uint32_t x_max = guest_w, y_max = guest_h;
  const auto vte_vp = register_file_->Get<reg::PA_CL_VTE_CNTL>();
  const bool vport_xform = vte_vp.vport_x_scale_ena && vte_vp.vport_y_scale_ena;
  // NHL_BETA_FLAT (route a): 3D passes render flat with their NATIVE guest viewport into
  // the logical-sized scratch RT — surface_pitch never enters, so the wide-into-narrow
  // EDRAM fold cannot form. Needs the un-clamped x_max so the true 1280-wide extent/ndc
  // is reported (else the player's viewport clamps to the 640 pitch and wraps).
  static const bool flat_mode = std::getenv("NHL_BETA_FLAT") != nullptr;
  if ((edram || flat_mode) && vport_xform) {
    x_max = 8192;  // ~xenos max RT size; viewport not clamped to pitch (scissor clamps instead)
    y_max = 8192;
  }
  draw_util::ViewportInfo vpi{};
  draw_util::GetHostViewportInfo(*register_file_, 1, 1, true, x_max, y_max, false, ndc, false,
                                 false, false, vpi);
  // FLAT un-fold (NHL_BETA_FLAT): scene_04's arena renders a guest viewport WIDER than the
  // surface pitch — PA_CL_VPORT_XSCALE=640 → window width 2*640=1280 — into a surface_pitch=640
  // EDRAM surface, relying on EDRAM address wrap. GetHostViewportInfo CROPS the X extent to
  // surface_pitch (640) regardless of the big x_max above (Y stays uncropped — the proven
  // asymmetry) and sets ndc_scale_x=2.0 to squish the 1280-wide projection into 640 → the fold.
  // On a FLAT host RT there is NO EDRAM wrap, so we render at the TRUE logical width: recompute
  // the X viewport straight from the guest VPORT registers (un-cropped) and reset X ndc to
  // identity. scene_02's player (XSCALE=320 → width 640 ≤ pitch 640) is never cropped, so the
  // `logical_w > extent` gate makes this a no-op there (and for any draw that already fits).
  if (flat_mode && vport_xform && !edram && !std::getenv("NHL_BETA_NO_UNFOLD")) {
    auto reg_f = [&](uint32_t idx) -> float {
      const uint32_t u = (*register_file_)[idx];
      float f;
      std::memcpy(&f, &u, sizeof(f));
      return f;
    };
    const float xs_raw = reg_f(0x210F);  // PA_CL_VPORT_XSCALE
    const float xscale = xs_raw < 0.0f ? -xs_raw : xs_raw;
    const float xoffset = reg_f(0x2110);  // PA_CL_VPORT_XOFFSET
    const uint32_t logical_w = uint32_t(2.0f * xscale + 0.5f);
    LONG l = LONG((xoffset - xscale) + 0.5f);  // window-space left edge of the guest viewport
    if (l < 0) l = 0;
    if (logical_w > vpi.xy_extent[0] && uint32_t(l) < beta_rt_width_) {
      const uint32_t w = std::min(logical_w, beta_rt_width_ - uint32_t(l));
      vpi.xy_offset[0] = uint32_t(l);
      vpi.xy_extent[0] = w;
      vpi.ndc_scale[0] = 1.0f;
      vpi.ndc_offset[0] = 0.0f;
      if (std::getenv("NHL_BETA_DEPTH_DIAG")) {
        REXLOG_INFO("[nhl-beta] UNFOLD #{}: pitch-cropped X {} -> logical {} (vp x={} w={})",
                    beta_takeover_rendered_, x_max == 8192 ? guest_w : x_max, logical_w, l, w);
      }
    }
  }

  // C-3b.2: dump this draw's data packet (system + fetch constants + shared-memory vertex bytes)
  // for the plume thread, so the translated Xenos VS can fetch + transform real vertices. Only
  // for the selected survey draw; gated by NHL_HIGHCUT_XLAT_TEST (p3_dump_data). vpi is final here.
  if (p3_dump_data && beta_current_vs_ && memory_) {
    namespace rg = rex::graphics;
    const uint32_t* regs = register_file_->values;
    // Fetch constants: the full 192-dword fetch register space -> the shader's uvec4[48] UBO.
    // REBASE the vertex fetch slot's base address to 0 so the VS indexes our SSBO from offset 0.
    uint32_t fetch_blob[192];
    std::memcpy(fetch_blob, &regs[0x4800], sizeof(fetch_blob));
    uint32_t vtx_base = 0, vtx_size = 0;
    if (!beta_current_vs_->vertex_bindings().empty()) {
      const auto& vb = beta_current_vs_->vertex_bindings()[0];
      xenos::xe_gpu_vertex_fetch_t f{};
      f.dword_0 = regs[0x4800 + vb.fetch_constant * 2];
      f.dword_1 = regs[0x4800 + vb.fetch_constant * 2 + 1];
      vtx_base = f.address << 2;
      vtx_size = f.size << 2;
      if (vtx_size > 16u * 0x100000u) vtx_size = 16u * 0x100000u;
      // Rebase: zero the address field (bits 2..31) of dword_0 in the UBO copy, keep type bits.
      fetch_blob[vb.fetch_constant * 2] &= 0x3u;
    }
    // System constants (SPIR-V layout): NDC transform + vertex index params + (C-4) the
    // PS-critical fields. color_exp_bias MUST be set or the PS's final `oC0 = color * exp_bias`
    // multiplies every pixel to 0 (the long-hunted all-black textured output); alpha test is set
    // ALWAYS-PASS for bring-up (a disabled alpha test must pass all 3 comparisons, else the PS
    // kills every fragment). flags otherwise 0 (no perspective divide) is the 2D-menu start.
    rg::SpirvShaderTranslator::SystemConstants spv_sys{};
    for (int i = 0; i < 3; ++i) {
      spv_sys.ndc_scale[i] = vpi.ndc_scale[i];
      spv_sys.ndc_offset[i] = vpi.ndc_offset[i];
    }
    spv_sys.vertex_base_index = register_file_->Get<reg::VGT_INDX_OFFSET>().indx_offset;
    spv_sys.vertex_index_endian = result.host_shader_index_endian;
    spv_sys.vertex_index_min = register_file_->Get<reg::VGT_MIN_VTX_INDX>().min_indx;
    spv_sys.vertex_index_max = register_file_->Get<reg::VGT_MAX_VTX_INDX>().max_indx;
    spv_sys.flags |= rg::SpirvShaderTranslator::kSysFlag_AlphaPassIfLess |
                     rg::SpirvShaderTranslator::kSysFlag_AlphaPassIfEqual |
                     rg::SpirvShaderTranslator::kSysFlag_AlphaPassIfGreater;
    // Vertex w-division control (PA_CL_VTE_CNTL) — REQUIRED for vte-enabled (projective) draws.
    // The VS epilogue computes w' = WNotReciprocal ? w : 1/w and gl_Position.w = w'; without
    // WNotReciprocal a real perspective draw gets w'=1/w and the host divide then MULTIPLIES xy
    // by w -> every vertex blasts off-screen -> 0 fragments (the C-4 draw#22 bug; same class as
    // the DXBC path's quarter-width symptom).
    {
      const auto vte_pkt = register_file_->Get<reg::PA_CL_VTE_CNTL>();
      if (vte_pkt.vtx_xy_fmt) spv_sys.flags |= rg::SpirvShaderTranslator::kSysFlag_XYDividedByW;
      if (vte_pkt.vtx_z_fmt) spv_sys.flags |= rg::SpirvShaderTranslator::kSysFlag_ZDividedByW;
      if (vte_pkt.vtx_w0_fmt) spv_sys.flags |= rg::SpirvShaderTranslator::kSysFlag_WNotReciprocal;
    }
    if (polygonal) spv_sys.flags |= rg::SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
    if (draw_util::IsPrimitiveLine(*register_file_)) {
      spv_sys.flags |= rg::SpirvShaderTranslator::kSysFlag_PrimitiveLine;
    }
    // D3D->Vulkan clip-space y-flip: the guest produces y-UP clip space (D3D convention, what the
    // beta D3D12 path renders); Vulkan NDC is y-DOWN and plume passes viewports verbatim (no
    // negative-height flip). Negate the y ndc transform so the plume image is upright.
    // NHL_HIGHCUT_NO_YFLIP disables for A/B.
    if (!std::getenv("NHL_HIGHCUT_NO_YFLIP")) {
      spv_sys.ndc_scale[1] = -spv_sys.ndc_scale[1];
      spv_sys.ndc_offset[1] = -spv_sys.ndc_offset[1];
    }
    {
      const int32_t guest_exp_bias = register_file_->Get<reg::RB_COLOR_INFO>().color_exp_bias;
      const float color_scale = std::exp2f(float(guest_exp_bias));
      for (int i = 0; i < 4; ++i) spv_sys.color_exp_bias[i] = color_scale;
    }
    // Shared-memory (vertex) bytes from guest RAM at the rebased range.
    const uint8_t* vtx_src =
        vtx_size ? memory_->TranslatePhysical<const uint8_t*>(vtx_base) : nullptr;

    // Bool/loop constants (8 bool + 32 loop dwords at 0x4900) — the b/loop UBO (set1 binding 3).
    const uint8_t* bool_src = reinterpret_cast<const uint8_t*>(&regs[0x4900]);
    constexpr uint32_t kBoolLoopBytes = 40 * 4;

    // Packed float constants (ascending storage index, matching GetPackedFloatConstantIndex). The
    // PS reads its bank from register 0x4400 (c256+), the VS from 0x4000 (c0+).
    auto pack_floats = [&](rg::Shader* sh, bool pixel) -> std::vector<uint8_t> {
      std::vector<uint8_t> out;
      if (!sh) return out;
      const uint32_t reg_base = pixel ? 0x4400u : 0x4000u;
      const auto& crm = sh->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t bits = crm.float_bitmap[i];
        while (bits) {
          const uint32_t s = i * 64u + uint32_t(std::countr_zero(bits));
          bits &= bits - 1;
          const uint8_t* src = reinterpret_cast<const uint8_t*>(&regs[reg_base + s * 4]);
          out.insert(out.end(), src, src + 16);
        }
      }
      return out;
    };
    const std::vector<uint8_t> vs_floats = pack_floats(beta_current_vs_, false);
    const std::vector<uint8_t> ps_floats = pack_floats(eff_ps, true);

    // C-4: untile each PS texture binding into a LINEAR blob + a TexturePacketDesc. Xenos textures
    // are tiled in 32x32-block tiles; GetTiledOffset2D (the texture cache's own addresser) gives
    // the per-block byte offset. Bring-up handles 2D 8888 + DXT1/2_3/4_5; anything else gets a 2x2
    // magenta placeholder so the bind/sample path still runs (and is visibly wrong if reached).
    std::vector<nhl::highcut::TexturePacketDesc> tex_descs;
    std::vector<std::vector<uint8_t>> tex_blobs;
    for (const auto& tb : p3_ps_texbinds) {
      const uint32_t slot = tb.fetch_constant;
      xenos::xe_gpu_texture_fetch_t tf{};
      std::memcpy(&tf, &regs[0x4800 + slot * 6], 6 * 4);
      nhl::highcut::TexturePacketDesc td{};
      td.fetch_slot = slot;
      td.is_signed = tb.is_signed ? 1u : 0u;
      const uint32_t width = uint32_t(tf.size_2d.width) + 1;
      const uint32_t height = uint32_t(tf.size_2d.height) + 1;
      const xenos::TextureFormat fmt = tf.format;
      const rg::FormatInfo* fi = rg::FormatInfo::Get(fmt);
      uint32_t pfmt = UINT32_MAX;
      bool expand_r8 = false;  // k_8: 1-byte coverage -> RGBA8 (v,v,v,v) so any channel reads the glyph
      switch (fmt) {
        case xenos::TextureFormat::k_8_8_8_8: pfmt = nhl::highcut::kTexRGBA8; break;
        // k_8: an 8-bit single-channel texture — the MENU FONT atlas (glyph coverage). Untile the
        // 1-byte texels then replicate to RGBA8 so whichever channel the text PS samples (the swizzle
        // is typically "rrrr"/"...r" for fonts) gets the coverage. Was unsupported -> magenta blocks.
        case xenos::TextureFormat::k_8: pfmt = nhl::highcut::kTexRGBA8; expand_r8 = true; break;
        case xenos::TextureFormat::k_DXT1:    pfmt = nhl::highcut::kTexBC1; break;
        case xenos::TextureFormat::k_DXT2_3:  pfmt = nhl::highcut::kTexBC2; break;
        case xenos::TextureFormat::k_DXT4_5:  pfmt = nhl::highcut::kTexBC3; break;
        default: break;
      }
      const uint32_t tex_base = uint32_t(tf.base_address) << 12;
      const bool ok2d = xenos::DataDimension(tf.dimension) == xenos::DataDimension::k2DOrStacked;
      const uint8_t* guest =
          (tex_base && memory_) ? memory_->TranslatePhysical<const uint8_t*>(tex_base) : nullptr;
      if (pfmt == UINT32_MAX || !fi || !ok2d || !guest || !width || !height || width > 8192 ||
          height > 8192) {
        td.width = 2; td.height = 2; td.tex_format = nhl::highcut::kTexRGBA8;
        td.row_pitch_bytes = 2 * 4; td.data_bytes = 2 * 2 * 4;
        std::vector<uint8_t> blob(td.data_bytes);
        for (size_t i = 0; i < blob.size(); i += 4) {
          blob[i] = 255; blob[i + 1] = 0; blob[i + 2] = 255; blob[i + 3] = 255;  // magenta
        }
        REXLOG_INFO("[highcut-C4] tex slot={} UNSUPPORTED fmt={} dim={} base=0x{:X} -> 2x2 magenta",
                    slot, uint32_t(fmt), uint32_t(tf.dimension), tex_base);
        tex_descs.push_back(td); tex_blobs.push_back(std::move(blob));
        continue;
      }
      const uint32_t bw = fi->block_width, bh = fi->block_height, bpb = fi->bytes_per_block();
      uint32_t bpb_log2 = 0;
      for (uint32_t v = bpb; v > 1; v >>= 1) ++bpb_log2;
      const uint32_t blocks_x = (width + bw - 1) / bw;
      const uint32_t blocks_y = (height + bh - 1) / bh;
      uint32_t pitch_texels = tf.pitch ? (uint32_t(tf.pitch) << 5) : ((width + 31u) & ~31u);
      uint32_t pitch_blocks = pitch_texels / bw;
      if (pitch_blocks < blocks_x) pitch_blocks = blocks_x;
      std::vector<uint8_t> blob(size_t(blocks_y) * blocks_x * bpb);
      for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
          size_t src;
          if (tf.tiled) {
            src = size_t(uint32_t(rg::texture_util::GetTiledOffset2D(
                int32_t(bx), int32_t(by), pitch_blocks, bpb_log2)));
          } else {
            src = (size_t(by) * pitch_blocks + bx) * bpb;
          }
          std::memcpy(&blob[(size_t(by) * blocks_x + bx) * bpb], guest + src, bpb);
        }
      }
      // Endian: 8888 -> per-texel 32-bit swap; BCn -> 16-bit swap (DXT endpoint/index words).
      const xenos::Endian end = xenos::Endian(tf.endianness);
      if (end != xenos::Endian::kNone) {
        if (pfmt == nhl::highcut::kTexRGBA8) {
          for (size_t i = 0; i + 4 <= blob.size(); i += 4) {
            uint32_t v; std::memcpy(&v, &blob[i], 4);
            v = xenos::GpuSwap(v, end);
            std::memcpy(&blob[i], &v, 4);
          }
        } else {
          for (size_t i = 0; i + 2 <= blob.size(); i += 2) {
            uint16_t v; std::memcpy(&v, &blob[i], 2);
            v = xenos::GpuSwap(v, end);
            std::memcpy(&blob[i], &v, 2);
          }
        }
      }
      td.width = width; td.height = height; td.tex_format = pfmt;
      td.row_pitch_bytes = blocks_x * bpb; td.data_bytes = uint32_t(blob.size());
      // k_8: replicate the 1-byte coverage to RGBA8 (v,v,v,v) — the font then samples correctly on
      // any channel without a single-channel host format / swizzle dependency.
      if (expand_r8) {
        std::vector<uint8_t> rgba(blob.size() * 4);
        for (size_t i = 0; i < blob.size(); ++i) {
          const uint8_t v = blob[i];
          rgba[i * 4 + 0] = v; rgba[i * 4 + 1] = v; rgba[i * 4 + 2] = v; rgba[i * 4 + 3] = v;
        }
        blob = std::move(rgba);
        td.row_pitch_bytes = blocks_x * 4u;
        td.data_bytes = uint32_t(blob.size());
      }
      REXLOG_INFO("[highcut-C4] tex slot={} {}x{} fmt={} pfmt={} tiled={} endian={} pitch_blk={} "
                  "blob={} is_signed={}",
                  slot, width, height, uint32_t(fmt), pfmt, uint32_t(tf.tiled), uint32_t(end),
                  pitch_blocks, td.data_bytes, td.is_signed);
      tex_descs.push_back(td); tex_blobs.push_back(std::move(blob));
    }

    REXLOG_INFO("[highcut-C3b3] draw types: guest_prim={} host_prim={} hvst={} host_vtx_count={} "
                "index_count(param)={} idx_type={}",
                uint32_t(result.guest_primitive_type), uint32_t(result.host_primitive_type),
                uint32_t(result.host_vertex_shader_type), result.host_draw_vertex_count,
                index_count, uint32_t(result.index_buffer_type));
    nhl::highcut::DrawPacketHeader hdr{};
    hdr.magic = nhl::highcut::kDrawPacketMagic;
    hdr.version = nhl::highcut::kDrawPacketVersion;
    // C-3b.3: pick the plume topology + host vertex count to match the translated VS. For a rect
    // list translated as kRectangleListAsTriangleStrip, each guest rect (3 verts) becomes a
    // 4-vertex triangle strip; otherwise draw the guest verts as a triangle list.
    if (result.guest_primitive_type == xenos::PrimitiveType::kRectangleList) {
      hdr.topology = nhl::highcut::kTopoTriangleStrip;
      hdr.vertex_count = (index_count / 3) * 4;  // 4 strip verts per guest rect (single rect -> 4)
    } else if (result.guest_primitive_type == xenos::PrimitiveType::kQuadList) {
      // C-5a.1: menu text/glyphs. No quad host-shader type in the translator, so the plume side
      // index-expands (4-vert quads -> 2 tris) and draws TRIANGLE_LIST; carry the guest vert count.
      hdr.topology = nhl::highcut::kTopoTriangleListQuadExpand;
      hdr.vertex_count = index_count;  // guest quad-vert count (4 * #quads)
    } else {
      hdr.topology = nhl::highcut::kTopoTriangleList;
      hdr.vertex_count = index_count;
    }
    hdr.fetch_bytes = sizeof(fetch_blob);
    hdr.sys_bytes = sizeof(spv_sys);
    hdr.shared_bytes = vtx_src ? vtx_size : 0u;
    hdr.bool_bytes = kBoolLoopBytes;
    hdr.vs_float_bytes = uint32_t(vs_floats.size());
    hdr.ps_float_bytes = uint32_t(ps_floats.size());
    hdr.vs_spirv_bytes = uint32_t(p3_vs_spirv.size());  // C-5a: inline masked VS (per-draw)
    hdr.ps_spirv_bytes = uint32_t(p3_ps_spirv.size());
    hdr.texture_count = uint32_t(tex_descs.size());
    hdr.ps_sampler_count = p3_ps_sampler_count;
    // C-5a: per-draw viewport (final vpi) so the plume replay places each draw correctly.
    hdr.vp_x = float(vpi.xy_offset[0]);
    hdr.vp_y = float(vpi.xy_offset[1]);
    hdr.vp_w = float(vpi.xy_extent[0]);
    hdr.vp_h = float(vpi.xy_extent[1]);
    hdr.vp_zmin = vpi.z_min;
    hdr.vp_zmax = vpi.z_max;
    // C-5a: per-draw blend from RB_BLENDCONTROL0 (factors/ops = xenos enum values == PacketBlend*).
    // Xenos has no explicit per-RT blend-enable; (One,Zero,Add) IS identity, so always enable and
    // let the factors decide. NHL_BETA_NOBLEND forces copy for A/B.
    const uint32_t bc0 = beta_noblend ? 0x00010001u
                                      : (*register_file_)[beta_reg::kBlendControl[0]];
    hdr.blend_enable = 1;
    hdr.blend_src = (bc0 >> 0) & 0x1F;
    hdr.blend_op = (bc0 >> 5) & 0x7;
    hdr.blend_dst = (bc0 >> 8) & 0x1F;
    hdr.blend_src_a = (bc0 >> 16) & 0x1F;
    hdr.blend_op_a = (bc0 >> 21) & 0x7;
    hdr.blend_dst_a = (bc0 >> 24) & 0x1F;
    hdr.color_write_mask = 0xF;  // C-5a: write all channels (per-RT mask refinement deferred)
    char pkt_path[64];
    if (frame_capture) {
      std::snprintf(pkt_path, sizeof(pkt_path), "highcut_frame_%u.bin", highcut_capture_idx_);
    } else {
      std::snprintf(pkt_path, sizeof(pkt_path), "highcut_p3_draw.bin");
    }
    if (std::FILE* pf = std::fopen(pkt_path, "wb")) {
      std::fwrite(&hdr, 1, sizeof(hdr), pf);
      std::fwrite(fetch_blob, 1, sizeof(fetch_blob), pf);
      std::fwrite(&spv_sys, 1, sizeof(spv_sys), pf);
      if (hdr.shared_bytes) std::fwrite(vtx_src, 1, hdr.shared_bytes, pf);
      std::fwrite(bool_src, 1, kBoolLoopBytes, pf);
      if (hdr.vs_float_bytes) std::fwrite(vs_floats.data(), 1, hdr.vs_float_bytes, pf);
      if (hdr.ps_float_bytes) std::fwrite(ps_floats.data(), 1, hdr.ps_float_bytes, pf);
      if (hdr.vs_spirv_bytes) std::fwrite(p3_vs_spirv.data(), 1, hdr.vs_spirv_bytes, pf);
      if (hdr.ps_spirv_bytes) std::fwrite(p3_ps_spirv.data(), 1, hdr.ps_spirv_bytes, pf);
      for (size_t i = 0; i < tex_descs.size(); ++i) {
        std::fwrite(&tex_descs[i], 1, sizeof(tex_descs[i]), pf);
        std::fwrite(tex_blobs[i].data(), 1, tex_blobs[i].size(), pf);
      }
      std::fclose(pf);
      REXLOG_INFO("[highcut-{}] dumped {}: verts={} topo={} vp=({},{},{},{}) vs_spirv={} ps_spirv={} "
                  "textures={} blend=src{}/dst{}/op{} flags=0x{:X} ndc_s=({},{}) ndc_o=({},{})",
                  frame_capture ? "C5" : "C4", pkt_path, hdr.vertex_count, hdr.topology, hdr.vp_x,
                  hdr.vp_y, hdr.vp_w, hdr.vp_h, hdr.vs_spirv_bytes, hdr.ps_spirv_bytes,
                  hdr.texture_count, hdr.blend_src, hdr.blend_dst, hdr.blend_op, spv_sys.flags,
                  spv_sys.ndc_scale[0], spv_sys.ndc_scale[1], spv_sys.ndc_offset[0],
                  spv_sys.ndc_offset[1]);
      if (frame_capture) ++highcut_capture_idx_;
    }
  }

  D3D12_VIEWPORT vp{};
  vp.MinDepth = vpi.z_min;
  vp.MaxDepth = vpi.z_max;
  D3D12_RECT scissor{};
  if (edram) {
    // EDRAM mode: the RT cache's host render targets are at the guest's native resolution
    // (resolution scale 1), so use THIS draw's host viewport directly in native pixels —
    // no offscreen-RT remap. 3D arena draws (vte transform enabled) get their projection
    // from the VPORT registers via ndc_scale/offset; 2D composite draws map 1:1. This is
    // the standard base-path viewport and replaces the offscreen-RTV sub-viewport hack.
    vp.TopLeftX = float(vpi.xy_offset[0]);
    vp.TopLeftY = float(vpi.xy_offset[1]);
    vp.Width = float(vpi.xy_extent[0]);
    vp.Height = float(vpi.xy_extent[1]);
    // Scissor clamp-to-surface-pitch (Xenia GetScissor clamp_to_surface_pitch=true): the
    // viewport may be wider than the surface for 3D draws, but only the in-pitch columns may
    // rasterize into the pitch-wide host RT. The viewport keeps full width so NDC mapping is
    // correct; the scissor restricts coverage to the surface.
    LONG sx1 = LONG(vpi.xy_offset[0] + vpi.xy_extent[0]);
    if (vport_xform) {
      uint32_t spitch = register_file_->Get<reg::RB_SURFACE_INFO>().surface_pitch;
      if (spitch) sx1 = std::min<LONG>(sx1, LONG(spitch));
    }
    scissor = {LONG(vpi.xy_offset[0]), LONG(vpi.xy_offset[1]), sx1,
               LONG(vpi.xy_offset[1] + vpi.xy_extent[1])};

    // PROTOTYPE (NHL_BETA_WINOFF): fold-aware predicated-tiling placement. The
    // create-player 3D model is drawn in two PA_SC_WINDOW_OFFSET passes into ONE
    // 640-pitch surface: pass1 win=(0,0) (left half, handled above), pass2 win=(-640,0)
    // (right half). GetHostViewportInfo folds the -640 origin into a 2x squish that
    // overwrites pass1. Instead, un-squish and translate the pass into the fold region
    // BELOW pass1 in the (pitch-wide, tall) host RT: a window X shift of -N*pitch maps
    // to host (x - N*pitch, y + N*image_height). The SDK resolve then un-folds the
    // stacked halves to the 1280-wide texture. Hypothesis test: if the resolve layout
    // is per-16px-tile-row interleaved instead, the result will be striped, not whole.
    if (std::getenv("NHL_BETA_WINOFF") && vport_xform) {
      const auto woff = register_file_->Get<reg::PA_SC_WINDOW_OFFSET>();
      const int32_t win_x = woff.window_x_offset, win_y = woff.window_y_offset;
      const uint32_t spitch = register_file_->Get<reg::RB_SURFACE_INFO>().surface_pitch;
      if (win_x < 0 && spitch) {
        const uint32_t wrap = uint32_t(-win_x) / spitch;
        const uint32_t full_w = spitch * (wrap + 1);
        const float img_h =
            vpi.xy_extent[1] > 0 ? float(vpi.xy_extent[1]) : float(beta_rt_height_);
        const float fold_y = float(win_y) + float(wrap) * img_h;
        // Un-squish: render the full-width image with the win=0 pass's NDC mapping.
        vpi.ndc_scale[0] = 1.f; vpi.ndc_scale[1] = 1.f; vpi.ndc_scale[2] = 1.f;
        vpi.ndc_offset[0] = 0.f; vpi.ndc_offset[1] = 0.f; vpi.ndc_offset[2] = 0.f;
        vp.TopLeftX = float(win_x);
        vp.TopLeftY = fold_y;
        vp.Width = float(full_w);
        vp.Height = img_h;
        scissor = {0, LONG(fold_y), LONG(spitch), LONG(fold_y + img_h)};
        if (std::getenv("NHL_BETA_DEPTH_DIAG")) {
          REXLOG_INFO("[nhl-beta] WINOFF #{}: win=({},{}) pitch={} wrap={} -> vp(x={} y={} w={} "
                      "h={}) scissor=({},{},{},{})",
                      beta_takeover_rendered_, win_x, win_y, spitch, wrap, vp.TopLeftX, vp.TopLeftY,
                      vp.Width, vp.Height, scissor.left, scissor.top, scissor.right, scissor.bottom);
        }
      }
    }
  } else if (flat_mode && vport_xform) {
    // Route (a) FLAT 3D: render this 3D draw with its NATIVE guest viewport (vpi from the
    // un-clamped x_max) directly into the flat scratch RT. vpi.xy_extent is the guest's
    // true viewport extent (1280 for the create-player model), so geometry lands 1:1 at
    // full width — no surface_pitch, no fold, no wrap. 2D draws are untouched.
    vp.TopLeftX = float(vpi.xy_offset[0]);
    vp.TopLeftY = float(vpi.xy_offset[1]);
    vp.Width = float(vpi.xy_extent[0]);
    vp.Height = float(vpi.xy_extent[1]);
    auto clampL = [](LONG v, LONG hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    scissor = {clampL(LONG(vpi.xy_offset[0]), LONG(beta_rt_width_)),
               clampL(LONG(vpi.xy_offset[1]), LONG(beta_rt_height_)),
               clampL(LONG(vpi.xy_offset[0] + vpi.xy_extent[0]), LONG(beta_rt_width_)),
               clampL(LONG(vpi.xy_offset[1] + vpi.xy_extent[1]), LONG(beta_rt_height_))};
  } else if (guest_w == beta_rt_width_ || std::getenv("NHL_BETA_VP_FULLRT")) {
    // 1:1 guest-surface==RT (the validated 2D menu/intro path) — keep the full-RT
    // viewport exactly as before so those frames stay byte-identical.
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = float(beta_rt_width_);
    vp.Height = float(beta_rt_height_);
    scissor = {0, 0, LONG(beta_rt_width_), LONG(beta_rt_height_)};
  } else {
    // Resolution mismatch (3D gameplay renders at e.g. 640x360 while our RT is 1280x720).
    // Map THIS draw's computed host sub-viewport (vpi.xy_extent/xy_offset, in guest-surface
    // units) onto the RT, scaled by RT/guest. Full-surface draws still fill the RT; sub-
    // viewport draws (the arena passes vs the overlay planes used DIFFERENT guest viewports
    // over the same 640x360 surface) now land at a consistent place/size instead of all
    // stretching to the full RT — the scene_04 mis-projection (arena crushed, planes adrift).
    const float rtx = float(beta_rt_width_) / float(guest_w);
    const float rty = float(beta_rt_height_) / float(guest_h ? guest_h : beta_rt_height_);
    vp.TopLeftX = vpi.xy_offset[0] * rtx;
    vp.TopLeftY = vpi.xy_offset[1] * rty;
    vp.Width = vpi.xy_extent[0] * rtx;
    vp.Height = vpi.xy_extent[1] * rty;
    auto clampi = [](float v, LONG lo, LONG hi) -> LONG {
      LONG i = LONG(v + 0.5f);
      return i < lo ? lo : (i > hi ? hi : i);
    };
    scissor = {clampi(vp.TopLeftX, 0, LONG(beta_rt_width_)),
               clampi(vp.TopLeftY, 0, LONG(beta_rt_height_)),
               clampi(vp.TopLeftX + vp.Width, 0, LONG(beta_rt_width_)),
               clampi(vp.TopLeftY + vp.Height, 0, LONG(beta_rt_height_))};
  }
  if (first_draw || std::getenv("NHL_BETA_DEPTH_DIAG")) {
    auto reg_f = [&](uint32_t idx) -> float {
      const uint32_t u = (*register_file_)[idx];
      float f;
      std::memcpy(&f, &u, sizeof(f));
      return f;
    };
    REXLOG_INFO("[nhl-beta] vpi #{}: guest_surface=({},{}) extent=({},{}) ndc_scale=({},{},{}) "
                "ndc_offset=({},{},{}) z=[{},{}] vte=0x{:X} VPORT(xs={} xo={} ys={} yo={}) "
                "vp_off=({},{})",
                beta_takeover_rendered_, guest_w, guest_h, vpi.xy_extent[0], vpi.xy_extent[1],
                vpi.ndc_scale[0], vpi.ndc_scale[1], vpi.ndc_scale[2], vpi.ndc_offset[0],
                vpi.ndc_offset[1], vpi.ndc_offset[2], vpi.z_min, vpi.z_max,
                register_file_->Get<reg::PA_CL_VTE_CNTL>().value, reg_f(0x210F), reg_f(0x2110),
                reg_f(0x2111), reg_f(0x2112), vpi.xy_offset[0], vpi.xy_offset[1]);
  }
  // EDRAM multi-pass correlation: per owned draw, log the EDRAM surface it renders into
  // (RB_COLOR_INFO.color_base in tiles + RB_DEPTH_INFO) and whether depth-test is on. Cross-
  // reference with the resolve sources to find which surface holds the 3D model and whether
  // that surface is ever resolved to a guest texture the composite samples.
  if (std::getenv("NHL_BETA_EDRAM_DIAG")) {
    const auto ci = register_file_->Get<reg::RB_COLOR_INFO>();
    const auto di = register_file_->Get<reg::RB_DEPTH_INFO>();
    const uint32_t color_base = ci.color_base | (uint32_t(ci.color_base_bit_11) << 11);
    const auto woff = register_file_->Get<reg::PA_SC_WINDOW_OFFSET>();
    REXLOG_INFO("[nhl-beta] edram-draw #{}: vte=0x{:X} z_en={} zfunc={} color_edram_tile={} "
                "color_fmt={} depth_edram_tile={} surf_pitch={} extent=({},{}) "
                "bound_bits=0x{:X} rtfmt_color={} rtfmt_depth={} msaa={} win_off=({},{})",
                beta_takeover_rendered_, register_file_->Get<reg::PA_CL_VTE_CNTL>().value,
                uint32_t(ndc.z_enable), uint32_t(ndc.zfunc), color_base,
                uint32_t(ci.color_format), uint32_t(di.depth_base),
                register_file_->Get<reg::RB_SURFACE_INFO>().surface_pitch, vpi.xy_extent[0],
                vpi.xy_extent[1], bound_rt_bits, rt_formats[1], rt_formats[0],
                uint32_t(register_file_->Get<reg::RB_SURFACE_INFO>().msaa_samples),
                int32_t(woff.window_x_offset), int32_t(woff.window_y_offset));
  }

  // System constants: a partial reproduction of D3D12CommandProcessor::
  // UpdateSystemConstantValues, limited to the GEOMETRY + ALPHA-critical fields
  // for the host-RT path (the EDRAM/ROV/stencil/blend fields are unused when the
  // pixel shader writes SV_Target to a bound RT). Zeroing these is what made the
  // first draw render misplaced + transparent-black: the shader reads the flags
  // to decide whether to apply the perspective divide, and reads the alpha-pass
  // flags to decide whether to keep the pixel (all-zero => kNever => kill).
  using Xlat = rex::graphics::DxbcShaderTranslator;
  rex::graphics::DxbcShaderTranslator::SystemConstants sys{};
  sys.tessellation_factor_range_min =
      std::bit_cast<float>((*register_file_)[0x2287]) + 1.0f;
  sys.tessellation_factor_range_max =
      std::bit_cast<float>((*register_file_)[0x2286]) + 1.0f;
  sys.line_loop_closing_index = result.line_loop_closing_index;
  uint32_t sys_flags = 0;
  // Vertex w-division control (PA_CL_VTE_CNTL): tells the VS whether guest
  // positions are already divided by W and whether W is reciprocal. Without these
  // the clip-space transform is wrong (our ¼-width symptom).
  const auto vte = register_file_->Get<reg::PA_CL_VTE_CNTL>();
  if (vte.vtx_xy_fmt) sys_flags |= Xlat::kSysFlag_XYDividedByW;
  if (vte.vtx_z_fmt) sys_flags |= Xlat::kSysFlag_ZDividedByW;
  if (vte.vtx_w0_fmt) sys_flags |= Xlat::kSysFlag_WNotReciprocal;
  if (polygonal) sys_flags |= Xlat::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(*register_file_)) {
    sys_flags |= Xlat::kSysFlag_PrimitiveLine;
  }
  if (register_file_->Get<reg::RB_DEPTH_INFO>().depth_format ==
      xenos::DepthRenderTargetFormat::kD24FS8) {
    sys_flags |= Xlat::kSysFlag_DepthFloat24;
  }
  // Alpha test (RB_COLORCONTROL). When disabled, ALL three pass flags must be set
  // (always-pass); otherwise the shader kills every pixel.
  const auto cc = register_file_->Get<reg::RB_COLORCONTROL>();
  if (cc.alpha_test_enable && !std::getenv("NHL_BETA_NOALPHA")) {
    const uint32_t af = uint32_t(cc.alpha_func);
    if (af & 0b001) sys_flags |= Xlat::kSysFlag_AlphaPassIfLess;
    if (af & 0b010) sys_flags |= Xlat::kSysFlag_AlphaPassIfEqual;
    if (af & 0b100) sys_flags |= Xlat::kSysFlag_AlphaPassIfGreater;
    sys.alpha_test_reference =
        std::bit_cast<float>((*register_file_)[0x210E]);  // RB_ALPHA_REF (kFloat)
  } else {
    sys_flags |= Xlat::kSysFlag_AlphaPassIfLess | Xlat::kSysFlag_AlphaPassIfEqual |
                 Xlat::kSysFlag_AlphaPassIfGreater;
  }
  sys.flags = sys_flags;
  for (int i = 0; i < 3; ++i) {
    sys.ndc_scale[i] = vpi.ndc_scale[i];
    sys.ndc_offset[i] = vpi.ndc_offset[i];
  }
  const auto clip_cntl = register_file_->Get<reg::PA_CL_CLIP_CNTL>();
  if (!clip_cntl.clip_disable) {
    float* clip_write = sys.user_clip_planes[0];
    for (uint32_t clip_index = 0; clip_index < 6; ++clip_index) {
      if (!(clip_cntl.ucp_ena & (uint32_t(1) << clip_index))) continue;
      std::memcpy(clip_write, &(*register_file_)[0x2388 + clip_index * 4],
                  4 * sizeof(float));
      clip_write += 4;
    }
  }
  if (primitive_type == xenos::PrimitiveType::kPointList) {
    const auto point_minmax = register_file_->Get<reg::PA_SU_POINT_MINMAX>();
    const auto point_size = register_file_->Get<reg::PA_SU_POINT_SIZE>();
    sys.point_vertex_diameter_min = float(point_minmax.min_size) * (2.0f / 16.0f);
    sys.point_vertex_diameter_max = float(point_minmax.max_size) * (2.0f / 16.0f);
    sys.point_constant_diameter[0] = float(point_size.width) * (2.0f / 16.0f);
    sys.point_constant_diameter[1] = float(point_size.height) * (2.0f / 16.0f);
    sys.point_screen_diameter_to_ndc_radius[0] =
        1.0f / float(std::max(vpi.xy_extent[0], uint32_t(1)));
    sys.point_screen_diameter_to_ndc_radius[1] =
        1.0f / float(std::max(vpi.xy_extent[1], uint32_t(1)));
  }
  uint32_t rov_used_texture_mask = 0;
  if (edram_rov) {
    rov_used_texture_mask =
        beta_current_vs_->GetUsedTextureMaskAfterTranslation() |
        (eff_ps ? eff_ps->GetUsedTextureMaskAfterTranslation() : 0u);
  }
  // Vertex index bounds/offset (auto-indexed rect: endian none, offset 0).
  sys.vertex_index_endian = result.host_shader_index_endian;
  sys.vertex_index_offset = register_file_->Get<reg::VGT_INDX_OFFSET>().indx_offset;
  sys.vertex_index_min = register_file_->Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  sys.vertex_index_max = register_file_->Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  // color_exp_bias holds the ALREADY-exp2'd per-RT output scale, NOT the raw exponent.
  // The pixel shader's FINAL instruction is `oC0 = color * xe_color_exp_bias[0].x`, so
  // leaving this 0 multiplies EVERY pixel to (0,0,0,0) — the long-hunted all-zero
  // textured output (black with blend off, alpha-0-invisible with blend on). It must be
  // exp2(guest RB_COLOR_INFO.color_exp_bias); 0 guest bias => 1.0 (no scaling).
  const int32_t guest_exp_bias = register_file_->Get<reg::RB_COLOR_INFO>().color_exp_bias;
  const float color_scale = std::exp2f(float(guest_exp_bias));
  for (int i = 0; i < 4; ++i) sys.color_exp_bias[i] = color_scale;
  if (edram_rov) {
    const auto surface_info = register_file_->Get<reg::RB_SURFACE_INFO>();
    sys.sample_count_log2[0] =
        surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1u : 0u;
    sys.sample_count_log2[1] =
        surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1u : 0u;
    sys.alpha_to_mask =
        cc.alpha_to_mask_enable ? ((cc.value >> 24) | (uint32_t(1) << 8)) : 0;

    const uint32_t edram_tile_dwords =
        xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples;
    const uint32_t pitch_samples =
        surface_info.surface_pitch *
        (surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2u : 1u);
    sys.edram_32bpp_tile_pitch_dwords_scaled =
        ((pitch_samples + xenos::kEdramTileWidthSamples - 1) /
         xenos::kEdramTileWidthSamples) *
        edram_tile_dwords;

    const auto depth_info = register_file_->Get<reg::RB_DEPTH_INFO>();
    const uint32_t depth_base =
        uint32_t(depth_info.depth_base) | (uint32_t(depth_info.depth_base_bit_11) << 11);
    sys.edram_depth_base_dwords_scaled =
        depth_base * edram_tile_dwords;

    bool depth_stencil_enabled = ndc.z_enable || ndc.stencil_enable;
    reg::RB_COLOR_INFO color_infos[4];
    float rt_clamp[4][4] = {};
    uint32_t rt_keep_masks[4][2] = {};
    for (uint32_t i = 0; i < 4; ++i) {
      color_infos[i].value = (*register_file_)[beta_reg::kColorInfo[i]];
      rex::graphics::RenderTargetCache::GetPSIColorFormatInfo(
          color_infos[i].color_format, (ncm >> (i * 4)) & 0xFu, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0],
          rt_keep_masks[i][1]);
      const uint32_t color_base =
          uint32_t(color_infos[i].color_base) |
          (uint32_t(color_infos[i].color_base_bit_11) << 11);
      if (depth_base == color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
      }
    }

    if (depth_stencil_enabled) {
      sys.flags |= Xlat::kSysFlag_ROVDepthStencil;
      if (ndc.z_enable) {
        sys.flags |= uint32_t(ndc.zfunc) << Xlat::kSysFlag_ROVDepthPassIfLess_Shift;
        if (ndc.z_write_enable) sys.flags |= Xlat::kSysFlag_ROVDepthWrite;
      } else {
        sys.flags |= Xlat::kSysFlag_ROVDepthPassIfLess |
                     Xlat::kSysFlag_ROVDepthPassIfEqual |
                     Xlat::kSysFlag_ROVDepthPassIfGreater;
      }
      if (ndc.stencil_enable) sys.flags |= Xlat::kSysFlag_ROVStencilTest;
      if ((!cc.alpha_test_enable ||
           cc.alpha_func == xenos::CompareFunction::kAlways) &&
          !cc.alpha_to_mask_enable) {
        sys.flags |= Xlat::kSysFlag_ROVDepthStencilEarlyWrite;
      }
    }

    for (uint32_t i = 0; i < 4; ++i) {
      const auto& color_info = color_infos[i];
      sys.color_exp_bias[i] = std::exp2f(float(int32_t(color_info.color_exp_bias)));
      sys.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      sys.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] == UINT32_MAX && rt_keep_masks[i][1] == UINT32_MAX) continue;
      const uint32_t color_base =
          uint32_t(color_info.color_base) |
          (uint32_t(color_info.color_base_bit_11) << 11);
      sys.edram_rt_base_dwords_scaled[i] =
          color_base * edram_tile_dwords;
      sys.edram_rt_format_flags[i] =
          rex::graphics::RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
      std::memcpy(sys.edram_rt_clamp[i], rt_clamp[i], sizeof(rt_clamp[i]));
      sys.edram_rt_blend_factors_ops[i] =
          beta_noblend
              ? 0x00010001u
              : ((*register_file_)[beta_reg::kBlendControl[i]] & 0x1FFF1FFFu);
    }
    for (uint32_t i = 0; i < 4; ++i) {
      sys.edram_blend_constant[i] =
          std::bit_cast<float>((*register_file_)[0x2105 + i]);
    }

    // Green-band probe: the left fold-band reads a draw-independent (0,127,15) = depth ~0.06 value.
    // Log the EDRAM color vs depth tile bases + pitch to check whether the left band's color address
    // aliases the depth EDRAM region (depth-as-color) for the 1280-wide fold composite passes.
    if (std::getenv("NHL_BETA_EDRAM_DIAG") &&
        (beta_takeover_rendered_ < 4 ||
         (beta_takeover_rendered_ >= 270 && beta_takeover_rendered_ <= 360) ||
         beta_takeover_rendered_ >= 540)) {
      const uint32_t color0_base =
          uint32_t(color_infos[0].color_base) | (uint32_t(color_infos[0].color_base_bit_11) << 11);
      REXLOG_INFO("[nhl-beta] ROV bases #{}: color0_base_tile={} depth_base_tile={} "
                  "tile_pitch_dw={} surf_pitch={} msaa={} depth_en={} color0_fmt={}",
                  beta_takeover_rendered_, color0_base, depth_base,
                  sys.edram_32bpp_tile_pitch_dwords_scaled, surface_info.surface_pitch,
                  uint32_t(surface_info.msaa_samples), depth_stencil_enabled,
                  uint32_t(color_infos[0].color_format));
    }

    const auto mode_cntl = register_file_->Get<reg::PA_SU_SC_MODE_CNTL>();
    float poly_front_scale = 0.0f;
    float poly_front_offset = 0.0f;
    float poly_back_scale = 0.0f;
    float poly_back_offset = 0.0f;
    if (polygonal) {
      if (mode_cntl.poly_offset_front_enable) {
        poly_front_scale = std::bit_cast<float>((*register_file_)[0x2380]);
        poly_front_offset = std::bit_cast<float>((*register_file_)[0x2381]);
      }
      if (mode_cntl.poly_offset_back_enable) {
        poly_back_scale = std::bit_cast<float>((*register_file_)[0x2382]);
        poly_back_offset = std::bit_cast<float>((*register_file_)[0x2383]);
      }
    } else if (mode_cntl.poly_offset_para_enable) {
      poly_front_scale = std::bit_cast<float>((*register_file_)[0x2380]);
      poly_front_offset = std::bit_cast<float>((*register_file_)[0x2381]);
      poly_back_scale = poly_front_scale;
      poly_back_offset = poly_front_offset;
    }
    poly_front_scale *= xenos::kPolygonOffsetScaleSubpixelUnit;
    poly_back_scale *= xenos::kPolygonOffsetScaleSubpixelUnit;
    sys.edram_poly_offset_front_scale = poly_front_scale;
    sys.edram_poly_offset_front_offset = poly_front_offset;
    sys.edram_poly_offset_back_scale = poly_back_scale;
    sys.edram_poly_offset_back_offset = poly_back_offset;

    if (depth_stencil_enabled && ndc.stencil_enable) {
      reg::RB_STENCILREFMASK stencil_front;
      reg::RB_STENCILREFMASK stencil_back;
      stencil_front.value = (*register_file_)[0x210D];
      stencil_back.value = (*register_file_)[0x210C];
      sys.edram_stencil_front_reference = stencil_front.stencilref;
      sys.edram_stencil_front_read_mask = stencil_front.stencilmask;
      sys.edram_stencil_front_write_mask = stencil_front.stencilwritemask;
      sys.edram_stencil_front_func_ops = (ndc.value >> 8) & 0xFFFu;
      if (polygonal && ndc.backface_enable) {
        sys.edram_stencil_back_reference = stencil_back.stencilref;
        sys.edram_stencil_back_read_mask = stencil_back.stencilmask;
        sys.edram_stencil_back_write_mask = stencil_back.stencilwritemask;
        sys.edram_stencil_back_func_ops = (ndc.value >> 20) & 0xFFFu;
      } else {
        std::memcpy(sys.edram_stencil_back, sys.edram_stencil_front,
                    sizeof(sys.edram_stencil_front));
      }
    }
  }

  // Upload constant buffers from the register file via the public constant pool.
  const uint64_t sub = GetCurrentSubmission();
  auto upload = [&](const void* data, size_t bytes) -> D3D12_GPU_VIRTUAL_ADDRESS {
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    uint8_t* p = GetConstantBufferPool().Request(sub, bytes, 256, nullptr, nullptr, &va);
    if (p) std::memcpy(p, data, bytes);
    return va;
  };
  const uint32_t* regs = &(*register_file_)[0];
  if (std::getenv("NHL_BETA_LIVE_TRACE"))
    REXLOG_INFO("[nhl-beta] LIVE-TRACE: pre-const-upload (cur_sub={} completed_sub={})",
                sub, GetCompletedSubmission());
  const D3D12_GPU_VIRTUAL_ADDRESS va_fetch = upload(&regs[0x4800], 32 * 6 * 4);     // fetch consts
  // Bool/loop constants are 8 bool dwords (0x4900..0x4907) + 32 loop dwords
  // (0x4908..0x4927) = 40 dwords; the translator's b1 cbuffer is declared that size.
  // Uploading fewer leaves the tail reading ZERO (D3D12 OOB CBV reads), which makes
  // any VS that drives its skinning loop from a high loop constant run 0 iterations
  // => position (0,0,0,0) => every triangle degenerate => the 3D player never
  // rasterizes a single pixel (the scene_02 create-player bug).
  const D3D12_GPU_VIRTUAL_ADDRESS va_bool = upload(&regs[0x4900], 40 * 4);          // bool/loop
  D3D12_GPU_VIRTUAL_ADDRESS va_sys = upload(&sys, sizeof(sys));
  // Float constants are TIGHTLY PACKED per shader, NOT the full 512-entry array.
  // The DXBC translator emits float-constant reads against a per-stage CBV that
  // holds only the constants the shader actually references, packed in ascending
  // storage-index order (GetPackedFloatConstantIndex), mirroring
  // D3D12CommandProcessor::UpdateBindings. Feeding the full array at natural
  // indices makes a shader reading non-contiguous constants pull the WRONG values
  // (a wrong WVP => degenerate vertex positions => the textured draws don't
  // rasterize at all). VS and PS have independent float_bitmaps => separate CBVs.
  // Xenos splits the 512-entry float constant file per stage: VERTEX constants
  // index from register 0x4000 (c0..c255), PIXEL constants from 0x4400 (c256..c511)
  // — the PS's "c0" is file entry 256 (Xenia UpdateBindings reads the pixel pack
  // from SHADER_CONSTANT_256_X). Reading both stages from 0x4000 fed the PS the
  // VS bank: the create-player post-grade PS then saw a view-matrix row as its
  // grain tint and 0.5 as its grain remap => the constant-green player.
  auto pack_floats = [&](rex::graphics::Shader* sh,
                         bool pixel_stage) -> D3D12_GPU_VIRTUAL_ADDRESS {
    alignas(16) static thread_local float packed[256 * 4];
    // NHL_BETA_PS_BANK_OLD: A/B toggle back to the (wrong) shared-bank read.
    static const bool ps_bank_old = std::getenv("NHL_BETA_PS_BANK_OLD") != nullptr;
    const uint32_t reg_base = (pixel_stage && !ps_bank_old) ? 0x4400 : 0x4000;
    uint32_t n = 0;
    if (sh) {
      const auto& crm = sh->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t bits = crm.float_bitmap[i];
        while (bits) {
          const uint32_t s = i * 64 + uint32_t(std::countr_zero(bits));
          bits &= bits - 1;
          std::memcpy(&packed[n * 4], &regs[reg_base + s * 4], 16);
          ++n;
        }
      }
    }
    if (!n) {  // root sig always has the float CBV param; bind a valid (zero) buffer
      packed[0] = packed[1] = packed[2] = packed[3] = 0.0f;
      n = 1;
    }
    return upload(packed, size_t(n) * 16);
  };
  const D3D12_GPU_VIRTUAL_ADDRESS va_floats_vs = pack_floats(beta_current_vs_, false);
  const D3D12_GPU_VIRTUAL_ADDRESS va_floats_ps = pack_floats(eff_ps, true);
  if (std::getenv("NHL_BETA_LIVE_TRACE"))
    REXLOG_INFO("[nhl-beta] LIVE-TRACE: post-const-upload");

  // Shared-memory SRV (so the VS can pull vertices). Make a broad range resident,
  // then write the raw SRV into the CP's GLOBAL bindful view heap (below). CRITICAL:
  // we must use the CP's global view/sampler heaps (via RequestView/SamplerBindful-
  // Descriptors), NOT a private heap of our own — the texture cache's RequestTextures
  // untile compute records SetComputeRootDescriptorTable/Dispatch against the global
  // heap and ASSUMES it is the bound heap. Binding a private heap leaves the global
  // heap unbound when the untile runs (D3D12 id=554/id=708 "descriptor heap differs
  // from currently set") -> the untile silently fails -> textures stay black. Sharing
  // the global heap keeps one heap bound across untile + draw.
  const bool beta_shmem = !std::getenv("NHL_BETA_NOSHMEM");
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const UINT view_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT samp_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  // Shared-memory residency + SRV: done once on the first rendered draw. The whole
  // sampled guest range is made resident (NHL_BETA_SHMEM_MB, default 512 MB), the raw
  // SRV is written into our shader-visible heap, and the buffer is transitioned to the
  // generic-read state the VS expects. No base draws touch it afterward. (Mid-frame
  // texture streaming is handled by the texture cache's untile, not a per-draw
  // re-upload — see the pipeline-state-desync fix; a per-draw REUP was tried and
  // confirmed unnecessary.)
  if (beta_shmem && first_draw) {
    // Default 512 MB (replay): the menu's cloud textures are fetched from guest addresses
    // up to ~0x1B39_5000 (~433 MB), so the whole sampled range must be made resident.
    // LIVE caps at 16 MB by default: the SDK upload ring is ~20 MB, and a single
    // RequestRange above it deadlocks in live (mid-recording flush while the game owns
    // the queue -- measured threshold: 20 MB sustains, 24 MB hangs). Live re-uploads a
    // capped range every frame, so textures fetched above the cap stay black (the menu's
    // high-memory clouds reach ~433 MB -- those need the ring-capacity fix). Proven:
    // 16 MB sustains 1000+ live frames. Override with NHL_BETA_SHMEM_MB.
    uint32_t shmem_bytes = (beta_live_ ? 16u : 512u) * 0x100000u;
    if (const char* mb = std::getenv("NHL_BETA_SHMEM_MB")) {
      shmem_bytes = uint32_t(std::strtoul(mb, nullptr, 10)) * 0x100000u;
    }
    // Inject loose-asset textures the trace never wrote into guest RAM BEFORE the
    // full-range upload below, so the injected bytes ride the same RequestRange.
    MaybeInjectBetaTextures();
    // Force our shared memory to (re)upload the guest data. Our beta instance never
    // receives the trace replay's memory-write invalidations (those go to the BASE
    // CP's shared memory), so RequestRange thinks the pages are already valid and
    // skips the upload -> the buffer is empty -> RequestTextures untiles BLACK
    // textures (confirmed: bound SRV was 100% (0,0,0,0)). Invalidating the range
    // marks it CPU-modified so RequestRange copies the guest RAM (which the replay
    // populated) into the buffer. This is REQUIRED (not optional): our beta shared
    // memory never sees the trace's mem-write invalidations, so without it RequestRange
    // skips the upload and every texture untiles black.
    static const bool live_trace = std::getenv("NHL_BETA_LIVE_TRACE") != nullptr;
    bool rr = false;
    if (live_trace) REXLOG_INFO("[nhl-beta] LIVE-TRACE: pre-RequestRange({} MB)", shmem_bytes / 0x100000);
    if (beta_live_) {
      // LIVE now uses PER-DRAW residency (the block further below makes each draw's
      // vertex/index/texture ranges resident with CURRENT data right before it draws).
      // The old first-draw-only broad RequestRange is GONE: it could not feed the game's
      // DYNAMIC geometry (vertex/index buffers at rolling addresses, ~122 new addresses/
      // frame) -- load-once never refreshed overwritten data and the one-frame defer fed
      // last frame's now-wrong addresses, so the menu/gameplay rendered "purple + chaotic
      // flashing geometry". Per-draw residency replaces it; nothing to upload here for live.
      (void)shmem_bytes;
    } else {
      // REPLAY: simulated TracePlaybackWroteMemory writes never trip beta's watches, so the
      // SDK thinks the pages are clean and skips the upload (-> textures untile black). Mark
      // the whole range CPU-modified first to force the copy.
      beta_shared_memory_->MemoryInvalidationCallback(0, shmem_bytes, false);
      rr = beta_shared_memory_->RequestRange(0, shmem_bytes);
    }
    if (live_trace) REXLOG_INFO("[nhl-beta] LIVE-TRACE: post-RequestRange (rr={})", rr);
    NotifyQueueOperationsDoneDirectly();
    if (live_trace) REXLOG_INFO("[nhl-beta] LIVE-TRACE: post-NotifyQueueOps");
    // (Live texture/vertex residency is now per-draw -- see the block before RequestTextures
    // below. The old first-draw-only deferred drain of beta_live_tex_wanted_ is gone: it
    // loaded ranges ONE FRAME LATE and ONCE, which cannot follow dynamic geometry.)
    if (std::getenv("NHL_BETA_BIND_DIAG") && memory_ && first_draw) {
      // Does guest RAM actually hold texture data at draw time? Scan it (sampled).
      // If mostly zero -> the trace didn't populate guest RAM (data missing upstream);
      // if substantially non-zero -> our shared-memory upload / untile is the problem.
      // Scan a WIDE range (default 512 MB, NHL_BETA_SCAN_MB) independent of the resident
      // cap -- the game's textures live at HIGH guest addresses, not in the low capped
      // range, so we must scan past shmem_bytes to find where real data is.
      uint32_t scan_bytes = 512u * 0x100000u;
      if (const char* sm = std::getenv("NHL_BETA_SCAN_MB"))
        scan_bytes = uint32_t(std::strtoul(sm, nullptr, 10)) * 0x100000u;
      uint32_t first_nz = UINT32_MAX;
      // A SPECIFIC probe address skips the scan entirely (reading high guest RAM hangs --
      // high physical pages read pathologically slow / protected). Use a known texture
      // address, e.g. NHL_BETA_PROBE_ADDR=0x17193000 (a menu cloud texture).
      if (const char* pa = std::getenv("NHL_BETA_PROBE_ADDR")) {
        first_nz = uint32_t(std::strtoul(pa, nullptr, 0));
      } else {
        if (live_trace) REXLOG_INFO("[nhl-beta] LIVE-TRACE: pre-scan({} MB)", scan_bytes / 0x100000);
        // Coarse scan: step 64 KB, read ONE byte per step (the fine scan over high guest
        // RAM hung). Enough to locate where the game's data lives.
        uint64_t nonzero = 0, sampled = 0;
        uint32_t last_nz = 0;
        for (uint32_t a = 0; a < scan_bytes; a += 0x10000) {
          uint8_t* p = memory_->TranslatePhysical<uint8_t*>(a);
          if (!p) continue;
          ++sampled;
          if (p[0]) {
            ++nonzero;
            if (a < first_nz) first_nz = a;
            last_nz = a;
          }
        }
        REXLOG_INFO("[nhl-beta] GUEST-RAM scan: {}/{} sampled pages non-zero ({:.1f}%), "
                    "data span [0x{:X}..0x{:X}]",
                    nonzero, sampled, sampled ? 100.0 * nonzero / sampled : 0.0, first_nz, last_nz);
      }
      if (first_nz != UINT32_MAX) {
        beta_shmem_probe_addr_ = first_nz;
        if (!beta_shmem_probe_buf_) {
          D3D12_HEAP_PROPERTIES hp{};
          hp.Type = D3D12_HEAP_TYPE_READBACK;
          D3D12_RESOURCE_DESC bd{};
          bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
          bd.Width = 4096;
          bd.Height = 1;
          bd.DepthOrArraySize = 1;
          bd.MipLevels = 1;
          bd.Format = DXGI_FORMAT_UNKNOWN;
          bd.SampleDesc.Count = 1;
          bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
          device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&beta_shmem_probe_buf_));
        }
        if (beta_shmem_probe_buf_) {
          // Make the (possibly HIGH-memory) probe page resident with a SMALL targeted
          // RequestRange (64 KB fits the ring -> no deadlock), since the main capped
          // RequestRange(0,16MB) above never touched it. In NO_INVALIDATE mode we do NOT
          // invalidate it first: if RequestRange still uploads it, the game's real writes
          // must have dirtied beta's page via physical write-watches => option (c) works.
          const uint32_t probe_page = beta_shmem_probe_addr_ & ~0xFFFFu;
          if (live_trace) REXLOG_INFO("[nhl-beta] LIVE-TRACE: pre-probe-RequestRange(@0x{:X})", probe_page);
          // Diagnostic A/B: NHL_BETA_PROBE_INVALIDATE forces the invalidation (control);
          // default (live) relies on write-watches.
          if (std::getenv("NHL_BETA_PROBE_INVALIDATE")) {
            beta_shared_memory_->MemoryInvalidationCallback(probe_page, 0x10000, false);
          }
          beta_shared_memory_->RequestRange(probe_page, 0x10000);
          NotifyQueueOperationsDoneDirectly();  // ack the new high tile's mapping queue op
          if (live_trace) REXLOG_INFO("[nhl-beta] LIVE-TRACE: post-probe-RequestRange");
          beta_shared_memory_->UseAsCopySource();  // COPY_DEST(after upload) -> COPY_SOURCE
          GetDeferredCommandList().D3DCopyBufferRegion(beta_shmem_probe_buf_.Get(), 0,
                                                       beta_shared_memory_->GetBuffer(),
                                                       beta_shmem_probe_addr_, 4096);
          beta_shmem_probe_pending_ = true;
        }
      }
    }
    // REPLAY transitions to the read state here (single broad upload above is complete).
    // LIVE does NOT: each draw re-uploads its own dynamic ranges and flips to read state
    // itself in the per-draw residency block, so a read transition here would just be
    // undone by the next per-draw RequestRange (-> COPY_DEST) anyway.
    if (!beta_live_) beta_shared_memory_->UseForReading();
    if (first_draw) {
      REXLOG_INFO("[nhl-beta] owned-draw: RequestRange(0,{} MB) -> {} | trace mem-writes mirrored to "
                  "beta caches: {} writes / {} MB (0 => override not firing -> textures stay black)",
                  shmem_bytes / 0x100000, rr, beta_trace_writes_seen_,
                  beta_trace_write_bytes_ / 0x100000);
    }
  }

  if (first_draw) {
    ID3D12PipelineState* pso_now = beta_pipeline_cache_->GetD3D12PipelineByHandle(pso_handle);
    REXLOG_INFO("[nhl-beta] owned-draw DIAG: vs_textures={} ps_textures={} pso_ready={} "
                "pipelines_creating={}",
                vs_tex, ps_tex, static_cast<const void*>(pso_now),
                beta_pipeline_cache_->IsCreatingPipelines());
  }

  // Texture + sampler binding (the UpdateBindings "extras"). RequestTextures makes
  // the shaders' textures resident (untile/upload) in the SRV state; then write
  // their SRVs into our view heap (slots 4+) and samplers into the sampler heap,
  // and bind the tables at the root-parameter indices the bindful root signature
  // reserved. RequestTextures records compute work, so it must run before the
  // graphics binds below. GetRootBindfulExtraParameterIndices is private, so we
  // reproduce its index assignment: extras are appended after the always-present
  // params (0..kRootParameter_Bindful_Count_Base-1 = 0..5) in the fixed order
  // {pixel textures, pixel samplers, vertex textures, vertex samplers}, each
  // present only if that stage has any.
  // Texture binding is ON by default (the validated takeover path needs it for any
  // textured content). The original root-signature param-count fault (708) was solved
  // by forcing the bindful root sig (d3d12_bindless=false, see SetupContext). Opt OUT
  // with NHL_BETA_NOTEX to isolate geometry/untextured behaviour for diagnostics.
  const bool beta_tex = std::getenv("NHL_BETA_NOTEX") == nullptr;
  const auto& ps_texs = eff_ps ? eff_ps->GetTextureBindingsAfterTranslation()
                               : beta_current_vs_->GetTextureBindingsAfterTranslation();
  const auto& vs_texs = beta_current_vs_->GetTextureBindingsAfterTranslation();
  const auto& vs_samps = beta_current_vs_->GetSamplerBindingsAfterTranslation();
  const size_t ps_tex_n = (beta_tex && eff_ps) ? ps_texs.size() : 0;
  const size_t ps_samp_n =
      (beta_tex && eff_ps) ? eff_ps->GetSamplerBindingsAfterTranslation().size() : 0;
  constexpr uint32_t kUnavail = UINT32_MAX;
  constexpr uint32_t kCountBase = 6;  // kRootParameter_Bindful_Count_Base
  uint32_t next_param = kCountBase;
  uint32_t idx_tex_ps = ps_tex_n ? next_param++ : kUnavail;
  uint32_t idx_samp_ps = ps_samp_n ? next_param++ : kUnavail;
  uint32_t idx_tex_vs = (beta_tex && vs_texs.size()) ? next_param++ : kUnavail;
  uint32_t idx_samp_vs = (beta_tex && vs_samps.size()) ? next_param++ : kUnavail;

  const uint32_t used_tex_mask =
      beta_tex ? (beta_current_vs_->GetUsedTextureMaskAfterTranslation() |
                  (beta_current_ps_ ? beta_current_ps_->GetUsedTextureMaskAfterTranslation() : 0u))
               : 0u;
  // PER-DRAW RESIDENCY (live, ungated): make THIS draw's vertex, index, and texture guest
  // ranges resident with CURRENT data right before it draws, then flip the shared-memory
  // buffer to the read state the IA/VS + the texture untile read from. This is the dynamic-
  // geometry model and replaces the old "discover-at-draw, load-NEXT-frame, load-once"
  // scheme: the game rewrites vertex/index buffers at ROLLING addresses every frame
  // (~122 new addresses/frame), so a load-once cache went stale and a one-frame defer fed
  // last frame's now-wrong addresses -> "purple + chaotic flashing geometry". RequestRange
  // uploads ONLY pages the game's physical write-watches marked dirty (live writes trip
  // them -- no invalidation needed), so static content (textures) uploads once and stays
  // resident while this frame's dynamic meshes re-upload exactly their changed pages.
  // Mirrors the base CP UpdateBindings order: RequestRange -> UseForReading -> RequestTextures.
  if (beta_live_ && beta_shmem) {
    static const bool live_trace2 = std::getenv("NHL_BETA_LIVE_TRACE") != nullptr;
    // DECISIVE TEST / FIX: write-watch reliance was only ever PROVEN for one texture page.
    // If the game's DYNAMIC vertex/index writes do NOT trip beta's watches, RequestRange
    // treats those pages as clean and skips the upload -> the VS/IA reads STALE shared-memory
    // bytes -> geometry rasterizes but mis-positioned ("purple + chaotic flickering"). The
    // replay path (which renders this exact menu perfectly) FORCE-invalidates before upload.
    // So force-invalidate the SMALL vertex+index ranges each draw before RequestRange: that
    // copies CURRENT guest RAM unconditionally. It's ring-safe because these ranges are tiny
    // (a few KB each) and dynamic anyway. Textures are NOT force-invalidated (their 6MB
    // windows would blow the ~20MB ring); they stay on write-watch reliance (and are static,
    // so first-upload suffices). Toggle off with NHL_BETA_NO_VTX_INVALIDATE to A/B test.
    static const bool vtx_invalidate = std::getenv("NHL_BETA_NO_VTX_INVALIDATE") == nullptr;
    // Don't force-upload a suspiciously large "vertex" range (a bad vfetch decode caps at
    // 16MB): re-uploading that every draw would approach the ring. Only invalidate sane sizes.
    uint32_t vtx_inv_max = 2u * 0x100000u;
    if (const char* m = std::getenv("NHL_BETA_VTX_INV_MAX_MB")) vtx_inv_max = uint32_t(std::strtoul(m, nullptr, 10)) * 0x100000u;
    static const bool vtx_diag = std::getenv("NHL_BETA_VTX_DIAG") != nullptr;
    uint32_t req_bytes = 0, inv_ranges = 0, inv_bytes = 0;
    auto request = [&](uint32_t base, uint32_t size, bool invalidate) {
      if (!base || base >= 512u * 0x100000u || !size) return;
      size = std::min(size, 512u * 0x100000u - base);
      if (invalidate && vtx_invalidate && size <= vtx_inv_max) {
        beta_shared_memory_->MemoryInvalidationCallback(base, size, false);
        ++inv_ranges;
        inv_bytes += size;
      }
      beta_shared_memory_->RequestRange(base, size);
      req_bytes += size;
    };
    // Vertices: the VS pulls vertex data from shared memory via vfetch constants; without
    // these resident the VS reads zeros -> degenerate triangles -> nothing rasterizes. The
    // vfetch constant gives the EXACT byte range (address<<2, size<<2).
    uint32_t vtx0_base = 0, vtx0_size = 0;
    if (beta_current_vs_) {
      for (const auto& vb : beta_current_vs_->vertex_bindings()) {
        xenos::xe_gpu_vertex_fetch_t f{};
        f.dword_0 = regs[0x4800 + vb.fetch_constant * 2];
        f.dword_1 = regs[0x4800 + vb.fetch_constant * 2 + 1];
        uint32_t size = f.size << 2;
        if (size > 16u * 0x100000u) size = 16u * 0x100000u;  // ring-safe cap (bad-decode guard)
        if (!vtx0_base) { vtx0_base = f.address << 2; vtx0_size = size; }
        request(f.address << 2, size, /*invalidate=*/true);
      }
    }
    // Index buffer (guest DMA only): the draw reads indices straight from shared memory at
    // guest_base; kHostConverted/kHostBuiltin index buffers live in host-owned buffers, not
    // shared memory, so they need no residency. The IndexBufferInfo gives the exact range.
    if (result.index_buffer_type ==
            rex::graphics::PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
        index_buffer_info) {
      request(index_buffer_info->guest_base, uint32_t(index_buffer_info->length),
              /*invalidate=*/true);
    }
    // VTX_DIAG: does guest RAM (CPU side) even hold vertex data at the decoded base? If nz=0
    // here, the address decode is wrong or the game wrote the verts elsewhere (residency can't
    // help); if nz>0, the data is present and the question is whether our upload delivers it.
    if (vtx_diag && vtx0_base && memory_ && beta_takeover_rendered_ < 48) {
      uint32_t nz = 0;
      const uint8_t* vp = memory_->TranslatePhysical<const uint8_t*>(vtx0_base);
      if (vp) for (int b = 0; b < 256; ++b) if (vp[b]) ++nz;
      REXLOG_INFO("[nhl-beta] VTX-DIAG #{} vbinds={} vtx0=0x{:X} size={} guestRAM-nz={}/256 "
                  "idx_type={} host_vtx={} vport={}",
                  beta_takeover_rendered_,
                  beta_current_vs_ ? uint32_t(beta_current_vs_->vertex_bindings().size()) : 0u,
                  vtx0_base, vtx0_size, nz, uint32_t(result.index_buffer_type),
                  result.host_draw_vertex_count, vport_xform ? 1 : 0);
    }
    // Textures (PS + VS, PRE-translation bindings -- the after-translation used mask is 0 on
    // many live draws so don't gate on it): SDK GetTextureTotalSize/FormatInfo::Get are
    // unexported, so use a fixed window -- the game's textures are packed contiguously so any
    // over-read lands in adjacent texture data (safe; a raw high-RAM scan HANGS). The untile
    // reads these from shared memory.
    uint32_t win = 6u * 0x100000u;
    if (const char* w = std::getenv("NHL_BETA_TEX_WIN_MB")) win = uint32_t(std::strtoul(w, nullptr, 10)) * 0x100000u;
    auto request_tex = [&](auto* sh) {
      if (!sh) return;
      for (const auto& tb : sh->texture_bindings()) {
        xenos::xe_gpu_texture_fetch_t tf{};
        std::memcpy(&tf, &regs[0x4800 + tb.fetch_constant * 6], 6 * 4);
        const uint32_t base = uint32_t(tf.base_address) << 12;
        const uint32_t tw = uint32_t(tf.size_2d.width) + 1, th = uint32_t(tf.size_2d.height) + 1;
        if (tw < 2 || tw > 8192 || th < 2 || th > 8192) continue;  // not a 2D texture fetch
        request(base, win, /*invalidate=*/false);  // textures: write-watch (6MB window blows ring if forced)
      }
    };
    request_tex(eff_ps);
    request_tex(beta_current_vs_);
    // Commit the uploads, then transition to the read state for the IA/VS fetch + the untile.
    NotifyQueueOperationsDoneDirectly();
    beta_shared_memory_->UseForReading();
    // HARD CONSTRAINT WATCH: a single FRAME's total dirty upload must stay under the SDK
    // upload ring (~20 MB) -- beta cannot flush mid-recording in live (the game owns the
    // queue submission; a RequestRange whose dirty payload exceeds the ring deadlocks). Per-
    // draw residency spreads it and RequestRange skips clean pages, so steady state only
    // moves this frame's dynamic deltas. The accumulator is an UPPER BOUND (it counts every
    // requested byte, not just dirty ones); if it nears the ring on a heavy frame, that is
    // the deadlock risk to investigate. Reset per frame in IssueSwap.
    beta_live_frame_req_bytes_ += req_bytes;
    if (req_bytes && beta_live_frame_req_bytes_ > 18u * 0x100000u && !beta_live_ring_warned_) {
      beta_live_ring_warned_ = true;
      REXLOG_WARN("[nhl-beta] LIVE per-draw residency: frame requested {} MB (> ~18 MB) -- "
                  "approaching the ~20 MB upload ring; a heavier frame may deadlock the flush",
                  beta_live_frame_req_bytes_ / 0x100000);
    }
    beta_live_frame_inv_bytes_ += inv_bytes;
    if (live_trace2 && req_bytes && beta_takeover_rendered_ < 64) {
      REXLOG_INFO("[nhl-beta] LIVE-TRACE: perdraw-resident #{} req={} KB inv={} ranges/{} KB "
                  "(frame total req {} KB, forced-upload {} KB)",
                  beta_takeover_rendered_, req_bytes / 1024, inv_ranges, inv_bytes / 1024,
                  beta_live_frame_req_bytes_ / 1024, beta_live_frame_inv_bytes_ / 1024);
    }
  }
  if (used_tex_mask) {
    // CRITICAL: invalidate the texture cache's per-slot binding sync before resolving.
    // The cache caches which guest texture each fetch-constant slot maps to and only
    // re-resolves slots marked out-of-sync via TextureFetchConstantsWritten — which the
    // BASE CP calls from WriteRegister on fetch-constant writes. Our beta cache never
    // receives those (the trace replay drives the base's flow), so without this every
    // draw kept the FIRST draw's texture (RenderDoc proved all draws bound resid 821,
    // the flag) — clouds/UI all rendered the flag. Marking all 32 slots dirty forces
    // RequestTextures to re-read the current draw's fetch constants; unchanged textures
    // stay cached by content hash, so this only re-resolves bindings, not data.
    if (std::getenv("NHL_BETA_LIVE_TRACE")) REXLOG_INFO("[nhl-beta] LIVE-TRACE: pre-RequestTextures(mask=0x{:X})", used_tex_mask);
    beta_texture_cache_->TextureFetchConstantsWritten(0, 31);
    beta_texture_cache_->RequestTextures(used_tex_mask);
    if (std::getenv("NHL_BETA_LIVE_TRACE")) REXLOG_INFO("[nhl-beta] LIVE-TRACE: post-RequestTextures");
  }
  if (edram_rov) {
    uint32_t textures_resolution_scaled = 0;
    for (uint32_t texture_index = 0; texture_index < 32; ++texture_index) {
      if (!(rov_used_texture_mask & (uint32_t(1) << texture_index))) continue;
      const uint32_t signs_shift = (texture_index & 3u) * 8u;
      const uint32_t signs_mask = 0xFFu << signs_shift;
      const uint32_t signs =
          uint32_t(beta_texture_cache_->GetActiveTextureSwizzledSigns(texture_index))
          << signs_shift;
      if (!std::getenv("NHL_BETA_ROV_ZERO_SIGNS")) {
        sys.texture_swizzled_signs[texture_index >> 2] =
            (sys.texture_swizzled_signs[texture_index >> 2] & ~signs_mask) | signs;
      }
      textures_resolution_scaled |=
          uint32_t(beta_texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
    sys.textures_resolution_scaled = textures_resolution_scaled;
    if (std::getenv("NHL_BETA_EDRAM_DIAG") && beta_takeover_rendered_ >= 280 &&
        beta_takeover_rendered_ <= 354) {
      REXLOG_INFO("[nhl-beta] ROV tex-signs #{}: used=0x{:X} scaled=0x{:X} "
                  "signs=[{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}]",
                  beta_takeover_rendered_, rov_used_texture_mask, textures_resolution_scaled,
                  sys.texture_swizzled_signs[0], sys.texture_swizzled_signs[1],
                  sys.texture_swizzled_signs[2], sys.texture_swizzled_signs[3],
                  sys.texture_swizzled_signs[4], sys.texture_swizzled_signs[5],
                  sys.texture_swizzled_signs[6], sys.texture_swizzled_signs[7]);
    }
    va_sys = upload(&sys, sizeof(sys));
  }
  // Allocate the view descriptors from the CP's GLOBAL bindful view heap via the
  // PUBLIC RequestOneUseSingleViewDescriptors (the private RequestViewBindfulDescriptors
  // isn't reachable from a subclass). In BINDFUL mode this allocates a CONTIGUOUS range
  // from the same view_bindful_heap_pool_ the untile uses and rebinds the heap THROUGH
  // the CP's own tracking (view_bindful_heap_current_) — so the untile's heap check
  // stays consistent and its compute no longer faults (the real id=554/id=708 cause was
  // our private SetDescriptorHeaps desyncing that tracking). Disasm of RVA 0x178448
  // (bindful branch) confirms contiguous allocation + tracked rebind. View range layout:
  //   [0]    shared-memory raw SRV     -> SharedMemoryAndEdram table (root param 5)
  //   [1..3] reserved (param-5 table span; unused by the host-RT pixel shader)
  //   [4..]  pixel-, then vertex-shader texture SRVs (root param 6, [8])
  namespace util = rex::ui::d3d12::util;
  constexpr uint32_t kViewPrefix = 4;
  const uint32_t vs_tex_n = beta_tex ? uint32_t(vs_texs.size()) : 0u;
  const uint32_t view_count = kViewPrefix + uint32_t(ps_tex_n) + vs_tex_n;
  std::vector<util::DescriptorCpuGpuHandlePair> view_handles(view_count);
  const bool view_ok = RequestOneUseSingleViewDescriptors(view_count, view_handles.data());
  D3D12_CPU_DESCRIPTOR_HANDLE view_cpu = view_ok ? view_handles[0].first : D3D12_CPU_DESCRIPTOR_HANDLE{};
  D3D12_GPU_DESCRIPTOR_HANDLE view_gpu = view_ok ? view_handles[0].second : D3D12_GPU_DESCRIPTOR_HANDLE{};
  if (beta_shmem && view_ok) {
    beta_shared_memory_->WriteRawSRVDescriptor(view_cpu);  // [0] = shared memory SRV
    D3D12_CPU_DESCRIPTOR_HANDLE shmem_uav{view_cpu.ptr + SIZE_T(view_inc)};
    util::CreateBufferRawUAV(device, shmem_uav, nullptr, 0);  // [1] = null UAV
    if (edram_rov) {
      D3D12_CPU_DESCRIPTOR_HANDLE edram_uav{view_cpu.ptr + SIZE_T(2) * view_inc};
      beta_render_target_cache_->WriteEdramUintPow2UAVDescriptor(edram_uav, 2);
    }
  }
  // Samplers (root param 7): the bindful sampler allocator (RequestSamplerBindful-
  // Descriptors) is PRIVATE and there is no public sampler-heap allocator reachable
  // from a subclass, so per-draw bindful samplers are blocked here (would need the
  // Samplers are written into our own shader-visible sampler heap and bound alongside
  // the (unchanged) global view heap — the SetDescriptorHeaps call below passes BOTH so
  // the view-heap tracking the untile depends on stays consistent. ON by default (the
  // validated path needs real samplers); opt OUT with NHL_BETA_NOSAMP for diagnostics.
  const bool beta_samp = std::getenv("NHL_BETA_NOSAMP") == nullptr;
  const uint32_t samp_count =
      beta_samp ? (uint32_t(ps_samp_n) + (beta_tex ? uint32_t(vs_samps.size()) : 0u)) : 0u;
  if (samp_count && !beta_sampler_heap_) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = 2048;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&beta_sampler_heap_));
  }
  // To bind our sampler heap we must re-issue SetDescriptorHeaps with BOTH heaps, and
  // the cbv/srv/uav heap argument MUST stay the global view heap RequestOneUseSingle-
  // ViewDescriptors just bound (else we'd re-desync the view tracking and break the
  // untile again). There is no public getter for it, so read the CP's private
  // `view_bindful_heap_current_` (D3D12CommandProcessor + 0xCE8; verified from the
  // rexruntimerd.dll disasm of RequestOneUseSingleViewDescriptors @0x178448, which
  // compares the pool's current heap against [this+0xCE8]). Gated by NHL_BETA_SAMP.
  ID3D12DescriptorHeap* global_view_heap = nullptr;
  if (samp_count && beta_sampler_heap_ && view_ok) {
    auto* base_cp = static_cast<d3d12::D3D12CommandProcessor*>(this);
    global_view_heap = ReadCpViewHeapChecked(base_cp);
    if (!global_view_heap) {
      REXLOG_WARN("[nhl-beta] CP view-heap read at +0xCE8 failed validation (SDK layout "
                  "changed?); skipping sampler table this draw");
    }
  }
  D3D12_CPU_DESCRIPTOR_HANDLE samp_cpu =
      (samp_count && beta_sampler_heap_) ? beta_sampler_heap_->GetCPUDescriptorHandleForHeapStart()
                                         : D3D12_CPU_DESCRIPTOR_HANDLE{};
  D3D12_GPU_DESCRIPTOR_HANDLE samp_gpu =
      (samp_count && beta_sampler_heap_) ? beta_sampler_heap_->GetGPUDescriptorHandleForHeapStart()
                                         : D3D12_GPU_DESCRIPTOR_HANDLE{};
  const D3D12_GPU_DESCRIPTOR_HANDLE shmem_table = view_gpu;  // param 5 base = [0]
  uint32_t view_off = kViewPrefix;
  uint32_t samp_off = 0;
  D3D12_GPU_DESCRIPTOR_HANDLE tex_table_vs{0}, tex_table_ps{0}, samp_table_vs{0}, samp_table_ps{0};
  const bool beta_faketex = std::getenv("NHL_BETA_FAKETEX") != nullptr;
  if (beta_faketex && ps_tex_n) EnsureBetaFakeTexture();
  auto write_tex_table = [&](const auto& tbs, D3D12_GPU_DESCRIPTOR_HANDLE& out,
                             bool is_vertex_stage) {
    if (tbs.empty() || !view_ok) return;
    out.ptr = view_gpu.ptr + UINT64(view_off) * view_inc;
    static const bool flat_sub = std::getenv("NHL_BETA_FLAT") != nullptr;
    for (size_t i = 0; i < tbs.size(); ++i) {
      D3D12_CPU_DESCRIPTOR_HANDLE h{view_cpu.ptr + SIZE_T(view_off + i) * view_inc};
      // Route (a): if this binding samples a captured resolve DEST, bind our flat host
      // RT (correct, no guest-RAM untile) instead of the texture cache's SRV.
      if (flat_sub) {
        xenos::xe_gpu_texture_fetch_t ftf{};
        std::memcpy(&ftf, &regs[0x4800 + tbs[i].fetch_constant * 6], 24);
        const uint32_t fbase = uint32_t(ftf.base_address) << 12;
        auto fit = beta_flat_resolves_.find(fbase);
        if (fit != beta_flat_resolves_.end() && fit->second.tex) {
          ID3D12Resource* srvtex = fit->second.tex.Get();
          // NHL_BETA_FLAT_FAKE: bind a solid-red texture instead of the captured one, to
          // isolate WHERE the composite places this resolved texture (does it sample it at
          // all / full-screen?) vs whether the captured content is empty.
          if (std::getenv("NHL_BETA_FLAT_FAKE")) {
            EnsureBetaFakeTexture();
            if (beta_fake_tex_ready_) srvtex = beta_fake_tex_.Get();
          }
          D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
          sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          sd.Texture2D.MipLevels = 1;
          GetD3D12Provider().GetDevice()->CreateShaderResourceView(srvtex, &sd, h);
          continue;
        }
      }
      // VS-texture CPU-upload path: linear k_32_32_32_32_FLOAT 2D textures sampled by
      // the vertex shader (the create-player skinning palette) load as ZERO through
      // the SDK cache on our owned path — bind a CPU-built copy instead (see
      // EnsureBetaVsTexSub). The shader declares texture2dARRAY, so the SRV must be
      // TEXTURE2DARRAY (a plain TEXTURE2D SRV samples as zero — the FAKETEX pitfall).
      static const bool no_vstex_cpu = std::getenv("NHL_BETA_NO_VSTEX_CPU") != nullptr;
      if (is_vertex_stage && !no_vstex_cpu) {
        xenos::xe_gpu_texture_fetch_t btf{};
        std::memcpy(&btf, &regs[0x4800 + tbs[i].fetch_constant * 6], 24);
        const uint32_t bbase = uint32_t(btf.base_address) << 12;
        if (bbase && uint32_t(btf.format) == uint32_t(xenos::TextureFormat::k_32_32_32_32_FLOAT) &&
            !btf.tiled && uint32_t(btf.dimension) == uint32_t(xenos::DataDimension::k2DOrStacked) &&
            !btf.stacked) {
          BetaVsTexEntry* e = EnsureBetaVsTexSub(bbase, uint32_t(btf.size_2d.width) + 1,
                                                 uint32_t(btf.size_2d.height) + 1);
          if (e && e->ready) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2DArray.MipLevels = 1;
            sd.Texture2DArray.ArraySize = 1;
            GetD3D12Provider().GetDevice()->CreateShaderResourceView(e->tex.Get(), &sd, h);
            continue;
          }
        }
      }
      // PS-texture diagnostics + missing-texture transparency mitigation. The scene_04
      // glass / end-netting forward pass derives its output ALPHA from a texture
      // (PS: oC0.w = tf0.a) whose guest data is absent in this trace; beta renders the
      // missing texture as white-opaque, turning the transparent glass into an opaque
      // wall over the near ice. NHL_BETA_PSTEX_DIAG logs each PS texture's binding
      // state; NHL_BETA_PS_TRANSP_RANGE=lo-hi binds a NULL SRV (samples (0,0,0,0) ->
      // alpha 0) for PS textures of draws in [lo,hi], so the geometry renders transparent.
      if (!is_vertex_stage) {
        if (std::getenv("NHL_BETA_PSTEX_DIAG")) {
          xenos::xe_gpu_texture_fetch_t df{};
          std::memcpy(&df, &regs[0x4800 + tbs[i].fetch_constant * 6], 24);
          const uint32_t dbase = uint32_t(df.base_address) << 12;
          uint32_t dnz = 0;
          if (memory_ && dbase) {
            const uint8_t* dp = memory_->TranslatePhysical<const uint8_t*>(dbase);
            if (dp) for (int b = 0; b < 4096; ++b) if (dp[b]) ++dnz;
          }
          REXLOG_INFO("[nhl-beta] pstex #{} slot{} fc={} base=0x{:X} {}x{} fmt={} "
                      "host_swizzle=0x{:X} guest-RAM nz={}/4096",
                      beta_takeover_rendered_, uint32_t(i), tbs[i].fetch_constant, dbase,
                      uint32_t(df.size_2d.width) + 1, uint32_t(df.size_2d.height) + 1,
                      uint32_t(df.format),
                      beta_texture_cache_->GetActiveTextureHostSwizzle(tbs[i].fetch_constant), dnz);
        }
        // Missing-texture transparency (OPT-IN: NHL_BETA_MISSING_TRANSP=1; or
        // NHL_BETA_PS_TRANSP_RANGE=lo-hi forces a draw range). An ALPHA-BLENDED 3D draw whose
        // sampled texture's guest data is ABSENT this frame (nz=0) gets a NULL SRV (samples
        // (0,0,0,0) -> alpha 0) instead of the texture cache's stale content. The scene_04
        // rink glass / end-netting derive their coverage alpha from such a texture
        // (PS: oC0.w = tf0.a); with stale white cache content beta drew them as an opaque wall
        // over the near ice — this flag makes them transparent, restoring the near-ice view.
        //
        // WHY OPT-IN (not default): nz=0 in CPU guest RAM does NOT reliably mean "missing".
        // A texture loaded earlier in the trace whose source page was later freed also reads
        // nz=0 yet has VALID cached content (e.g. scene_02's create-player equipment thumbnails
        // — same vte=0x43F/blended/nz=0/ext signature as the glass). beta cannot tell stale
        // garbage from valid-cached without inspecting the cached texels, so forcing transparent
        // would wrongly blank those. Default-off keeps every validated scene byte-identical;
        // enable for gameplay (scene_04) where the glass/netting trace-gap dominates. The real
        // fix is capture-side: a warm-texture capture provides the glass/netting alpha texture.
        bool force_transp = false;
        if (const char* rg = std::getenv("NHL_BETA_PS_TRANSP_RANGE")) {
          char* dash = nullptr;
          const unsigned long lo = std::strtoul(rg, &dash, 10);
          const unsigned long hi = (dash && *dash) ? std::strtoul(dash + 1, nullptr, 10) : lo;
          force_transp = beta_takeover_rendered_ >= uint32_t(lo) &&
                         beta_takeover_rendered_ <= uint32_t(hi);
        } else if (std::getenv("NHL_BETA_MISSING_TRANSP")) {
          const uint32_t blend0 = (*register_file_)[0x2201];  // RB_BLENDCONTROL0
          const bool blended = ((blend0 >> 8) & 0x1Fu) != 0u;  // color_dst_blend != ZERO
          if (blended && vport_xform) {  // 3D transparency geometry only
            xenos::xe_gpu_texture_fetch_t df{};
            std::memcpy(&df, &regs[0x4800 + tbs[i].fetch_constant * 6], 24);
            const uint32_t dbase = uint32_t(df.base_address) << 12;
            if (dbase && memory_) {
              const uint8_t* dp = memory_->TranslatePhysical<const uint8_t*>(dbase);
              if (dp) {
                bool absent = true;
                for (int b = 0; b < 4096; ++b) {
                  if (dp[b]) { absent = false; break; }  // present textures early-out cheaply
                }
                force_transp = absent;
              }
            }
          }
        }
        if (force_transp) {
          // NHL_BETA_PS_SHOW: bind the solid-RED fake texture (alpha 255 -> opaque) instead
          // of null, so the affected geometry shows its SHAPE in red (verification: confirm
          // the forced-transparent draws really are the rink glass panes + end nets).
          if (std::getenv("NHL_BETA_PS_SHOW")) {
            EnsureBetaFakeTexture();
            if (beta_fake_tex_ready_) {
              D3D12_SHADER_RESOURCE_VIEW_DESC sr{};
              sr.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
              sr.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
              sr.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
              sr.Texture2D.MipLevels = 1;
              GetD3D12Provider().GetDevice()->CreateShaderResourceView(beta_fake_tex_.Get(), &sr, h);
              continue;
            }
          }
          D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
          sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          sd.Texture2D.MipLevels = 1;
          GetD3D12Provider().GetDevice()->CreateShaderResourceView(nullptr, &sd, h);  // null -> 0
          continue;
        }
      }
      if (beta_faketex && beta_fake_tex_ready_) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        GetD3D12Provider().GetDevice()->CreateShaderResourceView(beta_fake_tex_.Get(), &sd, h);
      } else {
        beta_texture_cache_->WriteActiveTextureBindfulSRV(tbs[i], h);
      }
    }
    view_off += uint32_t(tbs.size());
  };
  auto write_samp_table = [&](const auto& sbs, D3D12_GPU_DESCRIPTOR_HANDLE& out) {
    if (sbs.empty() || !samp_count || !beta_sampler_heap_) return;
    out.ptr = samp_gpu.ptr + UINT64(samp_off) * samp_inc;
    for (size_t i = 0; i < sbs.size(); ++i) {
      auto params = beta_texture_cache_->GetSamplerParameters(sbs[i]);
      D3D12_CPU_DESCRIPTOR_HANDLE h{samp_cpu.ptr + SIZE_T(samp_off + i) * samp_inc};
      beta_texture_cache_->WriteSampler(params, h);
    }
    samp_off += uint32_t(sbs.size());
  };
  if (std::getenv("NHL_BETA_BIND_DIAG")) {
    REXLOG_INFO("[nhl-beta] bind#{}: ps_tex={} ps_samp={} vs_tex={} vs_samp={} -> idx(tp={} sp={} "
                "tv={} sv={}) total={} rast_done={} ncm=0x{:X} used_tex_mask=0x{:X} "
                "ps_used_mask=0x{:X} submission={} frame_open={}",
                beta_takeover_rendered_, ps_tex_n, ps_samp_n, vs_texs.size(), vs_samps.size(),
                idx_tex_ps, idx_samp_ps, idx_tex_vs, idx_samp_vs, next_param, rast_done, ncm,
                used_tex_mask, eff_ps ? eff_ps->GetUsedTextureMaskAfterTranslation() : 0u,
                GetCurrentSubmission(), GetCurrentFrame());
    // Geometry/blend/alpha state for textured draws (only): is the textured draw set
    // up to actually put visible color on screen, or is it culled / alpha-killed /
    // zero-blended / mis-transformed?
    if (ps_tex_n) {
      REXLOG_INFO("[nhl-beta]   draw-state #{}: host_prim={} host_vs_type={} host_verts={} "
                  "idx_type={} vte=0x{:X} ndc_scale=({:.4f},{:.4f},{:.4f}) "
                  "ndc_offset=({:.4f},{:.4f},{:.4f}) z=[{:.4f},{:.4f}] vp_extent=({},{}) "
                  "vp_off=({},{}) alpha_test={} alpha_func={} blendctl0=0x{:X}",
                  beta_takeover_rendered_, uint32_t(result.host_primitive_type),
                  uint32_t(result.host_vertex_shader_type), result.host_draw_vertex_count,
                  uint32_t(result.index_buffer_type), vte.value, vpi.ndc_scale[0], vpi.ndc_scale[1],
                  vpi.ndc_scale[2], vpi.ndc_offset[0], vpi.ndc_offset[1], vpi.ndc_offset[2],
                  vpi.z_min, vpi.z_max, vpi.xy_extent[0], vpi.xy_extent[1], vpi.xy_offset[0],
                  vpi.xy_offset[1], uint32_t(cc.alpha_test_enable), uint32_t(cc.alpha_func),
                  (*register_file_)[0x2201]);
      // Color-target encoding: guest RB_COLOR_INFO.color_format decides whether the RT
      // is gamma (k_8_8_8_8_GAMMA=1 / k_2_10_10_10_AS_10_10_10_10? ) vs linear UNORM.
      // We hardcode rt_formats[1]=0 (k_8_8_8_8); if the guest is the GAMMA variant the
      // base path gamma-encodes on write and we don't => systematic ~gamma color gap.
      {
        const auto ci = register_file_->Get<reg::RB_COLOR_INFO>();
        REXLOG_INFO("[nhl-beta]   color-fmt #{}: RB_COLOR_INFO.color_format={} exp_bias={}",
                    beta_takeover_rendered_, uint32_t(ci.color_format), int32_t(ci.color_exp_bias));
      }
      // Per-draw TEXTURE identity: parse each PS texture binding's fetch constant
      // (6 dwords from 0x4800) to see WHICH guest texture this draw samples (base
      // addr + dims). Reveals e.g. a cloud-background draw wrongly bound to the flag.
      if (eff_ps) {
        for (const auto& tb : eff_ps->texture_bindings()) {
          const uint32_t fc6 = 0x4800 + tb.fetch_constant * 6;
          xenos::xe_gpu_texture_fetch_t tf{};
          std::memcpy(&tf, &regs[fc6], 6 * 4);
          const uint32_t tex_base = uint32_t(tf.base_address) << 12;
          // DRAW-TIME residency: is this texture's guest data actually present in guest
          // RAM at the moment this draw runs? (The first-draw shmem-probe reads too early
          // for mid-frame-streamed textures.) Zeros here ⇒ the data never reaches our
          // guest RAM in this trace (pre-frame-static asset missing, or GPU-generated) ⇒
          // no per-draw re-upload can fix it. Non-zero ⇒ data IS resident ⇒ the bug is the
          // texture cache not re-untiling after the mid-frame invalidation.
          uint32_t tex_nz = 0;
          if (memory_ && tex_base) {
            const uint8_t* tram = memory_->TranslatePhysical<const uint8_t*>(tex_base);
            if (tram) {
              for (int i = 0; i < 4096; ++i) {
                if (tram[i]) ++tex_nz;
              }
            }
          }
          REXLOG_INFO("[nhl-beta]   tex#{} fc={} base=0x{:X} {}x{} fmt={} swizzle=0x{:X} "
                      "tiled={} endian={} pitch={} mip_min={} mip_max={} packed_mips={} "
                      "mip_addr=0x{:X} DRAW-TIME guest-RAM nz={}/4096",
                      beta_takeover_rendered_, tb.fetch_constant, tex_base,
                      uint32_t(tf.size_2d.width) + 1, uint32_t(tf.size_2d.height) + 1,
                      uint32_t(tf.format), uint32_t(tf.swizzle), uint32_t(tf.tiled),
                      uint32_t(tf.endianness), uint32_t(tf.pitch), uint32_t(tf.mip_min_level),
                      uint32_t(tf.mip_max_level), uint32_t(tf.packed_mips),
                      uint32_t(tf.mip_address) << 12, tex_nz);
        }
      }
      // VS texture identity (same decode as the PS loop above): the create-player VS
      // skins through a BONE-MATRIX texture fetched in the vertex shader (tf16). If its
      // guest data is absent/zero at draw time, the skinning matrix is 0 => all verts
      // collapse => zero rasterized pixels with every other state correct.
      if (beta_current_vs_) {
        for (const auto& tb : beta_current_vs_->texture_bindings()) {
          const uint32_t fc6 = 0x4800 + tb.fetch_constant * 6;
          xenos::xe_gpu_texture_fetch_t tf{};
          std::memcpy(&tf, &regs[fc6], 6 * 4);
          const uint32_t tex_base = uint32_t(tf.base_address) << 12;
          // Scan the FULL texture extent (the bone arena may be zero-padded at its base
          // with the live pose matrices at a higher offset): nonzero bytes + first/last
          // nonzero offset across width*height*8B (k_16_16_16_16_FLOAT).
          uint32_t tex_nz = 0;
          uint32_t first_nz_off = UINT32_MAX, last_nz_off = 0;
          const uint32_t scan_bytes =
              (uint32_t(tf.size_2d.width) + 1) * (uint32_t(tf.size_2d.height) + 1) * 8;
          if (memory_ && tex_base) {
            const uint8_t* tram = memory_->TranslatePhysical<const uint8_t*>(tex_base);
            if (tram) {
              for (uint32_t i = 0; i < scan_bytes; ++i) {
                if (tram[i]) {
                  ++tex_nz;
                  if (i < first_nz_off) first_nz_off = i;
                  last_nz_off = i;
                }
              }
            }
          }
          // host_swizzle == XE_GPU_TEXTURE_SWIZZLE_0000 (0) means the cache has NO
          // valid binding for this slot (rejected fetch constant / failed resolve)
          // => WriteActiveTextureBindfulSRV writes a NULL SRV => samples read zero.
          const uint32_t host_swiz = beta_texture_cache_->GetActiveTextureHostSwizzle(tb.fetch_constant);
          REXLOG_INFO("[nhl-beta]   VS-tex#{} fc={} base=0x{:X} {}x{} fmt={} swizzle=0x{:X} "
                      "tiled={} endian={} pitch={} stacked={} dim={} host_swizzle=0x{:X} "
                      "raw_fc=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] DRAW-TIME guest-RAM "
                      "nz={}/{} first_nz=0x{:X} last_nz=0x{:X}",
                      beta_takeover_rendered_, tb.fetch_constant, tex_base,
                      uint32_t(tf.size_2d.width) + 1, uint32_t(tf.size_2d.height) + 1,
                      uint32_t(tf.format), uint32_t(tf.swizzle), uint32_t(tf.tiled),
                      uint32_t(tf.endianness), uint32_t(tf.pitch), uint32_t(tf.stacked),
                      uint32_t(tf.dimension), host_swiz, regs[fc6], regs[fc6 + 1], regs[fc6 + 2],
                      regs[fc6 + 3], regs[fc6 + 4], regs[fc6 + 5], tex_nz, scan_bytes,
                      first_nz_off == UINT32_MAX ? 0 : first_nz_off, last_nz_off);
        }
      }
      // Dump the textured VS/PS ucode disassembly once so the fetch + transform
      // instructions can be inspected offline (why the verts may be degenerate).
      // NHL_BETA_UCODE_DRAW=<rendered idx> selects WHICH textured draw to dump
      // (default: the first one).
      static const char* ucode_draw_env = std::getenv("NHL_BETA_UCODE_DRAW");
      const bool ucode_this_draw =
          !ucode_draw_env ||
          beta_takeover_rendered_ == uint32_t(std::strtoul(ucode_draw_env, nullptr, 10));
      if (!beta_ucode_dumped_ && ucode_this_draw) {
        beta_ucode_dumped_ = true;
        FILE* f = std::fopen("beta_textured_ucode.txt", "w");
        if (f) {
          std::fprintf(f, "=== VS ucode (host_vs_type=%u) ===\n%s\n",
                       uint32_t(result.host_vertex_shader_type),
                       beta_current_vs_->ucode_disassembly().c_str());
          if (eff_ps) {
            std::fprintf(f, "\n=== PS ucode ===\n%s\n", eff_ps->ucode_disassembly().c_str());
          }
          std::fclose(f);
          REXLOG_INFO("[nhl-beta] wrote beta_textured_ucode.txt");
        }
        // Dump the translated DXBC blobs so the VS-output / PS-input signatures can be
        // compared offline (to find why interpolators don't reach the PS).
        auto dump = [](const char* path, const std::vector<uint8_t>& b) {
          if (b.empty()) return;
          FILE* o = std::fopen(path, "wb");
          if (o) { std::fwrite(b.data(), 1, b.size(), o); std::fclose(o); }
        };
        if (vs_tr && vs_tr->is_translated()) dump("beta_vs.dxbc", vs_tr->translated_binary());
        if (ps_tr && ps_tr->is_translated()) dump("beta_ps.dxbc", ps_tr->translated_binary());
        REXLOG_INFO("[nhl-beta] wrote beta_vs.dxbc / beta_ps.dxbc");
      }
      // VERTEX-FETCH probe (suspect a/d): the vte=0x43F textured draws are the first
      // true test of shared-memory vertex pulling. Read the fetch slot the VS actually
      // uses (vertex_bindings()[i].fetch_constant — NOT a guessed slot, the prior bug),
      // parse it with the real xe_gpu_vertex_fetch_t struct (address is in DWORDS), and
      // check whether guest RAM holds real vertex data there and whether the address is
      // inside our resident RequestRange. Zeros here ⇒ verts collapse to origin ⇒ no raster.
      uint32_t probe_shmem_mb = 512;
      if (const char* mb = std::getenv("NHL_BETA_SHMEM_MB")) {
        probe_shmem_mb = uint32_t(std::strtoul(mb, nullptr, 10));
      }
      const uint64_t probe_shmem_bytes = uint64_t(probe_shmem_mb) * 0x100000ull;
      const auto& vbs = beta_current_vs_->vertex_bindings();
      REXLOG_INFO("[nhl-beta]   vfetch #{}: vertex_bindings={}", beta_takeover_rendered_,
                  vbs.size());
      for (const auto& vb : vbs) {
        const uint32_t fc = vb.fetch_constant;  // [0-95], 2 dwords each from 0x4800
        xenos::xe_gpu_vertex_fetch_t f{};
        f.dword_0 = regs[0x4800 + fc * 2];
        f.dword_1 = regs[0x4800 + fc * 2 + 1];
        const uint32_t addr_bytes = f.address << 2;       // address is in dwords
        const uint32_t size_bytes = f.size << 2;          // size is in words(dwords)
        // Synchronous guest-RAM scan at the fetch address (data existence, not upload).
        uint32_t nz = 0, scan = std::min<uint32_t>(size_bytes ? size_bytes : 256u, 1024u);
        if (memory_) {
          if (uint8_t* p = memory_->TranslatePhysical<uint8_t*>(addr_bytes)) {
            for (uint32_t b = 0; b < scan; ++b) nz += (p[b] != 0);
          }
        }
        REXLOG_INFO("[nhl-beta]     binding fc={} type={} addr=0x{:X} size={}B stride_words={} "
                    "endian={} -> guestRAM nz={}/{} in_range={}",
                    fc, uint32_t(f.type), addr_bytes, size_bytes, vb.stride_words,
                    uint32_t(f.endian), nz, scan, addr_bytes < probe_shmem_bytes);
        // Decode the first few fetched POSITIONS exactly as the VS would: FMT_32_32_32
        // float at stride_words*4 bytes, big-endian guest data needs a k8in32 byte swap
        // (endian==2) to read as host float. If these are zeros/garbage the fetch source
        // is bad; if they're sane model coords the bug is shader-side (SRV bind / transform).
        const uint32_t stride_b = vb.stride_words * 4;
        if (memory_ && stride_b) {
          uint8_t* base = memory_->TranslatePhysical<uint8_t*>(addr_bytes);
          auto rdf = [](const uint8_t* b) {  // read big-endian (k8in32) float
            uint8_t s[4] = {b[3], b[2], b[1], b[0]};
            float r;
            std::memcpy(&r, s, 4);
            return r;
          };
          for (uint32_t v = 0; v < 2 && base; ++v) {
            const uint8_t* vb = base + size_t(v) * stride_b;
            // pos @0 (float3), color @24 (FMT_8_8_8_8 raw), attrib @32 (float4) — the
            // VS's o2 (interp 2) source. If attrib is the sky color the data is good.
            const uint8_t* cb = vb + 24;  // FMT_8_8_8_8 color bytes
            REXLOG_INFO("[nhl-beta]       vtx[{}] pos=({:.3f},{:.3f},{:.3f}) "
                        "color8888=[{:02X},{:02X},{:02X},{:02X}] "
                        "attrib@32=({:.3f},{:.3f},{:.3f},{:.3f})",
                        v, rdf(vb), rdf(vb + 4), rdf(vb + 8), cb[0], cb[1], cb[2], cb[3],
                        rdf(vb + 32), rdf(vb + 36), rdf(vb + 40), rdf(vb + 44));
          }
        }
      }
      // WVP probe (suspect b): the VS multiplies fetched positions by a float-constant
      // matrix. Dump the float4 constants the VS reads (tightly from 0x4000) so a
      // zero/garbage WVP is visible. A real transform shows 4 non-trivial float4 rows.
      // Vertex-index system constants drive the translator's SV_VertexID -> guest
      // index mapping (clamp to [min,max] + offset). If max is tiny / offset wrong,
      // all SV_VertexIDs collapse to one guest vertex -> all verts coincide -> no raster.
      REXLOG_INFO("[nhl-beta]   sys vidx: endian={} offset={} min={} max={} | host_draw_verts={}",
                  uint32_t(sys.vertex_index_endian), sys.vertex_index_offset, sys.vertex_index_min,
                  sys.vertex_index_max, result.host_draw_vertex_count);
      const auto& vs_crm = beta_current_vs_->constant_register_map();
      REXLOG_INFO("[nhl-beta]   vs float_count={} dyn={} bitmap=[0x{:X},0x{:X},0x{:X},0x{:X}] "
                  "(first non-zero float4s @0x4000):",
                  vs_crm.float_count, vs_crm.float_dynamic_addressing, vs_crm.float_bitmap[0],
                  vs_crm.float_bitmap[1], vs_crm.float_bitmap[2], vs_crm.float_bitmap[3]);
      for (uint32_t i = 0; i < 24; ++i) {
        const float* v = reinterpret_cast<const float*>(&regs[0x4000 + i * 4]);
        if (v[0] || v[1] || v[2] || v[3]) {
          REXLOG_INFO("[nhl-beta]     c[{}]=({:.3f},{:.3f},{:.3f},{:.3f})", i, v[0], v[1], v[2],
                      v[3]);
        }
      }
      // Skinning-texcoord constants (create-player VS): c44/c248/c255 scale the bone
      // indices into tf16 texel coords. Zero c248/c255 => every fetch lands on texel
      // (0,0) => zero matrix => collapsed verts. Also report whether the bitmap covers
      // them (a missing bit = the packed CBV omits them = translator reads other slots).
      for (uint32_t s : {44u, 248u, 255u}) {
        const float* v = reinterpret_cast<const float*>(&regs[0x4000 + s * 4]);
        const bool in_bitmap = (vs_crm.float_bitmap[s / 64] >> (s % 64)) & 1;
        REXLOG_INFO("[nhl-beta]     c[{}]=({:.6f},{:.6f},{:.6f},{:.6f}) in_bitmap={}", s, v[0],
                    v[1], v[2], v[3], in_bitmap);
      }
    }
  }
  if (idx_tex_ps != kUnavail) write_tex_table(ps_texs, tex_table_ps, /*is_vertex_stage=*/false);
  if (idx_samp_ps != kUnavail) {
    write_samp_table(eff_ps->GetSamplerBindingsAfterTranslation(), samp_table_ps);
  }
  if (idx_tex_vs != kUnavail) write_tex_table(vs_texs, tex_table_vs, /*is_vertex_stage=*/true);
  if (idx_samp_vs != kUnavail) write_samp_table(vs_samps, samp_table_vs);

  // NHL_BETA_VTX_DUMP=<draw_index>: ground-truth decode of this draw's vertex
  // attributes using the REAL per-attribute offsets+formats from the VS vertex
  // bindings (the generic pos@0 diag reads garbage for 2D composite quads), plus the
  // bound texture's clamp/wrap address mode. For the player composite (#430) this pins
  // whether the misplacement is the quad POSITIONS, the UVs, or a WRAP sampler.
  {
    static const char* vd_env = std::getenv("NHL_BETA_VTX_DUMP");
    if (vd_env && beta_takeover_rendered_ == uint32_t(std::strtoul(vd_env, nullptr, 10))) {
      namespace xe = rex::graphics::xenos;
      auto fbe = [](const uint8_t* b) {  // big-endian (k8in32) float
        uint8_t s[4] = {b[3], b[2], b[1], b[0]};
        float r;
        std::memcpy(&r, s, 4);
        return r;
      };
      const auto& vbs2 = beta_current_vs_->vertex_bindings();
      REXLOG_INFO("[nhl-beta] VTXDUMP #{}: {} vertex bindings, host_verts={} (vp x={} w={} | sc {},{},{},{})",
                  beta_takeover_rendered_, vbs2.size(), result.host_draw_vertex_count,
                  vp.TopLeftX, vp.Width, scissor.left, scissor.top, scissor.right, scissor.bottom);
      for (const auto& b : vbs2) {
        xe::xe_gpu_vertex_fetch_t vf{};
        vf.dword_0 = regs[0x4800 + b.fetch_constant * 2];
        vf.dword_1 = regs[0x4800 + b.fetch_constant * 2 + 1];
        const uint32_t addr = vf.address << 2;
        const uint8_t* vbase = memory_ ? memory_->TranslatePhysical<const uint8_t*>(addr) : nullptr;
        const uint32_t stride_b = b.stride_words * 4;
        REXLOG_INFO("[nhl-beta]   binding fc={} addr=0x{:X} stride={}B attrs={}", b.fetch_constant,
                    addr, stride_b, b.attributes.size());
        // half (k_16_16_FLOAT) decode: the guest stores k8in32 big-endian, so within a
        // 4-byte word the two halves are at [b3,b2] (h0) and [b1,b0] (h1).
        auto hbe = [](uint16_t h) {
          uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
          if (e == 0) bits = m ? s | ((127 - 15 + 1) << 23) | (m << 13) : s;  // (approx subnormal)
          else if (e == 31) bits = s | 0x7F800000u | (m << 13);
          else bits = s | ((e + 112) << 23) | (m << 13);
          float r; std::memcpy(&r, &bits, 4); return r;
        };
        for (size_t ai = 0; ai < b.attributes.size(); ++ai) {
          const auto& at = b.attributes[ai].fetch_instr.attributes;
          const uint32_t off_b = uint32_t(at.offset) * 4;
          const int nc = xe::GetVertexFormatComponentCount(at.data_format);
          const bool is_half = (at.data_format == xe::VertexFormat::k_16_16_FLOAT ||
                                at.data_format == xe::VertexFormat::k_16_16_16_16_FLOAT);
          for (uint32_t v = 0; v < 4 && vbase; ++v) {
            const uint8_t* p = vbase + size_t(v) * stride_b + off_b;
            char buf[200];
            int n = 0;
            const int words = (nc + 1) / 2 * (is_half ? 1 : 0) + (is_half ? 0 : nc);
            if (is_half) {
              for (int w = 0; w < (nc + 1) / 2; ++w) {
                const uint8_t* q = p + w * 4;
                uint16_t h0 = uint16_t((q[3] << 8) | q[2]), h1 = uint16_t((q[1] << 8) | q[0]);
                n += std::snprintf(buf + n, sizeof(buf) - size_t(n), "%.4f %.4f ", hbe(h0), hbe(h1));
              }
            } else {
              for (int c = 0; c < nc && c < 4; ++c)
                n += std::snprintf(buf + n, sizeof(buf) - size_t(n), "%.4f ", fbe(p + c * 4));
            }
            (void)words;
            // raw hex of the attribute span for manual decode of any format
            int hn = 0; char hx[64];
            for (int bb = 0; bb < nc * (is_half ? 2 : 4) && bb < 16; ++bb)
              hn += std::snprintf(hx + hn, sizeof(hx) - size_t(hn), "%02X", p[bb]);
            REXLOG_INFO("[nhl-beta]     attr{} fmt={} off_dw={} comps={} v{}: [{}] raw={}", ai,
                        uint32_t(at.data_format), at.offset, nc, v, buf, hx);
          }
        }
      }
      if (eff_ps) {
        for (const auto& tb : eff_ps->texture_bindings()) {
          xe::xe_gpu_texture_fetch_t tf{};
          std::memcpy(&tf, &regs[0x4800 + tb.fetch_constant * 6], 24);
          REXLOG_INFO("[nhl-beta]   tex fc={} base=0x{:X} {}x{} clamp_x={} clamp_y={} "
                      "(ClampMode 0=repeat/WRAP,2=clamp_to_edge,6=clamp_to_border)",
                      tb.fetch_constant, uint32_t(tf.base_address) << 12,
                      uint32_t(tf.size_2d.width) + 1, uint32_t(tf.size_2d.height) + 1,
                      uint32_t(tf.clamp_x), uint32_t(tf.clamp_y));
        }
      }
    }
  }

  // Record the draw into the open (ours, in takeover) command list. The view +
  // sampler heaps were already (re)bound by RequestView/SamplerBindfulDescriptors
  // above through the CP's own tracking, so we must NOT call SetDescriptorHeaps with
  // a private heap here (that desyncs the CP's view_bindful_heap_current_ and breaks
  // the untile — the original id=554/id=708 cause).
  d3d12::DeferredCommandList& dcl = GetDeferredCommandList();
  // Bind our sampler heap alongside the (unchanged) global view heap. Passing the same
  // global view heap keeps view_bindful_heap_current_ consistent so the next draw's
  // untile is unaffected; only the sampler heap (which the untile never uses) changes.
  if (samp_table_ps.ptr && global_view_heap && beta_sampler_heap_) {
    dcl.SetDescriptorHeaps(global_view_heap, beta_sampler_heap_.Get());
  }
  // EDRAM mode: the RT cache's Update() already bound the EDRAM-backed color+depth host
  // render targets (and the guest stream owns their clears via resolve-clears), so we do
  // NOT bind or clear our offscreen RTV/DSV here. The offscreen-RTV path (menu/intro/depth)
  // keeps its own bind+clear exactly as before.
  if (!edram) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = beta_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const bool depth_bound = beta_depth_enabled_ && beta_depth_ready_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        depth_bound ? beta_dsv_heap_->GetCPUDescriptorHandleForHeapStart()
                    : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (first_draw) {
      // Default clear is black; NHL_BETA_CLEAR_DIAG uses a distinctive purple so the
      // coverage of (otherwise black) untextured draws is visible for diagnosis.
      const float clear_black[4] = {0, 0, 0, 1};
      const float clear_diag[4] = {0.25f, 0.10f, 0.40f, 1};
      dcl.D3DClearRenderTargetView(
          rtv, std::getenv("NHL_BETA_CLEAR_DIAG") ? clear_diag : clear_black, 0, nullptr);
      beta_takeover_cleared_ = true;
    }
    if (depth_bound && !beta_depth_cleared_) {
      // Clear the depth target once to the far value (1.0 standard; NHL_BETA_DEPTH_CLEAR=0
      // for reversed-Z). The guest's own EDRAM depth clear isn't replayed into our target,
      // so we own this. Stencil cleared to 0.
      dcl.D3DClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                   beta_depth_clear_, 0, 0, nullptr);
      beta_depth_cleared_ = true;
    }
    dcl.D3DOMSetRenderTargets(1, &rtv, FALSE, depth_bound ? &dsv : nullptr);
  }
  dcl.RSSetViewport(vp);
  dcl.RSSetScissorRect(scissor);
  dcl.D3DSetGraphicsRootSignature(root_sig);
  dcl.SetPipelineStateHandle(pso_handle);
  // Keep the CP's pipeline/root-sig tracking consistent with what we just recorded.
  // We set the graphics PSO + root sig DIRECTLY on the deferred list, which does NOT
  // update the CP's current_external_pipeline_ / current_graphics_root_signature_.
  // The texture cache's untile compute keys its "already bound?" redundancy check on
  // those: if we leave them stale (== a prior untile's compute load pipeline) the
  // NEXT draw's untile SKIPS re-binding its compute pipeline and Dispatches with OUR
  // graphics PSO still current -> debug layer id=951 "Dispatch with a graphics
  // Pipeline State will do nothing" + id=953 root-sig mismatch -> the texture is
  // never untiled -> samples empty/black (the clouds parity gap). Telling the CP the
  // current pipeline/root-sig is now ours forces the next untile to re-bind compute.
  if (ID3D12PipelineState* pso = GetD3D12PipelineByHandle(pso_handle)) {
    SetExternalPipeline(pso);
  }
  SetExternalGraphicsRootSignature(root_sig);
  dcl.D3DIASetPrimitiveTopology(HostPrimToD3DTopology(uint32_t(result.host_primitive_type)));
  dcl.D3DSetGraphicsRootConstantBufferView(0, va_fetch);      // FetchConstants
  dcl.D3DSetGraphicsRootConstantBufferView(1, va_floats_vs);  // FloatConstantsVertex
  dcl.D3DSetGraphicsRootConstantBufferView(2, va_floats_ps);  // FloatConstantsPixel
  dcl.D3DSetGraphicsRootConstantBufferView(3, va_sys);     // SystemConstants
  dcl.D3DSetGraphicsRootConstantBufferView(4, va_bool);    // BoolLoopConstants
  if (beta_shmem) {
    dcl.D3DSetGraphicsRootDescriptorTable(5, shmem_table);  // SharedMemoryAndEdram
  }
  // Texture + sampler descriptor tables at the extra root-parameter indices.
  if (tex_table_ps.ptr) dcl.D3DSetGraphicsRootDescriptorTable(idx_tex_ps, tex_table_ps);
  if (samp_table_ps.ptr) dcl.D3DSetGraphicsRootDescriptorTable(idx_samp_ps, samp_table_ps);
  if (tex_table_vs.ptr) dcl.D3DSetGraphicsRootDescriptorTable(idx_tex_vs, tex_table_vs);
  if (samp_table_vs.ptr) dcl.D3DSetGraphicsRootDescriptorTable(idx_samp_vs, samp_table_vs);
  // NHL_BETA_TEXONLY: skip the geometry of UNtextured draws (still clears + binds) so
  // only textured draws paint — isolates whether textured draws produce visible output
  // vs. being overdrawn by later untextured scaffolding.
  const bool skip_untex = std::getenv("NHL_BETA_TEXONLY") && ps_tex_n == 0;
  // NHL_BETA_SKIP_DRAW / NHL_BETA_ONLY_DRAW: per-draw isolation (comma-separated draw
  // indices = beta_takeover_rendered_). SKIP removes those draws; ONLY renders only those
  // (other TEXTURED draws are skipped, but untextured clears/scaffolding still run so the
  // RT is initialised the same way). Used to identify which draw paints the background.
  auto idx_in_env = [&](const char* name) -> bool {
    const char* s = std::getenv(name);
    if (!s) return false;
    const uint32_t cur = beta_takeover_rendered_;
    while (*s) {
      char* end = nullptr;
      unsigned long v = std::strtoul(s, &end, 10);
      if (end == s) { ++s; continue; }
      if (uint32_t(v) == cur) return true;
      s = (*end) ? end + 1 : end;
    }
    return false;
  };
  bool skip_this = idx_in_env("NHL_BETA_SKIP_DRAW");
  if (std::getenv("NHL_BETA_ONLY_DRAW") && ps_tex_n != 0 && !idx_in_env("NHL_BETA_ONLY_DRAW")) {
    skip_this = true;
  }
  // NHL_BETA_SKIP_RANGE=lo-hi (diag): skip the GEOMETRY of every owned draw whose index is in
  // [lo,hi] (clears/binds still run, matching SKIP_DRAW semantics). Lets a whole pass be removed in
  // one knob for green-band pass bisection without listing hundreds of indices.
  if (const char* rg = std::getenv("NHL_BETA_SKIP_RANGE")) {
    char* dash = nullptr;
    const unsigned long lo = std::strtoul(rg, &dash, 10);
    const unsigned long hi = (dash && *dash) ? std::strtoul(dash + 1, nullptr, 10) : lo;
    const uint32_t cur = beta_takeover_rendered_;
    if (cur >= uint32_t(lo) && cur <= uint32_t(hi)) skip_this = true;
  }
  // NHL_BETA_ZEN_FILTER: render only depth-tested ("1") or only non-depth-tested ("0")
  // draws, to separate the 3D arena geometry from the big z_enable=0 overlay planes.
  if (const char* ze = std::getenv("NHL_BETA_ZEN_FILTER")) {
    const bool want_zen = ze[0] == '1';
    if (want_zen != (ndc.z_enable != 0)) skip_this = true;
  }
  if (first_draw && std::getenv("NHL_BETA_LIVE_TRACE")) REXLOG_INFO("[nhl-beta] LIVE-TRACE: pre-D3DDraw (vtx={})", result.host_draw_vertex_count);
  if (!std::getenv("NHL_BETA_NODRAW") && !skip_untex && !skip_this) {
    using PP = rex::graphics::PrimitiveProcessor;
    if (result.index_buffer_type == PP::kNone) {
      // Auto-indexed on the host: non-indexed draw. This covers TriangleList,
      // RectangleList (GS-expanded from a triangle), and QuadList. For QuadList the
      // IA topology was set to LINELIST_WITH_ADJ above so the quad geometry shader
      // (PipelineGeometryShader::kQuadList) receives each group of 4 verts as one
      // `lineadj` primitive and emits the two triangles itself — no host-side
      // index expansion (which desyncs the GS and renders nothing).
      dcl.D3DDrawInstanced(result.host_draw_vertex_count, 1, 0, 0);
    } else {
      // INDEXED draw: the primitive processor produced (or points at) an index
      // buffer that maps host vertices. Drawing these non-indexed pulls the wrong
      // vertices → degenerate/off-screen geometry → NOTHING rasterizes (the actual
      // cause of the black textured menu draws, which are quad→triangle expansions).
      D3D12_INDEX_BUFFER_VIEW ibv{};
      const bool i32 = result.host_index_format == xenos::IndexFormat::kInt32;
      ibv.Format = i32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
      const uint32_t istride = i32 ? 4u : 2u;
      bool have_ib = true;
      switch (result.index_buffer_type) {
        case PP::kGuestDMA:
          ibv.BufferLocation = beta_shared_memory_->GetGPUAddress() + result.guest_index_base;
          break;
        case PP::kHostConverted:
          ibv.BufferLocation = beta_primitive_processor_->GetConvertedIndexBufferGpuAddress(
              result.host_index_buffer_handle);
          break;
        case PP::kHostBuiltinForAuto:
        case PP::kHostBuiltinForDMA:
          ibv.BufferLocation = beta_primitive_processor_->GetBuiltinIndexBufferGpuAddress(
              result.host_index_buffer_handle);
          break;
        default:
          have_ib = false;
          break;
      }
      ibv.SizeInBytes = result.host_draw_vertex_count * istride;
      if (have_ib && ibv.BufferLocation) {
        dcl.D3DIASetIndexBuffer(&ibv);
        dcl.D3DDrawIndexedInstanced(result.host_draw_vertex_count, 1, 0, 0, 0);
      } else {
        dcl.D3DDrawInstanced(result.host_draw_vertex_count, 1, 0, 0);
      }
    }
  }
  ++beta_takeover_rendered_;
}

void NhlD3D12CommandProcessor::FinalizeBetaTakeoverCapture() {
  // Records the once-per-frame copy of our accumulated offscreen RT to the readback
  // buffer, into the still-open submission (executed by the base's EndSubmission at
  // IssueSwap). Runs once, before the first capture is dumped.
  if (!beta_offscreen_rt_ || beta_capture_pending_ || beta_capture_done_) {
    return;
  }
  REXLOG_INFO("[nhl-beta] takeover finalize: rendered={} skipped_textured={}",
              beta_takeover_rendered_, beta_takeover_skipped_textured_);
  if (beta_takeover_rendered_ == 0) {
    return;  // nothing drawn (all textured/skipped) — leave RT untouched
  }
  // Mid-frame tile-mapping may have closed the swap submission; reopen before the copy.
  if (!BetaEnsureSubmissionOpen(this)) {
    return;
  }
  d3d12::DeferredCommandList& dcl = GetDeferredCommandList();
  // Unbind the render target before reading it back.
  dcl.D3DOMSetRenderTargets(0, nullptr, FALSE, nullptr);
  if (beta_msaa_ > 1) {
    // MSAA: a multisample RT can't be copied straight to a readback buffer, and the
    // DeferredCommandList has no ResolveSubresource. Leave the RT in RENDER_TARGET and
    // do the resolve+copy on a one-shot list post-frame (ResolveBetaMsaaToReadback,
    // called from IssueSwap after the GPU is idle). Just mark the capture pending.
    beta_capture_pending_ = true;
    beta_capture_submission_ = GetCurrentSubmission();
    return;
  }
  PushTransitionBarrier(beta_offscreen_rt_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
  SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = beta_readback_.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = beta_rt_footprint_;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = beta_offscreen_rt_.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  dcl.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  PushTransitionBarrier(beta_offscreen_rt_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
  SubmitBarriers();
  // GPU-side ground truth for the offscreen-RT path too (was EDRAM-only): copy the
  // NHL_BETA_GPUDUMP guest addresses out of OUR shared-memory buffer at end of frame.
  RecordBetaGpuDumps();
  beta_capture_pending_ = true;
  beta_capture_submission_ = GetCurrentSubmission();
}

bool NhlD3D12CommandProcessor::EnsureBetaPresentPipeline() {
  if (beta_present_pso_) {
    return true;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (!device) {
    return false;
  }
  // Compute copy: read our RGBA8 RT (normalized float4), write the presenter's UAV output
  // (whatever its format is — R10G10B10A2, RGBA8, ... — the typed-UAV store converts).
  static const char kCS[] =
      "Texture2D<float4> src : register(t0);\n"
      "RWTexture2D<float4> dst : register(u0);\n"
      "[numthreads(8,8,1)]\n"
      "void main(uint3 id : SV_DispatchThreadID){\n"
      "  uint w,h; dst.GetDimensions(w,h);\n"
      "  if (id.x<w && id.y<h) dst[id.xy] = src[int2(id.xy)];\n"
      "}\n";
  Microsoft::WRL::ComPtr<ID3DBlob> cs, err;
  if (FAILED(D3DCompile(kCS, sizeof(kCS) - 1, "beta_present_cs", nullptr, nullptr, "main", "cs_5_0",
                        0, 0, &cs, &err))) {
    REXLOG_ERROR("[nhl-beta] present CS compile failed: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].OffsetInDescriptorsFromTableStart = 1;
  D3D12_ROOT_PARAMETER rp{};
  rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rp.DescriptorTable.NumDescriptorRanges = 2;
  rp.DescriptorTable.pDescriptorRanges = ranges;
  rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 1;
  rsd.pParameters = &rp;
  Microsoft::WRL::ComPtr<ID3DBlob> rsb, rse;
  if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse)) ||
      FAILED(device->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(),
                                         IID_PPV_ARGS(&beta_present_rootsig_)))) {
    REXLOG_ERROR("[nhl-beta] present root signature failed");
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = beta_present_rootsig_.Get();
  pd.CS.pShaderBytecode = cs->GetBufferPointer();
  pd.CS.BytecodeLength = cs->GetBufferSize();
  if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&beta_present_pso_)))) {
    REXLOG_ERROR("[nhl-beta] present PSO failed");
    beta_present_rootsig_.Reset();
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 2;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&beta_present_heap_)))) {
    REXLOG_ERROR("[nhl-beta] present descriptor heap failed");
    return false;
  }
  return true;
}

void NhlD3D12CommandProcessor::PresentBetaLiveFrame() {
  // Live present (NHL_BETA_LIVE): show our offscreen RT in the window every frame via the SDK
  // presenter. RefreshGuestOutput hands the refresher the presenter's UAV output texture; a
  // compute shader copies our RGBA8 RT into it (CopyResource can't convert RGBA8 -> the
  // presenter's R10G10B10A2). One-shot DIRECT-queue list, fence-synced. Runs AFTER base
  // IssueSwap's EndSubmission, so our beta draws have executed and the RT is rendered.
  if (!graphics_system_ || !beta_offscreen_rt_ || !EnsureBetaPresentPipeline()) {
    return;
  }
  auto* presenter = graphics_system_->presenter();
  if (!presenter) {
    return;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  ID3D12CommandQueue* queue = GetD3D12Provider().GetDirectQueue();
  if (!device || !queue) {
    return;
  }
  ID3D12Resource* src = beta_offscreen_rt_.Get();
  const uint32_t w = beta_rt_width_ ? beta_rt_width_ : 1280u;
  const uint32_t h = beta_rt_height_ ? beta_rt_height_ : 720u;
  presenter->RefreshGuestOutput(
      w, h, w, h,
      [this, device, queue, src, w, h](rex::ui::Presenter::GuestOutputRefreshContext& ctx) -> bool {
        ctx.SetIs8bpc(true);  // our RT is 8-bit-per-channel RGBA
        ID3D12Resource* out =
            static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(ctx)
                .resource_uav_capable();
        if (!out) {
          return false;
        }
        const UINT inc =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0 =
            beta_present_heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
        cpu1.ptr += inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(src, &srv, cpu0);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = out->GetDesc().Format;  // the presenter's actual output format
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(out, nullptr, &uav, cpu1);

        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&alloc))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                            nullptr, IID_PPV_ARGS(&list)))) {
          return false;
        }
        // src: RENDER_TARGET (last draw left it bound) -> SRV; out: COMMON -> UAV.
        D3D12_RESOURCE_BARRIER b[2] = {};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource = src;
        b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b[1] = b[0];
        b[1].Transition.pResource = out;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(2, b);
        ID3D12DescriptorHeap* heaps[] = {beta_present_heap_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(beta_present_rootsig_.Get());
        list->SetPipelineState(beta_present_pso_.Get());
        list->SetComputeRootDescriptorTable(
            0, beta_present_heap_->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);  // src -> RENDER_TARGET
        std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);  // out -> COMMON
        list->ResourceBarrier(2, b);
        if (FAILED(list->Close())) {
          return false;
        }
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
          queue->Signal(fence.Get(), 1);
          for (int i = 0; i < 4000 && fence->GetCompletedValue() < 1; ++i) Sleep(1);
        }
        return true;
      });
}

void NhlD3D12CommandProcessor::CaptureBetaEdramSwap() {
  // EDRAM final-frame capture. The visible image is the frontbuffer GUEST texture that the
  // last guest resolve wrote into beta_shared_memory_. RequestSwapTexture (the same call the
  // base present uses) reads the swap fetch-constant from the register file, untiles that
  // guest texture into a host texture, and returns it in NON_PIXEL_SHADER_RESOURCE state.
  // We copy it into a readback buffer here, in the still-open submission, and write the PNG
  // after it retires (DumpBetaEdramSwap). Runs once on the capture frame.
  if (beta_capture_pending_ || beta_capture_done_) {
    return;
  }
  if (beta_takeover_rendered_ == 0) {
    return;  // nothing drawn this frame (deferred to the capture frame)
  }
  if (!BetaEnsureSubmissionOpen(this)) {
    return;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
  xenos::TextureFormat guest_fmt{};
  uint32_t w = 0, h = 0;
  ID3D12Resource* swap = beta_texture_cache_->RequestSwapTexture(srv_desc, guest_fmt, &w, &h);
  if (!swap || !w || !h) {
    REXLOG_ERROR("[nhl-beta] edram-swap: RequestSwapTexture failed (swap={} {}x{})",
                 static_cast<void*>(swap), w, h);
    return;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const D3D12_RESOURCE_DESC sd = swap->GetDesc();
  beta_swap_fmt_ = sd.Format;
  beta_swap_w_ = w;
  beta_swap_h_ = h;
  // Decode the SRV swizzle the base would apply when presenting: if output R is fed by
  // source B (and vice-versa) the raw resource bytes are BGRA and we must swap on readback.
  {
    const UINT m = srv_desc.Shader4ComponentMapping;
    const UINT r_src = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(0, m);
    const UINT b_src = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(2, m);
    beta_swap_rb_ = (r_src == D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2 &&
                     b_src == D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
  }
  UINT num_rows = 0;
  UINT64 row_size = 0;
  device->GetCopyableFootprints(&sd, 0, 1, 0, &beta_swap_footprint_, &num_rows, &row_size,
                                &beta_swap_total_bytes_);
  if (!beta_swap_readback_) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = beta_swap_total_bytes_;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&beta_swap_readback_)))) {
      REXLOG_ERROR("[nhl-beta] edram-swap: readback CreateCommittedResource failed");
      return;
    }
  }
  // The swap texture is in NON_PIXEL_SHADER_RESOURCE; transition to COPY_SOURCE, copy, back.
  d3d12::DeferredCommandList& dcl = GetDeferredCommandList();
  PushTransitionBarrier(swap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
  SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = beta_swap_readback_.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = beta_swap_footprint_;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = swap;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  dcl.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  PushTransitionBarrier(swap, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  SubmitBarriers();
  // GPU-side ground truth: also copy the requested guest addresses straight out of OUR
  // beta shared-memory buffer (what beta's pipeline produced) into readback buffers,
  // recorded into THIS submission so they retire together with the frontbuffer copy.
  RecordBetaGpuDumps();
  beta_capture_pending_ = true;
  beta_capture_submission_ = GetCurrentSubmission();
  REXLOG_INFO("[nhl-beta] edram-swap: recorded frontbuffer copy {}x{} (dxgi=0x{:X} guest_fmt={} "
              "row_pitch={}) into submission {}",
              w, h, uint32_t(beta_swap_fmt_), uint32_t(guest_fmt),
              beta_swap_footprint_.Footprint.RowPitch, beta_capture_submission_);
}

void NhlD3D12CommandProcessor::DumpBetaEdramSwap(const char* path) {
  if (!beta_swap_readback_ || !beta_swap_w_ || !beta_swap_h_) {
    REXLOG_ERROR("[nhl-beta] edram-swap: nothing to dump");
    return;
  }
  void* mapped = nullptr;
  D3D12_RANGE rr{0, static_cast<SIZE_T>(beta_swap_total_bytes_)};
  if (FAILED(beta_swap_readback_->Map(0, &rr, &mapped)) || !mapped) {
    REXLOG_ERROR("[nhl-beta] edram-swap: readback Map failed");
    return;
  }
  const uint32_t w = beta_swap_w_, h = beta_swap_h_;
  const uint32_t row_pitch = beta_swap_footprint_.Footprint.RowPitch;
  const uint8_t* base = static_cast<const uint8_t*>(mapped);
  // The host swap texture is 8888; some present formats are BGRA. Emit RGBA with opaque
  // alpha (the present image is the final composite — no alpha-recomposite needed).
  const bool bgra = beta_swap_rb_ ||
                    (beta_swap_fmt_ == DXGI_FORMAT_B8G8R8A8_UNORM ||
                     beta_swap_fmt_ == DXGI_FORMAT_B8G8R8X8_UNORM ||
                     beta_swap_fmt_ == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
  std::vector<uint8_t> tight(static_cast<size_t>(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* s = base + static_cast<size_t>(y) * row_pitch;
    uint8_t* d = tight.data() + static_cast<size_t>(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* p = s + x * 4;
      if (bgra) {
        d[x * 4 + 0] = p[2];
        d[x * 4 + 1] = p[1];
        d[x * 4 + 2] = p[0];
      } else {
        d[x * 4 + 0] = p[0];
        d[x * 4 + 1] = p[1];
        d[x * 4 + 2] = p[2];
      }
      d[x * 4 + 3] = 255;
    }
  }
  D3D12_RANGE wr{0, 0};
  beta_swap_readback_->Unmap(0, &wr);
  nhl::replay::WritePng(path, w, h, tight.data());
  REXLOG_INFO("[nhl-beta] edram-swap: wrote {} ({}x{} bgra={}) first px=({},{},{})", path, w, h,
              bgra, tight[0], tight[1], tight[2]);
}

void NhlD3D12CommandProcessor::RecordBetaGpuDumps() {
  const char* gd = std::getenv("NHL_BETA_GPUDUMP");
  if (!gd || !beta_shared_memory_) {
    return;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  ID3D12Resource* shmem = beta_shared_memory_->GetBuffer();
  if (!device || !shmem) {
    return;
  }
  // A previous batch is still pending consumption by WriteBetaGpuDumps (whose GPU copy
  // may be in flight). Don't clobber it — clearing the vector here would drop those
  // ComPtrs mid-copy (audit M3). Skip this record; the next pass after the write runs.
  if (!beta_gpudump_bufs_.empty()) {
    REXLOG_INFO("[nhl-beta] gpudump: {} buffers from a prior batch not yet written; "
                "skipping this record",
                beta_gpudump_bufs_.size());
    return;
  }
  // Flip OUR shared-memory buffer to a copy source (it was last UseForReading), then copy
  // each requested guest address out of it into a readback buffer, recorded into the same
  // submission as the frontbuffer copy (matches the proven shmem-probe pattern at IssueDraw).
  beta_shared_memory_->UseAsCopySource();
  d3d12::DeferredCommandList& dcl = GetDeferredCommandList();
  for (const char* s = gd; *s;) {
    char* end = nullptr;
    const uint32_t addr = uint32_t(std::strtoul(s, &end, 16));
    if (end == s) { ++s; continue; }
    s = (*end) ? end + 1 : end;
    Microsoft::WRL::ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kBetaGpuDumpBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&rb)))) {
      continue;
    }
    dcl.D3DCopyBufferRegion(rb.Get(), 0, shmem, addr, kBetaGpuDumpBytes);
    beta_gpudump_bufs_.emplace_back(addr, rb);
    REXLOG_INFO("[nhl-beta] gpudump: recorded copy of 0x{:X} ({} bytes) from beta shared memory",
                addr, kBetaGpuDumpBytes);
  }
  // Return the buffer to the read state it was in on entry (it was last UseForReading
  // before we flipped it to a copy source above). Mirrors the round-trip in
  // RenderBetaOwnedDraw; without this the buffer stays parked in COPY_SOURCE and a
  // later read records a wrong/absent transition barrier.
  beta_shared_memory_->UseForReading();
}

void NhlD3D12CommandProcessor::WriteBetaGpuDumps() {
  REXLOG_INFO("[nhl-beta] gpudump: WriteBetaGpuDumps entry, {} buffers", beta_gpudump_bufs_.size());
  if (beta_gpudump_bufs_.empty()) {
    return;
  }
  // beta resolves write the 32bpp tiled layout; untile by default (NHL_BETA_GPUDUMP_LINEAR
  // for the linear interpretation). This reads OUR GPU buffer — the actual beta output.
  const bool linear = std::getenv("NHL_BETA_GPUDUMP_LINEAR") != nullptr;
  const bool dump_alpha = std::getenv("NHL_BETA_GPUDUMP_ALPHA") != nullptr;
  uint32_t W = 1280, H = 720;  // override for non-1280x720 surfaces (e.g. 640x360 RTTs)
  if (const char* wv = std::getenv("NHL_BETA_GPUDUMP_W")) W = uint32_t(std::strtoul(wv, nullptr, 0));
  if (const char* hv = std::getenv("NHL_BETA_GPUDUMP_H")) H = uint32_t(std::strtoul(hv, nullptr, 0));
  for (auto& [addr, rb] : beta_gpudump_bufs_) {
    void* mapped = nullptr;
    D3D12_RANGE rr{0, kBetaGpuDumpBytes};
    const HRESULT hr = rb ? rb->Map(0, &rr, &mapped) : E_POINTER;
    if (FAILED(hr) || !mapped) {
      REXLOG_ERROR("[nhl-beta] gpudump: Map failed for 0x{:X} hr=0x{:08X}", addr, uint32_t(hr));
      continue;
    }
    const uint8_t* g = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> rgba(size_t(W) * H * 4);
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        size_t srcb;
        if (linear) {
          srcb = (size_t(y) * W + x) * 4;
        } else {
          // EXACT Xenos 2D untile via the SDK's own addresser (the same function the
          // texture cache uses), so the dumped resolve dest is pixel-faithful — the old
          // approximate macro-tile formula (audit B4) interleaved 32-px column bands from
          // the wrong macro-tile for general pitches and scrambled the image. 8888 = 4
          // bytes/block => bytes_per_block_log2 = 2; the resolve dest is tiled with pitch
          // = copy_dest_pitch (== the dump width W here). GetTiledOffset2D returns the
          // byte offset of the block within the tiled surface.
          srcb = size_t(uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
              int32_t(x), int32_t(y), W, /*bytes_per_block_log2=*/2)));
        }
        const uint8_t* p = g + srcb;
        uint8_t* d = rgba.data() + (size_t(y) * W + x) * 4;
        if (dump_alpha) {
          d[0] = d[1] = d[2] = p[3]; d[3] = 255;  // visualize the alpha channel as luma
        } else {
          // ASSUMES the source is BGRA8 (audit B12): pure channel reorder, no format/
          // endian awareness — an RGBA or 2_10_10_10 surface dumps wrong. PNG is
          // "BGRA-assumed".
          d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = 255;  // BGRA->RGBA (assumed)
        }
      }
    }
    D3D12_RANGE wr{0, 0};
    rb->Unmap(0, &wr);
    char path[64];
    std::snprintf(path, sizeof(path), "gpudump_%08X.png", addr);
    nhl::replay::WritePng(path, W, H, rgba.data());
    REXLOG_INFO("[nhl-beta] gpudump: wrote {} (linear={}) px0=({},{},{})", path, linear, rgba[0],
                rgba[1], rgba[2]);
  }
  beta_gpudump_bufs_.clear();
}

void NhlD3D12CommandProcessor::ReadbackBetaEdramRegion() {
  // Direct EDRAM observability: copy beta's LIVE edram_buffer_ (the SDK's 10 MiB EDRAM the
  // draws render INTO) to the CPU and untile requested regions. The resolve only exposes the
  // guest's resolve dest; this shows the raw EDRAM tiles (color base 0, depth base 736, ...)
  // the green-fold composite reads/writes. Self-contained compute copy on a post-frame
  // GPU-idle direct list (the SDK's edram_buffer_ is private — reached via the cache's own
  // WriteEdramUintPow2SRVDescriptor, never a resource handle).
  const char* spec_env = std::getenv("NHL_BETA_EDRAMDUMP");
  if (!spec_env || !beta_render_target_cache_) {
    return;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  ID3D12CommandQueue* queue = GetD3D12Provider().GetDirectQueue();
  if (!device || !queue) {
    return;
  }
  const uint32_t kEdramBytes = uint32_t(xenos::kEdramSizeBytes);
  const uint32_t kEdramDwords = kEdramBytes / 4u;

  if (!beta_edramdump_pso_) {
    // Compute: dst.Store(i*4) = src[i], EDRAM raw dwords (typed R32_UINT SRV) -> raw UAV.
    static const char kCS[] =
        "Buffer<uint> src : register(t0);\n"
        "RWByteAddressBuffer dst : register(u0);\n"
        "[numthreads(64,1,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) { dst.Store(id.x << 2, src[id.x]); }\n";
    Microsoft::WRL::ComPtr<ID3DBlob> cs, err;
    if (FAILED(D3DCompile(kCS, sizeof(kCS) - 1, "edramdump_cs", nullptr, nullptr, "main", "cs_5_0",
                          0, 0, &cs, &err))) {
      REXLOG_ERROR("[nhl-beta] edramdump: CS compile failed: {}",
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
      return;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable.NumDescriptorRanges = 2;
    rp.DescriptorTable.pDescriptorRanges = ranges;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1;
    rsd.pParameters = &rp;
    Microsoft::WRL::ComPtr<ID3DBlob> rs_blob, rs_err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                           IID_PPV_ARGS(&beta_edramdump_rootsig_)))) {
      REXLOG_ERROR("[nhl-beta] edramdump: root signature creation failed");
      return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = beta_edramdump_rootsig_.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&beta_edramdump_pso_)))) {
      REXLOG_ERROR("[nhl-beta] edramdump: compute PSO creation failed");
      beta_edramdump_rootsig_.Reset();
      return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&beta_edramdump_heap_)))) {
      REXLOG_ERROR("[nhl-beta] edramdump: descriptor heap creation failed");
      return;
    }
    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kEdramBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&beta_edramdump_dst_)))) {
      REXLOG_ERROR("[nhl-beta] edramdump: dst buffer creation failed");
      return;
    }
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&beta_edramdump_readback_)))) {
      REXLOG_ERROR("[nhl-beta] edramdump: readback buffer creation failed");
      return;
    }
  }

  // (Re)write descriptors: slot0 = EDRAM raw SRV (4-byte typed), slot1 = dst raw UAV.
  const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = beta_edramdump_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
  cpu1.ptr += inc;
  beta_render_target_cache_->WriteEdramUintPow2SRVDescriptor(cpu0, /*element_size_bytes_pow2=*/2);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = kEdramDwords;
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(beta_edramdump_dst_.Get(), nullptr, &uav, cpu1);

  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
      FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                       IID_PPV_ARGS(&list)))) {
    return;
  }
  ID3D12DescriptorHeap* heaps[] = {beta_edramdump_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(beta_edramdump_rootsig_.Get());
  list->SetPipelineState(beta_edramdump_pso_.Get());
  list->SetComputeRootDescriptorTable(0, beta_edramdump_heap_->GetGPUDescriptorHandleForHeapStart());
  list->Dispatch((kEdramDwords + 63u) / 64u, 1, 1);
  D3D12_RESOURCE_BARRIER bar{};
  bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  bar.Transition.pResource = beta_edramdump_dst_.Get();
  bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  bar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &bar);
  list->CopyBufferRegion(beta_edramdump_readback_.Get(), 0, beta_edramdump_dst_.Get(), 0,
                         kEdramBytes);
  std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
  list->ResourceBarrier(1, &bar);  // back to UAV for any later reuse
  if (FAILED(list->Close())) {
    return;
  }
  ID3D12CommandList* lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
    queue->Signal(fence.Get(), 1);
    for (int i = 0; i < 8000 && fence->GetCompletedValue() < 1; ++i) Sleep(1);
  }

  void* mapped = nullptr;
  D3D12_RANGE rr{0, kEdramBytes};
  if (FAILED(beta_edramdump_readback_->Map(0, &rr, &mapped)) || !mapped) {
    REXLOG_ERROR("[nhl-beta] edramdump: readback map failed");
    return;
  }
  const uint32_t* edram = static_cast<const uint32_t*>(mapped);
  const bool dump_alpha = std::getenv("NHL_BETA_EDRAMDUMP_ALPHA") != nullptr;
  auto dump_region = [&](uint32_t base, uint32_t pitch_tiles, uint32_t W, uint32_t H) {
    if (!pitch_tiles) pitch_tiles = 8;
    if (!W) W = pitch_tiles * 80;
    if (!H) H = 720;
    std::vector<uint8_t> rgba(size_t(W) * H * 4);
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        // EDRAM 80x16 32bpp tiles, row-major at pitch_tiles; tile addressing wraps mod 2048.
        const uint32_t tile = base + (y / 16) * pitch_tiles + (x / 80);
        const uint32_t within = (y % 16) * 80 + (x % 80);
        const uint32_t dword = (tile % xenos::kEdramTileCount) * 1280u + within;
        const uint32_t v = (dword < kEdramDwords) ? edram[dword] : 0u;
        uint8_t* d = rgba.data() + (size_t(y) * W + x) * 4;
        if (dump_alpha) {
          const uint8_t a = uint8_t((v >> 24) & 0xFF);
          d[0] = d[1] = d[2] = a;
          d[3] = 255;
        } else {
          // BGRA-in-memory assumption (matches GPUDUMP); flip with _RGBA if a surface dumps swapped.
          d[0] = uint8_t((v >> 16) & 0xFF);
          d[1] = uint8_t((v >> 8) & 0xFF);
          d[2] = uint8_t(v & 0xFF);
          d[3] = 255;
        }
      }
    }
    char path[64];
    std::snprintf(path, sizeof(path), "edramdump_b%u.png", base);
    nhl::replay::WritePng(path, W, H, rgba.data());
    REXLOG_INFO("[nhl-beta] edramdump: wrote {} (base_tile={} pitch={}t {}x{})", path, base,
                pitch_tiles, W, H);
  };
  std::string spec = spec_env;
  if (spec.empty() || spec == "1") {
    dump_region(0, 8, 640, 720);    // color base 0 (player / composite color surface)
    dump_region(736, 8, 640, 720);  // depth base 736
  } else {
    size_t pos = 0;
    while (pos < spec.size()) {
      const size_t comma = spec.find(',', pos);
      const std::string one =
          spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      uint32_t f[4] = {0, 8, 0, 0};
      int fi = 0;
      size_t p = 0;
      while (fi < 4) {
        f[fi++] = uint32_t(std::strtoul(one.c_str() + p, nullptr, 10));
        const size_t colon = one.find(':', p);
        if (colon == std::string::npos) break;
        p = colon + 1;
      }
      dump_region(f[0], f[1], f[2], f[3]);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }
  D3D12_RANGE wr{0, 0};
  beta_edramdump_readback_->Unmap(0, &wr);
}

void NhlD3D12CommandProcessor::ResolveBetaMsaaToReadback() {
  // Post-frame (GPU idle): resolve the multisample RT to the 1X resolve target, then
  // copy it into the readback buffer — all on a throwaway DIRECT command list executed
  // on the base's direct queue. Runs exactly once (capture is one-shot under replay),
  // so the resolve target starts in COMMON and the MSAA RT in RENDER_TARGET.
  if (beta_msaa_ <= 1 || !beta_offscreen_rt_ || !beta_resolve_rt_) {
    return;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  ID3D12CommandQueue* queue = GetD3D12Provider().GetDirectQueue();
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
      FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                       IID_PPV_ARGS(&list)))) {
    REXLOG_ERROR("[nhl-beta] msaa: one-shot command list creation failed");
    return;
  }
  auto barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                     D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
  };
  barrier(beta_offscreen_rt_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
          D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  barrier(beta_resolve_rt_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  list->ResolveSubresource(beta_resolve_rt_.Get(), 0, beta_offscreen_rt_.Get(), 0,
                           DXGI_FORMAT_R8G8B8A8_UNORM);
  barrier(beta_resolve_rt_.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = beta_readback_.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = beta_rt_footprint_;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = beta_resolve_rt_.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (FAILED(list->Close())) {
    REXLOG_ERROR("[nhl-beta] msaa: one-shot list Close failed");
    return;
  }
  ID3D12CommandList* lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  // Wait for the resolve+copy to retire before the caller maps the readback buffer.
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
    queue->Signal(fence.Get(), 1);
    for (int i = 0; i < 4000 && fence->GetCompletedValue() < 1; ++i) Sleep(1);
  }
  REXLOG_INFO("[nhl-beta] msaa: resolved {}x -> 1X and copied to readback", beta_msaa_);
}

void NhlD3D12CommandProcessor::DumpBetaDepthStats() {
  // Post-frame (GPU idle): copy the depth target to a readback buffer and log the
  // spread of depth values. Decides whether the depth TEST is the issue (varying z but
  // no cull) or the VS clip-space Z (flat z => nothing to sort). R24G8: depth in the
  // low 24 bits of each 32-bit texel.
  if (!beta_depth_ready_ || !beta_depth_rt_) {
    return;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  ID3D12CommandQueue* queue = GetD3D12Provider().GetDirectQueue();
  D3D12_RESOURCE_DESC dd = beta_depth_rt_->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0;
  UINT64 row_size = 0, total = 0;
  device->GetCopyableFootprints(&dd, 0, 1, 0, &fp, &rows, &row_size, &total);
  Microsoft::WRL::ComPtr<ID3D12Resource> rb;
  D3D12_HEAP_PROPERTIES hr{};
  hr.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.Format = DXGI_FORMAT_UNKNOWN;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &bd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
      FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                       IID_PPV_ARGS(&list)))) {
    return;
  }
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = beta_depth_rt_.Get();
  b.Transition.Subresource = 0;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = rb.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = fp;
  src.pResource = beta_depth_rt_.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  list->ResourceBarrier(1, &b);
  if (FAILED(list->Close())) {
    return;
  }
  ID3D12CommandList* lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
    queue->Signal(fence.Get(), 1);
    for (int i = 0; i < 4000 && fence->GetCompletedValue() < 1; ++i) Sleep(1);
  }
  void* mapped = nullptr;
  D3D12_RANGE rr{0, SIZE_T(total)};
  if (FAILED(rb->Map(0, &rr, &mapped)) || !mapped) {
    return;
  }
  const uint8_t* base = static_cast<const uint8_t*>(mapped);
  uint32_t dmin = 0xFFFFFFFFu, dmax = 0, cleared = 0, sampled = 0;
  // The linear 24-bit normalization below (and the clear_u comparison) is only valid
  // for kD24S8 (UNORM24). For kD24FS8 the stored 24 bits are a float24, so the raw
  // min/max are reported but the 0..1 mapping and cleared% are NOT meaningful — flag
  // the format rather than printing a wrong-scaled value. (A faithful float24 decode
  // is deferred; the FLAT/VARYING verdict holds regardless of format.)
  const bool is_unorm24 =
      beta_depth_xenos_fmt_ == uint32_t(xenos::DepthRenderTargetFormat::kD24S8);
  const uint32_t clear_u = uint32_t(beta_depth_clear_ * 16777215.0f + 0.5f);
  for (uint32_t y = 0; y < fp.Footprint.Height; y += 7) {
    const uint32_t* row = reinterpret_cast<const uint32_t*>(base + size_t(y) * fp.Footprint.RowPitch);
    for (uint32_t x = 0; x < fp.Footprint.Width; x += 7) {
      const uint32_t depth = row[x] & 0x00FFFFFFu;
      dmin = std::min(dmin, depth);
      dmax = std::max(dmax, depth);
      if (is_unorm24 && depth == clear_u) ++cleared;
      ++sampled;
    }
  }
  D3D12_RANGE wr{0, 0};
  rb->Unmap(0, &wr);
  if (is_unorm24) {
    REXLOG_INFO("[nhl-beta] depth-stats(UNORM24): sampled={} dmin={} dmax={} ({:.4f}..{:.4f}) "
                "cleared(={})={:.1f}% => {}",
                sampled, dmin, dmax, dmin / 16777215.0, dmax / 16777215.0, clear_u,
                sampled ? 100.0 * cleared / sampled : 0.0,
                dmin == dmax ? "FLAT (VS z not varying or never written)" : "VARYING (z written)");
  } else {
    REXLOG_INFO("[nhl-beta] depth-stats(FLOAT24 raw bits; 0..1/cleared%% N/A): sampled={} "
                "dmin=0x{:06X} dmax=0x{:06X} => {}",
                sampled, dmin, dmax,
                dmin == dmax ? "FLAT (VS z not varying or never written)" : "VARYING (z written)");
  }
}

void NhlD3D12CommandProcessor::DrainBetaInfoQueue(const char* when) {
  if (!beta_info_queue_) {
    return;
  }
  const UINT64 n = beta_info_queue_->GetNumStoredMessages();
  REXLOG_INFO("[nhl-beta] InfoQueue drain @{}: {} stored messages", when, n);
  for (UINT64 i = 0; i < n; ++i) {
    SIZE_T len = 0;
    beta_info_queue_->GetMessage(i, nullptr, &len);
    if (!len) {
      continue;
    }
    std::vector<uint8_t> buf(len);
    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
    if (SUCCEEDED(beta_info_queue_->GetMessage(i, msg, &len)) && msg->pDescription) {
      REXLOG_ERROR("[nhl-beta][D3D12] sev={} id={}: {}", static_cast<int>(msg->Severity),
                   static_cast<int>(msg->ID), msg->pDescription);
    }
  }
  beta_info_queue_->ClearStoredMessages();
}

void NhlD3D12CommandProcessor::LogBetaDeviceRemoval() {
  DrainBetaInfoQueue("device-check");
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  // Present queues async, so the GPU may not have processed our submission yet.
  // Poll for up to ~4s so a TDR/removal triggered by our draw is observed here
  // (where we can dump DRED) rather than later in a fence wait the replay halts on.
  HRESULT reason = S_OK;
  for (int i = 0; i < 400; ++i) {
    reason = device->GetDeviceRemovedReason();
    if (reason != S_OK) {
      break;
    }
    Sleep(10);
  }
  if (reason == S_OK) {
    REXLOG_INFO("[nhl-beta] takeover: device healthy after frame (no removal observed)");
    return;
  }
  REXLOG_ERROR("[nhl-beta] DEVICE REMOVED reason=0x{:08X} (DXGI_ERROR_DEVICE_HUNG=0x887A0006, "
               "REMOVED=0x887A0005, INTERNAL_ERROR=0x887A0008)",
               static_cast<uint32_t>(reason));
  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    return;
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
    REXLOG_ERROR("[nhl-beta] DRED page-fault VA=0x{:016X}", static_cast<uint64_t>(pf.PageFaultVA));
    for (auto* n = pf.pHeadExistingAllocationNode; n; n = n->pNext) {
      REXLOG_ERROR("[nhl-beta] DRED existing-alloc at fault: '{}' type={}",
                   n->ObjectNameA ? n->ObjectNameA : "(unnamed)",
                   static_cast<int>(n->AllocationType));
    }
    for (auto* n = pf.pHeadRecentFreedAllocationNode; n; n = n->pNext) {
      REXLOG_ERROR("[nhl-beta] DRED recently-freed at fault: '{}' type={}",
                   n->ObjectNameA ? n->ObjectNameA : "(unnamed)",
                   static_cast<int>(n->AllocationType));
    }
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
    for (auto* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
      const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
      REXLOG_ERROR("[nhl-beta] DRED breadcrumb: cmdlist='{}' ops_completed={}/{}",
                   n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "(unnamed)", done,
                   n->BreadcrumbCount);
    }
  }
}

namespace {
// Forwards every D3D12 debug-layer message to our log (so the exact validation
// error is captured in the file, not lost to OutputDebugString).
void __stdcall BetaD3D12MessageCallback(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                                        D3D12_MESSAGE_ID id, LPCSTR description, void*) {
  REXLOG_ERROR("[nhl-beta][D3D12] sev={} id={}: {}", static_cast<int>(severity),
               static_cast<int>(id), description ? description : "");
}
}  // namespace

// Defined in gpu/hooks/highcut_spirv_probe.cpp — P-1 glslang link/compile probe (self-gated).
extern "C" void HighcutSpirvProbe();

bool NhlD3D12CommandProcessor::SetupContext() {
  HighcutSpirvProbe();  // P-1: prove glslang builds/links (no-op unless NHL_HIGHCUT_SPIRV_PROBE)
  // Default (alpha) path: forward to the base D3D12 backend unchanged. This builds
  // the base's own caches and the device/provider our beta caches will reference.
  const bool ok = d3d12::D3D12CommandProcessor::SetupContext();
  // When the debug layer is on, route its messages to our log and disable
  // break-on-severity (which would otherwise terminate the process at the bad call
  // with no debugger attached — masking the error).
  if (ok && std::getenv("NHL_BETA_D3D12_DEBUG")) {
    if (SUCCEEDED(GetD3D12Provider().GetDevice()->QueryInterface(IID_PPV_ARGS(&beta_info_queue_)))) {
      beta_info_queue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
      beta_info_queue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
      beta_info_queue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
      beta_info_queue_->SetMuteDebugOutput(FALSE);
      REXLOG_INFO("[nhl-beta] D3D12 InfoQueue obtained (break-on-severity off; will drain to log)");
      Microsoft::WRL::ComPtr<ID3D12InfoQueue1> iq1;
      if (SUCCEEDED(beta_info_queue_.As(&iq1))) {
        DWORD cookie = 0;
        iq1->RegisterMessageCallback(&BetaD3D12MessageCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                                     nullptr, &cookie);
      }
    } else {
      REXLOG_ERROR("[nhl-beta] could not obtain ID3D12InfoQueue (debug layer not active?)");
    }
  }
  if (ok && std::getenv("NHL_BACKEND") && std::strcmp(std::getenv("NHL_BACKEND"), "beta") == 0) {
    beta_enabled_ = true;
    REXLOG_INFO("[nhl-beta] NHL_BACKEND=beta: bringing up Tier-1 owned-backend caches "
                "(live rendering stays on the base/alpha path)");
    if (!BuildBetaCaches()) {
      REXLOG_ERROR("[nhl-beta] beta cache bring-up failed; tearing down, staying on alpha path");
      ShutdownBetaCaches();
      beta_enabled_ = false;
    } else if (std::getenv("NHL_BETA_TAKEOVER")) {
      beta_takeover_ = true;
      // Phase 5: NHL_BETA_EDRAM routes the owned draw through the RT cache (multi-pass /
      // resolve-aware) instead of the single offscreen RTV. Cached once here.
      beta_edram_enabled_ = std::getenv("NHL_BETA_EDRAM") != nullptr;
      // NHL_BETA_LIVE: render EVERY frame and present our RT to the window (continuous
      // interactive rendering), instead of the single-capture-frame -> PNG validation flow.
      beta_live_ = std::getenv("NHL_BETA_LIVE") != nullptr;
      if (beta_live_) {
        REXLOG_INFO("[nhl-beta] LIVE mode: every frame rendered by our backend + presented "
                    "via the SDK presenter (no capture-frame gate, no PNG readback)");
      }
      REXLOG_INFO("[nhl-beta] TAKEOVER mode (EDRAM multi-pass={}): IssueDraw will NOT delegate to "
                  "base; our backend renders the frame into the already-open submission (no base "
                  "draws -> no command-list contamination)",
                  beta_edram_enabled_);
      if (std::getenv("NHL_BETA_RENDERDOC")) {
        beta_renderdoc_ = rex::ui::RenderDocAPI::CreateIfConnected();
        REXLOG_INFO("[nhl-beta] RenderDoc capture {} (launch via RenderDoc to enable)",
                    beta_renderdoc_ ? "CONNECTED — will bracket the takeover frame" : "NOT connected");
      }
    }
    // Oracle capture: bracket the faithfully-replayed BASE frame (no takeover) so we
    // can inspect in RenderDoc what the base path actually samples for the cloud
    // draws (d6/d9 @ 0x19ED1000). RenderDoc replays the base SDK path faithfully
    // (the offscreen-RT divergence is takeover-specific).
    if (beta_enabled_ && !beta_takeover_ && std::getenv("NHL_BETA_RDOC_ORACLE")) {
      beta_renderdoc_ = rex::ui::RenderDocAPI::CreateIfConnected();
      REXLOG_INFO("[nhl-beta] RenderDoc ORACLE capture {} (base path, no takeover)",
                  beta_renderdoc_ ? "CONNECTED — will bracket the base frame" : "NOT connected");
    }
  }
  return ok;
}

void NhlD3D12CommandProcessor::ShutdownContext() {
  if (beta_enabled_) {
    ShutdownBetaCaches();
    REXLOG_INFO("[nhl-beta] Phase-2: beta caches torn down before base ShutdownContext");
  }
  d3d12::D3D12CommandProcessor::ShutdownContext();
}

void NhlD3D12CommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) {
  // Let the base invalidate ITS shared memory + primitive processor (the live/alpha
  // path depends on this to re-upload replayed guest RAM).
  d3d12::D3D12CommandProcessor::TracePlaybackWroteMemory(base_ptr, length);
  // Mirror it onto our OWN beta caches so they re-upload the same ranges on the next
  // draw. Without this our beta shared memory never learns the trace rewrote guest
  // RAM, keeps its pages "valid", skips the upload, and texture untiling reads an
  // empty buffer -> textures render black (the long-standing Phase-4/5 blocker).
  // exact_range=true matches the base's call (disasm: stack arg 0x20 = 1).
  if (beta_shared_memory_) {
    beta_shared_memory_->MemoryInvalidationCallback(base_ptr, length, /*exact_range=*/true);
  }
  if (beta_primitive_processor_) {
    beta_primitive_processor_->MemoryInvalidationCallback(base_ptr, length, /*exact_range=*/true);
  }
  ++beta_trace_writes_seen_;
  beta_trace_write_bytes_ += length;
  // VS-texture CPU-upload path: a trace write over a substituted texture's backing
  // range means its host copy is stale — re-upload at the next bind.
  for (auto& [sub_base, sub] : beta_vstex_subs_) {
    const uint64_t sub_bytes = uint64_t(sub.width) * sub.height * 16;
    if (sub_base < base_ptr + length && base_ptr < sub_base + sub_bytes) {
      sub.dirty = true;
    }
  }
  // Diagnostic: does the trace EVER deliver the bytes for a specific guest address?
  // Set NHL_BETA_WATCH_ADDR=0x17193000 to log every trace memory write that covers it.
  // If a black texture's base is never logged across the whole replay, the trace simply
  // does not contain that asset's data (referenced-memory cached before capture began) —
  // so no replay renderer (base or beta) can show it. exact_range writes go to guest RAM
  // first (by the trace player), then this callback fires.
  if (const char* wa = std::getenv("NHL_BETA_WATCH_ADDR")) {
    const uint32_t watch = uint32_t(std::strtoul(wa, nullptr, 0));
    if (watch >= base_ptr && watch < base_ptr + length) {
      uint32_t nz = 0;
      if (memory_) {
        const uint8_t* p = memory_->TranslatePhysical<const uint8_t*>(watch);
        if (p) {
          for (int i = 0; i < 256; ++i) {
            if (p[i]) ++nz;
          }
        }
      }
      // Bone-arena double-buffer fingerprint: per-frame nz at row 3 (+0x2400) and
      // row 6 (+0x4800) of the watched texture (3072B rows, k_16_16_16_16_FLOAT).
      // If the populated window ALTERNATES between frames, the game double-buffers
      // the matrix arena and a stale (pre-write) texture is what a frame's draws
      // actually need.
      uint32_t nz_r3 = 0, nz_r6 = 0;
      if (memory_) {
        if (const uint8_t* p3 = memory_->TranslatePhysical<const uint8_t*>(watch + 0x2400)) {
          for (int i = 0; i < 256; ++i) {
            if (p3[i]) ++nz_r3;
          }
        }
        if (const uint8_t* p6 = memory_->TranslatePhysical<const uint8_t*>(watch + 0x4800)) {
          for (int i = 0; i < 256; ++i) {
            if (p6[i]) ++nz_r6;
          }
        }
      }
      REXLOG_INFO("[nhl-beta] WATCH 0x{:X} COVERED by trace write [0x{:X}+0x{:X}); frame={} "
                  "guest-RAM nz={}/256 row3_nz={}/256 row6_nz={}/256 after write",
                  watch, base_ptr, length, frame_index_, nz, nz_r3, nz_r6);
    }
  }
}

void NhlD3D12CommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Base restore (into the base RT cache) — keeps the non-takeover oracle path correct
  // and is harmless under takeover.
  d3d12::D3D12CommandProcessor::RestoreEdramSnapshot(snapshot);
  if (!beta_edram_enabled_ || !beta_render_target_cache_ || !snapshot) return;
  // Cache the snapshot bytes so we can re-seed the beta EDRAM at the start of each beta
  // frame (the single-frame replay loops; re-seeding prevents cross-frame depth/color
  // accumulation that would self-occlude the model on frame 2+).
  const size_t edram_bytes = size_t(xenos::kEdramSizeBytes);
  beta_edram_snapshot_.assign(static_cast<const uint8_t*>(snapshot),
                              static_cast<const uint8_t*>(snapshot) + edram_bytes);
  beta_edram_snapshot_valid_ = true;
  // NHL_BETA_ZFAR: far-seed the player's depth EDRAM tiles in the cached snapshot.
  // Root cause of the missing create-a-player model: the player draws are z_en=1
  // zfunc=3 (LEQUAL) against depth tile 736, but the capture-instant snapshot holds
  // the FINAL (near) depth there, so every player fragment fails LEQUAL and is culled
  // (confirmed: NHL_BETA_NO_RESEED and NHL_BETA_DEPTH_FORCE_ALWAYS both un-cull it).
  // Overwriting only the depth tiles with 0xFF (far) lets the model render with correct
  // LEQUAL occlusion while the color tiles stay byte-faithful. Gated → no regression.
  //
  // NOT FAITHFUL — diagnostic only (audit B5): two known inaccuracies. (1) It treats
  // depth tiles [base..base+count) as ONE contiguous byte run, but EDRAM tiles for a
  // surface stride by the surface tile-pitch and a folded/MSAA depth surface interleaves
  // them — so this can both miss real depth tiles and clobber unrelated color tiles.
  // (2) 0xFF bytes are not the D24FS8 encoding of far-depth (float24 1.0 is not all-ones),
  // so even in-range tiles get a "far-ish by luck" value, not the correct far value. A
  // faithful version must derive the tile set from the real pitch/base/MSAA and seed the
  // format-correct far value (or clear via the RT cache). Do not treat ZFAR as a fix.
  if (std::getenv("NHL_BETA_ZFAR")) {
    constexpr size_t kTileBytes = size_t(xenos::kEdramTileHeightSamples) *
                                  xenos::kEdramTileWidthSamples * sizeof(uint32_t);
    uint32_t base_tile = 736;   // player depth surface base (EDRAM_DIAG depth_edram_tile)
    uint32_t tile_count = 360;  // 640-pitch (8 tiles) x 720 rows (45) = 360 depth tiles
    if (const char* b = std::getenv("NHL_BETA_ZFAR_BASE"))
      base_tile = uint32_t(std::strtoul(b, nullptr, 0));
    if (const char* c = std::getenv("NHL_BETA_ZFAR_COUNT"))
      tile_count = uint32_t(std::strtoul(c, nullptr, 0));
    const size_t begin = size_t(base_tile) * kTileBytes;
    const size_t end = std::min(beta_edram_snapshot_.size(),
                                begin + size_t(tile_count) * kTileBytes);
    if (begin < end) {
      std::fill(beta_edram_snapshot_.begin() + begin, beta_edram_snapshot_.begin() + end,
                uint8_t(0xFF));
      if (std::getenv("NHL_BETA_RESOLVE_DIAG") || std::getenv("NHL_BETA_DEPTH_DIAG")) {
        REXLOG_INFO("[nhl-beta] ZFAR: depth tiles [{}..{}) seeded to far in snapshot "
                    "([0x{:X}+0x{:X}))",
                    base_tile, base_tile + tile_count, begin, end - begin);
      }
    }
  }
  // Apply it now too (mirrors the base call's timing). RestoreEdramSnapshot records an
  // upload+copy on the open submission, so ensure one is open and the beta cache has
  // begun a submission first.
  if (!BetaEnsureSubmissionOpen(this)) return;
  const uint64_t sub_now = GetCurrentSubmission();
  if (!beta_rtcache_ticked_ || beta_rtcache_submission_ != sub_now) {
    beta_rtcache_ticked_ = true;
    beta_rtcache_submission_ = sub_now;
    beta_render_target_cache_->CompletedSubmissionUpdated();
    beta_render_target_cache_->BeginSubmission();
  }
  if (!std::getenv("NHL_BETA_NO_RESEED")) {
    // Reseed from the cached (possibly ZFAR-poked) copy, not the raw arg, so the
    // depth far-seed above applies on this initial apply as well as per-frame.
    beta_render_target_cache_->RestoreEdramSnapshot(beta_edram_snapshot_.data());
  }
  if (std::getenv("NHL_BETA_RESOLVE_DIAG") || std::getenv("NHL_BETA_DEPTH_DIAG")) {
    REXLOG_INFO("[nhl-beta] EDRAM snapshot restored to beta cache ({} bytes)", edram_bytes);
  }
}

bool NhlD3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                         rex::graphics::CommandProcessor::IndexBufferInfo*
                                             index_buffer_info,
                                         bool major_mode_explicit) {
  ++draws_this_frame_;
  ++draws_total_;

  // Stage-1 texture auto-mapping (NHL_INJECT_CORRELATE): hash this draw's textures'
  // guest RAM against the loose-.rx2 registry and record address->asset hits. Env-
  // gated internally; runs before the takeover early-outs so it covers replay too.
  CorrelateTexturesForInjection();

  // Full-frame takeover: render our own work instead of delegating. With no base
  // draws there is no shared-command-list contamination. Render the first draw with
  // our backend; no-op the rest (return true so the front-end keeps walking the
  // frame). Output is captured via the offscreen RT readback at IssueSwap.
  if (beta_takeover_) {
    // LIVE deferred takeover (NHL_BETA_LIVE_START_FRAME=N): let the BASE path render+present
    // the boot/loading frames normally, then start owned rendering at frame N. Taking over
    // from frame 0 stalls the game's loading phase (which depends on base GPU work at load
    // time), so deferring past the boot is needed for live-game rendering. Default 0 = no
    // defer (replay/validation takes over immediately).
    if (beta_live_ && !beta_live_active_) {
      // Arm the takeover by fixed frame (NHL_BETA_LIVE_START_FRAME, default 0 = immediate --
      // used by replay/automated runs) OR the F10 hotkey when NHL_BETA_LIVE_HOTKEY is set.
      // Interactive use: boot the real game, skip/wait through the dynamic intro VIDEO to a
      // static menu by eye, then press F10. Fixed frames can't reliably land on the menu
      // (boot timing is non-deterministic and beta's overhead slows it -> a fixed frame may
      // still be mid-video, where the per-frame-changing video texture renders black/green).
      static const bool hotkey_only = std::getenv("NHL_BETA_LIVE_HOTKEY") != nullptr;
      static const uint32_t live_start = []() -> uint32_t {
        const char* f = std::getenv("NHL_BETA_LIVE_START_FRAME");
        return f ? uint32_t(std::strtoul(f, nullptr, 10)) : 0u;
      }();
      constexpr int kVkF10 = 0x79;
      const bool f10 = (GetAsyncKeyState(kVkF10) & 0x8000) != 0;
      if ((!hotkey_only && frame_index_ >= live_start) || f10) {
        beta_live_active_ = true;
        REXLOG_INFO("[nhl-beta] LIVE takeover ACTIVE at frame {} (via {})", frame_index_,
                    f10 ? "F10 hotkey" : "start frame");
      } else {
        const bool ok0 = d3d12::D3D12CommandProcessor::IssueDraw(
            primitive_type, index_count, index_buffer_info, major_mode_explicit);
        if (ok0) ++draws_ok_this_frame_;
        return ok0;
      }
    }
    // We capture ONE frame for parity validation. Once it's dumped, stop doing owned
    // draws entirely: on a multi-frame streaming trace the base closes/reopens the
    // submission between frames, so a post-capture RenderBetaOwnedDraw would call
    // GetDeferredCommandList() with no submission open (assert submission_open_,
    // command_processor.h:72). No-op every later draw — the frame we needed is saved.
    // LIVE mode renders EVERY frame (continuous interactive rendering); the
    // capture-frame gate below is the validation flow (render one frame -> PNG).
    if (!beta_live_) {
      if (beta_capture_done_) {
        return true;
      }
      // NHL_BETA_CAPTURE_FRAME=N: capture frame N instead of frame 0 (streaming traces
      // often open on a dark transition/montage frame — frame 0 of gameplay is a fullscreen
      // dark overlay, useless for parity). Defer ALL owned rendering until frame N: since
      // nothing renders before it, beta_takeover_rendered_ stays 0, so frame N's first draw
      // triggers the one-time RT clear and its IssueSwap finalizes the capture.
      static const uint32_t capture_frame = []() -> uint32_t {
        const char* f = std::getenv("NHL_BETA_CAPTURE_FRAME");
        return f ? uint32_t(std::strtoul(f, nullptr, 10)) : 0u;
      }();
      if (frame_index_ != capture_frame) {
        return true;
      }
    }
    // Render every draw into our offscreen RT (accumulating); the per-frame copy
    // is recorded in FinalizeBetaTakeoverCapture() at IssueSwap. No base draws =>
    // no shared-command-list contamination. NHL_BETA_MAX_DRAW caps how many draws
    // we render (bisection for the per-draw binding mismatch).
    if (beta_renderdoc_ && !beta_rdoc_capturing_) {
      beta_rdoc_capturing_ = true;
      beta_renderdoc_->api_1_0_0()->StartFrameCapture(GetD3D12Provider().GetDevice(), nullptr);
      REXLOG_INFO("[nhl-beta] RenderDoc StartFrameCapture (takeover frame)");
    }
    // Guarantee an open submission before recording the owned draw (see the
    // beta_access shim above). The first owned draw's sparse tile-mapping ends the
    // base submission; reopen it here exactly as the base IssueDraw would. If the
    // device was removed, skip rather than assert/crash.
    if (!BetaEnsureSubmissionOpen(this)) {
      return true;
    }
    static const char* max_env = std::getenv("NHL_BETA_MAX_DRAW");
    const uint32_t draw_idx = beta_takeover_draw_seen_++;
    if (!max_env || draw_idx < uint32_t(std::strtoul(max_env, nullptr, 10))) {
      if (beta_edram_enabled_) {
        // Mirror the base IssueDraw's EDRAM-mode dispatch (Xenia): a resolve is a draw with
        // edram_mode == kCopy (the base would route it to IssueCopy), and kNoOperation draws
        // don't rasterize. Only the color/depth modes render. This is what makes the 24
        // scene_04 resolves actually run under takeover. Non-EDRAM mode keeps rendering every
        // draw verbatim so the validated single-pass menu/intro stay byte-identical.
        const auto em = register_file_->Get<rex::graphics::reg::RB_MODECONTROL>().edram_mode;
        if (em == xenos::EdramMode::kCopy) {
          BetaResolveEdram();
        } else if (em != xenos::EdramMode::kNoOperation) {
          RenderBetaOwnedDraw(primitive_type, index_count, index_buffer_info);
        }
      } else {
        // Flat path (route a): treat a guest resolve (kCopy) as a host capture of our
        // flat RT into the dest's host texture; render color/depth passes, skip no-ops.
        static const bool flat_rt = std::getenv("NHL_BETA_FLAT") != nullptr;
        if (flat_rt) {
          const auto em = register_file_->Get<rex::graphics::reg::RB_MODECONTROL>().edram_mode;
          if (em == xenos::EdramMode::kCopy) {
            BetaFlatResolve();
          } else if (em != xenos::EdramMode::kNoOperation) {
            RenderBetaOwnedDraw(primitive_type, index_count, index_buffer_info);
          }
        } else {
          RenderBetaOwnedDraw(primitive_type, index_count, index_buffer_info);
        }
      }
    }
    return true;
  }

  InitCaptureArmingOnce();  // idempotent; arms inventory before the first swap
  if (inventory_enabled_) {
    // Sample the live per-draw render state (we are on the CP worker thread, so
    // the register file reflects this draw's state). Register indices verified
    // against the SDK register_table.inc; bitfields per registers.h:
    //   0x2000 RB_SURFACE_INFO    msaa_samples +16:2
    //   0x2208 RB_MODECONTROL     edram_mode  +0:3
    //   0x2200 RB_DEPTHCONTROL    z_enable +1 / z_write +2
    //   0x2202 RB_COLORCONTROL    alpha_test_enable +3
    //   0x2205 PA_SU_SC_MODE_CNTL cull_front +0 / cull_back +1
    //   0x2201 RB_BLENDCONTROL0   per-RT0 blend factors/ops
    //   0x2001 RB_COLOR_INFO      RT0 base/format; 0x2003..05 = RT1..3 (MRT)
    ++inv_prim_[static_cast<uint32_t>(primitive_type)];
    (index_buffer_info ? ++inv_indexed_ : ++inv_auto_);
    ++inv_msaa_[(ReadRegisterValue(0x2000) >> 16) & 0x3];
    ++inv_edram_mode_[ReadRegisterValue(0x2208) & 0x7];
    const uint32_t depth = ReadRegisterValue(0x2200);
    if (depth & 0x2) ++inv_zenable_;
    if (depth & 0x4) ++inv_zwrite_;
    if (ReadRegisterValue(0x2202) & 0x8) ++inv_alphatest_;
    ++inv_cull_[ReadRegisterValue(0x2205) & 0x3];
    inv_blend_vals_.insert(ReadRegisterValue(0x2201));
    inv_color_info_.insert(ReadRegisterValue(0x2001));
    if (ReadRegisterValue(0x2003) || ReadRegisterValue(0x2004) || ReadRegisterValue(0x2005)) {
      ++inv_mrt_;  // any of color RT1/2/3 bound => multiple render targets this draw
    }
    if (index_count < inv_min_idx_) inv_min_idx_ = index_count;
    if (index_count > inv_max_idx_) inv_max_idx_ = index_count;
    ++inv_total_draws_;
  }
  // Oracle RenderDoc bracket: start the capture before the base renders the first
  // draw of the frame (ended in IssueSwap). Only when bracketing the base path.
  if (beta_renderdoc_ && !beta_takeover_ && !beta_rdoc_capturing_) {
    beta_rdoc_capturing_ = true;
    beta_renderdoc_->api_1_0_0()->StartFrameCapture(GetD3D12Provider().GetDevice(), nullptr);
    REXLOG_INFO("[nhl-beta] RenderDoc StartFrameCapture (oracle/base frame)");
  }

  const bool ok = d3d12::D3D12CommandProcessor::IssueDraw(primitive_type, index_count,
                                                          index_buffer_info, major_mode_explicit);
  if (ok) ++draws_ok_this_frame_;
  return ok;
}

bool NhlD3D12CommandProcessor::IssueCopy() {
  ++copies_this_frame_;
  const uint32_t copy_dest_base = ReadRegisterValue(0x2319);  // RB_COPY_DEST_BASE
  resolve_dest_bases_.push_back(copy_dest_base);
  // Parity diag: which guest address does this resolve write? The menu's cloud
  // backdrop (sampled by draws d6/d9 at 0x19ED1000 / 0x19CB1000) is render-to-texture
  // content produced by these resolves — the base/oracle path runs them, our beta
  // takeover skips them, which is the arena-vs-clouds parity gap.
  if (std::getenv("NHL_BETA_RESOLVE_DIAG")) {
    // RB_COPY_DEST_BASE holds the dest base; in Xenos it is a tiled 4KB-unit base
    // for some encodings. Log raw + (raw<<12) so we can match 0x19ED1000.
    REXLOG_INFO("[nhl-beta] IssueCopy #{}: RB_COPY_DEST_BASE raw=0x{:08X} (raw<<12=0x{:08X})",
                copies_this_frame_, copy_dest_base, copy_dest_base << 12);
  }

  // NOTE: under TAKEOVER the base never calls this virtual — Xenos resolves are DRAW
  // packets with RB_MODECONTROL.edram_mode == kCopy, which our IssueDraw intercepts. The
  // beta EDRAM resolve is therefore driven from IssueDraw (BetaResolveEdram), not here.
  const bool ok = d3d12::D3D12CommandProcessor::IssueCopy();
  if (ok) ++copies_ok_this_frame_;
  return ok;
}

void NhlD3D12CommandProcessor::BetaResolveEdram() {
  // Honor a guest resolve (a kCopy draw) on OUR caches: dump the current pass's host
  // render-target rectangle and copy it into the beta shared-memory guest texture at the
  // resolve dest, so later passes and the final composite SAMPLE it via beta_texture_cache_.
  // Without this the scene_04 resolves never run and the composite samples absent textures
  // (the crushed-band mis-projection). The first owned render draw ticks the caches, so
  // resolves (which always follow the passes that fill EDRAM) find them in-frame.
  if (beta_takeover_rendered_ == 0) {
    return;  // no pass has filled EDRAM yet — nothing to resolve
  }
  if (!BetaEnsureSubmissionOpen(this)) {
    return;
  }
  // DIAG: NHL_BETA_SKIP_RESOLVE=3,4,5 — skip the SDK Resolve (and its internal shared-memory
  // RangeWrittenByGpu) for the given 1-based resolve indices. The trace pre-loads the captured
  // guest RAM for these dests (e.g. the create-player depth-silhouette at 0x1C4A2000), so
  // skipping a broken beta resolve lets the texture cache upload the REAL trace data instead of
  // beta's (possibly empty) resolve output. Isolates resolve-side vs composite-side failures.
  const uint32_t this_resolve = beta_resolves_seen_ + 1;
  const auto copy_control = register_file_->Get<rex::graphics::reg::RB_COPY_CONTROL>();
  const auto copy_dest_info = register_file_->Get<rex::graphics::reg::RB_COPY_DEST_INFO>();
  const auto copy_dest_pitch = register_file_->Get<rex::graphics::reg::RB_COPY_DEST_PITCH>();
  // copy_src_select 0..3 selects color RT0..3; >=4 selects DEPTH as the resolve
  // source. We clamp to RT3 only so the color-info read below is in-range — but a
  // depth resolve is NOT color RT3, so flag it rather than silently mislabeling it
  // (this is exactly the depth-as-color aliasing the green-band probe is hunting).
  const bool resolve_src_is_depth = uint32_t(copy_control.copy_src_select) >= 4u;
  const auto source_color_info =
      register_file_->Get<rex::graphics::reg::RB_COLOR_INFO>(
          beta_reg::kColorInfo[std::min(uint32_t(copy_control.copy_src_select), 3u)]);
  if (resolve_src_is_depth && std::getenv("NHL_BETA_RESOLVE_DIAG")) {
    REXLOG_INFO("[nhl-beta] EDRAM Resolve #{}: copy_src_select={} selects DEPTH as source "
                "(not color RTn); the src_color(...) fields logged below are the clamped RT3 "
                "read and do NOT describe this resolve.",
                this_resolve, uint32_t(copy_control.copy_src_select));
  }
  if (std::getenv("NHL_BETA_RESOLVE_DIAG")) {
    REXLOG_INFO(
        "[nhl-beta] EDRAM Resolve #{} state: src={} sample={} command={} clear(c={},d={}) "
        "src_color(base={},fmt={},bias={}) dest(base=0x{:X},pitch={}x{},fmt={},number={},"
        "endian={},swap={},bias={},array={},slice={})",
        this_resolve, uint32_t(copy_control.copy_src_select),
        uint32_t(copy_control.copy_sample_select), uint32_t(copy_control.copy_command),
        uint32_t(copy_control.color_clear_enable), uint32_t(copy_control.depth_clear_enable),
        uint32_t(source_color_info.color_base) |
            (uint32_t(source_color_info.color_base_bit_11) << 11),
        uint32_t(source_color_info.color_format), int32_t(source_color_info.color_exp_bias),
        ReadRegisterValue(0x2319) << 12, uint32_t(copy_dest_pitch.copy_dest_pitch),
        uint32_t(copy_dest_pitch.copy_dest_height), uint32_t(copy_dest_info.copy_dest_format),
        uint32_t(copy_dest_info.copy_dest_number), uint32_t(copy_dest_info.copy_dest_endian),
        uint32_t(copy_dest_info.copy_dest_swap), int32_t(copy_dest_info.copy_dest_exp_bias),
        uint32_t(copy_dest_info.copy_dest_array), uint32_t(copy_dest_info.copy_dest_slice));
    // Green-band probe: dump the guest EDRAM clear color (RB_COLOR_CLEAR / _LO, regs 0x231E/0x231F)
    // and the per-RT EDRAM color base/format. The "green left fold-band" reads an additive green
    // base; if RB_COLOR_CLEAR decodes to ~(0,127,15) for a left-band resolve, the guest clear color
    // (mis-applied across the fold) is the source rather than a draw.
    const uint32_t rb_color_clear = ReadRegisterValue(0x231E);
    const uint32_t rb_color_clear_lo = ReadRegisterValue(0x231F);
    const auto ci0 = register_file_->Get<rex::graphics::reg::RB_COLOR_INFO>(0x2001);
    REXLOG_INFO(
        "[nhl-beta] EDRAM Resolve #{} clearcolor: RB_COLOR_CLEAR=0x{:08X} _LO=0x{:08X} "
        "(bytes hi[{},{},{},{}]) rt0(base_tile={} fmt={})",
        this_resolve, rb_color_clear, rb_color_clear_lo, (rb_color_clear >> 24) & 0xFF,
        (rb_color_clear >> 16) & 0xFF, (rb_color_clear >> 8) & 0xFF, rb_color_clear & 0xFF,
        uint32_t(ci0.color_base) | (uint32_t(ci0.color_base_bit_11) << 11),
        uint32_t(ci0.color_format));
  }
  if (const char* sr = std::getenv("NHL_BETA_SKIP_RESOLVE")) {
    for (const char* s = sr; *s;) {
      char* end = nullptr;
      const unsigned long v = std::strtoul(s, &end, 10);
      if (end == s) { ++s; continue; }
      if (uint32_t(v) == this_resolve) {
        ++beta_resolves_seen_;
        ++copies_this_frame_;
        if (std::getenv("NHL_BETA_RESOLVE_DIAG") || std::getenv("NHL_BETA_EDRAM_DIAG")) {
          REXLOG_INFO("[nhl-beta] EDRAM Resolve #{} SKIPPED (NHL_BETA_SKIP_RESOLVE)", this_resolve);
        }
        return;
      }
      s = (*end) ? end + 1 : end;
    }
  }
  // NHL_BETA_FORCE_SAMPLE=N (diag): override copy_sample_select (RB_COPY_CONTROL bits[4:6], reg
  // 0x2318) for the 1280-wide fold composites only. The frontbuffer composite (resolve #14) averages
  // samples 0&1 of a 4X-MSAA 640-pitch surface; the MSAA samples encode the pitch<width fold. Forcing
  // a single sample (0/1/2/3) and viewing the correctly-untiled frontbuffer reveals whether the
  // left-band green lives in a specific sample (resolve-side averaging) or is present regardless
  // (render-side EDRAM). Restored immediately so only this resolve is perturbed.
  uint32_t forced_saved_copy_control = 0;
  bool forced_sample = false;
  if (const char* fs = std::getenv("NHL_BETA_FORCE_SAMPLE")) {
    if (copy_dest_pitch.copy_dest_pitch == 1280) {
      forced_saved_copy_control = (*register_file_)[0x2318];
      const uint32_t sample = uint32_t(std::strtoul(fs, nullptr, 0)) & 0x7u;
      (*register_file_)[0x2318] = (forced_saved_copy_control & ~(0x7u << 4)) | (sample << 4);
      forced_sample = true;
      if (std::getenv("NHL_BETA_RESOLVE_DIAG")) {
        REXLOG_INFO("[nhl-beta] Resolve #{} FORCE_SAMPLE={} (RB_COPY_CONTROL 0x{:08X}->0x{:08X})",
                    this_resolve, sample, forced_saved_copy_control, (*register_file_)[0x2318]);
      }
    }
  }
  uint32_t written_addr = 0, written_len = 0;
  const bool r = beta_render_target_cache_->Resolve(*memory_, *beta_shared_memory_,
                                                    *beta_texture_cache_, written_addr, written_len);
  if (forced_sample) (*register_file_)[0x2318] = forced_saved_copy_control;
  // Resolve marks the destination through TextureCache::MarkRangeAsResolved, which also
  // notifies SharedMemory. Do not repeat the shared-memory invalidation here.
  ++beta_resolves_seen_;
  ++copies_this_frame_;
  if (r) ++copies_ok_this_frame_;
  if (std::getenv("NHL_BETA_RESOLVE_DIAG") || std::getenv("NHL_BETA_EDRAM_DIAG")) {
    const auto ci = register_file_->Get<rex::graphics::reg::RB_COLOR_INFO>();
    const uint32_t src_tile = ci.color_base | (uint32_t(ci.color_base_bit_11) << 11);
    REXLOG_INFO("[nhl-beta] EDRAM Resolve #{} (after draw #{}) src_edram_tile={} surf_pitch={} -> "
                "ok={} wrote [0x{:X}+0x{:X}) dest_base<<12=0x{:X} copy_dest=({}x{})",
                beta_resolves_seen_, beta_takeover_rendered_, src_tile,
                register_file_->Get<rex::graphics::reg::RB_SURFACE_INFO>().surface_pitch, r,
                written_addr, written_len, ReadRegisterValue(0x2319) << 12,
                uint32_t(copy_dest_pitch.copy_dest_pitch),
                uint32_t(copy_dest_pitch.copy_dest_height));
  }
}

void NhlD3D12CommandProcessor::ValidateBetaShaderTranslation(xenos::ShaderType type,
                                                             const uint32_t* host_address,
                                                             uint32_t dword_count) {
  // Load the ucode into our own pipeline cache (dedupes by ucode hash) and ensure
  // it is analyzed, then translate a DEFAULT modification with our own translator.
  // The default modification is the SDK's own no-special-state variant — enough to
  // prove the ucode->DXBC path works on this shader; the exact per-draw variant
  // (interpolator mask, depth mode, param-gen) comes later when we reconstruct the
  // full IssueDraw. We only translate each (shader, modification) once.
  d3d12::D3D12Shader* shader = beta_pipeline_cache_->LoadShader(type, host_address, dword_count);
  if (!shader) {
    ++beta_xlat_fail_;
    REXLOG_ERROR("[nhl-beta] xlat: PipelineCache::LoadShader returned null (dwords={})", dword_count);
    return;
  }
  if (!shader->is_ucode_analyzed()) {
    beta_pipeline_cache_->AnalyzeShaderUcode(*shader);
  }
  // Remember as the current shader of its type for the next draw.
  if (type == xenos::ShaderType::kVertex) {
    beta_current_vs_ = shader;
  } else {
    beta_current_ps_ = shader;
  }

  if (!beta_translator_) {
    const auto vendor = GetD3D12Provider().GetAdapterVendorID();
    const bool rov = beta_render_target_cache_->GetPath() ==
                     rex::graphics::RenderTargetCache::Path::kPixelShaderInterlock;
    beta_translator_ = std::make_unique<rex::graphics::DxbcShaderTranslator>(
        vendor, /*bindless_resources_used=*/false, /*edram_rov_used=*/rov,
        /*gamma_render_target_as_unorm8=*/false,
        /*msaa_2x_supported=*/beta_render_target_cache_->msaa_2x_supported());
    REXLOG_INFO("[nhl-beta] xlat: DxbcShaderTranslator created (edram_rov={}, msaa2x={})", rov,
                beta_render_target_cache_->msaa_2x_supported());
  }

  const uint64_t modification =
      (type == xenos::ShaderType::kVertex)
          ? beta_translator_->GetDefaultVertexShaderModification(0)
          : beta_translator_->GetDefaultPixelShaderModification(0);
  bool is_new = false;
  rex::graphics::Shader::Translation* translation = shader->GetOrCreateTranslation(modification,
                                                                                  &is_new);
  if (!is_new && translation->is_translated()) {
    return;  // already validated this shader/modification
  }

  const char* kind = type == xenos::ShaderType::kVertex ? "VS" : "PS";
  const bool ok = beta_translator_->TranslateAnalyzedShader(*translation);
  if (ok && translation->is_valid() && translation->is_translated()) {
    ++beta_xlat_ok_;
    REXLOG_INFO("[nhl-beta] xlat OK: {} ucode_dwords={} -> {} DXBC bytes (cumulative ok={} fail={})",
                kind, dword_count, translation->translated_binary().size(), beta_xlat_ok_,
                beta_xlat_fail_);
  } else {
    ++beta_xlat_fail_;
    REXLOG_ERROR("[nhl-beta] xlat FAIL: {} ucode_dwords={} valid={} translated={} (cumulative ok={} "
                 "fail={})",
                 kind, dword_count, translation->is_valid(), translation->is_translated(),
                 beta_xlat_ok_, beta_xlat_fail_);
  }
}

rex::graphics::Shader* NhlD3D12CommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                                            uint32_t guest_address,
                                                            const uint32_t* host_address,
                                                            uint32_t dword_count) {
  ++shader_loads_this_frame_;
  ++shader_loads_total_;
  if (beta_enabled_ && beta_pipeline_cache_) {
    ValidateBetaShaderTranslation(shader_type, host_address, dword_count);
  }
  return d3d12::D3D12CommandProcessor::LoadShader(shader_type, guest_address, host_address,
                                                  dword_count);
}

void NhlD3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                         uint32_t frontbuffer_height) {
  // One line per presented frame: the real NHL Legacy draw-stream shape, proving
  // the injection seam delivers the decoded callbacks to our backend. Logged
  // before the delegate so the count reflects the frame being swapped.
  REXLOG_INFO(
      "[nhl-gpu] frame {} swap: draws={} (ok={}) copies(resolves)={} (ok={}) shader_loads={} "
      "(total {}), frontbuffer={:08X} {}x{}",
      frame_index_, draws_this_frame_, draws_ok_this_frame_, copies_this_frame_,
      copies_ok_this_frame_, shader_loads_this_frame_, shader_loads_total_, frontbuffer_ptr,
      frontbuffer_width, frontbuffer_height);

  // Record our once-per-frame capture copy into the still-open submission BEFORE the base
  // closes it (base IssueSwap calls EndSubmission/ExecuteCommandLists). EDRAM mode reads the
  // resolved frontbuffer guest texture; the offscreen-RTV path copies our accumulated RT.
  // LIVE mode skips this — it presents directly from the RT after the base swap instead.
  if (beta_takeover_ && (!beta_live_ || beta_live_rt_dump_pending_)) {
    // In live, F11 arms a one-shot direct dump of beta's offscreen RT (ground truth for
    // what beta rendered). Reset the capture latch so a repeat F11 can re-dump.
    if (beta_live_) {
      beta_capture_pending_ = false;
      beta_capture_done_ = false;
      beta_live_rt_dump_pending_ = false;
    }
    if (beta_edram_enabled_) {
      CaptureBetaEdramSwap();
    } else {
      FinalizeBetaTakeoverCapture();
    }
  }

  d3d12::D3D12CommandProcessor::IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  // LIVE mode: present our just-rendered RT to the window, then reset per-frame state so the
  // next frame re-clears the RT + re-BeginFrame's the beta caches (gated on first_draw, i.e.
  // beta_takeover_rendered_ == 0). The base IssueSwap above already EndSubmission'd our draws.
  if (beta_takeover_ && beta_live_) {
    // NHL_BETA_NO_PRESENT: skip the present to isolate whether its one-shot DIRECT-queue
    // command list + fence wait is what deadlocks the per-frame ring/pool recycle.
    if (beta_takeover_rendered_ > 0 && !std::getenv("NHL_BETA_NO_PRESENT")) {
      PresentBetaLiveFrame();
    }
    // Option-(c) probe dump (one-shot, self-gates on pending_). PresentBetaLiveFrame's
    // fence-wait above guarantees the probe-copy submission (an earlier submission) has
    // completed, so the readback is valid. Logs whether RequestRange populated our buffer
    // -- decisive for whether live write-watches fired (see NHL_BETA_LIVE_NO_INVALIDATE).
    if (beta_shmem_probe_pending_) DumpBetaSharedMemoryProbe();
    // Reset per-frame state. With beta_takeover_rendered_ back to 0, the downstream
    // capture-mode 4s device poll (gated on >0) and PNG dump (gated on capture-pending,
    // never set in live) are both skipped, while frame_index_++ / NHL_SHOT still run.
    beta_takeover_rendered_ = 0;
    beta_takeover_draw_seen_ = 0;
    beta_takeover_cleared_ = false;
    beta_depth_cleared_ = false;
    beta_live_frame_req_bytes_ = 0;  // per-draw residency upload accounting resets per frame
    beta_live_frame_inv_bytes_ = 0;
    // C-5a frame capture: write the count of owned draws dumped this frame (highcut_frame.count) so
    // the plume replay knows how many highcut_frame_<N>.bin to load, then reset (next frame
    // overwrites). The last fully-captured frame before exit is the one the replay sees.
    if (std::getenv("NHL_HIGHCUT_FRAME_CAPTURE") && highcut_capture_idx_ > 0) {
      if (std::FILE* cf = std::fopen("highcut_frame.count", "w")) {
        std::fprintf(cf, "%u\n", highcut_capture_idx_);
        std::fclose(cf);
      }
      REXLOG_INFO("[highcut-C5] frame captured: {} owned draws -> highcut_frame.count",
                  highcut_capture_idx_);
    }
    highcut_capture_idx_ = 0;
  }

  // Oracle RenderDoc bracket end (base path, no takeover).
  if (!beta_takeover_ && beta_renderdoc_ && beta_rdoc_capturing_) {
    const uint32_t rok = beta_renderdoc_->api_1_0_0()->EndFrameCapture(
        GetD3D12Provider().GetDevice(), nullptr);
    beta_rdoc_capturing_ = false;
    REXLOG_INFO("[nhl-beta] RenderDoc EndFrameCapture (oracle/base frame) -> {}",
                rok ? "captured" : "FAILED");
  }

  // Only run the (expensive, ~4s) post-frame device-removal poll + drains on a frame
  // where we actually did owned rendering — i.e. the capture frame and not yet dumped.
  // On a streaming trace, every frame BEFORE NHL_BETA_CAPTURE_FRAME does no owned draws
  // (beta_takeover_rendered_ stays 0), so the poll there is pure wasted idle (~4s/frame)
  // that made reaching a late capture frame take minutes. Gating it lets the streaming
  // replay fast-forward to the capture frame. No rendering change.
  if (beta_takeover_ && beta_takeover_rendered_ > 0 && !beta_capture_done_) {
    if (beta_renderdoc_ && beta_rdoc_capturing_) {
      const uint32_t ok = beta_renderdoc_->api_1_0_0()->EndFrameCapture(
          GetD3D12Provider().GetDevice(), nullptr);
      beta_rdoc_capturing_ = false;
      REXLOG_INFO("[nhl-beta] RenderDoc EndFrameCapture -> {} (open the .rdc in RenderDoc; inspect "
                  "the textured DrawInstanced's bound root signature)", ok ? "captured" : "FAILED");
    }
    LogBetaDeviceRemoval();  // if our owned draw removed the device, report why
    DrainBetaInfoQueue("after-frame");  // any validation errors from the frame's draws
    DumpBetaSharedMemoryProbe();  // did RequestRange actually populate our buffer?
  }

  // 4b: dump our offscreen RT once the submission carrying the clear+copy has
  // retired (GetCompletedSubmission advances as the base processes later frames).
  // In takeover the LogBetaDeviceRemoval poll above already idled the GPU for ~4s,
  // and single-frame replay never runs a later frame to advance the fence — so
  // there, dump unconditionally (the readback data is valid once the GPU is idle).
  if (beta_enabled_ && beta_capture_pending_ && !beta_capture_done_ &&
      (beta_takeover_ || GetCompletedSubmission() >= beta_capture_submission_)) {
    if (beta_edram_enabled_) {
      // EDRAM mode: the swap-texture copy was recorded into the (now retired) submission.
      DumpBetaEdramSwap("beta_owned_draw.png");
      WriteBetaGpuDumps();  // GPU-side ground truth for NHL_BETA_GPUDUMP addresses
      ReadbackBetaEdramRegion();  // NHL_BETA_EDRAMDUMP: raw live-EDRAM tile readback
    } else {
      // MSAA: the deferred submission rendered into the multisample RT but did NOT copy
      // to readback (no ResolveSubresource on the deferred list). The GPU is now idle —
      // resolve to 1X + copy to readback on a one-shot list before mapping.
      ResolveBetaMsaaToReadback();
      DumpBetaOffscreenTarget("beta_owned_draw.png");
      WriteBetaGpuDumps();  // GPU-side ground truth for NHL_BETA_GPUDUMP addresses
      WriteBetaFlatResolveDumps();  // NHL_BETA_FLAT_DUMP: per-resolve host-texture snapshots
      if (std::getenv("NHL_BETA_DEPTH_DIAG")) {
        DumpBetaDepthStats();
      }
    }
    beta_capture_pending_ = false;
    beta_capture_done_ = true;
    // Stage-1: the capture frame's draws have run correlation; emit the sidecar.
    WriteInjectSidecar();
  }

  // Frame-feature inventory: dump the running aggregate every 30 frames (and the
  // very first frame), so a short run already yields a representative summary.
  if (inventory_enabled_ && (frame_index_ == 0 || frame_index_ % 30 == 0)) {
    PrintInventory();
  }

  // Interactive scene capture: poll F9 once per presented frame.
  if (hotkey_enabled_) {
    PollHotkeyCapture();
  }

  // Beta shader-translation coverage summary (Phase 3).
  if (beta_enabled_ && (frame_index_ == 0 || frame_index_ % 30 == 0)) {
    REXLOG_INFO("[nhl-beta] frame {}: shader translation coverage ok={} fail={}", frame_index_,
                beta_xlat_ok_, beta_xlat_fail_);
  }

  // Live ground-truth screenshot: with NHL_SHOT_FRAME=<n>, grab the presented
  // frame via the presenter (same path the replay tool uses) so we can compare a
  // live frame against its offline replay. Runs on the GPU worker thread.
  // NHL_SHOT_CONTINUOUS: overwrite live_frame.png EVERY frame (live boot, no replay).
  // Boot the game to the language menu, let it sit, kill it — live_frame.png then holds
  // the last presented (menu) frame. The replay path can't yield the composite (it stays
  // in EDRAM, never resolved to a linear guest buffer), so this is the real oracle.
  const char* shot_frame = std::getenv("NHL_SHOT_FRAME");
  const bool shot_continuous = std::getenv("NHL_SHOT_CONTINUOUS") != nullptr;
  // F11 = grab a screenshot on demand (interactive live takeover: press F10 to take over a
  // static menu, then F11 to snapshot what beta rendered into live_frame.png).
  constexpr int kVkF11 = 0x7A;
  const bool f11_down = (GetAsyncKeyState(kVkF11) & 0x8000) != 0;
  const bool f11_rising = f11_down && !beta_f11_prev_down_;
  beta_f11_prev_down_ = f11_down;
  // In live takeover, F11 also arms a direct dump of beta's offscreen RT (next frame ->
  // beta_owned_draw.png) so we can see what beta actually rendered, independent of the
  // presenter/frontbuffer. live_frame.png (presenter) shows the base's frozen frontbuffer.
  if (f11_rising && beta_live_ && beta_takeover_) beta_live_rt_dump_pending_ = true;
  if (shot_continuous || shot_frame || f11_rising) {
    const bool hit = shot_continuous || f11_rising ||
                     (shot_frame && frame_index_ == std::strtoull(shot_frame, nullptr, 10));
    if (hit && graphics_system_) {
      auto* presenter = graphics_system_->presenter();
      rex::ui::RawImage img;
      if (presenter && presenter->CaptureGuestOutput(img) && img.width && img.height) {
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
        nhl::replay::WritePng("live_frame.png", img.width, img.height, rgba.data());
        REXLOG_INFO("[nhl-shot] wrote live_frame.png at frame {} ({}x{})", frame_index_, img.width,
                    img.height);
      }
    }
  }

  // Ground-truth guest-RAM dumper (works on BASE path too, no takeover needed):
  // NHL_DUMP_ADDRS="0x1AF09000,0x1A7D9000,..." dumps each guest address as a
  // 1280x720 8888 image at frame NHL_DUMP_FRAME (default 30). The resolves write
  // their unfolded textures to these guest addresses; reading them on the correct
  // base render reveals exactly what each resolve contains (e.g. which one is the
  // create-player model and whether the fold is a clean block or interleaved).
  // Linear read first; NHL_DUMP_TILED untiles the Xenos 32bpp tiled layout.
  if (const char* da = std::getenv("NHL_DUMP_ADDRS")) {
    const char* df = std::getenv("NHL_DUMP_FRAME");
    const uint64_t want = df ? std::strtoull(df, nullptr, 10) : 30;
    if (frame_index_ == want && memory_) {
      const uint32_t W = 1280, H = 720;
      const bool tiled = std::getenv("NHL_DUMP_TILED") != nullptr;
      for (const char* s = da; *s;) {
        char* end = nullptr;
        const uint32_t addr = uint32_t(std::strtoul(s, &end, 16));
        if (end == s) { ++s; continue; }
        s = (*end) ? end + 1 : end;
        const uint8_t* g = memory_->TranslatePhysical<const uint8_t*>(addr);
        if (!g) continue;
        std::vector<uint8_t> rgba(size_t(W) * H * 4);
        for (uint32_t y = 0; y < H; ++y) {
          for (uint32_t x = 0; x < W; ++x) {
            size_t src;
            if (tiled) {
              // APPROXIMATE Xenos 32bpp untile (audit B4): row-major macro-tile order,
              // which does NOT match the SDK's GetTiledOffset2D interleaved X term for
              // general pitches (32-px column bands can come from the wrong macro-tile).
              // Spot-check only — not a pixel-exact oracle. See WriteBetaGpuDumps.
              const uint32_t aligned_w = (W + 31u) & ~31u;
              const uint32_t mt = ((y / 32) * (aligned_w / 32) + (x / 32));
              const uint32_t my = y & 31, mx = x & 31;
              const uint32_t mi = (my / 8) * 4 + (mx / 8);  // 4x4 micro-tiles of 8x8
              const uint32_t in = (my & 7) * 8 + (mx & 7);
              src = (size_t(mt) * 1024 + mi * 64 + in) * 4;
            } else {
              src = (size_t(y) * W + x) * 4;
            }
            const uint8_t* p = g + src;
            uint8_t* d = rgba.data() + (size_t(y) * W + x) * 4;
            d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = 255;  // BGRA-assumed (audit B12)
          }
        }
        char path[64];
        std::snprintf(path, sizeof(path), "dump_%08X.png", addr);
        nhl::replay::WritePng(path, W, H, rgba.data());
        REXLOG_INFO("[nhl-dump] wrote {} (1280x720 tiled={}) px0=({},{},{})", path, tiled,
                    rgba[0], rgba[1], rgba[2]);
      }
    }
  }

  ++frame_index_;
  draws_this_frame_ = 0;
  draws_ok_this_frame_ = 0;
  copies_this_frame_ = 0;
  copies_ok_this_frame_ = 0;
  shader_loads_this_frame_ = 0;
  // Per-frame: the resolve-dest list is a within-frame record (the offline
  // trace_replay tool reads it before any swap; it never reaches IssueSwap). Reset
  // it here so a long live session doesn't grow it without bound.
  resolve_dest_bases_.clear();
  // Stage-1: clear the per-frame correlation dedup so a base that missed early
  // (guest RAM not yet populated) is retried once it is resident on a later frame.
  inject_correlate_seen_.clear();

  // After the swap that ends the target frame, request a trace of the next full
  // frame. Done once.
  InitCaptureArmingOnce();
  if (capture_armed_ && !capture_done_ && frame_index_ == capture_target_frame_) {
    capture_done_ = true;
    RequestFrameTrace(capture_root_);
    REXLOG_INFO("[nhl-gpu] requested single-frame trace at frame {} -> {}/", frame_index_,
                capture_root_.string());
  }

  // Streaming capture window: BeginTracing at the start boundary (so the next
  // frame's commands are recorded), EndTracing at the end boundary.
  if (stream_armed_ && !stream_done_) {
    if (!stream_active_ && frame_index_ == stream_start_frame_) {
      stream_active_ = true;
      BeginTracing(capture_root_);
      if (full_capture_) InvalidateAllForCapture();
      REXLOG_INFO("[nhl-gpu] streaming trace BEGIN at frame {} -> {}/", frame_index_,
                  capture_root_.string());
    } else if (stream_active_ && frame_index_ == stream_end_frame_) {
      EndTracing();
      stream_active_ = false;
      stream_done_ = true;
      // Stage-1: emit the address->asset sidecar built during this stream window.
      WriteInjectSidecar();
      REXLOG_INFO("[nhl-gpu] streaming trace END at frame {} -> {}/", frame_index_,
                  capture_root_.string());
    }
  }
}

}  // namespace nhl::graphics
