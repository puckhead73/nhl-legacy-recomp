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
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/imgui_dialog.h>

#include "renderer/core/nhl_tunable_store.h"

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

// One node of the Advanced tunable browser's category tree. Built from each
// entry's '/'-separated group path (e.g. "AI/Injuries/Fighting"); `leaves` holds
// the entry indices whose category terminates exactly at this node.
struct TunTreeNode {
  std::string seg;                    // this segment's display label
  std::vector<int> leaves;            // entry indices at this exact path
  std::vector<TunTreeNode> children;  // sub-categories (sorted by seg)
};

// Display metadata for one tunable, loaded from tunables_schema.tsv (the refined
// schema). Drives the friendly schema panels; bound to the live store by name.
struct SchemaEntry {
  std::string name, label, section, unit, help;
  char type = 'f';     // 'f' float | 'i' int | 'b' bool
  char widget = 't';   // 'c' checkbox | 's' slider | 't' stepper
  float vmin = 0.0f, vmax = 0.0f, vstep = 0.0f;
  bool has_min = false, has_max = false, has_step = false;
  int store_idx = -1;  // resolved live-store index (or -1 if not present)
};

// A friendly panel (e.g. "Goalie", "Injuries") = the schema entries grouped by
// their top-level panel name.
struct SchemaPanel {
  std::string name;
  std::vector<int> entries;  // indices into schema_
};

class NhlEnhancementsDialog : public rex::ui::ImGuiDialog {
 public:
  // poll_pad:       returns the aggregated controller state (Guide button rising
  //                 edge toggles the overlay; buttons + stick drive ImGui nav).
  // perf:           returns the latest perf snapshot for the HUD section.
  // on_exit:        invoked when the user confirms "Exit Game" (clean quit).
  // set_fullscreen: requests borderless-fullscreen vs windowed (must marshal to
  //                 the UI thread — the app wires that). Null on non-VK paths.
  // is_fullscreen:  returns the live fullscreen state for the toggle's display.
  // tunables:       live engine-tunable store for the "Engine Tunables" section
  //                 (null disables the section). Owned by the app.
  // dev_scan_stick_list: fire-and-forget dev hook — scans live guest memory for
  //                 the create-player stick picker's item list, writing a report
  //                 next to the exe. Surfaced under "Developer Tools"; null hides
  //                 the button. Invoke it WHILE the stick picker is on-screen.
  NhlEnhancementsDialog(rex::ui::ImGuiDrawer* drawer,
                        std::function<PadState()> poll_pad,
                        std::function<PerfSnapshot()> perf,
                        std::function<void()> on_exit,
                        std::function<void(bool)> set_fullscreen = {},
                        std::function<bool()> is_fullscreen = {},
                        ITunableStore* tunables = nullptr,
                        std::function<void()> dev_scan_stick_list = {});
  ~NhlEnhancementsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Feed the current pad state into ImGui's gamepad-nav input queue.
  void FeedGamepadNav(ImGuiIO& io, const PadState& pad);

  // "Engine Tunables" section (live World-B constant editor).
  void DrawTunables();
  // Recompute filtered_ from the current search text + group selection.
  void RefreshTunableFilter();

  // Schema-driven friendly panels: load tunables_schema.tsv (refined label / type
  // / widget / range / unit / help per gXxx) and auto-render every panel, each
  // control bound to the live store by name.
  void EnsureSchemaLoaded();
  void DrawSchemaPanels();
  void DrawSchemaRow(const SchemaEntry& e);

  // Advanced raw browser: name filter + nested category tree over all tunables.
  void DrawTunableAdvanced();
  void RebuildTunableTree();          // rebuild tun_tree_root_ from tun_filtered_
  void RenderTunableTreeNode(const TunTreeNode& node);
  void RenderTunableRow(int idx);     // one value-editor row for entry `idx`

  // Lazily build the name -> entry-index map once the store is kReady.
  void EnsureTunableIndex();
  int FindTunable(const char* name) const;  // -1 if absent

  std::function<PadState()> poll_pad_;
  std::function<PerfSnapshot()> perf_;
  std::function<void()> on_exit_;
  std::function<void(bool)> set_fullscreen_;
  std::function<bool()> is_fullscreen_;
  ITunableStore* tunables_ = nullptr;
  std::function<void()> dev_scan_stick_list_;

  // "Developer Tools" section (RE diagnostics over the live guest).
  void DrawDevTools();

  bool visible_ = false;
  bool prev_guide_ = false;
  bool confirm_exit_ = false;
  bool dev_stick_scan_fired_ = false;  // latches the one-shot scan trigger

  // Tunable browser state.
  char tun_filter_[96] = {0};   // case-insensitive name/label substring
  bool tun_hex_ = false;        // edit every row as raw hex
  bool tun_only_overridden_ = false;
  bool tun_dirty_filter_ = true;        // filtered_ + tree need a rebuild
  std::string tun_last_filter_;         // detects filter-text changes
  std::vector<int> tun_filtered_;       // entry indices passing the filter
  TunTreeNode tun_tree_root_;           // nested category tree over tun_filtered_

  // Curated-panel support: name -> entry index, built once at kReady.
  std::unordered_map<std::string, int> tun_by_name_;
  bool tun_index_built_ = false;

  // Schema-driven view (loaded from tunables_schema.tsv next to the exe).
  std::vector<SchemaEntry> schema_;
  std::vector<SchemaPanel> panels_;
  int schema_state_ = 0;        // 0 = unloaded, 1 = loaded, 2 = file missing
  char schema_filter_[96] = {0};
};

}  // namespace nhl::ui
