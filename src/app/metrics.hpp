#pragma once

namespace lnb {

// Sizing and spacing knobs of the interface, persisted under "metrics" in
// data/settings.json. theme.json's `default_metrics` supplies the fallback when
// no override is stored, and each field is documented where it is used.
struct ThemeMetrics {
  // Sidebar holding "New Session" and the session list.
  int sidebar_width = 280;

  // Vertical gap between transcript entries.
  int message_spacing = 12;

  // Inset of the transcript from the window edges (also the composer's inset).
  int page_margin = 14;

  // Composer height, excluding the surrounding padding.
  int composer_height = 96;

  // Composer height when the expand toggle (the field's top-right button) is on.
  int composer_height_expanded = 280;

  int title_font_size = 15;

  // Session row height. The row holds two lines (title and time), so the delegate
  // treats this as a floor and never clips the second line.
  int session_item_height = 48;

  // Reading rhythm of message text. line_height_other / line_height_self are
  // percentages (100 is normal single spacing); paragraph_spacing is the gap
  // after each paragraph, in pixels. "Other" is the assistant's reply, "self"
  // is the user's own message.
  int line_height_other = 160;
  int line_height_self = 135;
  int paragraph_spacing = 10;

  // Gap after list items, in pixels. List items read better when they sit
  // flush, so this defaults to 0 while prose keeps paragraph_spacing.
  int list_paragraph_spacing = 0;

  // Maximum width of the whole conversation flow in pixels; 0 means unlimited.
  // When the pane is wider than this, both side insets grow so the flow is
  // centred instead of hugging the left edge.
  int message_max_width = 640;

  // Corner radius shared by the user's message block and the composer field, so
  // the two read as the same surface.
  int bubble_radius = 8;
};

} // namespace lnb
