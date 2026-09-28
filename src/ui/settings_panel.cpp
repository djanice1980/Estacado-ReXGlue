/**
 * @file        ui/settings_panel.cpp
 * @brief       Schema-driven settings panel (Dear ImGui).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/settings_panel.h>

#include <rex/ui/rtl_text.h>

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace rex::ui::settings {
namespace {

constexpr ImVec4 kText{0.90f, 0.89f, 0.87f, 1.00f};
constexpr ImVec4 kTextDim{0.60f, 0.59f, 0.57f, 1.00f};
constexpr ImVec4 kLive{0.45f, 0.78f, 0.52f, 1.00f};
constexpr ImVec4 kNextStart{0.86f, 0.66f, 0.36f, 1.00f};
constexpr ImVec4 kChanged{0.92f, 0.45f, 0.40f, 1.00f};

bool Rtl(const PanelModel& model) { return model.schema && model.schema->right_to_left; }

std::string Tr(const PanelModel& model, std::string_view english) {
  return Translate(model.schema, english);
}

// Text as ImGui draws it: shaped and in visual order when right-to-left.
std::string Shown(const PanelModel& model, std::string_view text) {
  return Rtl(model) ? rtl::VisualLine(text) : std::string(text);
}

float TextWidth(const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x; }

// One line of text; right-aligned in the current column when right-to-left.
void Line(const PanelModel& model, std::string_view text) {
  if (!Rtl(model)) {
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    return;
  }
  const std::string shown = rtl::VisualLine(text);
  const float width = TextWidth(shown);
  const float available = ImGui::GetContentRegionAvail().x;
  if (available > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
  ImGui::TextUnformatted(shown.c_str());
}

// A wrapped paragraph (the current wrap width is the column); right-aligned
// lines in reading order when right-to-left.
void Paragraph(const PanelModel& model, std::string_view text) {
  if (!Rtl(model)) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    return;
  }
  const float available = ImGui::GetContentRegionAvail().x;
  const float x = ImGui::GetCursorPosX();
  for (const std::string& line : rtl::WrapVisual(text, available, TextWidth)) {
    ImGui::SetCursorPosX(x + (std::max)(0.0f, available - TextWidth(line)));
    ImGui::TextUnformatted(line.c_str());
  }
}

bool ParseDouble(const std::string& text, double& value) {
  if (text.empty()) return false;
  char* end = nullptr;
  value = std::strtod(text.c_str(), &end);
  return end && *end == '\0' && std::isfinite(value);
}

int DisplayDecimals(const Setting& setting) {
  const double step = setting.step * setting.display_multiplier;
  if (step <= 0.0 || step >= 1.0) return 0;
  return std::clamp(int(std::ceil(-std::log10(step) - 1e-9)), 0, 6);
}

const std::vector<Choice>& ChoicesOf(const PanelModel& model, const Setting& setting) {
  if (const auto detected = model.detected_choices.find(setting.key);
      detected != model.detected_choices.end()) {
    return detected->second;
  }
  return setting.choices;
}

std::string ChoiceLabel(const PanelModel& model, const Setting& setting,
                        const std::string& value) {
  for (const Choice& choice : ChoicesOf(model, setting)) {
    if (choice.value == value) return choice.label;
  }
  return value.empty() ? Tr(model, "(not set)") : value;
}

void Badge(const char* text, const ImVec4& color) {
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::PopStyleColor();
}

void Set(PanelModel& model, const Setting& setting, std::string value,
         std::vector<Change>& changes) {
  const bool unchanged = ValueOf(model, setting.key) == value;
  model.values[setting.key] = value;
  if (!unchanged) changes.push_back({setting.key, std::move(value)});
}

// Binding name of the key pressed this frame (ParseVirtualKey names; the
// left/right modifier keys share one binding like the input driver's).
std::string PressedBindingKey() {
  struct Named {
    ImGuiKey key;
    const char* name;
  };
  static constexpr Named kNamed[] = {
      {ImGuiKey_Tab, "Tab"}, {ImGuiKey_LeftArrow, "Left"}, {ImGuiKey_RightArrow, "Right"},
      {ImGuiKey_UpArrow, "Up"}, {ImGuiKey_DownArrow, "Down"}, {ImGuiKey_PageUp, "PageUp"},
      {ImGuiKey_PageDown, "PageDown"}, {ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"},
      {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Delete, "Delete"},
      {ImGuiKey_Backspace, "Backspace"}, {ImGuiKey_Space, "Space"}, {ImGuiKey_Enter, "Return"},
      {ImGuiKey_Escape, "Escape"}, {ImGuiKey_LeftCtrl, "Control"},
      {ImGuiKey_RightCtrl, "Control"}, {ImGuiKey_LeftShift, "Shift"},
      {ImGuiKey_RightShift, "Shift"}, {ImGuiKey_LeftAlt, "Alt"}, {ImGuiKey_RightAlt, "Alt"},
      {ImGuiKey_Apostrophe, "Quote"}, {ImGuiKey_Comma, "Comma"}, {ImGuiKey_Minus, "Minus"},
      {ImGuiKey_Period, "Period"}, {ImGuiKey_Slash, "Slash"},
      {ImGuiKey_Semicolon, "Semicolon"}, {ImGuiKey_Equal, "Plus"},
      {ImGuiKey_LeftBracket, "LBracket"}, {ImGuiKey_Backslash, "Backslash"},
      {ImGuiKey_RightBracket, "RBracket"}, {ImGuiKey_GraveAccent, "Backtick"},
      {ImGuiKey_CapsLock, "CapsLock"}, {ImGuiKey_ScrollLock, "ScrollLock"},
      {ImGuiKey_NumLock, "NumLock"}, {ImGuiKey_PrintScreen, "PrintScreen"},
      {ImGuiKey_Pause, "Pause"}, {ImGuiKey_KeypadDivide, "NumpadSlash"},
      {ImGuiKey_KeypadMultiply, "NumpadStar"}, {ImGuiKey_KeypadSubtract, "NumpadMinus"},
      {ImGuiKey_KeypadAdd, "NumpadPlus"}, {ImGuiKey_KeypadEnter, "NumpadEnter"}};
  for (const Named& named : kNamed) {
    if (ImGui::IsKeyPressed(named.key, false)) return named.name;
  }
  for (int i = 0; i < 26; ++i) {
    if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_A + i), false)) return std::string(1, char('A' + i));
  }
  for (int i = 0; i < 10; ++i) {
    if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_0 + i), false)) return std::string(1, char('0' + i));
    if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_Keypad0 + i), false)) {
      return "Numpad" + std::to_string(i);
    }
  }
  for (int i = 0; i < 24; ++i) {
    if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_F1 + i), false)) return "F" + std::to_string(i + 1);
  }
  return {};
}

// F1-F12 belong to the host (settings overlay, debug overlay, markers,
// screenshots): never a game binding.
bool IsReservedHostKey(std::string_view name) {
  if (name.size() < 2 || name[0] != 'F') return false;
  const int number = std::atoi(std::string(name.substr(1)).c_str());
  return number >= 1 && number <= 12;
}

void DrawKeyEditor(PanelModel& model, const Setting& setting, const std::string& current,
                   std::vector<Change>& changes, float width) {
  if (model.capture_key != setting.key) {
    const std::string label = Shown(model, Tr(model, BindingLabel(current))) + "##key";
    if (ImGui::Button(label.c_str(), ImVec2(width, 0.0f))) {
      model.capture_key = setting.key;
      model.capture_armed = false;
      model.capture_note.clear();
    }
    return;
  }
  const ImGuiStyle& style = ImGui::GetStyle();
  const std::string cancel_text = Shown(model, Tr(model, "Cancel"));
  const float cancel_width = TextWidth(cancel_text) + style.FramePadding.x * 2.0f;
  const float field_width =
      (std::max)(width - cancel_width - style.ItemSpacing.x, ImGui::GetFontSize() * 4.0f);
  const std::string prompt =
      Shown(model, model.capture_note.empty() ? Tr(model, "Press a key or mouse button")
                                              : model.capture_note) +
      "##capture";
  ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
  ImGui::Button(prompt.c_str(), ImVec2(field_width, 0.0f));
  ImGui::PopStyleColor();
  ImGui::SameLine();
  const bool cancel = ImGui::Button((cancel_text + "##cancel").c_str());
  const bool over_cancel = ImGui::IsItemHovered();
  if (cancel) {
    model.capture_key.clear();
    model.capture_note.clear();
    return;
  }
  // The click that started the capture must be released first.
  if (!model.capture_armed) {
    if (!ImGui::IsAnyMouseDown()) model.capture_armed = true;
    return;
  }
  std::string captured = PressedBindingKey();
  if (captured.empty() && !over_cancel) {
    static constexpr const char* kMouse[] = {"LMB", "RMB", "MMB", "X1", "X2"};
    for (int button = 0; button < 5; ++button) {
      if (ImGui::IsMouseClicked(button)) {
        captured = kMouse[button];
        break;
      }
    }
  }
  if (captured.empty()) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel > 0.0f) captured = "WheelUp";
    if (wheel < 0.0f) captured = "WheelDown";
  }
  if (captured.empty()) return;
  if (IsReservedHostKey(captured)) {
    model.capture_note = FormatText(Tr(model, "{} is reserved - press another key"), {captured});
    return;
  }
  model.capture_key.clear();
  model.capture_note.clear();
  Set(model, setting, captured, changes);
}

void DrawChoice(PanelModel& model, const Setting& setting, const std::string& current,
                std::vector<Change>& changes, float width) {
  const std::string preview = Shown(model, ChoiceLabel(model, setting, current));
  ImGui::SetNextItemWidth(width);
  if (ImGui::BeginCombo("##value", preview.c_str())) {
    if (Rtl(model)) ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(1.0f, 0.5f));
    for (const Choice& choice : ChoicesOf(model, setting)) {
      const bool selected = choice.value == current;
      if (choice.advanced && !model.show_advanced && !selected) continue;
      const std::string item = Shown(model, choice.label) + "##" + choice.value;
      if (ImGui::Selectable(item.c_str(), selected)) {
        Set(model, setting, choice.value, changes);
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    if (Rtl(model)) ImGui::PopStyleVar();
    ImGui::EndCombo();
  }
}

void DrawEditor(PanelModel& model, const Setting& setting, std::vector<Change>& changes,
                float width) {
  const std::string current = ValueOf(model, setting.key);
  const bool detected = model.detected_choices.count(setting.key) != 0;
  if (detected || setting.editor == Editor::kChoice) {
    DrawChoice(model, setting, current, changes, width);
    return;
  }
  switch (setting.editor) {
    case Editor::kBoolean: {
      bool on = current == "true";
      if (ImGui::Checkbox("##value", &on)) Set(model, setting, on ? "true" : "false", changes);
      ImGui::SameLine();
      ImGui::TextUnformatted(Shown(model, Tr(model, on ? "On" : "Off")).c_str());
      break;
    }
    case Editor::kInteger:
    case Editor::kDisplay: {
      double parsed = setting.minimum;
      ParseDouble(current, parsed);
      int value = int(std::lround(parsed));
      ImGui::SetNextItemWidth(width);
      if (ImGui::SliderInt("##value", &value, int(setting.minimum), int(setting.maximum), "%d",
                           ImGuiSliderFlags_AlwaysClamp)) {
        Set(model, setting, std::to_string(value), changes);
      }
      break;
    }
    case Editor::kNumber: {
      double parsed = setting.minimum;
      ParseDouble(current, parsed);
      float shown = float(parsed * setting.display_multiplier);
      const float low = float(setting.minimum * setting.display_multiplier);
      const float high = float(setting.maximum * setting.display_multiplier);
      std::string units = setting.units == "%" ? "%%" : setting.units;
      char format[48];
      std::snprintf(format, sizeof(format), "%%.%df%s%s", DisplayDecimals(setting),
                    units.empty() || units == "%%" ? "" : " ", units.c_str());
      ImGui::SetNextItemWidth(width);
      if (ImGui::SliderFloat("##value", &shown, low, high, format,
                             ImGuiSliderFlags_AlwaysClamp)) {
        double value = double(shown) / setting.display_multiplier;
        if (setting.step > 0.0) {
          value = setting.minimum +
                  std::round((value - setting.minimum) / setting.step) * setting.step;
        }
        value = std::clamp(value, setting.minimum, setting.maximum);
        Set(model, setting, FormatNumberValue(setting, value), changes);
      }
      break;
    }
    case Editor::kKey:
      DrawKeyEditor(model, setting, current, changes, width);
      break;
    case Editor::kChoice:
      break;
  }
}

void DrawSetting(PanelModel& model, const Setting& setting, Surface surface,
                 std::vector<Change>& changes) {
  const bool rtl = Rtl(model);
  ImGui::PushID(setting.key.c_str());
  ImGui::TableNextRow();
  // Right-to-left: the label column is on the right.
  ImGui::TableSetColumnIndex(rtl ? 1 : 0);
  ImGui::AlignTextToFramePadding();
  std::vector<std::pair<std::string, ImVec4>> badges;
  if (surface == Surface::kInGame) {
    badges.push_back({Tr(model, setting.live ? "applies now" : "next start"),
                      setting.live ? kLive : kNextStart});
  }
  const auto edited = model.values.find(setting.key);
  const auto saved = model.saved.find(setting.key);
  if (edited != model.values.end() &&
      (saved == model.saved.end() || saved->second != edited->second)) {
    badges.push_back({Tr(model, "changed"), kChanged});
  }
  if (!rtl) {
    ImGui::TextUnformatted(setting.label.c_str());
    for (const auto& [text, color] : badges) Badge(text.c_str(), color);
  } else {
    Line(model, setting.label);
    if (!badges.empty()) {
      // Badges on their own line, the first one rightmost.
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
      std::vector<std::string> shown;
      float total = 0.0f;
      for (auto it = badges.rbegin(); it != badges.rend(); ++it) {
        shown.push_back(Shown(model, it->first));
        total += TextWidth(shown.back());
      }
      total += ImGui::GetStyle().ItemSpacing.x * float(shown.size() - 1);
      const float available = ImGui::GetContentRegionAvail().x;
      if (available > total) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - total);
      for (size_t i = 0; i < shown.size(); ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, badges[badges.size() - 1 - i].second);
        ImGui::TextUnformatted(shown[i].c_str());
        ImGui::PopStyleColor();
      }
      ImGui::PopFont();
    }
  }
  auto small_paragraph = [&](const std::string& text, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
    Paragraph(model, text);
    ImGui::PopFont();
    ImGui::PopStyleColor();
  };
  if (const auto note = model.detected_notes.find(setting.key);
      note != model.detected_notes.end() && !note->second.empty()) {
    small_paragraph(note->second, kLive);
  }
  if (const auto warning = model.detected_warnings.find(setting.key);
      warning != model.detected_warnings.end() && !warning->second.empty()) {
    small_paragraph(warning->second, kNextStart);
  }
  if (!setting.description.empty()) small_paragraph(setting.description, kTextDim);

  ImGui::TableSetColumnIndex(rtl ? 0 : 1);
  const auto fallback = model.defaults.find(setting.key);
  const bool can_reset =
      fallback != model.defaults.end() && fallback->second != ValueOf(model, setting.key);
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const std::string reset_text = Shown(model, Tr(model, "Reset"));
  const float reset_width = TextWidth(reset_text) + ImGui::GetStyle().FramePadding.x * 2;
  const float cell_x = ImGui::GetCursorPosX();
  const float editor_width = (std::max)(ImGui::GetContentRegionAvail().x - reset_width - spacing,
                                        ImGui::GetFontSize() * 4.0f);
  const bool locked = model.unavailable.count(setting.key) != 0 &&
                      (fallback == model.defaults.end() ||
                       fallback->second == ValueOf(model, setting.key));
  auto reset_button = [&]() {
    if (ImGui::Button((reset_text + "##reset").c_str())) {
      Set(model, setting, fallback->second, changes);
    }
    if (ImGui::IsItemHovered()) {
      const std::string label =
          setting.editor == Editor::kChoice ? ChoiceLabel(model, setting, fallback->second)
          : setting.editor == Editor::kKey  ? Tr(model, BindingLabel(fallback->second))
                                            : fallback->second;
      ImGui::SetTooltip(
          "%s", Shown(model, FormatText(Tr(model, "Back to the default ({})"), {label})).c_str());
    }
  };
  if (rtl) {
    // Mirrored: Reset on the left, the editor on the right.
    if (can_reset) {
      reset_button();
      ImGui::SameLine();
    }
    ImGui::SetCursorPosX(cell_x + reset_width + spacing);
    ImGui::BeginDisabled(locked);
    DrawEditor(model, setting, changes, editor_width);
    ImGui::EndDisabled();
  } else {
    ImGui::BeginDisabled(locked);
    DrawEditor(model, setting, changes, editor_width);
    ImGui::EndDisabled();
    if (can_reset) {
      // SameLine(x) inside a table cell measures from the column; the cursor
      // position helpers are window-relative like cell_x.
      ImGui::SameLine();
      ImGui::SetCursorPosX(cell_x + editor_width + spacing);
      reset_button();
    }
  }
  if (setting.editor == Editor::kKey && model.schema) {
    // One key may drive two actions; say so rather than refuse it.
    const std::string current = ValueOf(model, setting.key);
    std::string shared;
    for (const Setting& other : model.schema->settings) {
      if (other.editor != Editor::kKey || other.key == setting.key || current.empty()) continue;
      if (ValueOf(model, other.key) == current) {
        shared += (shared.empty() ? "" : ", ") + other.label;
      }
    }
    if (!shared.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kNextStart);
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
      Paragraph(model, FormatText(Tr(model, "Also bound to {}"), {shared}));
      ImGui::PopFont();
      ImGui::PopStyleColor();
    }
  }
  ImGui::PopID();
}

}  // namespace

std::string BindingLabel(std::string_view name) {
  struct Label {
    std::string_view name;
    const char* text;
  };
  static constexpr Label kLabels[] = {
      {"", "(none)"},          {"LMB", "Left mouse"},     {"RMB", "Right mouse"},
      {"MMB", "Middle mouse"}, {"X1", "Mouse 4"},         {"X2", "Mouse 5"},
      {"WheelUp", "Wheel up"}, {"WheelDown", "Wheel down"}, {"Return", "Enter"},
      {"LBracket", "["},       {"RBracket", "]"},         {"Backtick", "`"},
      {"Quote", "'"},          {"Semicolon", ";"},        {"Comma", ","},
      {"Period", "."},         {"Slash", "/"},            {"Backslash", "\\"},
      {"Minus", "-"},          {"Plus", "="},             {"Control", "Ctrl"}};
  for (const Label& label : kLabels) {
    if (label.name == name) return label.text;
  }
  return std::string(name);
}

std::string ValueOf(const PanelModel& model, std::string_view key) {
  if (const auto it = model.values.find(key); it != model.values.end()) return it->second;
  if (const auto it = model.saved.find(key); it != model.saved.end()) return it->second;
  if (const auto it = model.defaults.find(key); it != model.defaults.end()) return it->second;
  return {};
}

bool IsShown(const PanelModel& model, const Setting& setting) {
  if (setting.advanced && !model.show_advanced) return false;
  if (!setting.visible_when_key.empty() &&
      ValueOf(model, setting.visible_when_key) != setting.visible_when_value) {
    return false;
  }
  return true;
}

std::vector<std::string> PendingKeys(const PanelModel& model) {
  std::vector<std::string> keys;
  for (const auto& [key, value] : model.values) {
    const auto saved = model.saved.find(key);
    if (saved == model.saved.end() || saved->second != value) keys.push_back(key);
  }
  return keys;
}

void ApplyStyle(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  const float font_size = style.FontSizeBase;
  style = ImGuiStyle();
  style.FontSizeBase = font_size;
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 6.0f;
  style.ChildRounding = 4.0f;
  style.FrameRounding = 4.0f;
  style.PopupRounding = 4.0f;
  style.GrabRounding = 3.0f;
  style.ScrollbarRounding = 4.0f;
  style.WindowPadding = ImVec2(14.0f, 12.0f);
  style.FramePadding = ImVec2(10.0f, 6.0f);
  style.ItemSpacing = ImVec2(10.0f, 8.0f);
  style.CellPadding = ImVec2(8.0f, 9.0f);
  style.GrabMinSize = 12.0f;
  style.WindowBorderSize = 1.0f;
  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = kText;
  colors[ImGuiCol_TextDisabled] = kTextDim;
  colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.075f, 0.97f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.09f, 0.095f, 1.00f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.105f, 0.98f);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
  colors[ImGuiCol_Border] = ImVec4(0.22f, 0.20f, 0.20f, 1.00f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.155f, 1.00f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.19f, 0.19f, 1.00f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.20f, 0.20f, 1.00f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.07f, 0.075f, 1.00f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.08f, 0.08f, 1.00f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.86f, 0.30f, 0.26f, 1.00f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.72f, 0.24f, 0.21f, 1.00f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(0.88f, 0.32f, 0.27f, 1.00f);
  colors[ImGuiCol_Button] = ImVec4(0.20f, 0.19f, 0.19f, 1.00f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.42f, 0.17f, 0.15f, 1.00f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.20f, 0.17f, 1.00f);
  colors[ImGuiCol_Header] = ImVec4(0.40f, 0.14f, 0.12f, 0.80f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.46f, 0.18f, 0.15f, 0.80f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.20f, 0.17f, 1.00f);
  colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
  colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
  colors[ImGuiCol_TableBorderLight] = ImVec4(0.18f, 0.17f, 0.17f, 1.00f);
  colors[ImGuiCol_NavCursor] = ImVec4(0.86f, 0.30f, 0.26f, 1.00f);
  style.ScaleAllSizes(scale);
}

std::vector<Change> DrawPanel(PanelModel& model, Surface surface) {
  std::vector<Change> changes;
  if (!model.schema) return changes;
  const Schema& schema = *model.schema;
  std::vector<std::size_t> visible_sections;
  for (std::size_t index = 0; index < schema.sections.size(); ++index) {
    const bool any = std::any_of(
        schema.settings.begin(), schema.settings.end(), [&](const Setting& setting) {
          return setting.section == schema.sections[index] && IsShown(model, setting);
        });
    if (any) visible_sections.push_back(index);
  }
  if (visible_sections.empty()) return changes;
  if (std::find(visible_sections.begin(), visible_sections.end(), model.section) ==
      visible_sections.end()) {
    model.section = visible_sections.front();
  }

  const bool rtl = Rtl(model);
  const float sidebar = ImGui::GetFontSize() * 11.0f;
  auto draw_sections = [&]() {
    ImGui::BeginChild("##sections", ImVec2(sidebar, 0.0f), ImGuiChildFlags_Borders);
    if (rtl) ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(1.0f, 0.5f));
    for (const std::size_t index : visible_sections) {
      const std::string label = Shown(model, schema.sections[index]) + "##section";
      ImGui::PushID(int(index));
      if (ImGui::Selectable(label.c_str(), model.section == index, 0,
                            ImVec2(0.0f, ImGui::GetFrameHeight()))) {
        model.section = index;
      }
      ImGui::PopID();
    }
    if (rtl) ImGui::PopStyleVar();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() -
                         ImGui::GetStyle().WindowPadding.y);
    const std::string advanced_text = Shown(model, Tr(model, "Advanced"));
    if (rtl) {
      const float box = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                        TextWidth(advanced_text);
      const float available = ImGui::GetContentRegionAvail().x;
      if (available > box) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - box);
    }
    ImGui::Checkbox((advanced_text + "##advanced").c_str(), &model.show_advanced);
    ImGui::EndChild();
  };
  auto draw_settings = [&](float width) {
    ImGui::BeginChild("##settings", ImVec2(width, 0.0f), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##rows", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerH)) {
      // Right-to-left: values on the left, labels on the right.
      if (rtl) {
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthStretch, 0.52f);
      } else {
        ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
      }
      const std::string& section = schema.sections[model.section];
      for (const Setting& setting : schema.settings) {
        if (setting.section != section || !IsShown(model, setting)) continue;
        DrawSetting(model, setting, surface, changes);
      }
      ImGui::EndTable();
    }
    ImGui::EndChild();
  };
  if (rtl) {
    draw_settings((std::max)(ImGui::GetContentRegionAvail().x - sidebar -
                                 ImGui::GetStyle().ItemSpacing.x,
                             ImGui::GetFontSize() * 10.0f));
    ImGui::SameLine();
    draw_sections();
  } else {
    draw_sections();
    ImGui::SameLine();
    draw_settings(0.0f);
  }
  return changes;
}

}  // namespace rex::ui::settings
