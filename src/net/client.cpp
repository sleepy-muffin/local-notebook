#include "net/client.hpp"

#include "context/context.hpp"
#include "net/proxy.hpp"
#include "store/database.hpp"
#include "support/log.hpp"

#include <QByteArray>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lnb::net {

namespace {

// Whitespace-trimmed copy of `text`; empty when it is all whitespace. Used to
// decide whether a round produced any narration and how it ended.
std::string trimmed(const std::string& text) {
  const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  const auto begin = std::find_if_not(text.begin(), text.end(), is_space);
  const auto end = std::find_if_not(text.rbegin(), text.rend(), is_space).base();

  if (begin >= end) {
    return {};
  }

  return std::string(begin, end);
}

// A human-readable status for the tool the model just requested. Kept out of the
// loop so the mapping reads as a table.
std::string tool_status(const ToolCall& call) {
  if (call.name == QStringLiteral("read_file")) return "Reading files...";
  if (call.name == QStringLiteral("search_code")) return "Searching code...";
  if (call.name == QStringLiteral("list_directory")) return "Listing files...";
  if (call.name == QStringLiteral("fetch_url")) return "Fetching page...";
  if (call.name == QStringLiteral("web_search")) return "Searching the web...";
  return "Using tools...";
}

// A completed turn whose reply hit the completion cap is not an error: the text
// is kept and the user is told why it stops. The literal reason "length" is
// never surfaced.
void note_length_warning(const ChatCompletionResult& result, GenerationTask& task) {
  if (result.finish_reason == "length") {
    task.set_warning("The reply was cut off because it reached the response limit.");
  }
}

// Incrementally decodes an OpenAI-compatible Server-Sent Events stream.
//
// Parsing happens inside a Qt signal handler (readyRead), and throwing across a
// signal is undefined behaviour, so this class never throws: it records the
// failure in `failed_`/`error_` and reports "finished" so the caller can leave
// its event loop and then act on the recorded state.
class SseStream {
public:
  SseStream(GenerationTask& task, std::stop_token stop_token)
      : task_(task), stop_token_(stop_token) {}

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

  [[nodiscard]] std::vector<ToolCall> tool_calls() const {
    std::vector<ToolCall> calls;
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
      lnb::log::debug(
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
  std::map<int, ToolCall> tool_calls_;

  bool finished_ = false;
  bool failed_ = false;
  std::string error_;
};

// Assembles the system prompt (design-1.md §4): the assistant and session
// instructions first, each under its own header, then the shared assistant
// memory and the session memory. Shared by build_session_payload() and
// context_usage() so the reported cost matches the request actually sent.
std::string build_system_prompt(const Assistant& assistant, const Session& session) {
  std::string prompt;

  const auto append_block = [&prompt](const char* header, const std::string& body) {
    if (body.empty()) {
      return;
    }

    if (!prompt.empty()) {
      prompt += "\n\n";
    }

    prompt += header;
    prompt += '\n';
    prompt += body;
  };

  append_block("[ASSISTANT INSTRUCTIONS]", assistant.custom_instructions_);
  append_block("[SESSION INSTRUCTIONS]", session.custom_instructions_);
  append_block("[ASSISTANT MEMORY]", assistant.memory_);
  append_block("[SESSION MEMORY]", session.memory_);

  if (prompt.empty()) {
    prompt = "You are a helpful assistant.";
  }

  return prompt;
}

// The session-only part of the system prompt. The session summariser has no
// access to the assistant, so it plans its window against the session's own
// instructions and memory; this is a close estimate of the real prompt.
std::string session_system_prompt(const Session& session, const std::string& memory) {
  std::string prompt = session.custom_instructions_;

  if (!memory.empty()) {
    if (!prompt.empty()) {
      prompt += "\n\n";
    }

    prompt += "[CONVERSATION MEMORY]\n";
    prompt += memory;
  }

  return prompt;
}

} // namespace

QJsonObject build_session_payload(
    const Assistant& assistant, const Session& session,
    const std::vector<Message>& messages
) {
  const std::string system_prompt = build_system_prompt(assistant, session);
  const std::size_t system_tokens = context::prompt_tokens(system_prompt);

  // The verbatim window is the newest suffix of the session that fits the
  // session's single knob; the boundary message is kept whole.
  const context::Window window = context::plan_window(
      messages, system_tokens, session.summary_trigger_tokens_
  );

  QJsonArray payload_messages;
  payload_messages.append(
      QJsonObject{{"role", "system"}, {"content", QString::fromStdString(system_prompt)}}
  );

  for (std::size_t i = window.start; i < messages.size(); ++i) {
    const Message& message = messages[i];
    payload_messages.append(QJsonObject{
        {"role", message.role_ == Role::User ? "user" : "assistant"},
        {"content", QString::fromStdString(message.content_)},
    });
  }

  return QJsonObject{
      {"model", QString::fromStdString(assistant.model_)},
      // Network tools are always offered; the filesystem tools join them only
      // when the session has a shared directory.
      {"tools", tool_definitions(session.shared_directory_.has_value())},
      {"messages", payload_messages},
      {"max_tokens", static_cast<int>(session.response_token_budget_)},
      {"stream", true},
  };
}

ContextUsage context_usage(
    const Assistant& assistant, const Session& session,
    const std::vector<Message>& messages
) {
  ContextUsage usage;

  const std::string system_prompt = build_system_prompt(assistant, session);
  const std::size_t system_tokens = context::prompt_tokens(system_prompt);

  // Mirrors build_session_payload(): the same prompt cost and knob pick the
  // same verbatim window, so the reported usage matches the sent request.
  const context::Window window = context::plan_window(
      messages, system_tokens, session.summary_trigger_tokens_
  );

  usage.used_tokens = system_tokens + window.tokens;
  usage.limit_tokens = session.summary_trigger_tokens_;

  return usage;
}

ChatCompletionResult send_message_once(
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
    std::string message = "HTTP request failed: " + network_error_string.toStdString();

    if (http_status > 0) {
      message += " (HTTP " + std::to_string(http_status) + ")";
    }

    if (!captured.isEmpty()) {
      message += ": " + captured.toStdString();
    }

    throw std::runtime_error(message);
  }

  if (!stream.finished()) {
    // The stream closed without a `[DONE]` sentinel. The usual cause is a
    // non-streaming response — a plain JSON completion or an error body — which
    // carries no `data:` lines to parse; showing the start of the body makes
    // that immediately obvious.
    std::string message = "SSE stream ended before [DONE]";

    if (!captured.isEmpty()) {
      message += ": ";
      message += captured.left(400).toStdString();
    }

    throw std::runtime_error(message);
  }

  ChatCompletionResult result;
  result.text = stream.text();
  result.finish_reason = stream.finish_reason();
  result.tool_calls = stream.tool_calls();
  result.assistant_message = stream.assistant_message();

  lnb::log::info(
      "send_message_once tool_calls={}, finish_reason={}", result.tool_calls.size(),
      result.finish_reason
  );

  return result;
}

ChatCompletionResult generate_reply(
    QJsonObject payload, GenerationTask& task, std::stop_token stop_token,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy,
    const std::optional<std::string>& shared_directory, const Session& session
) {
  const std::size_t max_rounds = session.max_tool_rounds_;
  const std::size_t max_calls = session.max_tool_calls_;
  std::size_t total_calls = 0;

  ChatCompletionResult result;

  for (std::size_t round = 0; round < max_rounds; ++round) {
    if (stop_token.stop_requested()) {
      throw std::runtime_error("Generation cancelled");
    }

    lnb::log::info("generate_reply round={}", round);

    result = send_message_once(payload, task, stop_token, api_url, api_key, proxy);

    if (result.tool_calls.empty()) {
      note_length_warning(result, task);
      return result;
    }

    // Every round streams its narration onto the same task, so the persisted
    // reply is the concatenation of all rounds. Without a separator the end of
    // one round runs straight into the start of the next ("...done.Here's...").
    // Start a fresh paragraph once the round has finished a sentence; otherwise
    // fall back to a soft break, which renders as a single space, so a round
    // that stopped mid-sentence is never split apart.
    const std::string round_text = trimmed(result.text);

    if (!round_text.empty()) {
      const char last = round_text.back();
      const bool ends_sentence = last == '.' || last == '!' || last == '?' ||
          last == ':' || last == ';';
      task.append_content(ends_sentence ? "\n\n" : "\n");
    }

    QJsonArray messages = payload.value("messages").toArray();

    // The assistant message carrying the tool calls must be echoed back
    // verbatim before the results so the model can match them by id.
    messages.append(result.assistant_message);

    if (total_calls + result.tool_calls.size() > max_calls) {
      throw std::runtime_error("Tool-call budget exhausted");
    }
    total_calls += result.tool_calls.size();

    for (const ToolCall& call: result.tool_calls) {
      task.set_status(tool_status(call));

      // Filesystem tools need a shared directory; network tools do not, so the
      // gate lives inside execute_local_tool instead of around the call.
      //
      // Log the call and its result at debug level so a failing search can be
      // diagnosed: the exact arguments the model sent (and the resolved size of
      // what came back) are otherwise invisible, since tool calls and results
      // are not persisted.
      lnb::log::debug(
          "generate_reply: tool call: name={} id={} args={}", call.name.toStdString(),
          call.id.toStdString(), call.arguments.left(400).toStdString()
      );

      QString output = execute_local_tool(call, shared_directory, proxy);

      // Bound each tool result so the in-turn loop cannot grow the context
      // without limit across rounds.
      output = QString::fromStdString(
          context::clamp_tokens(output.toStdString(), session.tool_output_max_tokens_)
      );

      lnb::log::debug(
          "generate_reply: tool result: name={} -> {} chars", call.name.toStdString(),
          output.size()
      );

      messages.append(QJsonObject{
          {"role", "tool"},
          {"tool_call_id", call.id},
          {"content", output},
      });
    }

    // The loop is bounded by max_tool_rounds_ / max_tool_calls_, and every
    // result by tool_output_max_tokens_, so no separate context trim is needed.
    payload.insert("messages", messages);
  }

  lnb::log::info("generate_reply: reached the tool-call round cap ({})", max_rounds);

  note_length_warning(result, task);
  return result;
}

namespace {

// Folds the session's overflow — everything older than the verbatim window —
// into `session.memory_` and persists the result. `force` ignores the automatic
// trigger (a manual "summarise now"). Returns the resulting memory, which is
// the unchanged input when there was nothing to fold.
std::string fold_session_memory(
    store::database& db, const Session& session, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token, bool force
) {
  const std::vector<Message> messages = db.messages(session.id_);

  // Nothing to fold until there is an actual exchange: the newest message is
  // never a candidate, so a single message has no overflow.
  if (messages.size() < 2) {
    return session.memory_;
  }

  std::string memory = session.memory_;

  // The memory is part of the system prompt, so it is counted when planning the
  // verbatim window; everything before the window floor is the overflow.
  const std::size_t system_tokens =
      context::prompt_tokens(session_system_prompt(session, memory));
  const context::Window window = context::plan_window(
      messages, system_tokens, session.summary_trigger_tokens_
  );

  if (!force &&
      !context::should_summarize(
          messages, system_tokens, session.memory_watermark_,
          session.summary_trigger_tokens_
      )) {
    return memory;
  }

  // The completion cap reserves room for the summariser's hidden reasoning.
  const std::size_t max_tokens =
      session.summary_token_budget_ + session.summary_reasoning_headroom_tokens_;

  std::uint32_t watermark = 0;
  std::size_t first = 0;

  // Fold the overflow oldest-first in batches bounded by the trigger, advancing
  // `first` until the whole prefix is consumed. Batching keeps a large overflow
  // from overflowing one summariser call, and the loop condition guarantees
  // nothing is silently dropped.
  while (first < window.start) {
    if (stop_token.stop_requested()) {
      throw std::runtime_error("Generation cancelled");
    }

    const std::vector<const Message*> batch = context::summary_candidates(
        messages, first, window.start, session.summary_trigger_tokens_
    );

    if (batch.empty()) {
      break; // defensive: no progress is possible
    }

    task.set_status("Summarizing conversation...");

    const QJsonObject request =
        context::session_summary_request(model, memory, batch, max_tokens);

    // A silent, throwaway task: the summariser's text is internal and must not
    // be streamed into the visible transcript. The outer stop token is forwarded
    // so cancelling the turn also cancels summarisation.
    GenerationTask silent_task;
    silent_task.start();

    const ChatCompletionResult result = send_message_once(
        request, silent_task, stop_token, api_url, api_key, proxy
    );

    const std::string text = trimmed(result.text);

    if (text.empty()) {
      // An empty completion almost always means the model spent its whole budget
      // on hidden reasoning and never emitted the memory. A manual request must
      // say so instead of reporting "nothing to fold".
      if (force) {
        const std::string reason =
            result.finish_reason.empty() ? "unknown" : result.finish_reason;

        throw std::runtime_error(
            "The summariser returned no text (finish_reason=" + reason +
            "). Increase summary_reasoning_headroom_tokens for a reasoning model."
        );
      }

      break;
    }

    memory = text;
    watermark = batch.back()->id_;
    first += batch.size();
  }

  db.set_session_memory(session.id_, memory, watermark);
  return memory;
}

// Gathered, newest-first inputs for one assistant summarisation pass.
struct AssistantGather {
  // Messages across sessions interleaved by made_at_, newest first.
  std::vector<const Message*> recent;
  // Session ids whose memory_ contributes a compacted block, newest first.
  std::vector<std::uint32_t> compacted;
  // Newest gathered message id per session (for the watermark update).
  std::map<std::uint32_t, std::uint32_t> gathered_ids;
};

// Builds the `sessions_block` of the design-1.md §6.1 prompt: one `## Session`
// heading per contributing session, with whichever of the two blocks it has.
// Sessions that contribute neither are omitted.
std::string build_sessions_block(
    const std::vector<Session>& sessions,
    const std::map<std::uint32_t, std::vector<const Message*>>& recent_by_session,
    const std::vector<std::uint32_t>& compacted
) {
  std::string block;

  for (const Session& session: sessions) {
    const auto it = recent_by_session.find(session.id_);
    const std::vector<const Message*>* messages =
        it != recent_by_session.end() ? &it->second : nullptr;

    const bool has_memory =
        !session.memory_.empty() &&
        std::find(compacted.begin(), compacted.end(), session.id_) != compacted.end();

    if (!has_memory && (messages == nullptr || messages->empty())) {
      continue;
    }

    block += "## Session: ";
    block += session.title_.empty() ? "(untitled)" : session.title_;
    block += '\n';

    if (has_memory) {
      block += "\n### Compacted Historical Messages:\n";
      block += session.memory_;
      block += '\n';
    }

    if (messages != nullptr && !messages->empty()) {
      block += "\n### Recent Messages:\n";

      for (const Message* message: *messages) {
        block += (message->role_ == Role::User) ? "User: " : "Assistant: ";
        block += message->content_;
        block += '\n';
      }
    }

    block += '\n';
  }

  return block;
}

// Folds every session's contribution into the assistant memory (design-1.md
// §6). The two pools are gathered separately and never mixed. `strict` makes an
// empty summariser completion an error (a manual request); an automatic one
// keeps the previous memory instead.
std::string fold_assistant_memory(
    store::database& db, const Assistant& assistant, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token, bool strict
) {
  const std::vector<Session> sessions = db.sessions_by_recent();

  // Load each session's messages once; the gathered pool points into these.
  std::vector<std::vector<Message>> session_messages;
  session_messages.reserve(sessions.size());

  for (const Session& session: sessions) {
    session_messages.push_back(db.messages(session.id_));
  }

  struct Tagged {
    const Message* message;
    std::uint32_t session_id;
  };

  std::vector<Tagged> all;

  for (std::size_t i = 0; i < sessions.size(); ++i) {
    for (const Message& message: session_messages[i]) {
      all.push_back(Tagged{&message, sessions[i].id_});
    }
  }

  // Interleave the sessions by time, then take the newest suffix that fits the
  // recent-messages cap.
  std::stable_sort(all.begin(), all.end(), [](const Tagged& a, const Tagged& b) {
    return a.message->made_at_ < b.message->made_at_;
  });

  AssistantGather gather;
  std::size_t recent_tokens = 0;

  for (std::size_t i = all.size(); i-- > 0;) {
    const std::size_t cost = context::message_tokens(*all[i].message);

    // Always keep at least the newest message.
    if (!gather.recent.empty() &&
        recent_tokens + cost > assistant.summary_trigger_tokens_) {
      break;
    }

    recent_tokens += cost;
    gather.recent.push_back(all[i].message);

    auto& newest = gather.gathered_ids[all[i].session_id];
    if (all[i].message->id_ > newest) {
      newest = all[i].message->id_;
    }
  }

  // Compacted historical messages: the session memories, newest session first,
  // until the separate session-summary cap.
  std::size_t compacted_tokens = 0;

  for (const Session& session: sessions) {
    if (session.memory_.empty()) {
      continue;
    }

    const std::size_t cost = context::prompt_tokens(session.memory_);

    if (!gather.compacted.empty() &&
        compacted_tokens + cost >
            assistant.summary_token_budget_for_session_summarization_) {
      break;
    }

    compacted_tokens += cost;
    gather.compacted.push_back(session.id_);
  }

  std::map<std::uint32_t, std::vector<const Message*>> recent_by_session;

  for (const Message* message: gather.recent) {
    recent_by_session[message->session_id_].push_back(message);
  }

  const std::string sessions_block =
      build_sessions_block(sessions, recent_by_session, gather.compacted);

  if (sessions_block.empty()) {
    return assistant.memory_;
  }

  task.set_status("Summarizing assistant memory...");

  const QJsonObject request = context::assistant_summary_request(
      model, assistant.memory_, sessions_block,
      assistant.summary_token_budget_ + assistant.summary_reasoning_headroom_tokens_
  );

  GenerationTask silent_task;
  silent_task.start();

  const ChatCompletionResult result = send_message_once(
      request, silent_task, stop_token, api_url, api_key, proxy
  );

  const std::string memory = trimmed(result.text);

  if (memory.empty()) {
    if (strict) {
      const std::string reason =
          result.finish_reason.empty() ? "unknown" : result.finish_reason;

      throw std::runtime_error(
          "The summariser returned no text (finish_reason=" + reason +
          "). Increase summary_reasoning_headroom_tokens for a reasoning model."
      );
    }

    return assistant.memory_;
  }

  // Advance each contributing session's watermark to its newest gathered id, so
  // the automatic trigger counts only genuinely new messages next time.
  for (const auto& [session_id, id]: gather.gathered_ids) {
    db.set_assistant_memory_watermark(session_id, id);
  }

  return memory;
}

// The automatic assistant trigger (design-1.md §6): the tokens of messages
// newer than each session's assistant watermark, summed across all sessions.
bool assistant_trigger_reached(store::database& db, const Assistant& assistant) {
  std::size_t new_tokens = 0;

  for (const Session& session: db.sessions_by_recent()) {
    for (const Message& message: db.messages(session.id_)) {
      if (message.id_ > session.assistant_memory_watermark_) {
        new_tokens += context::message_tokens(message);
      }
    }
  }

  return new_tokens >= assistant.summary_trigger_tokens_;
}

} // namespace

std::string maybe_summarize_session(
    store::database& db, const Session& session, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token
) {
  try {
    return fold_session_memory(
        db, session, model, api_url, api_key, proxy, task, stop_token, false
    );
  } catch (const std::exception& error) {
    // Best effort: an automatic summarisation must never fail the user's turn,
    // so an unchanged memory is returned instead of propagating.
    lnb::log::info(
        "maybe_summarize_session: skipped for session {}: {}", session.id_,
        error.what()
    );
    return session.memory_;
  }
}

std::string summarize_session_now(
    store::database& db, const Session& session, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token
) {
  return fold_session_memory(
      db, session, model, api_url, api_key, proxy, task, stop_token, true
  );
}

std::string maybe_summarize_assistant(
    store::database& db, const Assistant& assistant, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token
) {
  try {
    // Empty when the automatic trigger is not reached, so the caller only
    // overwrites Assistant::memory_ when there is a new memory to persist.
    if (!assistant_trigger_reached(db, assistant)) {
      return {};
    }

    return fold_assistant_memory(
        db, assistant, model, api_url, api_key, proxy, task, stop_token, false
    );
  } catch (const std::exception& error) {
    lnb::log::info("maybe_summarize_assistant: skipped: {}", error.what());
    return {};
  }
}

std::string summarize_assistant_now(
    store::database& db, const Assistant& assistant, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token
) {
  return fold_assistant_memory(
      db, assistant, model, api_url, api_key, proxy, task, stop_token, true
  );
}

} // namespace lnb::net
