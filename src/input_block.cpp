// Guest controller-input block for the enhancements overlay (Vulkan build only).
//
// While the overlay is open we want the game to ignore the pad (so navigating
// the menu doesn't also drive gameplay), but the overlay itself must still read
// the controller. The SDK input drivers' GetState ignores the per-driver active
// callback, so that mechanism can't gate controller input. The clean seam is the
// guest-only entry point XamInputGetState: the recomp dispatch table maps guest
// address 0x8398CD6C -> __imp__XamInputGetState, so defining that symbol here
// overrides the SDK's. We replicate the SDK's flag/user handling exactly and,
// when the overlay flag is set, return a connected controller with neutral state.
//
// The overlay's own controller poll calls rex::input::InputSystem::GetState
// directly (not through xam), so it is unaffected — nav keeps working while the
// guest is blocked. This TU is compiled ONLY in the Vulkan build, so the default
// D3D12 build's input path is byte-for-byte unchanged.

#include <chrono>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>             // Runtime (emulator()) full definition
#include <rex/system/kernel_state.h>  // REX_KERNEL_STATE

#include "renderer/core/nhl_input_gate.h"
#include "renderer/core/nhl_vk_backend.h"  // ReadVkPerf (draws/frame)

namespace {

// Mirror the SDK xam_input.cpp constants.
constexpr uint32_t kXInputFlagGamepad = 0x01;
constexpr uint32_t kXInputFlagAnyUser = 1u << 30;
constexpr uint32_t kXErrorSuccess = 0x0;
constexpr uint32_t kXErrorDeviceNotConnected = 0x48F;

// --- Scripted controller (NHL_AUTOPLAY=1) ---------------------------------
// Unattended test/profiling driver: replaces pad 0 with a synthetic one so a
// session can reach and play a match with nobody at the controls. In menus
// (few draws per frame) it taps A, with an occasional Start for "press start"
// screens; once the frame's draw count says a match is on screen
// (NHL_AUTOPLAY_MATCH_DRAWS, default 900) it skates around, passes and shoots
// with a deterministic pseudo-random pattern and never presses Start (pause).
bool AutoplayEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("NHL_AUTOPLAY");
    return e && *e && *e != '0';
  }();
  return on;
}

uint32_t AutoHash(uint32_t x) {
  x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
  return x;
}

void AutoplayFill(rex::input::X_INPUT_STATE* dst) {
  using clock = std::chrono::steady_clock;
  static const clock::time_point t0 = clock::now();
  static const double match_draws = [] {
    const char* e = std::getenv("NHL_AUTOPLAY_MATCH_DRAWS");
    return e && *e ? std::atof(e) : 900.0;
  }();
  const uint32_t ms = uint32_t(
      std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
  const auto perf = nhl::graphics::ReadVkPerf();
  const bool in_match = perf.valid && perf.draws_per_frame >= match_draws;

  // NHL_AUTOPLAY_START=<seconds>: stay neutral that long after boot.
  static const uint32_t start_ms = [] {
    const char* e = std::getenv("NHL_AUTOPLAY_START");
    return e && *e ? uint32_t(std::atof(e) * 1000.0) : 0u;
  }();

  uint16_t buttons = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint8_t rt = 0;
  if (ms < start_ms) {
    // neutral
  } else if (!in_match) {
    // One tap every 1.5 s, held 150 ms. Every 6th tap is Start instead of A.
    const uint32_t slot = ms / 1500, phase = ms % 1500;
    if (phase < 150) {
      buttons = (slot % 6 == 5) ? rex::input::X_INPUT_GAMEPAD_START
                                : rex::input::X_INPUT_GAMEPAD_A;
    }
  } else {
    // Skate: new direction every 1.2 s; sprint most of the time.
    const uint32_t h = AutoHash(ms / 1200 + 0x9E3779B9u);
    static const int16_t kDir[8][2] = {{32767, 0},      {23170, 23170},  {0, 32767},
                                       {-23170, 23170}, {-32767, 0},     {-23170, -23170},
                                       {0, -32767},     {23170, -23170}};
    lx = kDir[h & 7][0];
    ly = kDir[h & 7][1];
    if ((h >> 3) & 3) rt = 255;
    // Every 700 ms: pass (A), shoot (right stick up), check (right stick
    // sideways), or nothing. Held 120 ms. The A taps also dismiss replays and
    // intermission screens.
    const uint32_t g = AutoHash(ms / 700 + 0x51ED270Bu), phase = ms % 700;
    if (phase < 120) {
      switch (g % 5) {
        case 0: case 3: buttons = rex::input::X_INPUT_GAMEPAD_A; break;
        case 1: ry = 32767; break;
        case 2: rx = (g & 0x100) ? 32767 : -32767; break;
        default: break;
      }
    }
  }
  std::memset(dst, 0, sizeof(*dst));
  dst->packet_number = ms / 8 + 1;
  dst->gamepad.buttons = buttons;
  dst->gamepad.right_trigger = rt;
  dst->gamepad.thumb_lx = lx;
  dst->gamepad.thumb_ly = ly;
  dst->gamepad.thumb_rx = rx;
  dst->gamepad.thumb_ry = ry;
}

u32 NhlXamInputGetState(u32 user_index, u32 flags,
                        ppc_ptr_t<rex::input::X_INPUT_STATE> input_state) {
  // Same flag/user handling as the SDK's XamInputGetState_entry.
  if ((flags & 0xFF) && (flags & kXInputFlagGamepad) == 0) {
    return kXErrorDeviceNotConnected;
  }
  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & kXInputFlagAnyUser)) {
    actual_user_index = 0;
  }

  // Overlay open -> connected controller, neutral state (zero == no buttons,
  // sticks centered; works for the big-endian guest fields too).
  if (nhl::g_block_guest_input.load(std::memory_order_relaxed) && input_state) {
    rex::input::X_INPUT_STATE* dst = input_state;
    std::memset(dst, 0, sizeof(*dst));
    return kXErrorSuccess;
  }

  if (AutoplayEnabled() && actual_user_index == 0 && input_state) {
    AutoplayFill(input_state);
    return kXErrorSuccess;
  }

  auto* is = static_cast<rex::input::InputSystem*>(
      REX_KERNEL_STATE()->emulator()->input_system());
  return is->GetState(actual_user_index, input_state);
}

}  // namespace

// Override the guest import (no registrar needed: the dispatch table references
// the symbol directly).
REX_HOOK(__imp__XamInputGetState, NhlXamInputGetState);
