/**
 * @file        rex/ui/settings_panel.h
 * @brief       Schema-driven settings panel (Dear ImGui) for every surface.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

// One panel implementation for the launcher window and the in-game overlay:
// sections on the left, the selected section's settings on the right, each
// with its editor, description, Reset (when a default is known) and, in
// game, whether a change applies now or at the next start. The host owns
// what an edit means (saved for the next start, applied live, both).

#include <rex/ui/settings_schema.h>

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace rex::ui::settings {

enum class Surface : uint8_t { kLauncher, kInGame };

struct PanelModel {
  const Schema* schema = nullptr;
  Values values;    // edits shown by the panel
  Values saved;     // what the configuration file holds
  Values defaults;  // reset targets
  // Host-detected choices replacing a setting's editor (e.g. displays).
  std::map<std::string, std::vector<Choice>, std::less<>> detected_choices;
  // Host-detected facts shown with a setting (e.g. "Your monitor: 144 Hz").
  std::map<std::string, std::string, std::less<>> detected_notes;
  // Host-detected cautions shown with a setting, highlighted (e.g. a texture
  // pack large for this graphics card).
  std::map<std::string, std::string, std::less<>> detected_warnings;
  // Settings this machine can't use (e.g. no texture pack installed): the
  // editor stays disabled at the default; a changed value can still go back.
  std::set<std::string, std::less<>> unavailable;
  std::size_t section = 0;
  bool show_advanced = false;
  // Key editor capture: the setting waiting for a key or mouse button (empty
  // when none); armed once the click that started it is released.
  std::string capture_key;
  bool capture_armed = false;
  std::string capture_note;  // e.g. why a key was refused
};

// True while a key editor waits for input: hosts must not treat keys
// (Escape) as their own shortcuts then.
inline bool IsCapturingKey(const PanelModel& model) { return !model.capture_key.empty(); }

// Display text for a binding name ("LMB" -> "Left mouse", "" -> "(none)").
std::string BindingLabel(std::string_view name);

struct Change {
  std::string key;
  std::string value;
};

// Edit, else saved, else default, else empty.
std::string ValueOf(const PanelModel& model, std::string_view key);
bool IsShown(const PanelModel& model, const Setting& setting);
// Keys whose edit differs from the saved value.
std::vector<std::string> PendingKeys(const PanelModel& model);

// Shared dark theme for every surface (scale = display scale factor).
void ApplyStyle(float scale);

// Draws into the current window's remaining area; returns this frame's edits
// (the model's values already hold them).
std::vector<Change> DrawPanel(PanelModel& model, Surface surface);

}  // namespace rex::ui::settings
