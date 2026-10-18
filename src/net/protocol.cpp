#include "net/protocol.hpp"

#include <QJsonValue>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <cctype>

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

QJsonArray responses_tools(bool filesystem) {
  QJsonArray tools;

  for (const QJsonValue& value: tool_definitions(filesystem)) {
    const QJsonObject tool = value.toObject();
    const QJsonObject function = tool.value("function").toObject();

    // Responses flattens the schema onto the tool itself instead of nesting it
    // under `function`.
    QJsonObject flattened{
        {"type", "function"},
        {"name", function.value("name")},
        {"description", function.value("description")},
        {"parameters", function.value("parameters")},
    };

    tools.append(flattened);
  }

  return tools;
}

} // namespace

QJsonObject build_request(ApiProtocol protocol, const Request& request) {
  if (protocol == ApiProtocol::Responses) {
    return build_responses_request(request);
  }

  return build_chat_request(request);
}

QJsonObject build_chat_request(const Request& request) {
  QJsonArray messages;

  if (!request.instructions.empty()) {
    messages.append(QJsonObject{
        {"role", "system"},
        {"content", QString::fromStdString(request.instructions)},
    });
  }

  for (const OutgoingMessage& message: request.messages) {
    switch (message.role) {
      case OutgoingMessage::Role::User:
        messages.append(QJsonObject{
            {"role", "user"},
            {"content", QString::fromStdString(message.content)},
        });
        break;

      case OutgoingMessage::Role::Assistant: {
        QJsonObject entry{
            {"role", "assistant"},
            {"content", QString::fromStdString(message.content)},
        };

        if (!message.tool_calls.empty()) {
          QJsonArray calls;

          for (const ToolCall& call: message.tool_calls) {
            calls.append(QJsonObject{
                {"id", call.id},
                {"type", "function"},
                {"function",
                 QJsonObject{{"name", call.name}, {"arguments", call.arguments}}},
            });
          }

          entry.insert("tool_calls", calls);
        }

        messages.append(entry);
        break;
      }

      case OutgoingMessage::Role::ToolResult:
        messages.append(QJsonObject{
            {"role", "tool"},
            {"tool_call_id", QString::fromStdString(message.tool_call_id)},
            {"content", QString::fromStdString(message.content)},
        });
        break;
    }
  }

  QJsonObject payload{
      {"model", QString::fromStdString(request.model)},
      {"messages", messages},
      {"stream", true},
  };

  if (request.include_tools) {
    payload.insert("tools", tool_definitions(request.filesystem_tools));
  }

  if (request.max_output_tokens > 0) {
    payload.insert("max_tokens", request.max_output_tokens);
  }

  if (!request.reasoning_effort.empty()) {
    payload.insert("reasoning_effort", QString::fromStdString(request.reasoning_effort));
  }

  return payload;
}

QJsonObject build_responses_request(const Request& request) {
  QJsonArray input;

  for (const OutgoingMessage& message: request.messages) {
    switch (message.role) {
      case OutgoingMessage::Role::User:
        input.append(QJsonObject{
            {"role", "user"},
            {"content", QString::fromStdString(message.content)},
        });
        break;

      case OutgoingMessage::Role::Assistant:
        // The assistant's own text is an ordinary message item; the calls it
        // requested become sibling `function_call` items, matched by `call_id`.
        if (!message.content.empty()) {
          input.append(QJsonObject{
              {"role", "assistant"},
              {"content", QString::fromStdString(message.content)},
          });
        }

        for (const ToolCall& call: message.tool_calls) {
          input.append(QJsonObject{
              {"type", "function_call"},
              {"call_id", call.id},
              {"name", call.name},
              {"arguments", call.arguments},
          });
        }
        break;

      case OutgoingMessage::Role::ToolResult:
        input.append(QJsonObject{
            {"type", "function_call_output"},
            {"call_id", QString::fromStdString(message.tool_call_id)},
            {"output", QString::fromStdString(message.content)},
        });
        break;
    }
  }

  // Earlier rounds of the tool loop (reasoning items, function calls and their
  // outputs) are replayed verbatim, after the conversation, in the exact order
  // they were produced. Reasoning items must keep their position ahead of the
  // call they belong to or the model loses the thread.
  for (const QJsonObject& item: request.raw_items) {
    input.append(item);
  }

  QJsonObject payload{
      {"model", QString::fromStdString(request.model)},
      {"input", input},
      {"stream", true},
  };

  if (request.include_tools) {
    payload.insert("tools", responses_tools(request.filesystem_tools));
  }

  if (!request.instructions.empty()) {
    payload.insert("instructions", QString::fromStdString(request.instructions));
  }

  if (request.max_output_tokens > 0) {
    payload.insert("max_output_tokens", request.max_output_tokens);
  }

  if (!request.reasoning_effort.empty()) {
    payload.insert(
        "reasoning", QJsonObject{{"effort", QString::fromStdString(request.reasoning_effort)}}
    );
  }

  return payload;
}

QJsonArray tools_for(ApiProtocol protocol, bool filesystem) {
  if (protocol == ApiProtocol::Responses) {
    return responses_tools(filesystem);
  }

  return tool_definitions(filesystem);
}

std::string normalize_base_url(const std::string& url) {
  std::string text = trim(url);

  for (const std::string_view suffix: {"/chat/completions", "/completions", "/responses"}) {
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
  const QUrl parsed(QString::fromStdString(trim(url)));

  if (!parsed.isValid() || parsed.host().isEmpty()) {
    return false;
  }

  const QString scheme = parsed.scheme();

  if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
    return false;
  }

  if (parsed.hasQuery() || parsed.hasFragment()) {
    return false;
  }

  QString path = parsed.path();

  while (path.endsWith(QLatin1Char('/'))) {
    path.chop(1);
  }

  return path.endsWith(QStringLiteral("/v1"));
}

std::string derive_endpoint(const std::string& base_url, ApiProtocol protocol) {
  const std::string base = normalize_base_url(base_url);

  return base +
      (protocol == ApiProtocol::Responses ? "/responses" : "/chat/completions");
}

std::vector<std::string> reasoning_efforts_for_model(const std::string& model) {
  const std::string id = normalize_model_id(model);

  if (id == "deepseek-v4.1-flash") {
    return {"none", "low", "high", "max"};
  }

  if (id == "gpt-6-luna") {
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

bool reasoning_effort_supported(const std::string& model, const std::string& effort) {
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
