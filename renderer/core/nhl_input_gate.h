// Shared flag between the enhancements overlay and the guest-input hook.
//
// Set true by NhlEnhancementsDialog while it is visible; read by the
// XamInputGetState override (src/input_block.cpp) to neutralize the guest's
// controller input so the game ignores the pad while the user drives the
// overlay. Our overlay reads the controller via rex::input::InputSystem
// directly, which this gate does NOT affect — only the guest (xam) path is
// blocked, so menu navigation keeps working.

#pragma once

#include <atomic>

namespace nhl {

// Inline so the single definition is shared across the overlay TU and the hook
// TU without a separate .cpp.
inline std::atomic<bool> g_block_guest_input{false};

}  // namespace nhl
