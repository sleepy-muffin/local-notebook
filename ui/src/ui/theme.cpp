#include "ui/theme.hpp"

#include "settings/settings.hpp"
#include "shark/log.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPainter>
#include <QPalette>
#include <QString>
#include <QWidget>

#include <optional>
#include <string>

namespace ui {
namespace {

struct ThemeState {
  Theme theme;

  // The effective metrics: the theme's defaults with the user override applied.
  lnb::ThemeMetrics metrics;

  QPixmap background;
  bool has_background = false;
  // The path the current background (or the last failed attempt) came from. An
  // unchanged path is never re-decoded, so a restyle does not re-read a large
  // image from disk.
  std::optional<std::string> background_path;

  QFont ui_font;
  QFont chat_font;
  QFont mono_font;

  std::optional<QColor> tint;
};

ThemeState& state() {
  static ThemeState instance;
  return instance;
}

QColor read_color(const QJsonObject& colors, const char* name, const QColor& fallback) {
  const QJsonValue value = colors.value(QLatin1String(name));

  if (!value.isArray()) {
    return fallback;
  }

  const QJsonArray array = value.toArray();
  if (array.size() < 4) {
    return fallback;
  }

  // Qt JSON stores every number as a double. Values above 1 are 0-255
  // components; smaller values are already normalised.
  const auto rgb = [](const QJsonValue& channel) {
    const double number = channel.toDouble();
    return static_cast<int>(number > 1.0 ? number : number * 255.0);
  };

  return QColor(
      rgb(array.at(0)), rgb(array.at(1)), rgb(array.at(2)),
      static_cast<int>(array.at(3).toDouble() * 255.0)
  );
}

ThemeColors parse_colors(const QJsonObject& colors) {
  ThemeColors result;
  result.window = read_color(colors, "window", QColor(17, 17, 27));
  result.surface = read_color(colors, "surface", QColor(24, 24, 37));
  result.base = read_color(colors, "base", QColor(30, 30, 46));

  result.text = read_color(colors, "text", QColor(205, 214, 244));
  result.text_muted = read_color(colors, "text_muted", QColor(166, 173, 200));

  result.accent = read_color(colors, "accent", QColor(245, 194, 231));
  result.accent_hover = read_color(colors, "accent_hover", QColor(242, 205, 205));
  result.accent_active = read_color(colors, "accent_active", QColor(235, 160, 172));
  result.accent_text = read_color(colors, "accent_text", QColor(17, 17, 27));

  result.bubble_user = read_color(colors, "bubble_user", QColor(49, 50, 68));
  result.bubble_user_text = read_color(colors, "bubble_user_text", QColor(205, 214, 244));
  result.bubble_assistant = read_color(colors, "bubble_assistant", QColor(49, 50, 68));
  result.bubble_assistant_text =
      read_color(colors, "bubble_assistant_text", QColor(205, 214, 244));
  result.bubble_assistant_code =
      read_color(colors, "bubble_assistant_code", QColor(49, 50, 68));

  // Default to the window colour so a theme that predates these keys keeps its
  // previous look (the transcript and the field were transparent over the window).
  result.conversation_background =
      read_color(colors, "conversation_background", result.window);
  result.input_background = read_color(colors, "input_background", result.window);

  result.border = read_color(colors, "border", QColor(69, 71, 90));

  result.danger = read_color(colors, "danger", QColor(243, 139, 168));
  result.danger_text = read_color(colors, "danger_text", QColor(17, 17, 27));

  return result;
}

lnb::ThemeMetrics parse_metrics(const QJsonObject& object) {
  lnb::ThemeMetrics metrics;

  const auto read_int = [&object](const char* key, int& field) {
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isDouble()) {
      field = value.toInt();
    }
  };

  read_int("sidebar_width", metrics.sidebar_width);
  read_int("message_spacing", metrics.message_spacing);
  read_int("page_margin", metrics.page_margin);
  read_int("composer_height", metrics.composer_height);
  read_int("composer_height_expanded", metrics.composer_height_expanded);
  read_int("title_font_size", metrics.title_font_size);
  read_int("session_item_height", metrics.session_item_height);
  read_int("line_height_other", metrics.line_height_other);
  read_int("line_height_self", metrics.line_height_self);
  read_int("paragraph_spacing", metrics.paragraph_spacing);
  read_int("list_paragraph_spacing", metrics.list_paragraph_spacing);
  read_int("message_max_width", metrics.message_max_width);
  read_int("bubble_radius", metrics.bubble_radius);

  return metrics;
}

std::optional<QJsonObject> read_theme_root() {
  const QStringList candidates = {
      QStringLiteral("themes/theme.json"),
      QStringLiteral("../themes/theme.json"),
      QStringLiteral("theme.json"),
      QCoreApplication::applicationDirPath() + QStringLiteral("/themes/theme.json"),
      // The theme compiled into the binary. It is the last candidate, so a file
      // above it still wins and the theme stays overridable, but a build always
      // has a usable default even when no file sits beside the executable.
      QStringLiteral(":/themes/theme.json"),
  };

  for (const QString& candidate : candidates) {
    QFile file(candidate);
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

Theme parse_theme(const std::optional<QJsonObject>& root, const std::string& name) {
  Theme theme;

  if (!root.has_value()) {
    return theme;
  }

  const QJsonObject themes = root->value(QStringLiteral("themes")).toObject();

  QJsonObject entry;
  const QString wanted = QString::fromStdString(name);

  if (themes.contains(wanted)) {
    entry = themes.value(wanted).toObject();
    theme.name = wanted;
  } else if (!themes.isEmpty()) {
    // Fall back to the first theme so a renamed theme never blanks the app.
    const QString first = themes.keys().first();
    entry = themes.value(first).toObject();
    theme.name = first;
  }

  theme.dark = entry.value(QStringLiteral("dark")).toBool(true);
  theme.colors = parse_colors(entry.value(QStringLiteral("colors")).toObject());
  theme.metrics = parse_metrics(root->value(QStringLiteral("default_metrics")).toObject());

  return theme;
}

void load_background(const lnb::Settings& settings) {
  ThemeState& current = state();

  const std::optional<std::string>& wanted = settings.background_url;

  // An unchanged path means the decoded pixmap (or the recorded failure) is
  // still current. Clearing the path, or changing it, always reloads.
  if (current.background_path == wanted) {
    return;
  }

  current.background_path = wanted;
  current.background = QPixmap{};
  current.has_background = false;

  if (!wanted.has_value() || wanted->empty()) {
    return;
  }

  const QString path = QString::fromStdString(*wanted);
  const QPixmap pixmap(path);

  if (pixmap.isNull()) {
    shark::log::error("background image could not be loaded: {}", path.toStdString());
    return;
  }

  current.background = pixmap;
  current.has_background = true;
}

std::optional<QColor> resolve_override(const lnb::Settings& settings) {
  if (!settings.background_override.has_value()) {
    return std::nullopt;
  }

  const auto& channels = *settings.background_override;
  const auto to_byte = [](float value) {
    return static_cast<int>(value > 1.0f ? value : value * 255.0f);
  };

  return QColor(
      to_byte(channels[0]), to_byte(channels[1]), to_byte(channels[2]),
      to_byte(channels[3])
  );
}

QString build_stylesheet() {
  const ThemeColors& c = state().theme.colors;
  const lnb::ThemeMetrics& m = state().metrics;

  const auto rgba = [](const QColor& color) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alphaF());
  };

  // A small, scoped stylesheet: the chrome (title bar, sidebar rows, transcript)
  // is painted by hand, so only the standard controls are styled here.
  //
  // Backgrounds and borders are emitted through rgba() so a translucent theme
  // lets the background image show through the standard controls too; QColor's
  // name() would drop the alpha and force them opaque.
  QString sheet;

  sheet += QStringLiteral("QWidget { color: %1; }").arg(rgba(c.text));

  sheet += QStringLiteral(
               "QLineEdit, QPlainTextEdit, QSpinBox, QComboBox {"
               "  background: %1; border: 1px solid %2; border-radius: 6px;"
               "  padding: 4px 6px; selection-background-color: %3;"
               "  selection-color: %4; }")
               .arg(rgba(c.base), rgba(c.border), rgba(c.accent), rgba(c.accent_text));

  sheet += QStringLiteral(
               "QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QComboBox:focus {"
               "  border: 1px solid %1; }")
               .arg(rgba(c.accent));

  sheet += QStringLiteral(
               "QPushButton {"
               "  background: %1; border: 1px solid %2; border-radius: 6px;"
               "  padding: 5px 12px; }"
               "QPushButton:hover { background: %3; }"
               "QPushButton:pressed { background: %4; }"
               "QPushButton:disabled { color: %5; }")
               .arg(rgba(c.base), rgba(c.border), rgba(c.surface), rgba(c.accent_active),
                    rgba(c.text_muted));

  sheet += QStringLiteral(
               "QToolButton { background: transparent; border: none; border-radius: 4px;"
               "  padding: 2px; }"
               "QToolButton:hover { background: %1; }")
               .arg(rgba(c.surface));

  sheet += QStringLiteral(
               "QScrollBar { background: transparent; }"
               "QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }"
               "QScrollBar::handle:vertical { background: %1; border-radius: 5px;"
               "  min-height: 24px; }"
               "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
               "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
               "  background: transparent; }"
               "QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }"
               "QScrollBar::handle:horizontal { background: %1; border-radius: 5px;"
               "  min-width: 24px; }"
               "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
               "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
               "  background: transparent; }")
               .arg(rgba(c.border));

  sheet += QStringLiteral(
               "QMenu { background: %1; border: 1px solid %2; padding: 4px; }"
               "QMenu::item { padding: 5px 18px; border-radius: 4px; }"
               "QMenu::item:selected { background: %3; }")
               .arg(rgba(c.surface), rgba(c.border), rgba(c.accent));

  sheet += QStringLiteral("QLabel { background: transparent; }");

  // The composer and the session list keep their own painted backgrounds.
  sheet += QStringLiteral(
               "QListView, QListWidget { background: transparent; border: none;"
               "  outline: none; }");

  // Dialogs stay opaque: a dialog is a top-level window with no background image
  // behind it, so an alpha fill would only darken it against the desktop.
  sheet += QStringLiteral(
               "QDialog { background: %1; }"
               "QSpinBox::up-button, QSpinBox::down-button { width: 14px; }")
               .arg(c.window.name());

  Q_UNUSED(m);
  return sheet;
}

} // namespace

void apply_theme(QApplication& app, const lnb::Settings& settings) {
  ThemeState& current = state();

  const std::optional<QJsonObject> root = read_theme_root();

  if (!root.has_value()) {
    // parse_theme() keeps its default-constructed colours when there is no
    // definition, which render as black; make that failure visible rather than
    // silently shipping a black window.
    shark::log::error(
        "no theme.json found in the working directory, next to the executable, "
        "or in the compiled resources; the UI will render without themed colours"
    );
  }

  current.theme = parse_theme(root, settings.theme);

  // The user override wins wholesale when present; otherwise the theme's own
  // defaults apply.
  current.metrics = settings.metrics.has_value() ? *settings.metrics : current.theme.metrics;

  QFont ui;
  if (!settings.ui_font.empty()) {
    ui.setFamily(QString::fromStdString(settings.ui_font));
  }
  if (settings.ui_font_size > 0) {
    ui.setPointSize(settings.ui_font_size);
  }
  app.setFont(ui);
  current.ui_font = ui;

  QFont chat = ui;
  if (!settings.font.empty()) {
    chat.setFamily(QString::fromStdString(settings.font));
  }
  if (settings.font_size > 0) {
    chat.setPointSize(settings.font_size);
  }
  current.chat_font = chat;

  QFont mono = chat;
  if (!settings.mono_font.empty()) {
    mono.setFamily(QString::fromStdString(settings.mono_font));
  }
  if (settings.mono_font_size > 0) {
    mono.setPointSize(settings.mono_font_size);
  }
  mono.setStyleHint(QFont::Monospace);
  mono.setFixedPitch(true);
  current.mono_font = mono;

  const ThemeColors& c = current.theme.colors;

  QPalette palette = app.palette();
  palette.setColor(QPalette::Window, c.window);
  palette.setColor(QPalette::WindowText, c.text);
  palette.setColor(QPalette::Base, c.base);
  palette.setColor(QPalette::AlternateBase, c.surface);
  palette.setColor(QPalette::Text, c.text);
  palette.setColor(QPalette::PlaceholderText, c.text_muted);
  palette.setColor(QPalette::Button, c.base);
  palette.setColor(QPalette::ButtonText, c.text);
  palette.setColor(QPalette::Highlight, c.accent);
  palette.setColor(QPalette::HighlightedText, c.accent_text);
  palette.setColor(QPalette::ToolTipBase, c.surface);
  palette.setColor(QPalette::ToolTipText, c.text);
  palette.setColor(QPalette::Disabled, QPalette::Text, c.text_muted);
  palette.setColor(QPalette::Disabled, QPalette::WindowText, c.text_muted);
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, c.text_muted);
  app.setPalette(palette);

  app.setStyleSheet(build_stylesheet());

  load_background(settings);
  current.tint = resolve_override(settings);

  shark::log::debug("theme '{}' applied", current.theme.name.toStdString());
}

const Theme& theme() {
  return state().theme;
}

const lnb::ThemeMetrics& metrics() {
  return state().metrics;
}

lnb::ThemeMetrics& mutable_metrics() {
  return state().metrics;
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
  return state().metrics.line_height_other;
}

int line_height_self() {
  return state().metrics.line_height_self;
}

int paragraph_spacing() {
  return state().metrics.paragraph_spacing;
}

int list_paragraph_spacing() {
  return state().metrics.list_paragraph_spacing;
}

int message_max_width() {
  return state().metrics.message_max_width;
}

QStringList theme_names() {
  const std::optional<QJsonObject> root = read_theme_root();
  if (!root.has_value()) {
    return {QStringLiteral("Catppuccin Mocha")};
  }

  return root->value(QStringLiteral("themes")).toObject().keys();
}

void refresh_stylesheet(QApplication& app) {
  app.setStyleSheet(build_stylesheet());
}

void show_parent_background(QWidget* widget) {
  widget->setStyleSheet(QStringLiteral("background: transparent;"));
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

QColor row_hover_background() {
  QColor color = state().theme.colors.base;
  color.setAlpha(120);
  return color;
}

QColor row_selected_background() {
  return state().theme.colors.base;
}

QIcon resource_icon(const QString& name, const QColor& color, const QColor& disabled) {
  const QPixmap artwork(QStringLiteral(":/icons/%1.png").arg(name));

  if (artwork.isNull()) {
    return QIcon();
  }

  // SourceIn keeps the artwork's own alpha (its shape) and takes the fill colour
  // for every painted pixel, so a black-on-transparent PNG becomes a tinted icon.
  const auto tint = [&artwork](const QColor& shade) {
    QPixmap pixmap = artwork;

    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), shade);
    painter.end();

    return pixmap;
  };

  QIcon icon;
  icon.addPixmap(tint(color), QIcon::Normal);
  icon.addPixmap(tint(disabled), QIcon::Disabled);
  return icon;
}

QIcon application_icon() {
  // Every embedded size, so the window manager, the taskbar and a high-DPI dock
  // each pick the one that matches their scale. Missing sizes are simply skipped.
  QIcon icon;

  for (const int size : {16, 24, 32, 48, 64, 128, 256, 512}) {
    const QPixmap pixmap(QStringLiteral(":/appicon/app-%1.png").arg(size));
    if (!pixmap.isNull()) {
      icon.addPixmap(pixmap);
    }
  }

  return icon;
}

} // namespace ui
