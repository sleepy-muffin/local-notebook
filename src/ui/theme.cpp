#include "theme.hpp"

#include "../ui.hpp" // lnb::general, lnb::color4, lnb::load_color

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPalette>
#include <QPixmap>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>

#include <algorithm>
#include <optional>
#include <string>

namespace ui {
namespace {

// The single, process-wide theme state. Populated once by apply_theme().
struct ThemeState {
  Theme theme;
  QPixmap background;
  bool has_background = false;

  // Source of the currently decoded background, so re-applying the theme does
  // not decode the same image again (a full-size decode is expensive).
  std::string background_url;

  // Installing a QStyle forces a full re-polish of every widget. The style is
  // fixed for the process, so it is installed once and skipped afterwards.
  bool style_installed = false;

  QFont ui_font;
  QFont chat_font;
  QFont mono_font;

  // Tint painted over the whole background image, below every component. Only
  // set when a background image is enabled and an override colour is configured.
  std::optional<QColor> tint;
};

ThemeState& state() {
  static ThemeState instance;
  return instance;
}

// -----------------------------------------------------------------------------
// theme.json accessors
// -----------------------------------------------------------------------------

// Wraps lnb::load_color (which understands both 0-1 and 0-255 channels) and
// converts the result to a QColor.
QColor read_color(const QJsonObject& object, const char* key, const QColor& fallback) {
  const lnb::color4 fallback_value{
      static_cast<float>(fallback.redF()),
      static_cast<float>(fallback.greenF()),
      static_cast<float>(fallback.blueF()),
      static_cast<float>(fallback.alphaF()),
  };

  const lnb::color4 value = lnb::load_color(object, key, fallback_value);

  return QColor::fromRgbF(value.r, value.g, value.b, value.a);
}

ThemeColors parse_colors(const QJsonObject& colors, ThemeColors result) {
  result.window = read_color(colors, "window", result.window);
  result.surface = read_color(colors, "surface", result.surface);
  result.base = read_color(colors, "base", result.base);

  result.text = read_color(colors, "text", result.text);
  result.text_muted = read_color(colors, "text_muted", result.text_muted);

  result.accent = read_color(colors, "accent", result.accent);
  result.accent_hover = read_color(colors, "accent_hover", result.accent_hover);
  result.accent_active = read_color(colors, "accent_active", result.accent_active);
  result.accent_text = read_color(colors, "accent_text", result.accent_text);

  result.bubble_user = read_color(colors, "bubble_user", result.bubble_user);
  result.bubble_user_text = read_color(colors, "bubble_user_text", result.bubble_user_text);
  result.bubble_assistant = read_color(colors, "bubble_assistant", result.bubble_assistant);
  result.bubble_assistant_text =
      read_color(colors, "bubble_assistant_text", result.bubble_assistant_text);
  result.bubble_assistant_code =
      read_color(colors, "bubble_assistant_code", result.bubble_assistant_code);

  result.border = read_color(colors, "border", result.border);

  result.danger = read_color(colors, "danger", result.danger);
  result.danger_text = read_color(colors, "danger_text", result.danger_text);

  return result;
}

ThemeMetrics parse_metrics(const QJsonObject& metrics, ThemeMetrics result) {
  const auto read_int = [&metrics](const char* key, int fallback) {
    return metrics.contains(key) ? metrics.value(key).toInt(fallback) : fallback;
  };

  result.nav_width = read_int("nav_width", result.nav_width);
  result.sidebar_width = read_int("sidebar_width", result.sidebar_width);
  result.bubble_radius = read_int("bubble_radius", result.bubble_radius);
  result.message_spacing = read_int("message_spacing", result.message_spacing);
  result.page_margin = read_int("page_margin", result.page_margin);
  result.composer_height = read_int("composer_height", result.composer_height);
  result.title_font_size = read_int("title_font_size", result.title_font_size);
  result.session_item_height = read_int("session_item_height", result.session_item_height);

  // Reading rhythm of the chat text. A line height below 100 would clip lines.
  result.line_height_other = std::max(100, read_int("line_height_other", result.line_height_other));
  result.line_height_self =
      std::max(100, read_int("line_height_self", result.line_height_self));
  result.paragraph_spacing =
      std::max(0, read_int("paragraph_spacing", result.paragraph_spacing));
  result.list_paragraph_spacing =
      std::max(0, read_int("list_paragraph_spacing", result.list_paragraph_spacing));

  // A maximum width of 0 means unlimited.
  result.message_max_width = std::max(0, read_int("message_max_width", result.message_max_width));

  return result;
}

// Reads the theme definition file: the embedded copy first, then a theme.json
// in the launching directory as a fallback.
std::optional<QJsonObject> read_theme_root() {
  const QStringList candidates{
      QStringLiteral(":/themes/theme.json"),
      QStringLiteral("theme.json"),
  };

  for (const QString& path: candidates) {
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly)) {
      continue;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());

    if (document.isObject()) {
      return document.object();
    }
  }

  return std::nullopt;
}

Theme parse_theme(const QJsonObject& root, const QString& requested) {
  Theme theme;
  theme.name = requested;

  const QJsonObject themes = root.value(QStringLiteral("themes")).toObject();
  QJsonObject entry = themes.value(requested).toObject();

  // An unknown name falls back to the first theme in the file so the app still
  // starts with a coherent look.
  if (entry.isEmpty() && !themes.isEmpty()) {
    theme.name = themes.keys().first();
    entry = themes.value(theme.name).toObject();
  }

  theme.dark = entry.value(QStringLiteral("dark")).toBool(theme.dark);

  ThemeMetrics metrics = parse_metrics(
      root.value(QStringLiteral("default_metrics")).toObject(), ThemeMetrics{}
  );

  theme.metrics = parse_metrics(entry.value(QStringLiteral("metrics")).toObject(), metrics);

  // Hard-coded fallback palette (Catppuccin Mocha) keeps the app usable even if
  // theme.json is missing or malformed.
  ThemeColors defaults;
  defaults.window = QColor(17, 17, 27);
  defaults.surface = QColor(24, 24, 37);
  defaults.base = QColor(30, 30, 46);
  defaults.text = QColor(205, 214, 244);
  defaults.text_muted = QColor(166, 173, 200);
  defaults.accent = QColor(137, 180, 250);
  defaults.accent_hover = QColor(180, 190, 254);
  defaults.accent_active = QColor(116, 199, 236);
  defaults.accent_text = QColor(17, 17, 27);
  defaults.bubble_user = QColor(137, 180, 250);
  defaults.bubble_user_text = QColor(17, 17, 27);
  defaults.bubble_assistant = QColor(49, 50, 68);
  defaults.bubble_assistant_text = QColor(205, 214, 244);
  defaults.bubble_assistant_code = QColor(17, 17, 27);
  defaults.border = QColor(69, 71, 90);
  defaults.danger = QColor(243, 139, 168);
  defaults.danger_text = QColor(17, 17, 27);

  theme.colors = parse_colors(entry.value(QStringLiteral("colors")).toObject(), defaults);

  return theme;
}

// -----------------------------------------------------------------------------
// Background image
// -----------------------------------------------------------------------------

void load_background(const lnb::general& config) {
  ThemeState& current = state();

  // Re-decoding the image on every theme apply is wasteful and shows up as a
  // pause. Keep the decoded pixmap while the configured URL is unchanged.
  const std::string requested = config.background_url_.value_or(std::string{});

  if (requested == current.background_url) {
    return;
  }

  current.background_url = requested;
  current.background = QPixmap{};
  current.has_background = false;

  if (requested.empty()) {
    return;
  }

  const QPixmap pixmap(QString::fromStdString(requested));

  if (pixmap.isNull()) {
    mmc::log::debug("Background image could not be loaded: {}", requested);
    return;
  }

  current.background = pixmap;
  current.has_background = true;
}

// Converts the optional configured override into a QColor, or nullopt when it
// is absent. Absence means "paint the image raw": there is no fallback tint.
std::optional<QColor> resolve_override(const std::optional<lnb::color4>& value) {
  if (!value.has_value()) {
    return std::nullopt;
  }

  return QColor::fromRgbF(value->r, value->g, value->b, value->a);
}

// -----------------------------------------------------------------------------
// Stylesheet
// -----------------------------------------------------------------------------

QString build_stylesheet(const Theme& theme, bool background) {
  const ThemeColors& c = theme.colors;
  const ThemeMetrics& m = theme.metrics;

  // With a background image enabled the window and the transcript canvas become
  // transparent, so the image (and its tint) shows through. Every component
  // keeps its palette colour and is simply painted on top.
  const QColor window = background ? QColor(0, 0, 0, 0) : c.window;
  const QColor canvas = background ? QColor(0, 0, 0, 0) : c.base;
  const QColor panel = c.surface;
  const QColor field = c.base;

  const auto rgba = [](const QColor& color) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
  };

  const QString title_size = QString::number(m.title_font_size + 3);

  QString sheet;

  // ---- Base ----
  sheet += "QWidget { color: " + rgba(c.text) + "; }\n";
  // Only the main window becomes transparent with a background image; dialogs
  // keep an opaque background so their content stays readable.
  sheet += "QMainWindow { background-color: " + rgba(window) + "; }\n";
  sheet += "QDialog { background-color: " + rgba(c.window) + "; }\n";

  // ---- Navigation ----
  sheet += "QListWidget { background-color: transparent; border: none; outline: none; }\n";
  sheet += "QListWidget::item { border-radius: 10px; padding: 8px 10px; margin: 2px; color: " +
      rgba(c.text_muted) + "; }\n";
  sheet += "QListWidget::item:hover:!selected { background-color: " + rgba(c.border) + "; color: " +
      rgba(c.text) + "; }\n";
  sheet += "QListWidget::item:selected { background-color: " + rgba(c.accent) + "; color: " +
      rgba(c.accent_text) + "; }\n";

  // The icon-only navigation rail: a vertical strip of checkable tool buttons.
  sheet += "QWidget#Nav { background-color: " + rgba(panel) + "; }\n";
  sheet += "QToolButton#NavButton { background-color: transparent; border: none; border-radius: 12px; color: " +
      rgba(c.text_muted) + "; }\n";
  sheet += "QToolButton#NavButton:hover { background-color: " + rgba(c.border) + "; color: " +
      rgba(c.text) + "; }\n";
  sheet += "QToolButton#NavButton:checked { background-color: " + rgba(c.accent) + "; color: " +
      rgba(c.accent_text) + "; }\n";

  // ---- Sidebar ----
  sheet += "QWidget#Sidebar { background-color: " + rgba(panel) + "; }\n";
  sheet += "QFrame#Card { background-color: " + rgba(panel) + "; border: 1px solid " +
      rgba(c.border) + "; border-radius: 16px; }\n";

  sheet += "QLabel#SidebarBrand { color: " + rgba(c.text) + "; font-size: 14px; font-weight: 600; padding: 2px 4px 8px 4px; }\n";
  sheet += "QLabel#SidebarTitle { color: " + rgba(c.text_muted) + "; font-size: 11px; font-weight: 600; letter-spacing: 1px; padding: 2px 4px 4px 4px; }\n";
  sheet += "QLabel#PageTitle { color: " + rgba(c.text) + "; font-size: " + title_size +
      "px; font-weight: 700; }\n";
  sheet += "QFrame#CurrentUser { background-color: " + rgba(c.base) + "; border: none; border-radius: 10px; }\n";
  sheet += "QLabel#CurrentUserName, QLabel#ConversationTitle { color: " + rgba(c.text) + "; font-weight: 600; }\n";
  sheet += "QLabel#CurrentUserId, QLabel#ConversationSubtitle { color: " + rgba(c.text_muted) + "; font-size: 11px; }\n";
  sheet += "QLabel#ConversationTimestamp { color: " + rgba(c.text_muted) + "; font-size: 11px; }\n";
  sheet += "QLabel#ConversationContext { color: " + rgba(c.text_muted) + "; font-size: 11px; }\n";
  sheet += "QToolButton#CurrentUserSettings { background: transparent; border: 0; outline: 0; padding: 0; margin: 0; color: " + rgba(c.text_muted) + "; }\n";
  sheet += "QToolButton#CurrentUserSettings:hover, QToolButton#CurrentUserSettings:pressed { background: transparent; border: 0; outline: 0; }\n";
  sheet += "QLabel#EmptyHint { color: " + rgba(c.text_muted) + "; font-size: 16px; }\n";

  // ---- Session list ----
  sheet += "QListView#Sessions { background-color: transparent; border: none; outline: none; }\n";
  sheet += "QListView#Sessions::item { border-radius: 10px; }\n";

  // ---- Pages / workspace / transcript ----
  sheet += "QStackedWidget#Pages, QStackedWidget#Workspace, QWidget#Page { background-color: " +
      rgba(window) + "; }\n";
  sheet += "QScrollArea#Transcript { background-color: " + rgba(canvas) + "; border: none; }\n";
  sheet += "QFrame#ConversationHeader { background-color: " + rgba(panel) + "; border-bottom: 1px solid " + rgba(c.border) + "; }\n";

  // Fenced code blocks in assistant replies: a rounded frame holding the code.
  sheet += "QFrame#CodeBlock { background-color: " + rgba(c.bubble_assistant_code) +
      "; border-radius: 8px; }\n";

  // The composer bar shares the transcript canvas colour, so the bottom strip
  // reads as part of the conversation flow instead of a separate panel.
  sheet += "QWidget#ComposerBar { background-color: " + rgba(canvas) + "; }\n";

  // ---- Composer input ----
  // The field uses the user-bubble colour so the composer reads as "your" input
  // and stands slightly apart from the transcript canvas behind it.
  sheet += "QPlainTextEdit#Composer { background-color: " + rgba(c.bubble_user) + "; border: 1px solid " +
      rgba(c.border) +
      "; border-radius: 14px; padding: 8px 12px; selection-background-color: " + rgba(c.accent) +
      "; selection-color: " + rgba(c.accent_text) + "; }\n";
  sheet += "QPlainTextEdit#Composer:focus { border: 1px solid " + rgba(c.accent) + "; }\n";

  // ---- Buttons ----
  sheet += "QPushButton { background-color: " + rgba(c.accent) + "; color: " + rgba(c.accent_text) +
      "; border: none; border-radius: 10px; padding: 8px 18px; font-weight: 600; }\n";
  sheet += "QPushButton:hover { background-color: " + rgba(c.accent_hover) + "; }\n";
  sheet += "QPushButton:pressed { background-color: " + rgba(c.accent_active) + "; }\n";
  sheet += "QPushButton:disabled { background-color: " + rgba(c.border) + "; color: " +
      rgba(c.text_muted) + "; }\n";
  sheet += "QPushButton#Secondary { background-color: " + rgba(c.surface) + "; color: " +
      rgba(c.text) + "; border: 1px solid " + rgba(c.border) + "; }\n";
  sheet += "QPushButton#Secondary:hover { background-color: " + rgba(c.border) + "; }\n";
  sheet += "QPushButton#Danger { background-color: " + rgba(c.danger) + "; color: " +
      rgba(c.danger_text) + "; }\n";

  // ---- Message hover actions ----
  sheet += "QWidget#MessageActions { background: transparent; }\n";
  sheet += "QToolButton#MessageAction { background-color: transparent; border: none; border-radius: 6px; color: " +
      rgba(c.text_muted) + "; }\n";
  sheet += "QToolButton#MessageAction:hover { background-color: " + rgba(c.border) + "; color: " +
      rgba(c.text) + "; }\n";
  sheet += "QToolButton#MessageAction:pressed { background-color: " + rgba(c.accent) + "; color: " +
      rgba(c.accent_text) + "; }\n";
  // The hover-only message id, shown as a quiet "#<id>" beside the actions.
  sheet += "QLabel#MessageId { color: " + rgba(c.text_muted) +
      "; font-size: 11px; padding: 0 6px; }\n";

  // ---- Menus ----
  sheet += "QMenu { background-color: " + rgba(panel) + "; border: 1px solid " + rgba(c.border) +
      "; border-radius: 10px; padding: 6px; }\n";
  sheet += "QMenu::item { padding: 7px 18px; border-radius: 6px; color: " + rgba(c.text) + "; }\n";
  sheet += "QMenu::item:selected { background-color: " + rgba(c.accent) + "; color: " +
      rgba(c.accent_text) + "; }\n";
  sheet += "QMenu::separator { height: 1px; background: " + rgba(c.border) + "; margin: 6px 8px; }\n";

  // ---- Scrollbars ----
  sheet += "QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }\n";
  sheet += "QScrollBar::handle:vertical { background: " + rgba(c.border) +
      "; border-radius: 5px; min-height: 32px; }\n";
  sheet += "QScrollBar::handle:vertical:hover { background: " + rgba(c.text_muted) + "; }\n";
  sheet += "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }\n";
  sheet += "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }\n";
  sheet += "QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }\n";
  sheet += "QScrollBar::handle:horizontal { background: " + rgba(c.border) +
      "; border-radius: 5px; min-width: 32px; }\n";
  sheet += "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }\n";
  sheet += "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }\n";

  // ---- Generic inputs (session editor, etc.) ----
  sheet += "QLineEdit, QPlainTextEdit { background-color: " + rgba(field) + "; border: 1px solid " +
      rgba(c.border) + "; border-radius: 8px; padding: 6px 10px; color: " + rgba(c.text) +
      "; selection-background-color: " + rgba(c.accent) + "; selection-color: " + rgba(c.accent_text) +
      "; }\n";
  sheet += "QLineEdit:focus, QPlainTextEdit:focus { border: 1px solid " + rgba(c.accent) + "; }\n";

  // The read-only reference text appended under the session editor's memory
  // fields: quiet, low-contrast, and clearly not an input.
  sheet += "QLabel#SessionDocs { color: " + rgba(c.text_muted) +
      "; font-size: 12px; padding: 8px 2px; }\n";

  // ---- Tooltips ----
  sheet += "QToolTip { background-color: " + rgba(panel) + "; color: " + rgba(c.text) +
      "; border: 1px solid " + rgba(c.border) + "; border-radius: 6px; padding: 4px 8px; }\n";

  // ---- Settings window ----
  sheet += "QDialog#SettingsWindow { background-color: " + rgba(c.window) + "; }\n";
  sheet += "QListWidget#SettingsNav { background-color: " + rgba(panel) +
      "; border: none; outline: none; padding: 10px; }\n";
  sheet += "QListWidget#SettingsNav::item { border-radius: 8px; padding: 9px 12px; margin: 2px 0; color: " +
      rgba(c.text_muted) + "; }\n";
  sheet += "QListWidget#SettingsNav::item:hover:!selected { background-color: " + rgba(c.border) +
      "; color: " + rgba(c.text) + "; }\n";
  sheet += "QListWidget#SettingsNav::item:selected { background-color: " + rgba(c.base.lighter(118)) +
      "; color: " + rgba(c.text) + "; }\n";
  sheet += "QScrollArea#SettingsPage, QWidget#SettingsPageInner { background: transparent; border: none; }\n";
  sheet += "QLabel#SettingsPageTitle { color: " + rgba(c.text) + "; font-size: 20px; font-weight: 700; }\n";
  sheet += "QLabel#SettingsPageSubtitle { color: " + rgba(c.text_muted) + "; font-size: 12px; }\n";
  sheet += "QFrame#SettingsCard { background-color: " + rgba(field) + "; border: 1px solid " +
      rgba(c.border) + "; border-radius: 12px; }\n";
  sheet += "QLabel#SettingsCardTitle { color: " + rgba(c.text) + "; font-weight: 600; }\n";
  sheet += "QLabel#SettingsKey { color: " + rgba(c.text_muted) + "; }\n";
  sheet += "QLabel#SettingsValue { color: " + rgba(c.text) + "; }\n";
  sheet += "QDialog#SettingsWindow QGroupBox { color: " + rgba(c.text_muted) +
      "; font-weight: 600; border: 1px solid " + rgba(c.border) +
      "; border-radius: 12px; margin-top: 16px; padding: 16px 16px 10px 16px; }\n";
  sheet += "QDialog#SettingsWindow QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; }\n";
  sheet += "QDialog#SettingsWindow QSpinBox, QDialog#SettingsWindow QComboBox { background-color: " +
      rgba(field) + "; border: 1px solid " + rgba(c.border) +
      "; border-radius: 8px; padding: 5px 8px; color: " + rgba(c.text) + "; }\n";
  sheet += "QDialog#SettingsWindow QSpinBox:focus, QDialog#SettingsWindow QComboBox:focus { border: 1px solid " +
      rgba(c.accent) + "; }\n";
  sheet += "QDialog#SettingsWindow QComboBox QAbstractItemView { background-color: " + rgba(panel) +
      "; border: 1px solid " + rgba(c.border) + "; selection-background-color: " + rgba(c.accent) +
      "; selection-color: " + rgba(c.accent_text) + "; }\n";
  sheet += "QLabel#AboutName { color: " + rgba(c.text) + "; font-size: 24px; font-weight: 700; }\n";
  sheet += "QLabel#AboutVersion { color: " + rgba(c.accent) + "; font-size: 13px; font-weight: 600; }\n";
  sheet += "QLabel#AboutTagline { color: " + rgba(c.text_muted) + "; font-size: 13px; }\n";
  sheet += "QLabel#SettingsLink { font-size: 13px; }\n";
  sheet += "QLabel#SettingsLink a { color: " + rgba(c.accent) + "; text-decoration: none; }\n";
  sheet += "QFrame#SettingsDivider { background-color: " + rgba(c.border) + "; border: none; }\n";

  return sheet;
}

} // namespace

void apply_theme(QApplication& app, const lnb::general& config) {
  ThemeState& current = state();

  const QJsonObject root = read_theme_root().value_or(QJsonObject{});
  current.theme = parse_theme(root, QString::fromStdString(config.theme_));

  // Metrics edited in the settings window (and persisted under "metrics" in
  // local-notebook.json) override the theme's own values. When the section is
  // absent the theme.json defaults are used untouched.
  if (config.metrics_.has_value()) {
    current.theme.metrics = *config.metrics_;
  }

  load_background(config);

  // The tint is only meaningful over a loaded image.
  current.tint = std::nullopt;

  // Installing a style re-polishes every widget; do it only once, since the
  // style does not change at runtime.
  if (!current.style_installed) {
    if (QStyle* style = QStyleFactory::create(QStringLiteral("breeze"))) {
      app.setStyle(style);
      current.style_installed = true;
    }
  }

  // The UI font drives every widget. The chat font derives from it so that an
  // unset chat family simply matches the rest of the interface; it is applied
  // by ConversationView to the transcript and the composer only.
  QFont ui = app.font();

  if (!config.ui_font_.empty()) {
    ui.setFamily(QString::fromStdString(config.ui_font_));
  }

  if (config.ui_font_size_ > 0) {
    ui.setPointSize(config.ui_font_size_);
  }

  QFont chat = ui;

  if (!config.font_.empty()) {
    chat.setFamily(QString::fromStdString(config.font_));
  }

  if (config.font_size_ > 0) {
    chat.setPointSize(config.font_size_);
  }

  // Code uses a fixed-pitch font: the configured family, or the system fixed
  // font, sized like the chat unless mono_font_size_ overrides it.
  QFont mono;

  if (config.mono_font_.empty()) {
    mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  } else {
    mono.setFamily(QString::fromStdString(config.mono_font_));
  }

  const int mono_size =
      config.mono_font_size_ > 0 ? config.mono_font_size_ : chat.pointSize();

  if (mono_size > 0) {
    mono.setPointSize(mono_size);
  }

  mono.setStyleHint(QFont::Monospace);
  mono.setFixedPitch(true);

  current.ui_font = ui;
  current.chat_font = chat;
  current.mono_font = mono;

  // Each global setter below re-polishes the whole widget tree, so it is only
  // issued when the value actually changed. This keeps a no-op Save cheap.
  if (app.font() != ui) {
    app.setFont(ui);
  }

  const ThemeColors& c = current.theme.colors;

  QPalette palette;
  palette.setColor(QPalette::Window, c.window);
  palette.setColor(QPalette::WindowText, c.text);
  palette.setColor(QPalette::Base, c.base);
  palette.setColor(QPalette::AlternateBase, c.surface);
  palette.setColor(QPalette::Text, c.text);
  palette.setColor(QPalette::Button, c.surface);
  palette.setColor(QPalette::ButtonText, c.text);
  palette.setColor(QPalette::Highlight, c.accent);
  palette.setColor(QPalette::HighlightedText, c.accent_text);
  palette.setColor(QPalette::PlaceholderText, c.text_muted);
  palette.setColor(QPalette::ToolTipBase, c.surface);
  palette.setColor(QPalette::ToolTipText, c.text);
  palette.setColor(QPalette::Midlight, c.border);

  if (app.palette() != palette) {
    app.setPalette(palette);
  }

  const QString sheet = build_stylesheet(current.theme, current.has_background);

  if (app.styleSheet() != sheet) {
    app.setStyleSheet(sheet);
  }

  mmc::log::debug(
      "Theme applied: {} (background image: {}, base: #{:02x}{:02x}{:02x})",
      current.theme.name.toStdString(),
      current.has_background ? "on" : "off",
      current.theme.colors.base.red(),
      current.theme.colors.base.green(),
      current.theme.colors.base.blue()
  );
}

const Theme& theme() {
  return state().theme;
}

const QFont& ui_font() {
  return state().ui_font;
}

const QFont& chat_font() {
  return state().chat_font;
}

const QFont& mono_font() {
  return state().mono_font;
}

int line_height_other() {
  return state().theme.metrics.line_height_other;
}

int paragraph_spacing() {
  return state().theme.metrics.paragraph_spacing;
}

int list_paragraph_spacing() {
  return state().theme.metrics.list_paragraph_spacing;
}

int line_height_self() {
  return state().theme.metrics.line_height_self;
}

int message_max_width() {
  return state().theme.metrics.message_max_width;
}

QStringList theme_names() {
  const QJsonObject root = read_theme_root().value_or(QJsonObject{});
  return root.value(QStringLiteral("themes")).toObject().keys();
}

ThemeMetrics& mutable_metrics() {
  return state().theme.metrics;
}

void refresh_stylesheet(QApplication& app) {
  app.setStyleSheet(build_stylesheet(state().theme, state().has_background));
}


bool background_enabled() {
  return state().has_background;
}

const QPixmap& background_image() {
  return state().background;
}

const std::optional<QColor>& background_tint() {
  return state().tint;
}

} // namespace ui
