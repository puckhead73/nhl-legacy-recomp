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

#include <cstring>

#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>             // Runtime (emulator()) full definition
#include <rex/system/kernel_state.h>  // REX_KERNEL_STATE

#include "renderer/core/nhl_input_gate.h"

namespace {

// Mirror the SDK xam_input.cpp constants.
constexpr uint32_t kXInputFlagGamepad = 0x01;
constexpr uint32_t kXInputFlagAnyUser = 1u << 30;
constexpr uint32_t kXErrorSuccess = 0x0;
constexpr uint32_t kXErrorDeviceNotConnected = 0x48F;

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

  auto* is = static_cast<rex::input::InputSystem*>(
      REX_KERNEL_STATE()->emulator()->input_system());
  return is->GetState(actual_user_index, input_state);
}

}  // namespace

// Override the guest import (no registrar needed: the dispatch table references
// the symbol directly).
REX_HOOK(__imp__XamInputGetState, NhlXamInputGetState);
