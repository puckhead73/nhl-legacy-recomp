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
