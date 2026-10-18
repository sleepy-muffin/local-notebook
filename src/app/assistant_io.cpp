#include "app/assistant_io.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <filesystem>

#include "net/protocol.hpp"
#include "store/util.hpp"
#include "support/log.hpp"

namespace lnb {
namespace {

QJsonObject to_json(const Assistant& assistant) {
  QJsonObject object;
  object["name"] = QString::fromStdString(assistant.name_);
  object["custom_instructions"] = QString::fromStdString(assistant.custom_instructions_);
  object["api_url"] = QString::fromStdString(assistant.api_url_);
  object["authentication_token"] = QString::fromStdString(assistant.authentication_token_);
  object["model"] = QString::fromStdString(assistant.model_);
  object["proxy"] = assistant.proxy_.has_value()
      ? QJsonValue(QString::fromStdString(*assistant.proxy_))
      : QJsonValue(QJsonValue::Null);
  object["memory"] = QString::fromStdString(assistant.memory_);

  // The wire protocol this assistant talks, and the reasoning level a new
  // session starts on. Both are written even when defaulted, so the file always
  // states the choice explicitly.
  object["protocol"] = QString::fromStdString(net::protocol_name(assistant.protocol_));
  object["reasoning_effort"] = QString::fromStdString(assistant.reasoning_effort_);

  object["summary_token_budget"] = static_cast<double>(assistant.summary_token_budget_);
  object["summary_reasoning_headroom_tokens"] =
      static_cast<double>(assistant.summary_reasoning_headroom_tokens_);
  object["summary_trigger_tokens"] =
      static_cast<double>(assistant.summary_trigger_tokens_);
  object["summary_token_budget_for_session_summarization"] =
      static_cast<double>(assistant.summary_token_budget_for_session_summarization_);

  return object;
}

Assistant from_json(const QJsonObject& object) {
  Assistant assistant;

  assistant.name_ = object.value("name").toString().toStdString();
  assistant.custom_instructions_ =
      object.value("custom_instructions").toString().toStdString();
  assistant.api_url_ = object.value("api_url").toString().toStdString();
  assistant.authentication_token_ =
      object.value("authentication_token").toString().toStdString();
  assistant.model_ = object.value("model").toString().toStdString();

  if (object.value("proxy").isString()) {
    const QString proxy = object.value("proxy").toString();
    if (!proxy.isEmpty()) {
      assistant.proxy_ = proxy.toStdString();
    }
  }

  assistant.memory_ = object.value("memory").toString().toStdString();

  // A missing `protocol` reads as Responses, so an assistant written before the
  // switch existed routes through the Responses API by default. The base URL is
  // normalised so the pre-update full-endpoint value migrates to the base.

  assistant.summary_token_budget_ = static_cast<std::size_t>(
      object.value("summary_token_budget").toDouble(assistant.summary_token_budget_));
  assistant.summary_reasoning_headroom_tokens_ = static_cast<std::size_t>(
      object.value("summary_reasoning_headroom_tokens")
          .toDouble(assistant.summary_reasoning_headroom_tokens_));
  assistant.summary_trigger_tokens_ = static_cast<std::size_t>(
      object.value("summary_trigger_tokens").toDouble(assistant.summary_trigger_tokens_));
  assistant.summary_token_budget_for_session_summarization_ = static_cast<std::size_t>(
      object.value("summary_token_budget_for_session_summarization")
          .toDouble(assistant.summary_token_budget_for_session_summarization_));

  assistant.protocol_ =
      net::protocol_from_name(object.value("protocol").toString().toStdString());
  assistant.reasoning_effort_ =
      object.value("reasoning_effort").toString().toStdString();
  assistant.api_url_ = net::normalize_base_url(assistant.api_url_);

  return assistant;
}

} // namespace

Assistant load_assistant(const std::string& uuid) {
  const auto path = store::paths::assistant_settings(uuid);

  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::ReadOnly)) {
    return Assistant{};
  }

  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject()) {
    log::error("{} is not a JSON object", path.string());
    return Assistant{};
  }

  return from_json(document.object());
}

void save_assistant(const std::string& uuid, const Assistant& assistant) {
  const auto directory = store::paths::assistant_dir(uuid);
  std::error_code error;
  std::filesystem::create_directories(directory, error);

  const auto path = store::paths::assistant_settings(uuid);

  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    log::error("cannot write {}", path.string());
    return;
  }

  file.write(QJsonDocument(to_json(assistant)).toJson(QJsonDocument::Indented));
}

} // namespace lnb
