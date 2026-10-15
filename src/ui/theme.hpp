#pragma once

#include "app/metrics.hpp"

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

#include <optional>

class QApplication;

namespace lnb {
struct Settings;
}

namespace ui {

// The palette of one named theme. Every colour is fully resolved (alpha
// included) so widgets can use them directly.
struct ThemeColors {
  QColor window;  // outermost window background
  QColor surface; // sidebars, title bar, cards
  QColor base;    // input fields and list backgrounds

  QColor text;
  QColor text_muted;

  QColor accent;
  QColor accent_hover;
  QColor accent_active;
  QColor accent_text;

  // Subtle fills: the user's own message block and assistant code blocks.
  QColor bubble_user;
  QColor bubble_user_text;
  QColor bubble_assistant;
  QColor bubble_assistant_text;
  QColor bubble_assistant_code;

  // The conversation (transcript) pane and the composer input field. Themes that
  // sit over a background image keep both fully transparent, so the tinted image
  // shows through; plain themes use the window colour.
  QColor conversation_background;
  QColor input_background;

  QColor border;

  QColor danger;
  QColor danger_text;
};

struct Theme {
  QString name = QStringLiteral("Catppuccin Mocha");
  bool dark = true;
  ThemeColors colors;
  // The theme's own defaults, before any user override is applied.
  lnb::ThemeMetrics metrics;
};

// Loads theme.json (from the working directory, with a compiled-in Catppuccin
// Mocha fallback), selects the theme named by settings.theme, applies the user
// metric override (settings.metrics) on top of the theme defaults, loads the
// optional background image and installs the global look (palette, stylesheet
// and fonts) on the application.
//
// Must be called once, before any window is created.
void apply_theme(QApplication& app, const lnb::Settings& settings);

// The theme currently in effect. Valid after apply_theme() has run.
const Theme& theme();

// The effective layout metrics (theme defaults with the user override applied).
const lnb::ThemeMetrics& metrics();

// Mutable access to the effective metrics, so the settings window can preview
// metric changes at runtime.
lnb::ThemeMetrics& mutable_metrics();

// The general UI font (every widget), the chat font (transcript + composer)
// and the monospace font (code). All are valid after apply_theme() has run.
const QFont& ui_font();
const QFont& chat_font();
const QFont& mono_font();

// Convenience readers for the reading rhythm of message text.
int line_height_other();
int line_height_self();
int paragraph_spacing();
int list_paragraph_spacing();
int message_max_width();

// Names of every theme defined in theme.json, in file order.
QStringList theme_names();

// Rebuilds the application stylesheet from the current theme state. Useful when
// the metrics are changed in place.
void refresh_stylesheet(QApplication& app);

// True when a background image was configured and loaded successfully.
bool background_enabled();

// The background image, already loaded, or a null pixmap.
const QPixmap& background_image();

// The colour painted over the whole background image (below every component),
// or nullopt when there is no image or no override configured.
const std::optional<QColor>& background_tint();

// Subtle list-row states shared by the sidebar session list and the assistant
// picker, so both read the same (low-contrast) way: a barely-visible hover and
// a slightly brighter selected row, the way Zed's chrome behaves.
QColor row_hover_background();
QColor row_selected_background();

// Loads a bundled monochrome icon (:/icons/<name>.png) and recolours it to the
// active theme: `color` for the normal state and `disabled` for the disabled
// one, so the artwork follows the palette instead of a fixed tint. Returns a
// null icon when the resource is missing, letting the caller fall back to the
// system icon theme or a standard icon.
QIcon resource_icon(const QString& name, const QColor& color, const QColor& disabled);

} // namespace ui
