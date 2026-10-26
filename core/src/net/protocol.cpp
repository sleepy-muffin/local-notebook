#include "net/protocol.hpp"

#include <boost/url.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace lnb::net {
namespace {

std::string trim(const std::string& text) {
  const auto not_space = [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) == 0;
  };

  std::size_t begin = 0;
  std::size_t end = text.size();

  while (begin < end && !not_space(text[begin])) {
    ++begin;
  }
  while (end > begin && !not_space(text[end - 1])) {
    --end;
  }

  return text.substr(begin, end - begin);
}

std::string lower_ascii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

// Model ids are compared with spaces folded to dashes, so the selector's list is
// not sensitive to how the user typed the name.
std::string normalize_model_id(const std::string& model) {
  std::string id = lower_ascii(trim(model));
  std::replace(id.begin(), id.end(), ' ', '-');
  return id;
}

boost::json::array responses_tools(const ToolPermissions& permissions) {
  boost::json::array tools;

  for (const boost::json::value& value: tool_definitions(permissions)) {
    const boost::json::object& tool = value.as_object();
    const boost::json::object& function = tool.at("function").as_object();

    // Responses flattens the schema onto the tool itself instead of nesting it
    // under `function`.
    boost::json::object flattened;
    flattened["type"] = "function";
    flattened["name"] = function.at("name");
    flattened["description"] = function.at("description");
    flattened["parameters"] = function.at("parameters");

    tools.emplace_back(std::move(flattened));
  }

  return tools;
}

// Responses output items carry `status`; the input schema does not accept it.
// Drop that bookkeeping while keeping the pairing fields (`id` for reasoning,
// `call_id` for function calls).
boost::json::object replay_item(const boost::json::object& item) {
  boost::json::object copy = item;

  if (copy.contains("status")) {
    copy.erase("status");
  }

  return copy;
}

} // namespace

boost::json::object build_request(ApiProtocol protocol, const Request& request) {
  if (protocol == ApiProtocol::Responses) {
    return build_responses_request(request);
  }

  return build_chat_request(request);
}

boost::json::object build_chat_request(const Request& request) {
  boost::json::array messages;

  if (!request.instructions.empty()) {
    boost::json::object entry;
    entry["role"] = "system";
    entry["content"] = request.instructions;
    messages.emplace_back(std::move(entry));
  }

  for (const OutgoingMessage& message: request.messages) {
    switch (message.role) {
      case OutgoingMessage::Role::User: {
        boost::json::object entry;
        entry["role"] = "user";
        entry["content"] = message.content;
        messages.emplace_back(std::move(entry));
        break;
      }

      case OutgoingMessage::Role::Assistant: {
        boost::json::object entry;
        entry["role"] = "assistant";
        entry["content"] = message.content;

        if (!message.tool_calls.empty()) {
          boost::json::array calls;

          for (const ToolCall& call: message.tool_calls) {
            boost::json::object function;
            function["name"] = call.name;
            function["arguments"] = call.arguments;

            boost::json::object item;
            item["id"] = call.id;
            item["type"] = "function";
            item["function"] = std::move(function);

            calls.emplace_back(std::move(item));
          }

          entry["tool_calls"] = std::move(calls);
        }

        messages.emplace_back(std::move(entry));
        break;
      }

      case OutgoingMessage::Role::ToolResult: {
        boost::json::object entry;
        entry["role"] = "tool";
        entry["tool_call_id"] = message.tool_call_id;
        entry["content"] = message.content;
        messages.emplace_back(std::move(entry));
        break;
      }
    }
  }

  boost::json::object payload;
  payload["model"] = request.model;
  payload["messages"] = std::move(messages);
  payload["stream"] = true;
  // Ask for the usage chunk the tasks window reports against. Without it the
  // readout could only be a character-based estimate.
  payload["stream_options"] = boost::json::object{{"include_usage", true}};

  if (request.include_tools) {
    payload["tools"] = tool_definitions(request.tool_permissions);
  }

  if (request.max_output_tokens > 0) {
    payload["max_tokens"] = request.max_output_tokens;
  }

  if (!request.reasoning_effort.empty()) {
    payload["reasoning_effort"] = request.reasoning_effort;
  }

  return payload;
}

boost::json::object build_responses_request(const Request& request) {
  boost::json::array input;

  for (const OutgoingMessage& message: request.messages) {
    switch (message.role) {
      case OutgoingMessage::Role::User: {
        boost::json::object entry;
        entry["role"] = "user";
        entry["content"] = message.content;
        input.emplace_back(std::move(entry));
        break;
      }

      case OutgoingMessage::Role::Assistant:
        // The assistant's own text is an ordinary message item; the calls it
        // requested become sibling `function_call` items, matched by `call_id`.
        if (!message.content.empty()) {
          boost::json::object entry;
          entry["role"] = "assistant";
          entry["content"] = message.content;
          input.emplace_back(std::move(entry));
        }

        for (const ToolCall& call: message.tool_calls) {
          boost::json::object entry;
          entry["type"] = "function_call";
          entry["call_id"] = call.id;
          entry["name"] = call.name;
          entry["arguments"] = call.arguments;
          input.emplace_back(std::move(entry));
        }
        break;

      case OutgoingMessage::Role::ToolResult: {
        boost::json::object entry;
        entry["type"] = "function_call_output";
        entry["call_id"] = message.tool_call_id;
        entry["output"] = message.content;
        input.emplace_back(std::move(entry));
        break;
      }
    }
  }

  // Earlier rounds of the tool loop (reasoning items, function calls and their
  // outputs) are replayed verbatim, after the conversation, in the exact order
  // they were produced. Reasoning items must keep their position ahead of the
  // call they belong to or the model loses the thread.
  for (const boost::json::object& item: request.raw_items) {
    input.emplace_back(replay_item(item));
  }

  boost::json::object payload;
  payload["model"] = request.model;
  payload["input"] = std::move(input);
  payload["stream"] = true;

  if (request.include_tools) {
    payload["tools"] = responses_tools(request.tool_permissions);
  }

  if (!request.instructions.empty()) {
    payload["instructions"] = request.instructions;
  }

  if (request.max_output_tokens > 0) {
    payload["max_output_tokens"] = request.max_output_tokens;
  }

  if (!request.reasoning_effort.empty()) {
    boost::json::object reasoning;
    reasoning["effort"] = request.reasoning_effort;
    payload["reasoning"] = std::move(reasoning);
  }

  return payload;
}

boost::json::array tools_for(
    ApiProtocol protocol, const ToolPermissions& permissions
) {
  if (protocol == ApiProtocol::Responses) {
    return responses_tools(permissions);
  }

  return tool_definitions(permissions);
}

std::string normalize_base_url(const std::string& url) {
  std::string text = trim(url);

  for (const std::string_view suffix:
       {"/chat/completions", "/completions", "/responses"}) {
    if (text.ends_with(suffix)) {
      text.erase(text.size() - suffix.size());
      break;
    }
  }

  while (!text.empty() && text.back() == '/') {
    text.pop_back();
  }

  return text;
}

bool is_valid_base_url(const std::string& url) {
  // parse_uri returns a view over the input, so the trimmed string must outlive
  // it. Parsing a temporary here leaves `result` pointing at freed storage and
  // makes scheme()/host()/path() return garbage.
  const std::string text = trim(url);
  const auto result = boost::urls::parse_uri(text);

  if (!result) {
    return false;
  }

  const auto view = *result;
  const std::string_view scheme = view.scheme();

  if (scheme != "http" && scheme != "https") {
    return false;
  }

  if (view.host().empty()) {
    return false;
  }

  if (view.has_query() || view.has_fragment()) {
    return false;
  }

  // `path()` decodes percent-escapes and returns a temporary string, so it must
  // be owned here rather than bound to a `string_view`.
  std::string path = view.path();

  while (!path.empty() && path.back() == '/') {
    path.pop_back();
  }

  return path.ends_with("/v1");
}

std::string derive_endpoint(
    const std::string& base_url, ApiProtocol protocol
) {
  const std::string base = normalize_base_url(base_url);

  return base +
      (protocol == ApiProtocol::Responses ? "/responses" : "/chat/completions");
}

std::vector<std::string> reasoning_efforts_for_model(const std::string& model) {
  const std::string id = normalize_model_id(model);

  if (id == "deepseek-v4.1-flash") {
    return {"none", "low", "high", "max"};
  }

  if (id == "gpt-6-luna" || id == "gpt-5.6-luna") {
    return {"none", "low", "medium", "high", "xhigh", "max"};
  }

  // GPT-6 Astra and GPT-6.1 Sol return HTTP 400 for `none`, so it is omitted
  // from their list (and from the rest of the GPT-6 family, which Luna aside
  // cannot disable reasoning either).
  if (id == "gpt-6-astra" || id == "gpt-6.1-sol" || id == "gpt-6-sol") {
    return {"low", "medium", "high", "xhigh", "max"};
  }

  return {"none", "low", "high"};
}

std::string default_reasoning_effort(const std::string& model) {
  const std::vector<std::string> efforts = reasoning_efforts_for_model(model);

  // The list is never empty, so front() is always a valid level.
  return efforts.front();
}

std::vector<std::string> reasoning_efforts_with_stored(
    const std::string& model, const std::string& stored
) {
  std::vector<std::string> efforts = reasoning_efforts_for_model(model);
  const std::string wanted = trim(stored);

  // Empty means "the model's own default", which the list already covers by not
  // naming it; only a concrete, unsupported level is appended.
  if (!wanted.empty() && !reasoning_effort_supported(model, wanted)) {
    efforts.push_back(wanted);
  }

  return efforts;
}

bool reasoning_effort_supported(
    const std::string& model, const std::string& effort
) {
  // An empty effort defers to the model's own default, which is always allowed.
  if (trim(effort).empty()) {
    return true;
  }

  const std::string wanted = lower_ascii(trim(effort));

  for (const std::string& candidate: reasoning_efforts_for_model(model)) {
    if (lower_ascii(candidate) == wanted) {
      return true;
    }
  }

  return false;
}

std::string protocol_name(ApiProtocol protocol) {
  return protocol == ApiProtocol::Responses ? "responses" : "chat_completions";
}

ApiProtocol protocol_from_name(const std::string& name) {
  // Anything unrecognised (including a missing key, read as "") defaults to
  // Responses, so an assistant predating this switch routes through Responses.
  return lower_ascii(trim(name)) == "chat_completions"
      ? ApiProtocol::ChatCompletions
      : ApiProtocol::Responses;
}

std::string ensure_scheme(const std::string& url) {
  const std::string text = trim(url);
  const std::string lowered = lower_ascii(text);

  if (lowered.starts_with("http://") || lowered.starts_with("https://")) {
    return text;
  }

  return "https://" + text;
}

} // namespace lnb::net
