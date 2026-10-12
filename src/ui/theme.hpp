//
// Created by menv on 10/11/26.
//

#pragma once

#include "metrics.hpp"

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QString>
#include <QStringList>

#include <optional>

class QApplication;

namespace lnb {
struct general;
}

namespace ui {

// The palette of one named theme. Every colour is fully resolved (alpha
// included) so widgets can use them directly.
struct ThemeColors {
  QColor window; // outermost window / dialog background
  QColor surface; // navigation, sidebars, cards, menus
  QColor base; // input fields and list backgrounds

  QColor text;
  QColor text_muted;

  QColor accent;
  QColor accent_hover;
  QColor accent_active;
  QColor accent_text;

  QColor bubble_user;
  QColor bubble_user_text;
  QColor bubble_assistant;
  QColor bubble_assistant_text;
  // Background drawn behind code (blocks and inline code) in assistant replies.
  QColor bubble_assistant_code;

  QColor border;

  QColor danger;
  QColor danger_text;
};

struct Theme {
  QString name = QStringLiteral("Catppuccin Mocha");
  bool dark = true;
  ThemeColors colors;
  ThemeMetrics metrics;
};

// Loads theme.json (the embedded resource takes priority, otherwise a
// theme.json in the current working directory), selects the theme named by
// config.theme_, loads the optional background image and installs the global
// look (style, palette, stylesheet and font) on the application.
//
// Must be called once, before any window is created.
void apply_theme(QApplication &app, const lnb::general &config);

// The theme currently in effect. Valid after apply_theme() has run.
const Theme &theme();

// The general UI font (every widget), the chat font (transcript + composer
// only) and the monospace font (code in replies). All are valid after
// apply_theme() has run.
const QFont &ui_font();
const QFont &chat_font();
const QFont &mono_font();

// Reading rhythm of message text, from the active theme's metrics.
// line_height_other()/line_height_self() are percentages (100 is normal single
// spacing); paragraph_spacing() is the gap after each paragraph in pixels.
int line_height_other();
int line_height_self();
int paragraph_spacing();

// Effective maximum width of a message body in pixels. 0 means the width is
// unlimited (the transcript margin is the only bound).
int message_max_width();

// Names of every theme defined in theme.json, in file order.
QStringList theme_names();

// Mutable access to the active theme's metrics, so the settings window can apply
// metric changes at runtime.
ThemeMetrics &mutable_metrics();

// Rebuilds the application stylesheet from the current theme state. Useful when
// the theme metrics are changed in place (apply_theme() rebuilds it too).
void refresh_stylesheet(QApplication &app);

// True when a background image was configured and loaded successfully.
bool background_enabled();

// The background image, already loaded, or a null pixmap.
const QPixmap &background_image();

// The colour painted over the whole background image (below every component),
// or nullopt when there is no image or no override configured. Components keep
// their palette colours, so this only affects the area right above the image.
const std::optional<QColor> &background_tint();

} // namespace ui
