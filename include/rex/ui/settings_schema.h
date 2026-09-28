/**
 * @file        rex/ui/settings_schema.h
 * @brief       Host-supplied settings schema shared by settings surfaces.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

// A title/host describes its user-facing settings once (sections, labels,
// editors, ranges, choices, descriptions, visibility rules and whether a
// change applies while the game runs). Every settings surface - a launcher
// window or the in-game overlay - renders the same description with
// rex/ui/settings_panel.h. The description crosses the embedded plugin
// boundary as TOML text (SerializeSchema / ParseSchema); values are a flat
// key -> string map with the same text the host's configuration writer
// accepts.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::ui::settings {

inline constexpr int kSchemaVersion = 1;

// kDisplay: an integer display index; surfaces show the connected displays
// (detected choices) and fall back to an integer editor.
// kKey: a key or mouse-button binding (a BindingKeyNames() name or empty);
// surfaces capture the next key/button press.
enum class Editor : uint8_t { kBoolean, kInteger, kNumber, kChoice, kDisplay, kKey };

// Names a binding accepts: rex::ui::ParseVirtualKey names (keybinds.cpp)
// plus the mouse wheel pulses of the keyboard/mouse input driver.
inline constexpr std::string_view kBindingKeyNames[] = {
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P",
    "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Backtick", "Minus", "Plus", "Comma", "Period", "Semicolon", "Slash", "Backslash",
    "LBracket", "RBracket", "Quote",
    "Escape", "Return", "Space", "Tab", "Backspace", "Delete", "Insert", "Home", "End",
    "PageUp", "PageDown", "Left", "Right", "Up", "Down", "Shift", "Control", "Alt",
    "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6",
    "Numpad7", "Numpad8", "Numpad9", "NumpadEnter", "NumpadPlus", "NumpadMinus",
    "NumpadStar", "NumpadSlash", "PrintScreen", "Pause", "CapsLock", "NumLock",
    "ScrollLock", "LMB", "RMB", "MMB", "X1", "X2", "WheelUp", "WheelDown"};

inline bool IsBindingKeyName(std::string_view name) {
  for (std::string_view candidate : kBindingKeyNames) {
    if (candidate == name) return true;
  }
  return false;
}

struct Choice {
  std::string value;
  std::string label;
  // Offered only in the advanced view (still shown when it is the value).
  bool advanced = false;
};

struct Setting {
  std::string key;
  std::string section;
  std::string label;
  std::string description;
  std::string units;  // shown after numbers ("degrees", "%")
  Editor editor = Editor::kChoice;
  double minimum = 0.0;
  double maximum = 0.0;
  double step = 0.0;
  double display_multiplier = 1.0;
  std::vector<Choice> choices;
  // Shown only while visible_when_key has visible_when_value (empty: always).
  std::string visible_when_key;
  std::string visible_when_value;
  bool advanced = false;
  // Applied immediately while the game runs (otherwise saved for next start).
  bool live = false;
};

struct Schema {
  std::vector<std::string> sections;
  std::vector<Setting> settings;
  // The host's interface language: right-to-left layout (Arabic) and the
  // panel's own words (English source text -> translation; missing entries
  // stay English). Section, label, description and choice texts arrive
  // already translated.
  bool right_to_left = false;
  std::map<std::string, std::string, std::less<>> text;

  const Setting* Find(std::string_view key) const;
};

// The host's translation of one of the surfaces' own texts (the English text
// when there is none). Templates use {} for values: FormatText fills them.
std::string Translate(const Schema* schema, std::string_view english);
std::string FormatText(std::string_view format, const std::vector<std::string>& values);

using Values = std::map<std::string, std::string, std::less<>>;

std::string SerializeSchema(const Schema& schema);
std::optional<Schema> ParseSchema(std::string_view text, std::string* error = nullptr);

std::string SerializeValues(const Values& values);
std::optional<Values> ParseValues(std::string_view text, std::string* error = nullptr);

// Canonical text for a number at the setting's step (no float noise; always
// carries a decimal point so TOML keeps it a float).
std::string FormatNumberValue(const Setting& setting, double value);

}  // namespace rex::ui::settings
