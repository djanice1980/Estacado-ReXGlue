/**
 * @file        ui/settings_schema.cpp
 * @brief       Settings schema serialization (TOML) and number formatting.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/settings_schema.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace rex::ui::settings {
namespace {

const char* EditorName(Editor editor) {
  switch (editor) {
    case Editor::kBoolean: return "boolean";
    case Editor::kInteger: return "integer";
    case Editor::kNumber: return "number";
    case Editor::kChoice: return "choice";
    case Editor::kDisplay: return "display";
    case Editor::kKey: return "key";
  }
  return "choice";
}

std::optional<Editor> EditorFromName(std::string_view name) {
  if (name == "boolean") return Editor::kBoolean;
  if (name == "integer") return Editor::kInteger;
  if (name == "number") return Editor::kNumber;
  if (name == "choice") return Editor::kChoice;
  if (name == "display") return Editor::kDisplay;
  if (name == "key") return Editor::kKey;
  return std::nullopt;
}

void Fail(std::string* error, std::string text) {
  if (error) *error = std::move(text);
}

}  // namespace

const Setting* Schema::Find(std::string_view key) const {
  for (const Setting& setting : settings) {
    if (setting.key == key) return &setting;
  }
  return nullptr;
}

std::string SerializeSchema(const Schema& schema) {
  toml::table root;
  root.insert("version", kSchemaVersion);
  toml::array sections;
  for (const std::string& section : schema.sections) sections.push_back(section);
  root.insert("sections", std::move(sections));
  toml::array settings;
  for (const Setting& setting : schema.settings) {
    toml::table entry;
    entry.insert("key", setting.key);
    entry.insert("section", setting.section);
    entry.insert("label", setting.label);
    entry.insert("description", setting.description);
    entry.insert("units", setting.units);
    entry.insert("editor", EditorName(setting.editor));
    entry.insert("minimum", setting.minimum);
    entry.insert("maximum", setting.maximum);
    entry.insert("step", setting.step);
    entry.insert("display_multiplier", setting.display_multiplier);
    toml::array choices;
    for (const Choice& choice : setting.choices) {
      toml::array pair;
      pair.push_back(choice.value);
      pair.push_back(choice.label);
      if (choice.advanced) pair.push_back(true);
      choices.push_back(std::move(pair));
    }
    entry.insert("choices", std::move(choices));
    entry.insert("visible_when_key", setting.visible_when_key);
    entry.insert("visible_when_value", setting.visible_when_value);
    entry.insert("advanced", setting.advanced);
    entry.insert("live", setting.live);
    settings.push_back(std::move(entry));
  }
  root.insert("setting", std::move(settings));
  root.insert("right_to_left", schema.right_to_left);
  toml::table text;
  for (const auto& [english, translated] : schema.text) text.insert(english, translated);
  root.insert("text", std::move(text));
  std::ostringstream stream;
  stream << root;
  return stream.str();
}

std::optional<Schema> ParseSchema(std::string_view text, std::string* error) {
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error& parse_error) {
    Fail(error, std::string("settings schema is not valid TOML: ") +
                    std::string(parse_error.description()));
    return std::nullopt;
  }
  if (root["version"].value_or(0) != kSchemaVersion) {
    Fail(error, "settings schema version is not supported");
    return std::nullopt;
  }
  Schema schema;
  schema.right_to_left = root["right_to_left"].value_or(false);
  if (const toml::table* text = root["text"].as_table()) {
    for (const auto& [english, translated] : *text) {
      if (auto value = translated.value<std::string>()) {
        schema.text.emplace(std::string(english.str()), *value);
      }
    }
  }
  if (const toml::array* sections = root["sections"].as_array()) {
    for (const toml::node& node : *sections) {
      if (auto value = node.value<std::string>()) schema.sections.push_back(*value);
    }
  }
  const toml::array* settings = root["setting"].as_array();
  if (!settings) {
    Fail(error, "settings schema has no settings");
    return std::nullopt;
  }
  for (const toml::node& node : *settings) {
    const toml::table* entry = node.as_table();
    if (!entry) continue;
    Setting setting;
    const toml::table& table = *entry;
    setting.key = table["key"].value_or(std::string{});
    setting.section = table["section"].value_or(std::string{});
    setting.label = table["label"].value_or(std::string{});
    setting.description = table["description"].value_or(std::string{});
    setting.units = table["units"].value_or(std::string{});
    const auto editor = EditorFromName(table["editor"].value_or(std::string{}));
    if (setting.key.empty() || setting.label.empty() || !editor) {
      Fail(error, "settings schema entry is incomplete: " + setting.key);
      return std::nullopt;
    }
    setting.editor = *editor;
    setting.minimum = table["minimum"].value_or(0.0);
    setting.maximum = table["maximum"].value_or(0.0);
    setting.step = table["step"].value_or(0.0);
    setting.display_multiplier = table["display_multiplier"].value_or(1.0);
    if (const toml::array* choices = table["choices"].as_array()) {
      for (const toml::node& choice_node : *choices) {
        const toml::array* pair = choice_node.as_array();
        if (!pair || pair->size() < 2 || pair->size() > 3) continue;
        auto value = (*pair)[0].value<std::string>();
        auto label = (*pair)[1].value<std::string>();
        const bool advanced = pair->size() == 3 && (*pair)[2].value_or(false);
        if (value && label) setting.choices.push_back({*value, *label, advanced});
      }
    }
    setting.visible_when_key = table["visible_when_key"].value_or(std::string{});
    setting.visible_when_value = table["visible_when_value"].value_or(std::string{});
    setting.advanced = table["advanced"].value_or(false);
    setting.live = table["live"].value_or(false);
    if (std::find(schema.sections.begin(), schema.sections.end(), setting.section) ==
        schema.sections.end()) {
      schema.sections.push_back(setting.section);
    }
    schema.settings.push_back(std::move(setting));
  }
  return schema;
}

std::string SerializeValues(const Values& values) {
  toml::table root;
  for (const auto& [key, value] : values) root.insert(key, value);
  std::ostringstream stream;
  stream << root;
  return stream.str();
}

std::optional<Values> ParseValues(std::string_view text, std::string* error) {
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error& parse_error) {
    Fail(error, std::string("settings values are not valid TOML: ") +
                    std::string(parse_error.description()));
    return std::nullopt;
  }
  Values values;
  for (const auto& [key, node] : root) {
    if (auto value = node.value<std::string>()) values[std::string(key.str())] = *value;
  }
  return values;
}

std::string FormatNumberValue(const Setting& setting, double value) {
  int decimals = 0;
  if (setting.step > 0.0 && setting.step < 1.0) {
    decimals = std::clamp(int(std::ceil(-std::log10(setting.step) - 1e-9)), 0, 6);
  }
  char text[64];
  std::snprintf(text, sizeof(text), "%.*f", decimals, value);
  std::string result(text);
  if (result.find('.') == std::string::npos) result += ".0";
  return result;
}

std::string Translate(const Schema* schema, std::string_view english) {
  if (schema) {
    if (const auto it = schema->text.find(english); it != schema->text.end()) return it->second;
  }
  return std::string(english);
}

std::string FormatText(std::string_view format, const std::vector<std::string>& values) {
  std::string out;
  size_t next = 0;
  for (size_t i = 0; i < format.size(); ++i) {
    if (format[i] == '{' && i + 1 < format.size() && format[i + 1] == '}') {
      if (next < values.size()) out += values[next];
      ++next;
      ++i;
    } else {
      out.push_back(format[i]);
    }
  }
  return out;
}

}  // namespace rex::ui::settings
