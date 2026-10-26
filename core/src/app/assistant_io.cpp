#include "app/assistant_io.hpp"

#include "shark/net/utils.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "shark/log.hpp"
#include "net/protocol.hpp"
#include "utils/util.hpp"

namespace lnb {
namespace {

boost::json::object to_json(const Assistant& assistant) {
  boost::json::object object;
  object["name"] = assistant.name_;
  object["custom_instructions"] = assistant.custom_instructions_;
  object["api_url"] = assistant.api_url_;
  object["authentication_token"] = assistant.authentication_token_;
  object["model"] = assistant.model_;
  object["proxy"] = assistant.proxy_.has_value()
      ? boost::json::value(*assistant.proxy_)
      : boost::json::value(nullptr);
  object["memory"] = assistant.memory_;

  object["turn_timeout_seconds"] =
      static_cast<std::uint64_t>(assistant.turn_timeout_seconds_);
  object["summary_timeout_seconds"] =
      static_cast<std::uint64_t>(assistant.summary_timeout_seconds_);

  // The wire protocol this assistant talks, and the reasoning levels a new
  // session starts on and the assistant-wide summariser runs at. All are written
  // even when defaulted, so the file always states the choice explicitly.
  object["protocol"] = net::protocol_name(assistant.protocol_);
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

Assistant from_json(const boost::json::object& object) {
  Assistant assistant;

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

  assistant.protocol_ = net::protocol_from_name(string_or(object, "protocol"));
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

  assistant.api_url_ = net::normalize_base_url(assistant.api_url_);

  return assistant;
}

} // namespace

Assistant load_assistant(const std::string& uuid) {
  const auto path = store::paths::assistant_settings(uuid);

  std::ifstream in(path, std::ios::binary);

  if (!in) {
    return Assistant{};
  }

  const std::string text{
      std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
  };

  boost::system::error_code error;
  const boost::json::value parsed = boost::json::parse(text, error);

  if (error || !parsed.is_object()) {
    shark::log::error("{} is not a JSON object", path.string());
    return Assistant{};
  }

  return from_json(parsed.as_object());
}

void save_assistant(const std::string& uuid, const Assistant& assistant) {
  const auto directory = store::paths::assistant_dir(uuid);
  std::error_code error;
  std::filesystem::create_directories(directory, error);

  const auto path = store::paths::assistant_settings(uuid);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);

  if (!out) {
    shark::log::error("cannot write {}", path.string());
    return;
  }

  out << boost::json::serialize(to_json(assistant)) << '\n';
}

} // namespace lnb
