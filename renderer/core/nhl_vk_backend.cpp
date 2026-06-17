#include "renderer/core/nhl_vk_backend.h"

#include <mutex>

#include <rex/logging.h>

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
  // Forward unchanged — rendering is identical to the stock Vulkan backend.
  return rex::graphics::vulkan::VulkanCommandProcessor::IssueDraw(
      primitive_type, index_count, index_buffer_info, major_mode_explicit);
}

void NhlVkCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  // Present the frame via the SDK's own Vulkan presenter, then sample timing.
  rex::graphics::vulkan::VulkanCommandProcessor::IssueSwap(
      frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  const auto now = std::chrono::steady_clock::now();
  last_frame_draws_ = draws_this_frame_;
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
    window_start_ = now;
    window_frames_ = 0;
    window_draws_ = 0;
  }
}

}  // namespace nhl::graphics
