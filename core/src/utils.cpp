#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <utility>
#include <fstream>

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include "shark/log.hpp"

#include "local_notebook/core/utils.hpp"
#include "local_notebook/core/model.hpp"

#include "net/protocol.hpp"

namespace {

boost::json::object to_json(const lnb::core::Assistant& assistant) {
  boost::json::object object;
  object["name"] = assistant.name_;
  object["custom_instructions"] = assistant.custom_instructions_;
  object["api_url"] = assistant.api_url_;
  object["authentication_token"] = assistant.authentication_token_;
  object["model"] = assistant.model_;
  object["proxy"] = assistant.proxy_.has_value()
      ? boost::json::value(*assistant.proxy_)
      : boost::json::value(nullptr);
  object["search_endpoint"] = assistant.search_endpoint_;
  object["memory"] = assistant.memory_;

  object["turn_timeout_seconds"] =
      static_cast<std::uint64_t>(assistant.turn_timeout_seconds_);
  object["summary_timeout_seconds"] =
      static_cast<std::uint64_t>(assistant.summary_timeout_seconds_);

  // The wire protocol this assistant talks, and the reasoning levels a new
  // session starts on and the assistant-wide summariser runs at. All are written
  // even when defaulted, so the file always states the choice explicitly.
  object["protocol"] = protocol_name(assistant.protocol_);
  object["reasoning_effort"] = assistant.reasoning_effort_;
  object["summary_reasoning_effort"] = assistant.summary_reasoning_effort_;

  object["summary_token_budget"] =
      static_cast<std::uint64_t>(assistant.summary_token_budget_);
  object["summary_trigger_tokens"] =
      static_cast<std::uint64_t>(assistant.summary_trigger_tokens_);
  object["summary_token_budget_for_session_summarization"] =
      static_cast<std::uint64_t>(
          assistant.summary_token_budget_for_session_summarization_
      );

  return object;
}

std::string string_or(
    const boost::json::object& object, std::string_view key
) {
  const boost::json::value* value = object.if_contains(key);

  return value != nullptr && value->is_string()
      ? std::string(value->as_string())
      : std::string{};
}

std::size_t size_or(
    const boost::json::object& object, std::string_view key, std::size_t fallback
) {
  const boost::json::value* value = object.if_contains(key);

  if (value == nullptr || !value->is_number()) {
    return fallback;
  }

  return static_cast<std::size_t>(value->to_number<double>());
}

lnb::core::Assistant from_json(const boost::json::object& object) {
  lnb::core::Assistant assistant;

  assistant.name_ = string_or(object, "name");
  assistant.custom_instructions_ = string_or(object, "custom_instructions");
  assistant.api_url_ = string_or(object, "api_url");
  assistant.authentication_token_ = string_or(object, "authentication_token");
  assistant.model_ = string_or(object, "model");

  if (const boost::json::value* proxy = object.if_contains("proxy");
      proxy != nullptr && proxy->is_string() && !proxy->as_string().empty()) {
    assistant.proxy_ = std::string(proxy->as_string());
  }

  assistant.memory_ = string_or(object, "memory");
  assistant.search_endpoint_ = string_or(object, "search_endpoint");

  assistant.turn_timeout_seconds_ = size_or(
      object, "turn_timeout_seconds", assistant.turn_timeout_seconds_
  );
  assistant.summary_timeout_seconds_ = size_or(
      object, "summary_timeout_seconds", assistant.summary_timeout_seconds_
  );

  // A missing `protocol` reads as Responses, so an assistant written before the
  // switch existed routes through the Responses API by default. The base URL is
  // normalised so the pre-update full-endpoint value migrates to the base.
  assistant.summary_token_budget_ = size_or(
      object, "summary_token_budget", assistant.summary_token_budget_
  );
  assistant.summary_trigger_tokens_ = size_or(
      object, "summary_trigger_tokens", assistant.summary_trigger_tokens_
  );
  assistant.summary_token_budget_for_session_summarization_ = size_or(
      object, "summary_token_budget_for_session_summarization",
      assistant.summary_token_budget_for_session_summarization_
  );

  assistant.protocol_ = lnb::core::protocol_from_name(string_or(object, "protocol"));
  assistant.reasoning_effort_ = string_or(object, "reasoning_effort");

  // A missing summary level reads as the shipped default ("none"), not the empty
  // "model default" that string_or() would return. A present value is kept
  // verbatim, including an empty one.
  if (const boost::json::value* summary_effort =
          object.if_contains("summary_reasoning_effort");
      summary_effort != nullptr && summary_effort->is_string()) {
    assistant.summary_reasoning_effort_ =
        std::string(summary_effort->as_string());
  }

  assistant.api_url_ = lnb::core::normalize_base_url(assistant.api_url_);

  return assistant;
}

} // namespace

/*
 * `uuid` must be valid or this function will throw exceptions
 */
lnb::core::Assistant lnb::core::store::load_assistant(const std::string& uuid) const {
  const auto path = this->assistant_settings(uuid);

  std::ifstream in(path, std::ios::binary);

  if (!in) {
    // return Assistant{};
    shark::raise("std::ifstream failed to open {}", path.string());
  }

  const std::string text{
      std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
  };

  boost::system::error_code error;
  const boost::json::value parsed = boost::json::parse(text, error);

  if (error || !parsed.is_object()) {
    // shark::log::error("{} is not a JSON object", path.string());
    shark::raise("{} is not a JSON object", path.string());
  }

  return from_json(parsed.as_object());
}

void lnb::core::store::save_assistant(const std::string& uuid, const Assistant& assistant) const {
  const auto directory = this->assistant_dir(uuid);
  std::error_code error;
  std::filesystem::create_directories(directory, error);

  const auto path = this->assistant_settings(uuid);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);

  if (!out) {
    shark::log::error("cannot write {}", path.string());
    return;
  }

  out << boost::json::serialize(to_json(assistant)) << '\n';
}

std::filesystem::path lnb::core::store::data_root() const {
  return this->g_data_root;
}

std::filesystem::path lnb::core::store::global_settings_path() const {
  return this->data_root() / "settings.json";
}


std::vector<std::string> lnb::core::store::scan_assistants() const {
  std::vector<std::string> result;

  std::error_code error;
  const std::filesystem::path root = this->data_root();
  if (!std::filesystem::is_directory(root, error)) {
    return result;
  }

  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    if (error) {
      break;
    }
    if (!entry.is_directory()) {
      continue;
    }
    if (std::filesystem::exists(entry.path() / "settings.json", error)) {
      result.push_back(entry.path().filename().string());
    }
  }

  std::sort(result.begin(), result.end());
  return result;
}


std::filesystem::path lnb::core::store::assistant_dir(std::string_view uuid) const {
  return data_root() / std::string(uuid);
}

std::filesystem::path lnb::core::store::assistant_settings(std::string_view uuid) const {
  return assistant_dir(uuid) / "settings.json";
}

std::filesystem::path lnb::core::store::assistant_db(std::string_view uuid) {
  return assistant_dir(uuid) / "data.db";
}

std::filesystem::path lnb::core::store::assistant_log(std::string_view uuid) {
  return assistant_dir(uuid) / "local-notebook.log";
}

std::filesystem::path lnb::core::store::assistant_lock(std::string_view uuid) {
  return assistant_dir(uuid) / "lock";
}
