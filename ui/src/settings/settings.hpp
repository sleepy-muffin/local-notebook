#pragma once

#include <array>
#include <optional>
#include <string>

#include "settings/metrics.hpp"

namespace lnb {

// App-global settings, shared by every assistant, persisted at
// data/settings.json (design-1.md §8). Assistant- and session-level settings
// live elsewhere.
struct Settings {
  // UUID of the assistant to reopen at startup; empty on first run.
  std::string last_assistant;

  // When true, the last assistant is reopened automatically at startup; when
  // false the assistant picker is always shown. Toggled from the picker and
  // from the Account page of the settings window.
  bool auto_login = true;

  std::string theme = "Catppuccin Mocha";

  // UI language code: "en", "zh" or "ja". Applied at startup (and on Save
  // from the Configuration page); see support/i18n.hpp.
  std::string language = "en";

  // Optional background image, resolved relative to the working directory.
  std::optional<std::string> background_url;

  // RGBA colour painted over the background image. nullopt shows it untinted.
  std::optional<std::array<float, 4>> background_override =
      std::array<float, 4>{0.094f, 0.098f, 0.149f, 0.85f};

  // General UI font (every widget), chat font (transcript + composer) and
  // monospace font (code). An empty family keeps the system default.
  std::string ui_font;
  int ui_font_size = 11;
  std::string font;
  int font_size = 12;
  std::string mono_font;
  int mono_font_size = 11;

  // Inline code size as a fraction of the surrounding text. Inline code is
  // scaled from the chat font so it never feels oversized next to the prose.
  float inline_code_scale = 0.9f;

  // When true, Enter inserts a newline and Ctrl+Enter sends; when false the
  // reverse.
  bool enter_with_modifier = true;

  // Transcript rendering window. Only the most recent messages are rendered at
  // once; older ones are revealed a page at a time on demand. This bounds
  // rendering only — the full history is still loaded and sent.
  int rendered_message_limit = 20;
  int history_page_size = 20;

  // Base64 window geometry (Qt saveGeometry), restored on the next launch.
  std::optional<std::string> geometry;

  // Theme metric overrides. nullopt keeps the theme's own defaults.
  std::optional<ThemeMetrics> metrics;
};

// data/settings.json. A missing or malformed file yields the defaults.
Settings load_settings();
void save_settings(const Settings& settings);

} // namespace lnb
