#include "settings/settings.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <filesystem>

#include "local-notebook-core/interface.hpp"
#include "mmc/log.hpp"

namespace lnb {
namespace {

QJsonObject metrics_to_json(const ThemeMetrics& metrics) {
  QJsonObject object;
  object["sidebar_width"] = metrics.sidebar_width;
  object["message_spacing"] = metrics.message_spacing;
  object["page_margin"] = metrics.page_margin;
  object["composer_height"] = metrics.composer_height;
  object["composer_height_expanded"] = metrics.composer_height_expanded;
  object["title_font_size"] = metrics.title_font_size;
  object["session_item_height"] = metrics.session_item_height;
  object["line_height_other"] = metrics.line_height_other;
  object["line_height_self"] = metrics.line_height_self;
  object["paragraph_spacing"] = metrics.paragraph_spacing;
  object["list_paragraph_spacing"] = metrics.list_paragraph_spacing;
  object["message_max_width"] = metrics.message_max_width;
  object["bubble_radius"] = metrics.bubble_radius;
  return object;
}

ThemeMetrics metrics_from_json(const QJsonObject& object, ThemeMetrics metrics) {
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

QJsonArray override_to_json(const std::array<float, 4>& color) {
  QJsonArray array;
  for (const float channel : color) {
    array.append(static_cast<double>(channel));
  }
  return array;
}

std::optional<std::array<float, 4>> override_from_json(const QJsonValue& value) {
  if (!value.isArray()) {
    return std::nullopt;
  }

  const QJsonArray array = value.toArray();
  if (array.size() < 4) {
    return std::nullopt;
  }

  return std::array<float, 4>{
      static_cast<float>(array.at(0).toDouble()),
      static_cast<float>(array.at(1).toDouble()),
      static_cast<float>(array.at(2).toDouble()),
      static_cast<float>(array.at(3).toDouble()),
  };
}

QJsonObject to_json(const Settings& settings) {
  QJsonObject object;
  object["last_assistant"] = QString::fromStdString(settings.last_assistant);
  object["theme"] = QString::fromStdString(settings.theme);
  object["language"] = QString::fromStdString(settings.language);

  object["background_url"] = settings.background_url.has_value()
      ? QJsonValue(QString::fromStdString(*settings.background_url))
      : QJsonValue(QJsonValue::Null);

  object["background_override"] = settings.background_override.has_value()
      ? QJsonValue(override_to_json(*settings.background_override))
      : QJsonValue(QJsonValue::Null);

  object["ui_font"] = QString::fromStdString(settings.ui_font);
  object["ui_font_size"] = settings.ui_font_size;
  object["font"] = QString::fromStdString(settings.font);
  object["font_size"] = settings.font_size;
  object["mono_font"] = QString::fromStdString(settings.mono_font);
  object["mono_font_size"] = settings.mono_font_size;

  object["auto_login"] = settings.auto_login;
  object["enter_with_modifier"] = settings.enter_with_modifier;
  object["rendered_message_limit"] = settings.rendered_message_limit;
  object["history_page_size"] = settings.history_page_size;

  object["geometry"] = settings.geometry.has_value()
      ? QJsonValue(QString::fromStdString(*settings.geometry))
      : QJsonValue(QJsonValue::Null);

  if (settings.metrics.has_value()) {
    object["metrics"] = metrics_to_json(*settings.metrics);
  }

  return object;
}

Settings from_json(const QJsonObject& object) {
  Settings settings;

  settings.last_assistant = object.value("last_assistant").toString().toStdString();

  const QString theme = object.value("theme").toString();
  if (!theme.isEmpty()) {
    settings.theme = theme.toStdString();
  }

  const QString language = object.value("language").toString();
  if (!language.isEmpty()) {
    settings.language = language.toStdString();
  }

  if (object.value("background_url").isString()) {
    settings.background_url = object.value("background_url").toString().toStdString();
  }
  settings.background_override = override_from_json(object.value("background_override"));

  settings.ui_font = object.value("ui_font").toString().toStdString();
  settings.ui_font_size = object.value("ui_font_size").toInt(settings.ui_font_size);
  settings.font = object.value("font").toString().toStdString();
  settings.font_size = object.value("font_size").toInt(settings.font_size);
  settings.mono_font = object.value("mono_font").toString().toStdString();
  settings.mono_font_size = object.value("mono_font_size").toInt(settings.mono_font_size);

  settings.auto_login = object.value("auto_login").toBool(settings.auto_login);
  settings.enter_with_modifier =
      object.value("enter_with_modifier").toBool(settings.enter_with_modifier);
  settings.rendered_message_limit =
      object.value("rendered_message_limit").toInt(settings.rendered_message_limit);
  settings.history_page_size =
      object.value("history_page_size").toInt(settings.history_page_size);

  if (object.value("geometry").isString()) {
    settings.geometry = object.value("geometry").toString().toStdString();
  }

  if (object.value("metrics").isObject()) {
    settings.metrics = metrics_from_json(object.value("metrics").toObject(), ThemeMetrics{});
  }

  return settings;
}

} // namespace

Settings load_settings() {
  const auto path = lnb::api::global_settings_path();

  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::ReadOnly)) {
    return Settings{};
  }

  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject()) {
    mmc::log::error("{} is not a JSON object; using defaults", path.string());
    return Settings{};
  }

  return from_json(document.object());
}

void save_settings(const Settings& settings) {
  const auto path = lnb::api::global_settings_path();

  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);

  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    mmc::log::error("cannot write {}", path.string());
    return;
  }

  file.write(QJsonDocument(to_json(settings)).toJson(QJsonDocument::Indented));
}

} // namespace lnb
