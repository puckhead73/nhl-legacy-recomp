// Headless animation-decode harness — invoke the real recompiled DctAnimationAsset
// decoder offline (the runtime-hook route is dead; see
// docs/nhl12-decomp-reference/animation/cba-format.md §6).
//
// Milestone 0 (this file): prove the host->guest call mechanism. Build a
// synthetic asset in guest RAM and call the two LEAF decode functions
// (sub_82CE3750 descriptor-builder, sub_82CA7330 size-calc) directly, then
// assert their outputs against hand-derived arithmetic. Both are pure (no
// allocator, no globals), so this validates PPCContext + guest stack + symbol
// linkage with zero live-guest dependency — the precondition for the full
// prime -> seek -> sub_82CA7BD0 ×3 pipeline (milestones 1-3).
//
// Gated by NHL_ANIM_DECODE=<out> in NhllegacyApp::LaunchModule.

#pragma once

namespace rex::memory {
class Memory;
}

namespace nhllegacy {

// Runs the harness against guest memory `mem` (valid after Runtime::Setup),
// writing a human-readable report to `out_path`. Returns true iff every check
// passed. Self-contained: allocates its own guest stack + scratch buffers.
bool RunAnimDecode(rex::memory::Memory* mem, const char* out_path);

// Milestone 1 recon (boot-then-scan): walk committed guest memory for resident
// GD records (magic 0x030e6205 in BOTH byte orders), histogram their type_hashes,
// hexdump DctAnimationAsset (0xf8ff03e3) hits, and locate clip-name strings — to
// determine whether/where a real loaded DctAnimationAsset can be resolved (route
// A). `vbase` = Memory::virtual_membase() (host ptr to guest VA 0). Must run AFTER
// the guest has booted (and ideally loaded anim.cba). Writes report to out_path.
void RunAnimScan(const unsigned char* vbase, const char* out_path);

// Polls (every ~3 s, up to timeout_ms) for anim.cba becoming resident — detected
// by a known clip-name string appearing in HEAP (outside the guest image), which
// only happens once the game loads the bundle (on-ice). Returns true if detected,
// false on timeout. Lets the recon auto-fire at the right moment instead of racing
// a fixed delay against the user navigating into a game.
bool WaitForAnimData(const unsigned char* vbase, unsigned timeout_ms);

}  // namespace nhllegacy
