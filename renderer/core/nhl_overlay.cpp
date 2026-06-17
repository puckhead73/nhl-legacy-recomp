#include "renderer/core/nhl_overlay.h"

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include "renderer/core/nhl_input_gate.h"
#include "renderer/core/nhl_settings.h"

// SDK GPU cvars driven by the cheap-win knobs. draw_resolution_scale_* are
// defined in the SDK texture cache and read at backend init (VulkanTextureCache::
// Create) — changing them live does NOT reallocate the internal buffers, so the
// supersampling slider is restart-required (labelled as such in the UI).
REXCVAR_DECLARE(int32_t, draw_resolution_scale_x);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_y);
REXCVAR_DECLARE(bool, vsync);

namespace nhl::ui {

namespace {
// Standard XInput button bits (mirror rex::input::X_INPUT_GAMEPAD_*); the app
// fills PadState::buttons with these same values.
constexpr uint16_t kPadDpadUp = 0x0001;
constexpr uint16_t kPadDpadDown = 0x0002;
constexpr uint16_t kPadDpadLeft = 0x0004;
constexpr uint16_t kPadDpadRight = 0x0008;
constexpr uint16_t kPadStart = 0x0010;
constexpr uint16_t kPadBack = 0x0020;
constexpr uint16_t kPadLShoulder = 0x0100;
constexpr uint16_t kPadRShoulder = 0x0200;
constexpr uint16_t kPadGuide = 0x0400;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadX = 0x4000;
constexpr uint16_t kPadY = 0x8000;
}  // namespace

NhlEnhancementsDialog::NhlEnhancementsDialog(rex::ui::ImGuiDrawer* drawer,
                                             std::function<PadState()> poll_pad,
                                             std::function<PerfSnapshot()> perf,
                                             std::function<void()> on_exit)
    : rex::ui::ImGuiDialog(drawer),
      poll_pad_(std::move(poll_pad)),
      perf_(std::move(perf)),
      on_exit_(std::move(on_exit)) {
  // Keyboard fallback toggle. Backed by a "Keybinds" cvar so it is rebindable
  // and appears in the SDK settings overlay (F4). The base ReXApp::OnKeyDown
  // dispatches registered binds via ProcessKeyEvent.
  rex::ui::RegisterBind("bind_nhl_overlay", "F1",
                        "Toggle NHL enhancements overlay",
                        [this] { visible_ = !visible_; });
}

NhlEnhancementsDialog::~NhlEnhancementsDialog() {
  rex::ui::UnregisterBind("bind_nhl_overlay");
  // Never leave the guest with input blocked if we're torn down while open.
  nhl::g_block_guest_input.store(false, std::memory_order_relaxed);
}

void NhlEnhancementsDialog::FeedGamepadNav(ImGuiIO& io, const PadState& pad) {
  // ImGui reads these from its input queue on the next NewFrame() (one frame of
  // latency, imperceptible). Feed digital buttons at level (ImGui derives edges)
  // and the left stick as analog so nav movement + slider tweaking both work.
  // ImGui face-button naming is positional: FaceDown = bottom = Xbox A (activate),
  // FaceRight = right = Xbox B (cancel).
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (pad.buttons & kPadDpadUp) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (pad.buttons & kPadDpadDown) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (pad.buttons & kPadDpadLeft) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (pad.buttons & kPadDpadRight) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (pad.buttons & kPadA) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (pad.buttons & kPadB) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, (pad.buttons & kPadX) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceUp, (pad.buttons & kPadY) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadL1, (pad.buttons & kPadLShoulder) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadR1, (pad.buttons & kPadRShoulder) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadStart, (pad.buttons & kPadStart) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadBack, (pad.buttons & kPadBack) != 0);

  // Left stick -> directional analog nav (deadzoned). Each direction is a
  // separate 0..1 magnitude key.
  constexpr float kDeadzone = 0.25f;
  auto analog = [&io](ImGuiKey key, float v) {
    const float m = v > kDeadzone ? v : 0.0f;
    io.AddKeyAnalogEvent(key, m > 0.0f, m);
  };
  analog(ImGuiKey_GamepadLStickLeft, -pad.lx);
  analog(ImGuiKey_GamepadLStickRight, pad.lx);
  analog(ImGuiKey_GamepadLStickUp, pad.ly);
  analog(ImGuiKey_GamepadLStickDown, -pad.ly);
}

void NhlEnhancementsDialog::OnDraw(ImGuiIO& io) {
  // Runs every presented frame (the dialog is always registered). Poll the pad
  // and toggle on the Guide/PS button's rising edge — this works while hidden, so
  // the button both opens AND closes the overlay.
  PadState pad;
  if (poll_pad_) {
    pad = poll_pad_();
    const bool guide = (pad.buttons & kPadGuide) != 0;
    if (guide && !prev_guide_) {
      visible_ = !visible_;
      if (!visible_) {
        confirm_exit_ = false;  // reset the exit confirm when closing
      }
    }
    prev_guide_ = guide;
  }
  // Block the guest's controller input while the overlay is open (the game keeps
  // running but ignores the pad — see src/input_block.cpp). Set every frame so it
  // clears as soon as the overlay closes.
  nhl::g_block_guest_input.store(visible_, std::memory_order_relaxed);
  if (!visible_) {
    return;
  }

  // Enable + feed gamepad navigation while the menu is open so it is fully
  // controller-drivable (no mouse needed). NOTE: the guest still receives the
  // same input — proper input-blocking while the overlay is open is a separate
  // follow-up.
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  FeedGamepadNav(io, pad);

  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);
  if (ImGui::Begin("NHL Legacy \xE2\x80\x94 Enhancements##nhl_enh", nullptr,
                   ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextDisabled("Guide / PS button or F1 to toggle. D-pad / stick + A.");
    ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f),
                       "\xE2\x8F\xB8 Game input is paused while this menu is open.");
    ImGui::Separator();

    // --- Performance HUD (live, from the Vulkan IssueSwap tap) ---
    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
      const PerfSnapshot p = perf_ ? perf_() : PerfSnapshot{};
      if (p.valid && p.frames_total > 0) {
        ImGui::Text("%.1f FPS   %.2f ms / frame", p.fps, p.frame_ms);
        ImGui::Text("%.0f draws / frame", p.draws_per_frame);
        ImGui::TextDisabled("frames rendered: %llu",
                            static_cast<unsigned long long>(p.frames_total));
      } else {
        ImGui::TextDisabled("waiting for first 1s window...");
      }
    }

    // --- Rendering ---
    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
      int scale = REXCVAR_GET(draw_resolution_scale_x);
      if (scale < 1) scale = 1;
      if (scale > 4) scale = 4;
      if (ImGui::SliderInt("Supersampling", &scale, 1, 4, "%dx")) {
        REXCVAR_SET(draw_resolution_scale_x, scale);
        REXCVAR_SET(draw_resolution_scale_y, scale);
        nhl::SaveSupersampling(scale);  // persist so it applies on the next launch
      }
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "[restart]");
      ImGui::TextDisabled("internal %dx%d \xE2\x80\x94 saved; applies on next launch",
                          1280 * scale, 720 * scale);

      bool vsync_on = REXCVAR_GET(vsync);
      if (ImGui::Checkbox("V-Sync", &vsync_on)) {
        REXCVAR_SET(vsync, vsync_on);
      }
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "[restart]");
    }

    // --- Lighting (placeholder) ---
    if (ImGui::CollapsingHeader("Lighting")) {
      ImGui::TextDisabled("Exposure / tone-map knobs need an SDK exposure");
      ImGui::TextDisabled("uniform on the fsi shader path (follow-up).");
    }

    ImGui::Separator();
    if (ImGui::Button("Close")) {
      visible_ = false;
      confirm_exit_ = false;
      nhl::g_block_guest_input.store(false, std::memory_order_relaxed);
    }
    ImGui::SameLine();
    // Exit Game — two-step confirm so it isn't hit by accident mid-game.
    if (!confirm_exit_) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
      if (ImGui::Button("Exit Game")) {
        confirm_exit_ = true;
      }
      ImGui::PopStyleColor();
    } else {
      ImGui::TextUnformatted("Exit to desktop?");
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
      if (ImGui::Button("Confirm Exit") && on_exit_) {
        nhl::g_block_guest_input.store(false, std::memory_order_relaxed);
        on_exit_();
      }
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        confirm_exit_ = false;
      }
    }
  }
  ImGui::End();
}

}  // namespace nhl::ui
