// nhllegacy — in-game enhancements / settings overlay (Vulkan-fsi path).
//
// An ImGui dialog that surfaces the "cheap win" enhancement knobs (internal-res
// supersampling, perf HUD) over the live game. It is ALWAYS registered with the
// SDK ImGui drawer so its per-frame OnDraw can poll the controller Guide/PS
// button and toggle visibility even while the window itself is hidden — that
// per-present tick is the only reliable host-side hook for the pad button.
//
// Backend-agnostic by design: it takes the controller poll and the perf source
// as std::function hooks (the app wires them to the Vulkan fps tap +
// rex::input::InputSystem), so this TU pulls in no backend headers. Toggled by
// the Guide/PS button or the F1 keybind (rebindable, shows in the SDK settings
// overlay); adjusted with the mouse.

#pragma once

#include <cstdint>
#include <functional>

#include <rex/ui/imgui_dialog.h>

namespace nhl::ui {

// Plain snapshot the overlay displays; the app fills it from the Vulkan
// IssueSwap fps/draws tap (nhl::graphics::ReadVkPerf).
struct PerfSnapshot {
  double fps = 0.0;
  double frame_ms = 0.0;
  double draws_per_frame = 0.0;
  uint64_t frames_total = 0;
  bool valid = false;
};

// Aggregated controller state the overlay needs for the Guide-button toggle and
// ImGui gamepad navigation. The app fills it from rex::input::InputSystem
// (OR-ing buttons across all pads; left stick from the most-deflected pad), so
// this TU stays free of any backend/input header. `buttons` uses the standard
// XInput bit layout (== rex::input::X_INPUT_GAMEPAD_* values).
struct PadState {
  bool connected = false;
  uint16_t buttons = 0;
  float lx = 0.0f;  // left stick X, normalized -1..1 (+ = right)
  float ly = 0.0f;  // left stick Y, normalized -1..1 (+ = up)
};

class NhlEnhancementsDialog : public rex::ui::ImGuiDialog {
 public:
  // poll_pad: returns the aggregated controller state (Guide button rising edge
  //           toggles the overlay; buttons + stick drive ImGui gamepad nav).
  // perf:     returns the latest perf snapshot for the HUD section.
  // on_exit:  invoked when the user confirms "Exit Game" (app quits cleanly).
  NhlEnhancementsDialog(rex::ui::ImGuiDrawer* drawer,
                        std::function<PadState()> poll_pad,
                        std::function<PerfSnapshot()> perf,
                        std::function<void()> on_exit);
  ~NhlEnhancementsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Feed the current pad state into ImGui's gamepad-nav input queue.
  void FeedGamepadNav(ImGuiIO& io, const PadState& pad);

  std::function<PadState()> poll_pad_;
  std::function<PerfSnapshot()> perf_;
  std::function<void()> on_exit_;
  bool visible_ = false;
  bool prev_guide_ = false;
  bool confirm_exit_ = false;
};

}  // namespace nhl::ui
