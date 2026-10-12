#include "external.hpp"
#include "context_manager.hpp"
#include "ui.hpp"

#include <QByteArray>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <mmc/log.hpp>
#include <mmc/str.hpp>

#include <deque>
#include <format>
#include <map>
#include <string_view>

// ==============================================================================================
// LLAMA API (Qt networking / Qt JSON)
// ==============================================================================================

namespace lnb {
namespace {

// Points the access manager at the session's SOCKS5 proxy, if one is set.
void configure_proxy(
    QNetworkAccessManager& manager, const std::optional<std::string>& proxy
) {
  if (!proxy.has_value() || proxy->empty()) {
    return;
  }

  const auto parts = mmc::str::split(*proxy, ':');

  if (parts.size() != 2 || parts[0].empty() || parts[1].empty()) {
    throw std::runtime_error("Invalid SOCKS5 proxy address: " + *proxy);
  }

  const int port = std::stoi(parts[1]);

  if (port <= 0 || port > 65535) {
    throw std::runtime_error("Invalid SOCKS5 proxy port: " + *proxy);
  }

  QNetworkProxy network_proxy;
  network_proxy.setType(QNetworkProxy::Socks5Proxy);
  network_proxy.setHostName(QString::fromStdString(parts[0]));
  network_proxy.setPort(static_cast<quint16>(port));

  manager.setProxy(network_proxy);
}

// Incrementally decodes an OpenAI-compatible Server-Sent Events stream.
//
// Parsing happens inside a Qt signal handler (readyRead), and throwing across
// a signal is undefined behaviour, so this class never throws: it records the
// failure in `failed_`/`error_` and reports "finished" so the caller can leave
// its event loop and then act on the recorded state.
class SseStream {
public:
  SseStream(GenerationTask& task, std::stop_token stop_token)
      : task_(task), stop_token_(stop_token) {
  }

  // Feeds a chunk of raw bytes. Returns false once no more input is needed
  // (either [DONE] arrived or a fatal error was recorded).
  bool feed(const QByteArray& chunk) {
    buffer_.append(chunk);

    int newline = -1;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
      const QByteArray line = buffer_.left(newline);
      buffer_.remove(0, newline + 1);

      if (!handle_line(line)) {
        return false;
      }
    }

    return true;
  }

  // Processes a trailing record that was not terminated by a newline.
  void flush() {
    if (finished_ || buffer_.isEmpty()) {
      return;
    }

    QByteArray line = buffer_;
    buffer_.clear();
    handle_line(line);
  }

  [[nodiscard]] bool finished() const { return finished_; }
  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] const std::string& text() const { return full_reply_; }
  [[nodiscard]] const std::string& finish_reason() const { return finish_reason_; }

  [[nodiscard]] std::vector<external_tools::ToolCall> tool_calls() const {
    std::vector<external_tools::ToolCall> calls;
    calls.reserve(tool_calls_.size());

    for (const auto& [index, call]: tool_calls_) {
      calls.push_back(call);
    }

    return calls;
  }

  [[nodiscard]] QJsonObject assistant_message() const {
    QJsonObject message{
        {"role", "assistant"},
        {"content", QString::fromStdString(full_reply_)},
    };

    if (!tool_calls_.empty()) {
      QJsonArray calls;

      for (const auto& [index, call]: tool_calls_) {
        calls.append(QJsonObject{
            {"id", call.id},
            {"type", "function"},
            {"function",
             QJsonObject{{"name", call.name}, {"arguments", call.arguments}}},
        });
      }

      message.insert("tool_calls", calls);
    }

    return message;
  }

private:
  // Handles a single SSE line. Returns false when processing should stop.
  bool handle_line(const QByteArray& raw_line) {
    if (stop_token_.stop_requested()) {
      finished_ = true;
      return false;
    }

    QByteArray line = raw_line;
    if (line.endsWith('\r')) {
      line.chop(1);
    }

    // Only `data:` records carry payload; comments and keep-alives are ignored.
    if (!line.startsWith("data:")) {
      return true;
    }

    QByteArray data = line.mid(5);
    if (data.startsWith(' ')) {
      data.remove(0, 1);
    }

    if (data == "[DONE]") {
      finished_ = true;
      return false;
    }

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parse_error);

    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
      // A malformed keep-alive must not abort the whole stream.
      mmc::log::debug(
          "SSE JSON parse failed: {}", parse_error.errorString().toStdString()
      );
      return true;
    }

    const QJsonObject root = document.object();
    const QJsonArray choices = root.value("choices").toArray();

    if (choices.isEmpty()) {
      return true;
    }

    const QJsonObject choice = choices.first().toObject();

    const QJsonValue reason = choice.value("finish_reason");
    if (reason.isString()) {
      finish_reason_ = reason.toString().toStdString();
    }

    const QJsonObject delta = choice.value("delta").toObject();

    const QJsonValue content = delta.value("content");
    if (content.isString()) {
      const QByteArray token = content.toString().toUtf8();

      if (!task_.append_content(
              std::string_view(token.constData(), static_cast<std::size_t>(token.size()))
          )) {
        failed_ = true;
        error_ = "Generation is no longer active";
        finished_ = true;
        return false;
      }

      full_reply_.append(token.constData(), static_cast<std::size_t>(token.size()));
    }

    // Tool calls arrive split across chunks; merge them per `index`.
    const QJsonArray calls = delta.value("tool_calls").toArray();

    for (const QJsonValue& item: calls) {
      const QJsonObject part = item.toObject();

      const int index = part.value("index").toInt(static_cast<int>(tool_calls_.size()));

      auto& call = tool_calls_[index];
      call.index = index;

      const QString id = part.value("id").toString();
      if (!id.isEmpty()) {
        call.id = id;
      }

      const QJsonObject function = part.value("function").toObject();
      call.name += function.value("name").toString();
      call.arguments += function.value("arguments").toString();
    }

    return true;
  }

  GenerationTask& task_;
  std::stop_token stop_token_;

  QByteArray buffer_;
  std::string full_reply_;
  std::string finish_reason_;
  std::map<int, external_tools::ToolCall> tool_calls_;

  bool finished_ = false;
  bool failed_ = false;
  std::string error_;
};

// Declares the local tools exposed to the model.
QJsonArray make_repository_tools() {
  const QJsonObject list_function{
      {"name", "list_directory"},
      {"description",
       "List files and subdirectories immediately inside a directory in the "
       "configured shared directory. Directories end with /."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{{"path",
                         QJsonObject{
                             {"type", "string"},
                             {"description", "Directory-relative path; use . for the shared directory"}}}}},
           {"required", QJsonArray{"path"}},
           {"additionalProperties", false}}},
  };

  const QJsonObject search_function{
      {"name", "search_code"},
      {"description",
       "Search source code in the configured local directories. Returns matching file paths, line numbers, and surrounding context."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{
                {"query", QJsonObject{{"type", "string"}, {"description", "Symbol name or text to search for"}}},
                {"path", QJsonObject{{"type", "string"}, {"description", "Directory-relative search path"}}}
            }},
           {"required", QJsonArray{"query", "path"}},
           {"additionalProperties", false}
       }}
  };


  const QJsonObject read_function{
      {"name", "read_file"},
      {"description",
       "Read a limited range of lines from a file in the configured local "
       "directories."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{
                {"path",
                 QJsonObject{
                     {"type", "string"},
                     {"description", "Repository-relative file path"}}},
                {"start_line", QJsonObject{{"type", "integer"}}},
                {"end_line", QJsonObject{{"type", "integer"}}}}},
           {"required", QJsonArray{"path", "start_line", "end_line"}},
           {"additionalProperties", false}}},
  };

  return QJsonArray{
      QJsonObject{{"type", "function"}, {"function", list_function}},
      QJsonObject{{"type", "function"}, {"function", search_function}},
      QJsonObject{{"type", "function"}, {"function", read_function}},
  };
}

} // namespace

std::optional<QJsonObject> prepare_payload(
    ApplicationState& app_state, uint32_t session_id, uint32_t self_id,
    const std::string& model
) {
  const auto& cfg = *app_state.uni_config_;
  const size_t response_budget = cfg.response_token_budget_;

  auto messages = app_state.uni_db_->get_messages_for_session(session_id, self_id);
  auto lore_entries = app_state.uni_db_->get_lorebook_for_session(session_id);

  std::optional<session> session_info_opt = app_state.uni_db_->get_session_by_id(session_id);

  // A missing session is a normal, recoverable condition: the caller shows an
  // error bubble instead of aborting the whole event handler.
  if (!session_info_opt.has_value()) {
    mmc::log::info("prepare_payload: session {} does not exist", session_id);
    return std::nullopt;
  }

  const session& session_info = *session_info_opt;

  if (session_info.custom_instructions_.empty()) {
    mmc::log::info(
        "Session {} has no custom instructions; using a generic system prompt",
        session_id
    );
  }

  std::string context_text;
  context_text.reserve(messages.size() * 64);

  for (const auto& m: messages) {
    context_text += m.content;
    context_text += '\n';
  }

  std::vector<std::string> injected_lore;
  injected_lore.reserve(lore_entries.size());

  const std::string context_lower = mmc::str::to_lower(context_text);

  for (const auto& lore: lore_entries) {
    if (lore.keyword.empty()) {
      continue;
    }

    const std::string keyword_lower = mmc::str::to_lower(lore.keyword);

    if (context_lower.find(keyword_lower) != std::string::npos) {
      injected_lore.emplace_back(lore.keyword + ": " + lore.content);
    }
  }

  std::string system_prompt = session_info.custom_instructions_;

  if (!session_info.memory_summary_.empty()) {
    if (!system_prompt.empty()) system_prompt += "\n\n";
    system_prompt += "[CONVERSATION MEMORY]\n";
    system_prompt += session_info.memory_summary_;
  }

  if (!injected_lore.empty()) {
    if (!system_prompt.empty()) {
      system_prompt += "\n\n";
    }

    system_prompt += "[LOREBOOK]\n";

    for (const auto& l: injected_lore) {
      system_prompt += "- ";
      system_prompt += l;
      system_prompt += '\n';
    }
  }

  if (system_prompt.empty()) {
    system_prompt = "You are a helpful assistant.";
  }

  QJsonArray payload_messages;
  payload_messages.append(
      QJsonObject{{"role", "system"}, {"content", QString::fromStdString(system_prompt)}}
  );

  const auto selected = context::select_recent(
      messages, self_id, system_prompt, session_info.memory_summary_, cfg);
  for (const auto& item : selected.messages) {
    payload_messages.append(item);
  }

  return QJsonObject{
      {"model", QString::fromStdString(model)},
      {"tools", make_repository_tools()},
      {"messages", payload_messages},
      {"max_tokens", static_cast<int>(response_budget)},
      {"stream", true},
  };
}

ChatCompletionResult send_message_llama_once(
    const QJsonObject& payload, GenerationTask& task, std::stop_token stop_token,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy
) {
  auto throw_if_stopped = [&stop_token]() {
    if (stop_token.stop_requested()) {
      throw std::runtime_error("Generation cancelled");
    }
  };

  throw_if_stopped();

  QNetworkAccessManager manager;
  // Abort a request that stalls for too long so a dead server cannot pin a
  // worker thread forever. The timeout measures inactivity, so an actively
  // streaming response is never interrupted.
  manager.setTransferTimeout(60'000);

  configure_proxy(manager, proxy);

  QNetworkRequest request(QUrl(QString::fromStdString(api_url)));
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setRawHeader("Accept", "text/event-stream");

  // Force HTTP/1.1; the input is an SSE stream and HTTP/2 adds no benefit here.
  request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

  if (!api_key.empty()) {
    request.setRawHeader(
        "Authorization", QByteArray("Bearer ") + QByteArray::fromStdString(api_key)
    );
  }

  QNetworkReply* reply =
      manager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));

  SseStream stream(task, stop_token);

  QEventLoop loop;

  // Keep the first bytes of the response around so an HTTP error body can be
  // reported; on success the captured prefix is simply ignored.
  QByteArray captured;
  constexpr int CAPTURE_LIMIT = 2048;

  QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
    const QByteArray chunk = reply->readAll();

    if (captured.size() < CAPTURE_LIMIT) {
      captured.append(chunk.left(CAPTURE_LIMIT - captured.size()));
    }

    if (!stream.feed(chunk)) {
      loop.quit();
    }
  });

  QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() { loop.quit(); });

  // Cancellation has to be observed from inside the event loop, so poll the
  // stop token and abort the request when the user cancels.
  QTimer watchdog;
  watchdog.setInterval(50);
  QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() {
    if (stop_token.stop_requested()) {
      reply->abort();
    }
  });
  watchdog.start();

  loop.exec();

  const QByteArray trailing = reply->readAll();

  if (!trailing.isEmpty()) {
    if (captured.size() < CAPTURE_LIMIT) {
      captured.append(trailing.left(CAPTURE_LIMIT - captured.size()));
    }

    stream.feed(trailing);
  }

  stream.flush();

  const bool aborted_by_stop = stop_token.stop_requested();
  const QNetworkReply::NetworkError network_error = reply->error();
  const QString network_error_string = reply->errorString();
  const int http_status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  watchdog.stop();

  // The reply was created without a parent and no event loop remains, so
  // destroy it directly rather than with deleteLater().
  delete reply;

  if (aborted_by_stop) {
    throw std::runtime_error("Generation cancelled");
  }

  if (stream.failed()) {
    throw std::runtime_error(stream.error());
  }

  if (network_error != QNetworkReply::NoError) {
    std::string message =
        "HTTP request failed: " + network_error_string.toStdString();

    if (http_status > 0) {
      message += std::format(" (HTTP {})", http_status);
    }

    if (!captured.isEmpty()) {
      message += ": " + captured.toStdString();
    }

    throw std::runtime_error(message);
  }

  if (!stream.finished()) {
    throw std::runtime_error("SSE stream ended before [DONE]");
  }

  ChatCompletionResult result;
  result.text = stream.text();
  result.finish_reason = stream.finish_reason();
  result.tool_calls = stream.tool_calls();
  result.assistant_message = stream.assistant_message();

  mmc::log::info(
      "send_message_llama_once tool_calls={}, finish_reason={}",
      result.tool_calls.size(), result.finish_reason
  );

  return result;
}

ChatCompletionResult generate_reply(
    QJsonObject payload, GenerationTask& task, std::stop_token stop_token,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy,
    const std::optional<std::string>& shared_directory,
    int max_tool_rounds,
    int max_tool_calls
) {
  const int max_rounds = max_tool_rounds;
  const int max_calls = max_tool_calls;
  int total_calls = 0;

  ChatCompletionResult result;

  for (int round = 0; round < max_rounds; ++round) {
    if (stop_token.stop_requested()) {
      throw std::runtime_error("Generation cancelled");
    }

    result = send_message_llama_once(payload, task, stop_token, api_url, api_key, proxy);

    if (result.tool_calls.empty()) {
      return result;
    }

    QJsonArray messages = payload.value("messages").toArray();

    // The assistant message carrying the tool calls must be echoed back
    // verbatim before the results so the model can match them by id.
    messages.append(result.assistant_message);

    if (total_calls + static_cast<int>(result.tool_calls.size()) > max_calls) {
      throw std::runtime_error("Tool-call budget exhausted");
    }
    total_calls += static_cast<int>(result.tool_calls.size());

    for (const auto& call: result.tool_calls) {
      task.set_status(
          call.name == QStringLiteral("read_file")
              ? "Reading files..."
              : call.name == QStringLiteral("search_code")
                  ? "Searching code..."
                  : "Using tools..."
      );

      QString output;

      if (!shared_directory.has_value() || shared_directory->empty()) {
        output = "Tool error: this session has no shared directory configured";
      } else {
        output = external_tools::execute_local_tool(call, *shared_directory);
      }

      messages.append(QJsonObject{
          {"role", "tool"},
          {"tool_call_id", call.id},
          {"content", output},
      });
    }

    payload.insert("messages", messages);
  }

  mmc::log::info("generate_reply: reached the tool-call round cap ({})", max_rounds);

  return result;
}

} // namespace lnb
