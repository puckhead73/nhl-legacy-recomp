#include "renderer/core/nhl_overlay.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
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

// Bilinear filtering for guest depth/shadow maps (SDK texture cache). Read
// per-draw, so toggling it takes effect immediately — no restart.
REXCVAR_DECLARE(bool, shadow_filter_linear);
// Extra depth-domain blur passes on depth/shadow maps (0..4). Read per texture
// upload, so it takes effect within a frame or two — no restart.
REXCVAR_DECLARE(int32_t, shadow_softness);

// NHL Legacy color-grade post-process cvars (defined in the SDK Vulkan command
// processor; applied in place on the guest-output image right after the swap
// gamma-apply). All hot-reloadable, so slider edits take effect on the next
// present. Defaults are identity, so the pass is a no-op until enabled.
REXCVAR_DECLARE(bool, present_grade_enable);
REXCVAR_DECLARE(double, present_grade_exposure);
REXCVAR_DECLARE(double, present_grade_contrast);
REXCVAR_DECLARE(double, present_grade_saturation);
REXCVAR_DECLARE(double, present_grade_brightness);
REXCVAR_DECLARE(double, present_grade_temperature);
REXCVAR_DECLARE(double, present_grade_tint);
REXCVAR_DECLARE(double, present_grade_tonemap);

// AMD FidelityFX present-time scaler cvars (UI/Presenter). These only exist when
// the SDK is built with REXGLUE_ENABLE_FIDELITYFX=ON — the same switch that
// defines REX_HAS_FIDELITYFX_SDK (exported as an interface compile-def), so this
// whole control surface compiles in lockstep with the SDK's capability.
#if defined(REX_HAS_FIDELITYFX_SDK)
REXCVAR_DECLARE(std::string, present_effect);
REXCVAR_DECLARE(double, present_cas_additional_sharpness);
REXCVAR_DECLARE(double, present_fsr_sharpness_reduction);
#endif

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
                                             std::function<void()> on_exit,
                                             std::function<void(bool)> set_fullscreen,
                                             std::function<bool()> is_fullscreen,
                                             ITunableStore* tunables,
                                             std::function<void()> dev_scan_stick_list)
    : rex::ui::ImGuiDialog(drawer),
      poll_pad_(std::move(poll_pad)),
      perf_(std::move(perf)),
      on_exit_(std::move(on_exit)),
      set_fullscreen_(std::move(set_fullscreen)),
      is_fullscreen_(std::move(is_fullscreen)),
      tunables_(tunables),
      dev_scan_stick_list_(std::move(dev_scan_stick_list)) {
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

    // --- Display ---
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
      // Display mode: Windowed vs Borderless Fullscreen. Toggles live (the app
      // marshals SetFullscreen to the UI thread) and is persisted for next
      // launch. Exclusive fullscreen is intentionally not offered here — it
      // breaks ReShade/overlays; borderless is the compatible default.
      if (set_fullscreen_) {
        const bool fs = is_fullscreen_ ? is_fullscreen_() : false;
        int mode = fs ? 1 : 0;
        const char* kModes[] = {"Windowed", "Borderless Fullscreen"};
        if (ImGui::Combo("Display mode", &mode, kModes, IM_ARRAYSIZE(kModes))) {
          set_fullscreen_(mode == 1);  // persists internally
        }
      } else {
        ImGui::TextDisabled("Display-mode toggle is Vulkan-path only.");
      }

      // Monitor index: 0 = current/default display, 1.. = a specific monitor.
      // Read once at window open (SDK `monitor` cvar), so restart-required.
      int monitor = nhl::LoadMonitor(/*fallback=*/0);
      if (ImGui::SliderInt("Monitor", &monitor, 0, 8,
                           monitor == 0 ? "default" : "%d")) {
        nhl::SaveMonitor(monitor);
      }
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "[restart]");
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
        nhl::SaveVsync(vsync_on);  // persist so it applies on the next launch
      }
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "[restart]");

      // Soften shadows: restore bilinear filtering on the guest's depth/shadow
      // maps. Read per-draw in the texture cache, so this is live (no restart).
      bool soften_shadows = REXCVAR_GET(shadow_filter_linear);
      if (ImGui::Checkbox("Soften shadows (bilinear)", &soften_shadows)) {
        REXCVAR_SET(shadow_filter_linear, soften_shadows);
        nhl::SaveShadowFilterLinear(soften_shadows);
      }
      ImGui::TextDisabled("smooths hard, pixelated shadow edges (no effect if the "
                          "GPU can't linear-filter depth)");

      // Extra depth-domain blur on shadow maps, beyond bilinear. Applied on upload,
      // so it takes effect within a frame or two. 0 = off.
      int softness = REXCVAR_GET(shadow_softness);
      if (softness < 0) softness = 0;
      if (softness > 4) softness = 4;
      if (ImGui::SliderInt("Shadow softness", &softness, 0, 4,
                           softness == 0 ? "off" : "%d")) {
        REXCVAR_SET(shadow_softness, softness);
        nhl::SaveShadowSoftness(softness);
      }
      ImGui::TextDisabled("extra blur passes for a softer penumbra; higher can mildly "
                          "halo at contact points");
    }

    // --- Upscaling & Sharpening (AMD FidelityFX) ---
#if defined(REX_HAS_FIDELITYFX_SDK)
    if (ImGui::CollapsingHeader("Upscaling & Sharpening (FidelityFX)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      // present_effect is a restart-required SDK cvar; persist the choice so it
      // re-applies at launch (OnPreSetup). Pairs with Supersampling: render at a
      // higher internal res and CAS-sharpen, or FSR-upscale a lower one for perf.
      static const char* kEffects[] = {"bilinear", "cas", "fsr", "fsr2", "fsr3"};
      const std::string cur = REXCVAR_GET(present_effect);
      int idx = 0;
      for (int i = 0; i < IM_ARRAYSIZE(kEffects); ++i) {
        if (cur == kEffects[i]) { idx = i; break; }
      }
      if (ImGui::Combo("Effect", &idx, kEffects, IM_ARRAYSIZE(kEffects))) {
        REXCVAR_SET(present_effect, std::string(kEffects[idx]));
        nhl::SaveFfxEffect(kEffects[idx]);
      }
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "[restart]");
      ImGui::TextDisabled("cas = sharpen only; fsr = spatial upscale; "
                          "fsr2/fsr3 = temporal");

      float cas = static_cast<float>(REXCVAR_GET(present_cas_additional_sharpness));
      if (ImGui::SliderFloat("CAS sharpness", &cas, 0.0f, 1.0f, "%.2f")) {
        REXCVAR_SET(present_cas_additional_sharpness, double(cas));
        nhl::SaveFfxCasSharpness(cas);
      }

      float fsr = static_cast<float>(REXCVAR_GET(present_fsr_sharpness_reduction));
      if (ImGui::SliderFloat("FSR sharpness (stops, lower=sharper)", &fsr, 0.0f,
                             2.0f, "%.2f")) {
        REXCVAR_SET(present_fsr_sharpness_reduction, double(fsr));
        nhl::SaveFfxFsrSharpness(fsr);
      }
    }
#else
    if (ImGui::CollapsingHeader("Upscaling & Sharpening (FidelityFX)")) {
      ImGui::TextDisabled("Unavailable in this build. Rebuild the rexglue SDK");
      ImGui::TextDisabled("with REXGLUE_ENABLE_FIDELITYFX=ON to enable FSR/CAS.");
    }
#endif

    // --- Lighting / Color Grade ---
    // Drives the SDK present_grade_* cvars (an in-place compute pass after the
    // swap gamma-apply). Hot-reloadable: edits take effect on the next present.
    // Each control persists so the look survives a relaunch (OnPreSetup re-applies
    // it). The whole pass is identity until "Enable color grade" is ticked.
    if (ImGui::CollapsingHeader("Lighting / Color Grade", ImGuiTreeNodeFlags_DefaultOpen)) {
      bool grade_on = REXCVAR_GET(present_grade_enable);
      if (ImGui::Checkbox("Enable color grade", &grade_on)) {
        REXCVAR_SET(present_grade_enable, grade_on);
        nhl::SaveGradeEnable(grade_on);
      }

      ImGui::BeginDisabled(!grade_on);

      auto grade_slider = [&](const char* label, const char* fmt, float lo, float hi,
                              double (*getter)(), void (*setter)(double),
                              void (*saver)(double)) {
        float v = static_cast<float>(getter());
        if (ImGui::SliderFloat(label, &v, lo, hi, fmt)) {
          setter(double(v));
          saver(double(v));
        }
      };

      grade_slider(
          "Exposure (EV)", "%.2f", -2.0f, 2.0f, [] { return REXCVAR_GET(present_grade_exposure); },
          [](double v) { REXCVAR_SET(present_grade_exposure, v); }, &nhl::SaveGradeExposure);
      grade_slider(
          "Contrast", "%.2f", 0.5f, 1.5f, [] { return REXCVAR_GET(present_grade_contrast); },
          [](double v) { REXCVAR_SET(present_grade_contrast, v); }, &nhl::SaveGradeContrast);
      grade_slider(
          "Saturation", "%.2f", 0.0f, 2.0f, [] { return REXCVAR_GET(present_grade_saturation); },
          [](double v) { REXCVAR_SET(present_grade_saturation, v); }, &nhl::SaveGradeSaturation);
      grade_slider(
          "Brightness", "%.2f", -0.25f, 0.25f,
          [] { return REXCVAR_GET(present_grade_brightness); },
          [](double v) { REXCVAR_SET(present_grade_brightness, v); }, &nhl::SaveGradeBrightness);
      grade_slider(
          "Temperature (warm+)", "%.2f", -1.0f, 1.0f,
          [] { return REXCVAR_GET(present_grade_temperature); },
          [](double v) { REXCVAR_SET(present_grade_temperature, v); },
          &nhl::SaveGradeTemperature);
      grade_slider(
          "Tint (green+)", "%.2f", -1.0f, 1.0f, [] { return REXCVAR_GET(present_grade_tint); },
          [](double v) { REXCVAR_SET(present_grade_tint, v); }, &nhl::SaveGradeTint);
      grade_slider(
          "Filmic tone-map", "%.2f", 0.0f, 1.0f,
          [] { return REXCVAR_GET(present_grade_tonemap); },
          [](double v) { REXCVAR_SET(present_grade_tonemap, v); }, &nhl::SaveGradeTonemap);

      if (ImGui::Button("Reset grade to neutral")) {
        REXCVAR_SET(present_grade_exposure, 0.0);
        nhl::SaveGradeExposure(0.0);
        REXCVAR_SET(present_grade_contrast, 1.0);
        nhl::SaveGradeContrast(1.0);
        REXCVAR_SET(present_grade_saturation, 1.0);
        nhl::SaveGradeSaturation(1.0);
        REXCVAR_SET(present_grade_brightness, 0.0);
        nhl::SaveGradeBrightness(0.0);
        REXCVAR_SET(present_grade_temperature, 0.0);
        nhl::SaveGradeTemperature(0.0);
        REXCVAR_SET(present_grade_tint, 0.0);
        nhl::SaveGradeTint(0.0);
        REXCVAR_SET(present_grade_tonemap, 0.0);
        nhl::SaveGradeTonemap(0.0);
      }

      ImGui::EndDisabled();
    }

    // --- Engine Tunables (live World-B constants) ---
    DrawTunables();

    // --- Developer Tools (RE diagnostics) ---
    DrawDevTools();

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

namespace {
// A dimmed "(?)" that shows `text` on hover. Used by the curated panels.
void HelpMarker(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// Sort a tunable-tree node's children by segment (and recurse) so the browser
// renders categories alphabetically.
void SortTunNode(TunTreeNode& n) {
  std::sort(n.children.begin(), n.children.end(),
            [](const TunTreeNode& a, const TunTreeNode& b) { return a.seg < b.seg; });
  for (TunTreeNode& c : n.children) SortTunNode(c);
}

// Case-insensitive substring test (needle assumed already lowercased).
bool ContainsCi(const char* hay, const char* needle_lower) {
  if (!needle_lower[0]) return true;
  for (; *hay; ++hay) {
    const char* h = hay;
    const char* n = needle_lower;
    while (*h && *n &&
           static_cast<char>(std::tolower(static_cast<unsigned char>(*h))) == *n) {
      ++h;
      ++n;
    }
    if (!*n) return true;
  }
  return false;
}
}  // namespace

void NhlEnhancementsDialog::RefreshTunableFilter() {
  tun_filtered_.clear();
  if (!tunables_) return;
  char needle[sizeof(tun_filter_)];
  for (size_t i = 0; i < sizeof(needle); ++i)
    needle[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(tun_filter_[i])));
  const size_t count = tunables_->Count();
  for (size_t i = 0; i < count; ++i) {
    const TunableEntry e = tunables_->Info(i);
    if (tun_only_overridden_ && !tunables_->IsOverridden(i)) continue;
    // Match the gXxx name OR the human label (so a search for "injury" finds
    // both gInjury* and "Injury Occurrence Level").
    if (!ContainsCi(e.name, needle) && !(e.label[0] && ContainsCi(e.label, needle)))
      continue;
    tun_filtered_.push_back(static_cast<int>(i));
  }
  RebuildTunableTree();
}

void NhlEnhancementsDialog::EnsureTunableIndex() {
  if (tun_index_built_ || !tunables_) return;
  const size_t count = tunables_->Count();
  tun_by_name_.reserve(count * 2);
  for (size_t i = 0; i < count; ++i)
    tun_by_name_.emplace(tunables_->Info(i).name, static_cast<int>(i));
  tun_index_built_ = true;
}

int NhlEnhancementsDialog::FindTunable(const char* name) const {
  auto it = tun_by_name_.find(name);
  return it == tun_by_name_.end() ? -1 : it->second;
}

void NhlEnhancementsDialog::DrawTunables() {
  if (!tunables_) return;
  if (!ImGui::CollapsingHeader("Engine Tunables")) return;

  using State = ITunableStore::State;
  const State st = tunables_->GetState();
  if (st == State::kUnavailable) {
    ImGui::TextDisabled("No guest memory to scan on this path.");
    return;
  }
  if (st == State::kIdle) {
    ImGui::TextWrapped(
        "Live editor for the engine's 12,000+ gXxx constants (physics, AI, "
        "animation, rules). Friendly panels below; full list under Advanced. "
        "Building the index scans guest memory once.");
    if (ImGui::Button("Build tunable index")) tunables_->RequestBuild();
    return;
  }
  if (st == State::kBuilding) {
    ImGui::TextDisabled("Building tunable index\xE2\x80\xA6 (one-time scan)");
    return;
  }

  // Ready.
  EnsureTunableIndex();
  EnsureSchemaLoaded();
  ImGui::TextDisabled("%zu tunables, %zu overridden", tunables_->Count(),
                      tunables_->OverrideCount());
  if (ImGui::Button("Save")) tunables_->Save();
  ImGui::SameLine();
  if (ImGui::Button("Revert all")) {
    tunables_->ClearAll();
    tun_dirty_filter_ = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(saved values re-apply on next launch)");

  // Schema-driven friendly panels (primary view).
  if (schema_state_ == 1) {
    DrawSchemaPanels();
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                       "tunables_schema.tsv not found next to the exe \xE2\x80\x94 "
                       "raw list only.");
  }

  // The full raw browser (every gXxx by category) stays as a fallback.
  if (ImGui::CollapsingHeader("Advanced \xE2\x80\x94 raw list")) {
    DrawTunableAdvanced();
  }
}

// --- Schema-driven panels -------------------------------------------------

void NhlEnhancementsDialog::EnsureSchemaLoaded() {
  if (schema_state_ != 0) return;
  const std::filesystem::path path =
      rex::filesystem::GetExecutableFolder() / "tunables_schema.tsv";
  std::ifstream f(path);
  if (!f) {
    schema_state_ = 2;  // missing
    return;
  }
  auto to_f = [](const std::string& s, bool& has) -> float {
    if (s.empty()) { has = false; return 0.0f; }
    has = true;
    return std::strtof(s.c_str(), nullptr);
  };
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    // Split into 11 tab-separated fields.
    std::string col[11];
    size_t start = 0;
    int n = 0;
    for (; n < 11; ++n) {
      const size_t tab = line.find('\t', start);
      if (tab == std::string::npos || n == 10) {
        col[n] = line.substr(start);
        ++n;
        break;
      }
      col[n] = line.substr(start, tab - start);
      start = tab + 1;
    }
    if (n < 6 || col[0].empty()) continue;  // need at least name..widget
    SchemaEntry e;
    e.name = col[0];
    // col[1] = panel, col[2] = section
    e.section = col[2];
    e.label = col[3].empty() ? col[0] : col[3];
    e.type = col[4].empty() ? 'f' : col[4][0];
    e.widget = col[5].empty() ? 't' : col[5][0];
    e.vmin = to_f(col[6], e.has_min);
    e.vmax = to_f(col[7], e.has_max);
    e.vstep = to_f(col[8], e.has_step);
    e.unit = col[9];
    e.help = col[10];
    e.store_idx = FindTunable(e.name.c_str());

    const int sidx = static_cast<int>(schema_.size());
    schema_.push_back(std::move(e));
    // Group into panels (first-seen order).
    const std::string& panel = col[1].empty() ? std::string("Misc") : col[1];
    SchemaPanel* p = nullptr;
    for (SchemaPanel& cand : panels_)
      if (cand.name == panel) { p = &cand; break; }
    if (!p) {
      panels_.push_back(SchemaPanel{panel, {}});
      p = &panels_.back();
    }
    p->entries.push_back(sidx);
  }
  // Sort panels alphabetically, "Misc" last; entries within a panel by section.
  std::sort(panels_.begin(), panels_.end(),
            [](const SchemaPanel& a, const SchemaPanel& b) {
              if ((a.name == "Misc") != (b.name == "Misc"))
                return b.name == "Misc";
              return a.name < b.name;
            });
  for (SchemaPanel& p : panels_)
    std::sort(p.entries.begin(), p.entries.end(), [this](int a, int b) {
      if (schema_[a].section != schema_[b].section)
        return schema_[a].section < schema_[b].section;
      return schema_[a].label < schema_[b].label;
    });
  schema_state_ = 1;
}

void NhlEnhancementsDialog::DrawSchemaRow(const SchemaEntry& e) {
  if (e.store_idx < 0) {
    ImGui::TextDisabled("%s \xE2\x80\x94 (n/a)", e.label.c_str());
    return;
  }
  const size_t idx = static_cast<size_t>(e.store_idx);
  const uint32_t bits = tunables_->Get(idx);
  const bool over = tunables_->IsOverridden(idx);

  ImGui::PushID(e.name.c_str());
  ImGui::SetNextItemWidth(180.0f);
  bool changed = false;
  uint32_t new_bits = bits;
  if (e.widget == 'c' || e.type == 'b') {
    bool b = bits != 0;
    if (ImGui::Checkbox("##v", &b)) { new_bits = b ? 1u : 0u; changed = true; }
  } else if (e.type == 'i') {
    int v = static_cast<int>(bits);
    const bool sl = e.widget == 's' && e.has_min && e.has_max;
    if (sl ? ImGui::SliderInt("##v", &v, static_cast<int>(e.vmin),
                              static_cast<int>(e.vmax))
           : ImGui::InputInt("##v", &v, 1, 10)) {
      if (sl) v = v < e.vmin ? static_cast<int>(e.vmin)
                             : (v > e.vmax ? static_cast<int>(e.vmax) : v);
      new_bits = static_cast<uint32_t>(v);
      changed = true;
    }
  } else {  // float
    float v;
    std::memcpy(&v, &bits, 4);
    const bool sl = e.widget == 's' && e.has_min && e.has_max;
    if (sl ? ImGui::SliderFloat("##v", &v, e.vmin, e.vmax, "%.3f")
           : ImGui::InputFloat("##v", &v, 0.0f, 0.0f, "%.4f",
                               ImGuiInputTextFlags_EnterReturnsTrue)) {
      std::memcpy(&new_bits, &v, 4);
      changed = true;
    }
  }
  if (changed) tunables_->Set(idx, new_bits);

  ImGui::SameLine();
  if (over) {
    if (ImGui::SmallButton("\xE2\x86\xBA")) tunables_->Reset(idx);
    ImGui::SameLine();
  }
  // Label (+ unit), orange when overridden.
  const char* lbl = e.label.c_str();
  if (over)
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f), "%s", lbl);
  else
    ImGui::TextUnformatted(lbl);
  if (!e.unit.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", e.unit.c_str());
  }
  if (!e.help.empty()) HelpMarker(e.help.c_str());
  ImGui::PopID();
}

void NhlEnhancementsDialog::DrawSchemaPanels() {
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##schema_filter", "search all settings (name or label)",
                           schema_filter_, sizeof(schema_filter_));
  char needle[sizeof(schema_filter_)];
  for (size_t i = 0; i < sizeof(needle); ++i)
    needle[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(schema_filter_[i])));
  const bool filtering = needle[0] != '\0';

  for (const SchemaPanel& p : panels_) {
    // Gather entries that pass the filter.
    int matches = 0;
    for (int si : p.entries) {
      const SchemaEntry& e = schema_[si];
      if (!filtering || ContainsCi(e.label.c_str(), needle) ||
          ContainsCi(e.name.c_str(), needle))
        ++matches;
    }
    if (matches == 0) continue;

    if (filtering) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    char header[128];
    std::snprintf(header, sizeof(header), "%s (%d)##panel_%s", p.name.c_str(),
                  matches, p.name.c_str());
    if (!ImGui::CollapsingHeader(header)) continue;

    ImGui::Indent();
    std::string cur_section = "\x01";  // sentinel so the first section emits
    for (int si : p.entries) {
      const SchemaEntry& e = schema_[si];
      if (filtering && !ContainsCi(e.label.c_str(), needle) &&
          !ContainsCi(e.name.c_str(), needle))
        continue;
      if (e.section != cur_section) {
        cur_section = e.section;
        if (!cur_section.empty()) ImGui::SeparatorText(cur_section.c_str());
      }
      DrawSchemaRow(e);
    }
    ImGui::Unindent();
  }
}

// --- Advanced raw browser (nested category tree) --------------------------

void NhlEnhancementsDialog::RebuildTunableTree() {
  tun_tree_root_ = TunTreeNode{};
  if (!tunables_) return;
  for (int idx : tun_filtered_) {
    const TunableEntry e = tunables_->Info(static_cast<size_t>(idx));
    TunTreeNode* node = &tun_tree_root_;
    const char* s = e.group;
    while (*s) {
      const char* slash = std::strchr(s, '/');
      const std::string seg = slash ? std::string(s, slash - s) : std::string(s);
      TunTreeNode* child = nullptr;
      for (TunTreeNode& c : node->children)
        if (c.seg == seg) { child = &c; break; }
      if (!child) {
        node->children.push_back(TunTreeNode{seg, {}, {}});
        child = &node->children.back();
      }
      node = child;
      if (!slash) break;
      s = slash + 1;
    }
    node->leaves.push_back(idx);
  }
  SortTunNode(tun_tree_root_);
}

void NhlEnhancementsDialog::RenderTunableTreeNode(const TunTreeNode& node) {
  for (const TunTreeNode& c : node.children) {
    if (ImGui::TreeNode(c.seg.c_str())) {
      RenderTunableTreeNode(c);
      ImGui::TreePop();
    }
  }
  for (int idx : node.leaves) RenderTunableRow(idx);
}

void NhlEnhancementsDialog::RenderTunableRow(int idx) {
  const TunableEntry e = tunables_->Info(static_cast<size_t>(idx));
  const uint32_t bits = tunables_->Get(static_cast<size_t>(idx));
  const bool over = tunables_->IsOverridden(static_cast<size_t>(idx));

  ImGui::PushID(idx);
  ImGui::SetNextItemWidth(150.0f);
  bool changed = false;
  uint32_t new_bits = bits;
  if (tun_hex_) {
    int hv = static_cast<int>(bits);
    if (ImGui::InputInt("##v", &hv, 0, 0,
                        ImGuiInputTextFlags_CharsHexadecimal |
                            ImGuiInputTextFlags_EnterReturnsTrue)) {
      new_bits = static_cast<uint32_t>(hv);
      changed = true;
    }
  } else if (e.type == TunableType::kBool) {
    bool b = bits != 0;
    if (ImGui::Checkbox("##v", &b)) {
      new_bits = b ? 1u : 0u;
      changed = true;
    }
  } else if (e.type == TunableType::kInt) {
    int iv = static_cast<int>(bits);
    if (ImGui::InputInt("##v", &iv, 1, 10,
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
      new_bits = static_cast<uint32_t>(iv);
      changed = true;
    }
  } else {  // float (and unknown — edited as float)
    float fv;
    std::memcpy(&fv, &bits, 4);
    if (ImGui::InputFloat("##v", &fv, 0.0f, 0.0f, "%.4f",
                          ImGuiInputTextFlags_EnterReturnsTrue)) {
      std::memcpy(&new_bits, &fv, 4);
      changed = true;
    }
  }
  if (changed) tunables_->Set(static_cast<size_t>(idx), new_bits);

  ImGui::SameLine();
  if (over) {
    if (ImGui::SmallButton("\xE2\x86\xBA")) tunables_->Reset(static_cast<size_t>(idx));
    ImGui::SameLine();
  }
  // Prefer the human label as the row text; fall back to the gXxx name.
  const char* shown = e.label[0] ? e.label : e.name;
  if (over)
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f), "%s", shown);
  else
    ImGui::TextUnformatted(shown);
  if (ImGui::IsItemHovered()) {
    const char* tn = e.type == TunableType::kBool   ? "bool"
                     : e.type == TunableType::kInt   ? "int"
                     : e.type == TunableType::kFloat ? "float"
                                                     : "unknown";
    float cf;
    std::memcpy(&cf, &e.captured_bits, 4);
    ImGui::SetTooltip("%s\ngroup: %s\ntype: %s%s\nstock: %.4f (0x%08X)", e.name,
                      e.group, tn, e.scalar ? "" : " (non-scalar)", cf,
                      e.captured_bits);
  }
  ImGui::PopID();
}

void NhlEnhancementsDialog::DrawTunableAdvanced() {
  // Filters. Any change marks the filtered list + tree dirty.
  if (ImGui::InputTextWithHint("##tun_filter", "filter by name or label",
                               tun_filter_, sizeof(tun_filter_)))
    tun_dirty_filter_ = true;
  ImGui::Checkbox("Raw hex", &tun_hex_);
  ImGui::SameLine();
  if (ImGui::Checkbox("Only overridden", &tun_only_overridden_))
    tun_dirty_filter_ = true;

  // Rebuild the filtered index + tree when text/flags change.
  if (tun_dirty_filter_ || tun_last_filter_ != tun_filter_) {
    RefreshTunableFilter();  // also rebuilds the tree
    tun_last_filter_ = tun_filter_;
    tun_dirty_filter_ = false;
  }

  ImGui::TextDisabled("%zu shown", tun_filtered_.size());
  if (ImGui::BeginChild("##tun_tree", ImVec2(0, 360), true)) {
    RenderTunableTreeNode(tun_tree_root_);
  }
  ImGui::EndChild();
}

void NhlEnhancementsDialog::DrawDevTools() {
  if (!ImGui::CollapsingHeader("Developer Tools")) return;

  ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                     "\xE2\x9A\xA0 RE diagnostics \xE2\x80\x94 for development.");

  // Stick-picker list scan. The create-player stick picker shows a fixed subset
  // of the on-disk sticks; the list isn't a static data array, so we scan live
  // guest memory for it. Must be run WHILE the stick picker is on-screen.
  ImGui::TextWrapped(
      "Scan stick list: open Create/Edit Player \xE2\x86\x92 the stick picker "
      "FIRST, then click. Walks guest memory for the picker's item list and "
      "writes stick_list_scan.txt next to the exe.");
  if (!dev_scan_stick_list_) {
    ImGui::TextDisabled("(unavailable on this path)");
    return;
  }
  if (ImGui::Button("Scan stick list")) {
    dev_scan_stick_list_();
    dev_stick_scan_fired_ = true;
  }
  if (dev_stick_scan_fired_) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f),
                       "scan dispatched \xE2\x86\x92 stick_list_scan.txt");
  }
}

}  // namespace nhl::ui
