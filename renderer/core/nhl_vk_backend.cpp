#include "renderer/core/nhl_vk_backend.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

// Win32 keyboard poll for the F9 hotkey capture (declared directly to keep
// <windows.h> out of this TU, mirroring the D3D12 path).
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int v_key);

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

void NhlVkCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  // Present the frame via the SDK's own Vulkan presenter, then sample timing.
  rex::graphics::vulkan::VulkanCommandProcessor::IssueSwap(
      frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  // F9 streaming capture toggle (gameplay-trace benchmark source).
  PollHotkeyCapture();

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
