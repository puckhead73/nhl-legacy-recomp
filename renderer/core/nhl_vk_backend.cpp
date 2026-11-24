#include "renderer/core/nhl_vk_backend.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

// Win32 keyboard poll for the F9 hotkey capture (declared directly to keep
// <windows.h> out of this TU, mirroring the D3D12 path). Elsewhere the hotkey
// is simply never "down" (dev-only frame capture).
#if defined(_WIN32)
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int v_key);
#else
static short GetAsyncKeyState(int) { return 0; }
#endif

namespace nhl::graphics {

namespace {
std::mutex g_perf_mutex;
NhlVkPerfSnapshot g_perf;
}  // namespace

void PublishVkPerf(const NhlVkPerfSnapshot& snapshot) {
  std::lock_guard<std::mutex> lock(g_perf_mutex);
  g_perf = snapshot;
}

NhlVkPerfSnapshot ReadVkPerf() {
  std::lock_guard<std::mutex> lock(g_perf_mutex);
  return g_perf;
}

std::string NhlVkGraphicsSystem::name() const {
  return "nhl-vulkan (rexglue ROV + fps tap)";
}

std::unique_ptr<rex::graphics::CommandProcessor>
NhlVkGraphicsSystem::CreateCommandProcessor() {
  // Mirror the D3D12 subclass: `this` is the VulkanGraphicsSystem the base CP ctor
  // expects; kernel_state() is the public GraphicsSystem accessor populated before
  // the command processor is created.
  return std::make_unique<NhlVkCommandProcessor>(this, kernel_state());
}

bool NhlVkCommandProcessor::IssueDraw(
    rex::graphics::xenos::PrimitiveType primitive_type, uint32_t index_count,
    rex::graphics::CommandProcessor::IndexBufferInfo* index_buffer_info,
    bool major_mode_explicit) {
  ++draws_this_frame_;
  namespace reg = rex::graphics::reg;
  namespace xe = rex::graphics::xenos;
  if (!register_file_) {
    return rex::graphics::vulkan::VulkanCommandProcessor::IssueDraw(
        primitive_type, index_count, index_buffer_info, major_mode_explicit);
  }
  if (rtlog_ < 0) rtlog_ = std::getenv("NHL_VK_RTLOG") ? 1 : 0;
  if (rtlog_) RtLogDraw();
  // Resolve experiment env once.
  if (!exp_resolved_) {
    exp_skip_dxt3_ = std::getenv("NHL_VK_SKIP_DXT3") != nullptr;
    exp_skip_alphatest_ = std::getenv("NHL_VK_SKIP_ALPHATEST") != nullptr;
    exp_skip_blend_ = std::getenv("NHL_VK_SKIP_BLEND") != nullptr;
    exp_skip_netlike_ = std::getenv("NHL_VK_NET_SKIP") != nullptr;
    exp_noblend_ = std::getenv("NHL_VK_NET_NOBLEND") != nullptr;
    if (const char* a = std::getenv("NHL_VK_SKIP_ADDR"))
      exp_skip_addr_ = uint32_t(std::strtoul(a, nullptr, 16));
    if (const char* a = std::getenv("NHL_VK_NET_ADDR"))
      exp_net_addr_ = uint32_t(std::strtoul(a, nullptr, 16));
    if (const char* r = std::getenv("NHL_VK_NET_REF")) { exp_ref_on_ = true; exp_ref_ = float(std::atof(r)); }
    if (const char* r = std::getenv("NHL_VK_NET_FORCE_AT")) { exp_force_at_on_ = true; exp_force_at_ref_ = float(std::atof(r)); }
    atoc_fix_on_ = std::getenv("NHL_VK_NO_ATOC_FIX") == nullptr;
    if (const char* r = std::getenv("NHL_VK_ATOC_REF")) atoc_ref_ = float(std::atof(r));
    // The diagnostic/inventory machinery (texture loop, skips, net-target
    // overrides) is only needed when something is asking for it; keep the
    // shipping path lean. The a2c fix below does NOT need it.
    diag_active_ = std::getenv("NHL_VK_DRAWLOG") || exp_skip_dxt3_ || exp_skip_alphatest_ ||
                   exp_skip_blend_ || exp_skip_netlike_ || exp_noblend_ || exp_ref_on_ ||
                   exp_force_at_on_ || exp_skip_addr_ != 0;
    exp_resolved_ = true;
  }

  const auto cc = register_file_->Get<reg::RB_COLORCONTROL>();
  if (cc.alpha_test_enable) ++alpha_test_draws_;
  if (cc.alpha_to_mask_enable) ++alpha_to_mask_draws_;
  const auto bc = register_file_->Get<reg::RB_BLENDCONTROL>();
  const bool blend_on =
      uint32_t(bc.color_srcblend) != 1u || uint32_t(bc.color_destblend) != 0u;
  if (blend_on) ++blend_draws_;

  // Diagnostics: per-window texture inventory + category skips, to identify which
  // draw is which (used to find that the net is an alpha-to-mask DXT3). Gated off
  // in the shipping path. The net = a DXT3 drawn with alpha-to-coverage, or
  // whatever binds the net addr (NHL_VK_NET_ADDR).
  bool net_target = false;
  if (diag_active_) {
    bool binds_dxt3 = false, binds_skip_addr = false, binds_net_addr = false;
    for (uint32_t i = 0; i < xe::kTextureFetchConstantCount; ++i) {
      const auto tf = register_file_->GetTextureFetch(i);
      if (tf.type != xe::FetchConstantType::kTexture) continue;
      const uint32_t base = tf.base_address << 12;
      if (tf.format == xe::TextureFormat::k_DXT2_3) binds_dxt3 = true;
      if (exp_skip_addr_ && base == exp_skip_addr_) binds_skip_addr = true;
      if (base == exp_net_addr_) binds_net_addr = true;
      // Dedup into the per-window inventory by base address; keep largest area seen.
      const uint32_t w = uint32_t(tf.size_2d.width) + 1u;
      const uint32_t h = uint32_t(tf.size_2d.height) + 1u;
      const uint32_t area = w * h;
      uint32_t slot = kInvMax;
      for (uint32_t j = 0; j < inv_count_; ++j) {
        if (inv_[j].base == base) { slot = j; break; }
      }
      if (slot == kInvMax && inv_count_ < kInvMax) slot = inv_count_++;
      if (slot != kInvMax) {
        TexDrawState& s = inv_[slot];
        s.draws++;
        if (!s.seen || area >= s.area) {  // record the dominant (largest) use
          s.seen = true; s.base = base; s.w = w; s.h = h; s.area = area;
          s.fmt = uint32_t(tf.format);
          s.alpha_test = cc.alpha_test_enable; s.alpha_to_mask = cc.alpha_to_mask_enable;
          s.blend = blend_on;
          s.blendctl0 = (*register_file_)[reg::RB_BLENDCONTROL::register_index];
          s.colorctl = (*register_file_)[reg::RB_COLORCONTROL::register_index];
          s.alpha_func = uint32_t(cc.alpha_func);
          s.alpha_ref = std::bit_cast<float>((*register_file_)[0x210E]);
          s.colormask = (*register_file_)[0x2104] & 0xFu;
        }
      }
    }
    net_target = (binds_dxt3 && cc.alpha_to_mask_enable) || binds_net_addr;
    const bool skip = (exp_skip_dxt3_ && binds_dxt3) ||
                      (exp_skip_alphatest_ && cc.alpha_test_enable) ||
                      (exp_skip_blend_ && blend_on) ||
                      (binds_skip_addr) ||
                      (exp_skip_netlike_ && net_target);
    if (skip) { ++skipped_draws_; return true; }
  }

  // THE FIX: emulate alpha-to-coverage with alpha-test for any draw whose only
  // alpha path is alpha-to-mask (FSI doesn't cull on a2c -> the rink net renders
  // opaque). Diagnostic experiment overrides on the net target compose on top.
  const bool atoc_fix = atoc_fix_on_ && cc.alpha_to_mask_enable && !cc.alpha_test_enable;
  const bool exp_override = net_target && (exp_ref_on_ || exp_noblend_ || exp_force_at_on_);
  if (atoc_fix || exp_override) {
    const uint32_t kAlphaRef = 0x210E;
    const uint32_t blend_idx = reg::RB_BLENDCONTROL::register_index;
    const uint32_t color_idx = reg::RB_COLORCONTROL::register_index;
    const uint32_t saved_ref = (*register_file_)[kAlphaRef];
    const uint32_t saved_blend = (*register_file_)[blend_idx];
    const uint32_t saved_color = (*register_file_)[color_idx];
    if (atoc_fix) {
      // alpha_func=Greater(4), alpha_test_enable=1, alpha_to_mask=0 in low 5 bits.
      (*register_file_)[color_idx] = (saved_color & ~0x1Fu) | 0x0Cu;
      (*register_file_)[kAlphaRef] = std::bit_cast<uint32_t>(atoc_ref_);
    }
    // Diagnostic experiment overrides (take precedence on the net target).
    if (exp_ref_on_) (*register_file_)[kAlphaRef] = std::bit_cast<uint32_t>(exp_ref_);
    if (exp_noblend_) (*register_file_)[blend_idx] = 0x00010001u;
    if (exp_force_at_on_) {
      (*register_file_)[color_idx] = (saved_color & ~0x1Fu) | 0x0Cu;
      (*register_file_)[kAlphaRef] = std::bit_cast<uint32_t>(exp_force_at_ref_);
    }
    const bool rv = rex::graphics::vulkan::VulkanCommandProcessor::IssueDraw(
        primitive_type, index_count, index_buffer_info, major_mode_explicit);
    (*register_file_)[kAlphaRef] = saved_ref;
    (*register_file_)[blend_idx] = saved_blend;
    (*register_file_)[color_idx] = saved_color;
    return rv;
  }
  // Experiment (NHL_VK_EXP_ZALWAYS=1): depth-tested draws pass the depth test
  // unconditionally (ZFUNC=ALWAYS, writes kept) - tells "depth buffer content
  // rejects everything" apart from other host-RT draw losses.
  // Experiment (NHL_VK_EXP_SKIP_MSAA4=1): drop draws into 4x-MSAA surfaces (the
  // game's EDRAM clear trick at pitch 320), to test whether host-RT ownership
  // transfers between the 4x/320 and 2x/640 layouts lose the scene.
  // Value = bit mask of which 4x draws to drop: 1 = color(+depth) writes
  // (mode 4, cmask!=0), 2 = depth-only (mode 5), 4 = no-write (cmask==0, no z).
  static const uint32_t exp_skip_msaa4 = [] {
    const char* e = std::getenv("NHL_VK_EXP_SKIP_MSAA4");
    return (e && *e) ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u;
  }();
  if (exp_skip_msaa4 && (((*register_file_)[0x2000] >> 16) & 3u) == 2u) {
    const uint32_t mode = (*register_file_)[0x2208] & 7u;
    const uint32_t cmask = (*register_file_)[0x2104] & 0xFFFFu;
    const bool z_on = ((*register_file_)[0x2200] & 0x7u) != 0;
    uint32_t kind;
    if (mode == 5) kind = 2;
    else if (cmask == 0 && !z_on) kind = 4;
    else kind = 1;
    if (exp_skip_msaa4 & kind) return true;
  }
  static const bool exp_zalways = std::getenv("NHL_VK_EXP_ZALWAYS") != nullptr;
  if (exp_zalways) {
    constexpr uint32_t kZCtl = 0x2200;
    uint32_t& z = (*register_file_)[kZCtl];
    const uint32_t saved = z;
    if (z & 0x2u) z = (z & ~(7u << 4)) | (7u << 4);
    const bool rv = rex::graphics::vulkan::VulkanCommandProcessor::IssueDraw(
        primitive_type, index_count, index_buffer_info, major_mode_explicit);
    z = saved;
    return rv;
  }
  // Forward unchanged — rendering is identical to the stock Vulkan backend.
  return rex::graphics::vulkan::VulkanCommandProcessor::IssueDraw(
      primitive_type, index_count, index_buffer_info, major_mode_explicit);
}

void NhlVkCommandProcessor::PollHotkeyCapture() {
  if (!hotkey_checked_) {
    hotkey_checked_ = true;
    hotkey_enabled_ = std::getenv("NHL_HOTKEY_CAPTURE") != nullptr;
    if (hotkey_enabled_) {
      // Continue numbering after any existing gpu_trace/scene_NN so we never
      // overwrite the canonical captures (scene_00..scene_03).
      std::error_code ec;
      const std::filesystem::path root = "gpu_trace";
      if (std::filesystem::exists(root, ec)) {
        for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
          if (!e.is_directory()) continue;
          const std::string n = e.path().filename().string();
          if (n.rfind("scene_", 0) != 0) continue;
          char* end = nullptr;
          const unsigned long idx = std::strtoul(n.c_str() + 6, &end, 10);
          if (end && *end == '\0' && idx + 1 > hotkey_capture_index_) {
            hotkey_capture_index_ = uint32_t(idx + 1);
          }
        }
      }
      REXLOG_INFO(
          "[nhl-cap] F9 hotkey streaming capture enabled (press F9 in a GAMEPLAY "
          "scene to start, F9 again to stop -> gpu_trace/scene_{:02}/); replay with "
          "NHL_REPLAY_XTR / NHL_REPLAY_BENCH.",
          hotkey_capture_index_);
    }
  }
  if (!hotkey_enabled_) return;

  constexpr int kVkF9 = 0x78;
  const bool down = (GetAsyncKeyState(kVkF9) & 0x8000) != 0;
  const bool rising = down && !hotkey_prev_down_;
  hotkey_prev_down_ = down;
  if (!rising) return;

  if (!hotkey_capturing_) {
    char dir[32];
    std::snprintf(dir, sizeof(dir), "scene_%02u", hotkey_capture_index_);
    const std::filesystem::path path = std::filesystem::path("gpu_trace") / dir;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    BeginTracing(path);  // streaming starts on the next primary-buffer execute
    hotkey_capturing_ = true;
    REXLOG_INFO("[nhl-cap] F9 capture BEGIN -> {}/ (frame {})", path.string(),
                frames_total_);
  } else {
    EndTracing();
    hotkey_capturing_ = false;
    REXLOG_INFO("[nhl-cap] F9 capture END (frame {}) -> gpu_trace/scene_{:02}/",
                frames_total_, hotkey_capture_index_);
    ++hotkey_capture_index_;
  }
}

// --- Render-target / resolve inventory (NHL_VK_RTLOG=1) ---------------------
// Raw Xenos register dwords (see register_table.inc) so the log is easy to
// decode against Xenia/SDK docs.
namespace {
constexpr uint32_t kRbSurfaceInfo = 0x2000, kRbColorInfo = 0x2001, kRbDepthInfo = 0x2002,
                   kRbColor1Info = 0x2003, kWindowOffset = 0x2080, kWindowScissorBr = 0x2082,
                   kRbColorMask = 0x2104, kVportXScale = 0x210F, kVportYScale = 0x2111,
                   kRbDepthControl = 0x2200, kRbModeControl = 0x2208, kVizQuery = 0x2293,
                   kCopyControl = 0x2318, kCopyDestBase = 0x2319, kCopyDestPitch = 0x231A,
                   kCopyDestInfo = 0x231B;
}  // namespace

void NhlVkCommandProcessor::RtLogDraw() {
  const auto& rf = *register_file_;
  RtDrawKey k{rf[kRbSurfaceInfo], rf[kRbColorInfo],   rf[kRbColor1Info], rf[kRbDepthInfo],
              rf[kRbModeControl], rf[kRbDepthControl], rf[kRbColorMask], rf[kWindowOffset]};
  RtDrawStat* s = nullptr;
  for (auto& e : rt_draws_) {
    if (e.key == k) { s = &e; break; }
  }
  if (!s) {
    if (rt_draws_.size() >= 256) return;
    rt_draws_.push_back(RtDrawStat{k});
    s = &rt_draws_.back();
  }
  ++s->draws;
  const uint32_t idx = uint32_t(s - rt_draws_.data());
  if (!rt_seq_.empty() && rt_seq_.back().first == idx) {
    ++rt_seq_.back().second;
  } else if (rt_seq_.size() < 512) {
    rt_seq_.emplace_back(idx, 1u);
  }
  if (rf[kVizQuery] & 1u) ++s->viz_query_draws;
  s->vp_w = std::max(s->vp_w, 2.0f * std::abs(std::bit_cast<float>(rf[kVportXScale])));
  s->vp_h = std::max(s->vp_h, 2.0f * std::abs(std::bit_cast<float>(rf[kVportYScale])));
  s->scissor_br = rf[kWindowScissorBr];
}

bool NhlVkCommandProcessor::IssueCopy() {
  if (rtlog_ > 0 && register_file_) {
    const auto& rf = *register_file_;
    RtCopyStat c{rf[kRbSurfaceInfo], rf[kRbColorInfo],  rf[kRbDepthInfo],
                 rf[kCopyControl],   rf[kCopyDestBase], rf[kCopyDestPitch],
                 rf[kCopyDestInfo],  rf[kWindowScissorBr], rf[kWindowOffset]};
    bool found = false;
    uint32_t cidx = 0;
    for (auto& e : rt_copies_) {
      if (e.surface_info == c.surface_info && e.color_info == c.color_info &&
          e.depth_info == c.depth_info && e.copy_control == c.copy_control &&
          e.dest_base == c.dest_base && e.dest_pitch == c.dest_pitch &&
          e.dest_info == c.dest_info && e.scissor_br == c.scissor_br) {
        ++e.count;
        found = true;
        break;
      }
      ++cidx;
    }
    if (!found && rt_copies_.size() < 256) {
      c.count = 1;
      rt_copies_.push_back(c);
    }
    if (rt_seq_.size() < 512) rt_seq_.emplace_back(0x8000u | cidx, 1u);
  }
  // Experiment (NHL_VK_EXP_COPY_SAMPLE=<0..6>): force RB_COPY_CONTROL's
  // copy_sample_select for color resolves, restoring it afterwards - separates
  // "MSAA draws never landed" from "multi-sample resolve loses them".
  static const int exp_sample = [] {
    const char* e = std::getenv("NHL_VK_EXP_COPY_SAMPLE");
    return (e && *e) ? int(std::strtol(e, nullptr, 10)) : -1;
  }();
  if (exp_sample >= 0 && register_file_) {
    uint32_t& cc = (*register_file_)[kCopyControl];
    const uint32_t saved = cc;
    if ((cc & 7u) != 4u) {  // color source only (4 = depth)
      cc = (cc & ~(7u << 4)) | (uint32_t(exp_sample & 7) << 4);
    }
    const bool r = rex::graphics::vulkan::VulkanCommandProcessor::IssueCopy();
    cc = saved;
    return r;
  }
  return rex::graphics::vulkan::VulkanCommandProcessor::IssueCopy();
}

// Decode helpers per Xenos: SURFACE_INFO pitch[13:0] msaa[17:16];
// COLOR_INFO base[11:0] (EDRAM tiles) format[19:16]; DEPTH_INFO base[11:0]
// format[16]; COPY_CONTROL src[2:0] sample[6:4] cmd[21:20].
void NhlVkCommandProcessor::RtLogDump() {
  if (rt_draws_.empty() && rt_copies_.empty()) return;
  REXLOG_INFO("[nhl-rt] ---- frame {}: {} rt configs, {} resolve kinds ----", frames_total_,
              rt_draws_.size(), rt_copies_.size());
  uint32_t di = 0;
  for (const auto& e : rt_draws_) {
    const auto& k = e.key;
    REXLOG_INFO(
        "[nhl-rt] D{:<2} x{:<5} pitch={:<4} msaa={} c0base={:<4} c0fmt={:<2} c1={:08X} "
        "dbase={:<4} dfmt={} mode={} zctl={:08X} cmask={:04X} woff={:08X} vp={:.0f}x{:.0f} "
        "sc_br={:08X} viz={}",
        di++, e.draws, k.surface_info & 0x3FFFu, (k.surface_info >> 16) & 3u,
        k.color_info & 0xFFFu, (k.color_info >> 16) & 0xFu, k.color1_info,
        k.depth_info & 0xFFFu, (k.depth_info >> 16) & 1u, k.modecontrol & 7u, k.depthcontrol,
        k.colormask & 0xFFFFu, k.window_offset, e.vp_w, e.vp_h, e.scissor_br,
        e.viz_query_draws);
  }
  uint32_t ci = 0;
  for (const auto& c : rt_copies_) {
    REXLOG_INFO(
        "[nhl-rt] C{:<2} x{:<3} pitch={:<4} msaa={} c0base={:<4} c0fmt={:<2} dbase={:<4} "
        "src={} sample={} cmd={} clr={}{} dest=0x{:08X} dpitch={}x{} dinfo={:08X} "
        "sc_br={:08X} woff={:08X}",
        ci++, c.count, c.surface_info & 0x3FFFu, (c.surface_info >> 16) & 3u, c.color_info & 0xFFFu,
        (c.color_info >> 16) & 0xFu, c.depth_info & 0xFFFu, c.copy_control & 7u,
        (c.copy_control >> 4) & 7u, (c.copy_control >> 20) & 3u, (c.copy_control >> 8) & 1u,
        (c.copy_control >> 9) & 1u, c.dest_base, c.dest_pitch & 0x3FFFu,
        (c.dest_pitch >> 16) & 0x3FFFu, c.dest_info, c.scissor_br, c.window_offset);
  }
  // In-order sequence, "D<draw config #>x<run>" / "C<copy #>", indices as listed above.
  std::string seq;
  for (const auto& [id, run] : rt_seq_) {
    char buf[24];
    if (id & 0x8000u) {
      std::snprintf(buf, sizeof(buf), "C%u ", id & 0x7FFFu);
    } else {
      std::snprintf(buf, sizeof(buf), "D%ux%u ", id, run);
    }
    seq += buf;
  }
  REXLOG_INFO("[nhl-rt] seq: {}", seq);
  rt_draws_.clear();
  rt_copies_.clear();
  rt_seq_.clear();
}

// Present frame cap. On Windows the guest's presents pace themselves (60 fps in
// menus, 30 in matches). On Linux the same build presents uncapped in menus
// (~200+ fps, measured on a Steam Deck) - game *timing* stays correct (movies
// play at their encoded rate), the extra frames just burn CPU/GPU power. Holding
// the CP thread here backpressures the guest render thread to the cap without
// touching simulation time. NHL_FPS_CAP=<fps> overrides (0 = off). Default 60
// off Windows; off on Windows, where it never triggers and the coarse system
// timer could only add jitter.
void NhlVkCommandProcessor::ThrottlePresent() {
  static const double cap = [] {
    if (const char* e = std::getenv("NHL_FPS_CAP"); e && *e) {
      const double v = std::strtod(e, nullptr);
      return v > 0.0 ? v : 0.0;
    }
#if defined(_WIN32)
    return 0.0;
#else
    return 60.0;
#endif
  }();
  if (cap <= 0.0) return;
  using clock = std::chrono::steady_clock;
  const auto interval =
      std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / cap));
  const auto now = clock::now();
  if (present_deadline_ == clock::time_point{} || now - present_deadline_ > interval) {
    // First frame, or we fell more than a frame behind (slow scene): resync
    // instead of bursting to catch up.
    present_deadline_ = now + interval;
    return;
  }
  if (present_deadline_ > now) std::this_thread::sleep_until(present_deadline_);
  present_deadline_ += interval;
}

void NhlVkCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  // Pace presents to the cap, then present the frame via the SDK's own Vulkan
  // presenter and sample timing.
  ThrottlePresent();
  rex::graphics::vulkan::VulkanCommandProcessor::IssueSwap(
      frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  // F9 streaming capture toggle (gameplay-trace benchmark source).
  PollHotkeyCapture();
  if (rtlog_ > 0) RtLogDump();

  const auto now = std::chrono::steady_clock::now();
  last_frame_draws_ = draws_this_frame_;
  last_alpha_test_ = alpha_test_draws_;
  last_alpha_to_mask_ = alpha_to_mask_draws_;
  last_blend_ = blend_draws_;
  last_skipped_ = skipped_draws_;
  alpha_test_draws_ = 0;
  alpha_to_mask_draws_ = 0;
  blend_draws_ = 0;
  skipped_draws_ = 0;
  window_draws_ += draws_this_frame_;
  draws_this_frame_ = 0;
  ++window_frames_;
  ++frames_total_;

  if (!started_) {
    started_ = true;
    window_start_ = now;
    window_frames_ = 0;
    window_draws_ = 0;
    return;
  }

  const double elapsed = std::chrono::duration<double>(now - window_start_).count();
  if (elapsed >= 1.0 && window_frames_ > 0) {
    const double fps = window_frames_ / elapsed;
    const double frame_ms = 1000.0 * elapsed / double(window_frames_);
    const double avg_draws = double(window_draws_) / double(window_frames_);
    REXLOG_INFO(
        "[nhl-vk-fps] fps={:.1f} frame_ms={:.2f} draws/frame={:.0f} (last_frame_draws={}) "
        "frames_total={}",
        fps, frame_ms, avg_draws, last_frame_draws_, frames_total_);
    // Publish for the enhancements overlay's perf HUD.
    PublishVkPerf(NhlVkPerfSnapshot{fps, frame_ms, avg_draws, frames_total_, true});
    if (std::getenv("NHL_VK_DRAWLOG")) {
      REXLOG_INFO(
          "[nhl-vk-draw] last_frame: draws={} alpha_test={} alpha_to_mask={} blend={} skipped={}",
          last_frame_draws_, last_alpha_test_, last_alpha_to_mask_, last_blend_, last_skipped_);
      // Texture inventory, largest area first. The visible net should be findable
      // here by its dimensions; then NHL_VK_SKIP_ADDR=<base> confirms it.
      uint32_t order[kInvMax];
      for (uint32_t i = 0; i < inv_count_; ++i) order[i] = i;
      for (uint32_t a = 0; a < inv_count_; ++a)
        for (uint32_t b = a + 1; b < inv_count_; ++b)
          if (inv_[order[b]].area > inv_[order[a]].area) std::swap(order[a], order[b]);
      const uint32_t show = inv_count_ < 24u ? inv_count_ : 24u;
      for (uint32_t k = 0; k < show; ++k) {
        const TexDrawState& s = inv_[order[k]];
        REXLOG_INFO(
            "[nhl-vk-draw]   tex base=0x{:08X} dims={}x{} fmt={} draws={} | at={} a2m={} "
            "blend={} | BLENDCTL0=0x{:08X} COLORCTL=0x{:08X} func={} ref={:.3f} cmask=0x{:X}",
            s.base, s.w, s.h, s.fmt, s.draws, s.alpha_test, s.alpha_to_mask, s.blend,
            s.blendctl0, s.colorctl, s.alpha_func, s.alpha_ref, s.colormask);
      }
    }
    for (uint32_t i = 0; i < inv_count_; ++i) inv_[i] = TexDrawState{};
    inv_count_ = 0;
    window_start_ = now;
    window_frames_ = 0;
    window_draws_ = 0;
  }
}

}  // namespace nhl::graphics
