// nhllegacy custom renderer — Vulkan backend subclass (SPIKE Phase A/B)
//
// Minimal subclass of the SDK's native Vulkan ROV backend, compiled ONLY when
// linking a rexglue SDK built with REXGLUE_USE_VULKAN=ON (gated by the CMake
// option NHLLEGACY_VULKAN_BACKEND -> NHL_HAVE_VULKAN_BACKEND). It mirrors the
// D3D12 log-and-delegate subclass: every override forwards to the base Vulkan
// implementation, so rendering is identical to the stock backend.
//
// Purpose: (1) measure real fps + draws/frame from IssueSwap/IssueDraw — the
// decisive perf comparison against the high-cut plume path's ~3 fps wall; and
// (2) prove the Phase-B hook surface (we can subclass VulkanCommandProcessor and
// intercept the same IssueDraw/IssueSwap callbacks the D3D12 beta path uses).

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/graphics/xenos.h>

namespace nhl::graphics {

// Latest 1 Hz perf window published by NhlVkCommandProcessor::IssueSwap (CP
// thread) and read by the enhancements overlay (UI thread). Thread-safe.
struct NhlVkPerfSnapshot {
  double fps = 0.0;
  double frame_ms = 0.0;
  double draws_per_frame = 0.0;
  uint64_t frames_total = 0;
  bool valid = false;
};
void PublishVkPerf(const NhlVkPerfSnapshot& snapshot);
NhlVkPerfSnapshot ReadVkPerf();

class NhlVkCommandProcessor : public rex::graphics::vulkan::VulkanCommandProcessor {
 public:
  // Inherit the (VulkanGraphicsSystem*, KernelState*) constructor.
  using rex::graphics::vulkan::VulkanCommandProcessor::VulkanCommandProcessor;

 protected:
  // Per-present hook: forward to base, then accumulate a 1 Hz fps/draws report.
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  // Per-draw hook: count draws this frame, then forward to base (unchanged render).
  bool IssueDraw(rex::graphics::xenos::PrimitiveType primitive_type, uint32_t index_count,
                 rex::graphics::CommandProcessor::IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;

 private:
  // NHL_VK_DRAWLOG diagnostic: per-frame counts of draws that enable alpha-test,
  // alpha-to-mask, or non-opaque blend — to see whether the transparency paths
  // are driven during gameplay (net-transparency investigation).
  uint32_t alpha_test_draws_ = 0;
  uint32_t alpha_to_mask_draws_ = 0;
  uint32_t blend_draws_ = 0;
  uint32_t last_alpha_test_ = 0;
  uint32_t last_alpha_to_mask_ = 0;
  uint32_t last_blend_ = 0;
  // Per-draw transparency state for a bound texture, to pin the net bug. The net
  // turned out not to be the DXT3+alpha-test+blend draw, so we now inventory every
  // distinct bound texture per window and support category skips to find it.
  struct TexDrawState {
    bool seen = false;
    uint32_t base = 0, w = 0, h = 0, fmt = 0, area = 0, draws = 0;
    bool alpha_test = false, alpha_to_mask = false, blend = false;
    uint32_t blendctl0 = 0, colorctl = 0, alpha_func = 0, colormask = 0;
    float alpha_ref = 0.0f;
  };
  static constexpr uint32_t kInvMax = 48;
  TexDrawState inv_[kInvMax]{};  // distinct bound textures this window (dedup by base)
  uint32_t inv_count_ = 0;
  // Skip bookkeeping so we can SEE an experiment firing (not just guess).
  uint32_t skipped_draws_ = 0, last_skipped_ = 0;
  // Cached env (resolved once): category skips + targeted overrides.
  bool exp_resolved_ = false;
  bool diag_active_ = false;  // any diagnostic/experiment on -> run the texture loop
  bool exp_skip_dxt3_ = false, exp_skip_alphatest_ = false, exp_skip_blend_ = false;
  uint32_t exp_skip_addr_ = 0;   // NHL_VK_SKIP_ADDR (hex), 0 = off
  bool exp_ref_on_ = false; float exp_ref_ = 0.0f;     // NHL_VK_NET_REF
  bool exp_noblend_ = false;                            // NHL_VK_NET_NOBLEND
  bool exp_skip_netlike_ = false;                       // NHL_VK_NET_SKIP (net target)
  // The net = a DXT3 drawn with alpha-to-coverage (a2m), or whatever binds this addr.
  uint32_t exp_net_addr_ = 0x191CB000u;                 // NHL_VK_NET_ADDR (hex)
  bool exp_force_at_on_ = false; float exp_force_at_ref_ = 0.5f;  // NHL_VK_NET_FORCE_AT=<ref>
  // THE FIX (on by default): the FSI render-target path doesn't cull on
  // alpha-to-coverage, so alpha-to-mask-only draws (the rink net) render opaque.
  // Emulate a2c with the working alpha-test path. Opt out: NHL_VK_NO_ATOC_FIX;
  // tune the cutoff with NHL_VK_ATOC_REF (default 0.5, validated on the net).
  bool atoc_fix_on_ = true;
  float atoc_ref_ = 0.5f;

  uint32_t draws_this_frame_ = 0;
  uint32_t last_frame_draws_ = 0;
  uint64_t window_frames_ = 0;
  uint64_t window_draws_ = 0;
  uint64_t frames_total_ = 0;
  std::chrono::steady_clock::time_point window_start_{};
  bool started_ = false;
};

class NhlVkGraphicsSystem : public rex::graphics::vulkan::VulkanGraphicsSystem {
 public:
  std::string name() const override;

 protected:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace nhl::graphics
