#pragma once

namespace ui {

// Sizing / spacing knobs for the interface. These are persisted in
// local-notebook.json under the "metrics" section; theme.json's
// `default_metrics` provides the fallback when that section is absent.
struct ThemeMetrics {
  int nav_width = 68;
  int sidebar_width = 280;
  int bubble_radius = 16;
  int message_spacing = 12;
  int page_margin = 14;
  int composer_height = 76;
  int title_font_size = 15;
  int session_item_height = 36;

  // Reading rhythm of the chat text. line_height_other / line_height_self are
  // percentages (100 is normal single spacing); paragraph_spacing is the gap
  // after each paragraph, in pixels. "Other" is the assistant's reply, "self"
  // is the user's own message.
  int line_height_other = 160;
  int line_height_self = 135;
  int paragraph_spacing = 10;

  // Maximum width of a message body in pixels; 0 means unlimited.
  int message_max_width = 640;
};

} // namespace ui
