#include "external.hpp"
#include "context_manager.hpp"
#include "proxy.hpp"
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

#include <format>
#include <map>
#include <string_view>

// ==============================================================================================
// LLAMA API (Qt networking / Qt JSON)
// ==============================================================================================

namespace lnb {
namespace {

// Upper bound on how many summarisation passes one turn may trigger. Each pass
// folds a prefix of the not-yet-summarised transcript and advances the
// watermark, so the loop terminates well before this in practice; the cap is a
// guard against a pathological model that never returns usable text.
constexpr int kMaxSummaryRounds = 8;

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
       "configured shared directory. Directories end with /. Call it with no "
       "arguments to list the shared directory itself."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{{"path",
                         QJsonObject{
                             {"type", "string"},
                             {"description", "Directory to list: either a path relative to the shared directory or an absolute path inside it. Omit to list the shared directory."}}}}},
           {"required", QJsonArray{}},
           {"additionalProperties", false}}},
  };

  const QJsonObject search_function{
      {"name", "search_code"},
      {"description",
       "Search text files in the configured shared directory. Returns matching file paths, line numbers, and surrounding context. Matching is case-insensitive unless the query contains an uppercase letter; binary files are skipped."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{
                {"query", QJsonObject{{"type", "string"}, {"description", "Symbol name or text to search for"}}},
                {"path", QJsonObject{{"type", "string"}, {"description", "Directory to search: a path relative to the shared directory or an absolute path inside it. Defaults to the shared directory."}}}
            }},
           {"required", QJsonArray{"query"}},
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
                     {"description", "File to read: a path relative to the shared directory or an absolute path inside it"}}},
                {"start_line", QJsonObject{{"type", "integer"}}},
                {"end_line", QJsonObject{{"type", "integer"}}}}},
           {"required", QJsonArray{"path", "start_line", "end_line"}},
           {"additionalProperties", false}}},
  };

  const QJsonObject fetch_function{
      {"name", "fetch_url"},
      {"description",
       "Fetch an http/https URL and return its visible text. Use it to read a "
       "web page referenced by the user. HTML is converted to plain text."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{{"url",
                         QJsonObject{
                             {"type", "string"},
                             {"description", "Absolute http:// or https:// URL to fetch"}}}}},
           {"required", QJsonArray{"url"}},
           {"additionalProperties", false}}},
  };

  const QJsonObject web_search_function{
      {"name", "web_search"},
      {"description",
       "Search the web and return the top results with their titles, URLs and "
       "snippets. Use it to find sources online; then use fetch_url to read a "
       "result in full."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{{"query",
                         QJsonObject{
                             {"type", "string"},
                             {"description", "Search terms"}}}}},
           {"required", QJsonArray{"query"}},
           {"additionalProperties", false}}},
  };

  return QJsonArray{
      QJsonObject{{"type", "function"}, {"function", list_function}},
      QJsonObject{{"type", "function"}, {"function", search_function}},
      QJsonObject{{"type", "function"}, {"function", read_function}},
      QJsonObject{{"type", "function"}, {"function", fetch_function}},
      QJsonObject{{"type", "function"}, {"function", web_search_function}},
  };
}

// Assembles the system prompt from the session's custom instructions, its
// persisted memory summary and any lorebook entries whose keyword occurs in the
// transcript. Shared by prepare_payload() and context_usage() so the cost the UI
// reports always matches the prompt actually sent.
std::string build_system_prompt(
    const session& session_info, const std::vector<message>& messages,
    const std::vector<lore_row>& lore_entries
) {
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

  return system_prompt;
}

} // namespace

std::optional<QJsonObject> prepare_payload(
    ApplicationState& app_state, uint32_t session_id, uint32_t self_id,
    const std::string& model
) {
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

  // Budgets are session-bound: each session may target a different model, so
  // the context window and response reserve travel with the session.
  const generation_settings& cfg = session_info.generation_;
  const size_t response_budget = cfg.response_token_budget_;

  if (session_info.custom_instructions_.empty()) {
    mmc::log::info(
        "Session {} has no custom instructions; using a generic system prompt",
        session_id
    );
  }

  const std::string system_prompt =
      build_system_prompt(session_info, messages, lore_entries);

  QJsonArray payload_messages;
  payload_messages.append(
      QJsonObject{{"role", "system"}, {"content", QString::fromStdString(system_prompt)}}
  );

  // The summary is already part of `system_prompt` above, so it is counted
  // exactly once: `plan_window` receives the prompt cost and the watermark,
  // then keeps the newest messages that are not yet folded into the summary.
  const std::size_t system_tokens = context::prompt_tokens(system_prompt);
  const context::Window window = context::plan_window(
      messages, system_tokens, session_info.memory_watermark_, context::request_budget(cfg)
  );

  for (std::size_t i = window.start; i < messages.size(); ++i) {
    const message& m = messages[i];
    payload_messages.append(QJsonObject{
        {"role", m.sender_id == self_id ? "user" : "assistant"},
        {"content", QString::fromStdString(m.content)},
    });
  }

  return QJsonObject{
      {"model", QString::fromStdString(model)},
      {"tools", make_repository_tools()},
      {"messages", payload_messages},
      {"max_tokens", static_cast<int>(response_budget)},
      {"stream", true},
  };
}

ContextUsage context_usage(
    ApplicationState& app_state, uint32_t session_id, uint32_t self_id
) {
  ContextUsage usage;

  auto session_info_opt = app_state.uni_db_->get_session_by_id(session_id);

  if (!session_info_opt.has_value()) {
    return usage;
  }

  const session& session_info = *session_info_opt;
  const auto messages = app_state.uni_db_->get_messages_for_session(session_id, self_id);
  const auto lore_entries = app_state.uni_db_->get_lorebook_for_session(session_id);

  const std::string system_prompt =
      build_system_prompt(session_info, messages, lore_entries);
  const std::size_t system_tokens = context::prompt_tokens(system_prompt);

  // Mirrors prepare_payload(): the same prompt cost, watermark and budget pick
  // the same verbatim window, so the reported usage matches the sent request.
  const context::Window window = context::plan_window(
      messages, system_tokens, session_info.memory_watermark_,
      context::request_budget(session_info.generation_)
  );

  usage.used_tokens = system_tokens + window.tokens;
  usage.limit_tokens = session_info.generation_.model_context_tokens_;

  return usage;
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

namespace {

// Core folding pass shared by the automatic and manual entry points. `force`
// folds regardless of the soft trigger and keeps only the newest message live,
// which is what a manual "Summarize now" needs. A manual request also surfaces
// every failure; an automatic one stays best-effort.
std::string fold_memory(
    ApplicationState& app_state, uint32_t session_id, uint32_t self_id,
    GenerationTask& task, std::stop_token stop_token, const std::string& model,
    const std::string api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, bool force
) {
  std::string summary;

  try {
    const auto session_opt = app_state.uni_db_->get_session_by_id(session_id);

    if (!session_opt.has_value()) {
      return {};
    }

    // Budgets are session-bound: read them from the row being summarised.
    const generation_settings& cfg = session_opt->generation_;

    const auto messages = app_state.uni_db_->get_messages_for_session(session_id, self_id);

    // Nothing to summarise until there is an actual exchange.
    if (messages.size() < 2) {
      return session_opt->memory_summary_;
    }

    summary = session_opt->memory_summary_;
    uint32_t watermark = session_opt->memory_watermark_;

    const std::size_t budget = context::request_budget(cfg);

    for (int round = 0; round < kMaxSummaryRounds; ++round) {
      if (stop_token.stop_requested()) {
        throw std::runtime_error("Generation cancelled");
      }

      // The summary is part of the system prompt, so it has to be counted when
      // planning the verbatim window. Use the current summary so the plan and
      // the request that follows agree.
      std::string system_prompt = session_opt->custom_instructions_;

      if (!summary.empty()) {
        if (!system_prompt.empty()) {
          system_prompt += "\n\n";
        }

        system_prompt += "[CONVERSATION MEMORY]\n";
        system_prompt += summary;
      }

      if (system_prompt.empty()) {
        system_prompt = "You are a helpful assistant.";
      }

      const std::size_t system_tokens = context::prompt_tokens(system_prompt);

      // The first message not yet covered by the summary. Everything from here
      // on must reach the model verbatim.
      std::size_t first_unsummarised = messages.size();

      for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].id > watermark) {
          first_unsummarised = i;
          break;
        }
      }

      if (first_unsummarised == messages.size()) {
        break; // the whole transcript is already folded
      }

      // A manual request folds unconditionally; the automatic one folds when the
      // transcript crosses the soft trigger, or when the hard request budget
      // would otherwise silently drop unsummarised messages.
      bool over_trigger = false;

      if (!force) {
        const std::size_t hard_start =
            context::plan_window(messages, system_tokens, watermark, budget).start;
        over_trigger =
            context::should_summarize(messages, system_tokens, watermark, cfg);
        const bool would_drop_unsummarised = hard_start > first_unsummarised;

        if (!over_trigger && !would_drop_unsummarised) {
          break;
        }
      }

      // Keep the newest `summary_keep_tokens_` verbatim and fold what precedes
      // it. The newest message is never a candidate, so it always stays live.
      // The keep window must stay inside the real request budget: otherwise
      // messages between the two windows would be dropped without being folded.
      // A manual request keeps nothing extra: only the newest message survives.
      std::size_t keep_budget = force
          ? 0
          : (cfg.summary_keep_tokens_ < budget ? cfg.summary_keep_tokens_ : budget);

      // The trigger wins over a larger keep window: once the transcript crosses
      // the soft trigger, only that much stays verbatim. Otherwise a keep window
      // larger than the whole transcript protects every message and the trigger
      // can never fold anything.
      if (over_trigger && keep_budget > cfg.summary_trigger_tokens_) {
        keep_budget = cfg.summary_trigger_tokens_;
      }
      const context::Window keep = context::plan_window(
          messages, system_tokens, watermark, keep_budget
      );
      const auto candidates = context::summary_candidates(
          messages, keep.start, watermark, context::summarization_budget(cfg)
      );

      if (candidates.empty()) {
        break;
      }

      task.set_status("Summarizing conversation...");

      const QJsonObject request = context::summary_request(
          model, summary, candidates, self_id, context::summary_request_max_tokens(cfg)
      );

      // A silent, throwaway task: the summariser's text is internal and must
      // not be streamed into the visible transcript. The outer stop token is
      // forwarded so cancelling the turn also cancels summarisation.
      GenerationTask silent_task;
      silent_task.start();

      const ChatCompletionResult result = send_message_llama_once(
          request, silent_task, stop_token, api_url, api_key, proxy
      );

      const auto trimmed = mmc::str::trim(result.text);

      if (!trimmed.has_value() || trimmed->empty()) {
        // An empty completion almost always means the model spent its whole
        // budget on hidden reasoning and never emitted the memory. A manual
        // request must say so instead of reporting "nothing to fold".
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

      summary = *trimmed;
      watermark = candidates.back()->id;
      app_state.uni_db_->update_session_memory(session_id, summary, watermark);
    }
  } catch (const std::exception& error) {
    // Cancellation must propagate. A manual request also surfaces every failure
    // so the UI can report it; an automatic one is best-effort and only logged,
    // because it must never fail the user's turn.
    if (stop_token.stop_requested() || force) {
      throw;
    }

    mmc::log::info("maybe_summarize: skipped for session {}: {}", session_id, error.what());
  }

  return summary;
}

} // namespace

void maybe_summarize(
    ApplicationState& app_state, uint32_t session_id, uint32_t self_id,
    GenerationTask& task, std::stop_token stop_token, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy
) {
  fold_memory(
      app_state, session_id, self_id, task, stop_token, model, api_url, api_key,
      proxy, false
  );
}

std::string summarize_session_now(
    ApplicationState& app_state, uint32_t session_id, uint32_t self_id,
    GenerationTask& task, std::stop_token stop_token, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy
) {
  return fold_memory(
      app_state, session_id, self_id, task, stop_token, model, api_url, api_key,
      proxy, true
  );
}

ChatCompletionResult generate_reply(
    QJsonObject payload, GenerationTask& task, std::stop_token stop_token,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy,
    const std::optional<std::string>& shared_directory,
    const generation_settings& cfg
) {
  const int max_rounds = cfg.max_tool_rounds_;
  const int max_calls = cfg.max_tool_calls_;
  const std::size_t context_budget = context::request_budget(cfg);
  int total_calls = 0;

  ChatCompletionResult result;

  for (int round = 0; round < max_rounds; ++round) {
    if (stop_token.stop_requested()) {
      throw std::runtime_error("Generation cancelled");
    }

    mmc::log::info("generate_reply round={}", round);

    result = send_message_llama_once(payload, task, stop_token, api_url, api_key, proxy);

    if (result.tool_calls.empty()) {
      return result;
    }

    // Every round streams its narration onto the same task, so the persisted
    // reply is the concatenation of all rounds. Without a separator the end of
    // one round runs straight into the start of the next ("...done.Here's...").
    // Start a fresh paragraph once the round has finished a sentence; otherwise
    // fall back to a soft break, which renders as a single space, so a round
    // that stopped mid-sentence is never split apart.
    const auto round_text = mmc::str::trim(result.text);

    if (round_text.has_value() && !round_text->empty()) {
      const char last = round_text->back();
      const bool ends_sentence = last == '.' || last == '!' || last == '?' ||
          last == ':' || last == ';';
      task.append_content(ends_sentence ? "\n\n" : "\n");
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
                  : call.name == QStringLiteral("list_directory")
                      ? "Listing files..."
                      : call.name == QStringLiteral("fetch_url")
                          ? "Fetching page..."
                          : call.name == QStringLiteral("web_search")
                              ? "Searching the web..."
                              : "Using tools..."
      );

      // Filesystem tools need a shared directory; network tools do not, so the
      // gate lives inside execute_local_tool instead of around the call.
      //
      // Log the call and its result at info level so a failing search can be
      // diagnosed from stdout: the exact arguments the model sent (and the
      // resolved size of what came back) are otherwise invisible, since tool
      // calls and results are not persisted.
      mmc::log::debug(
          "generate_reply: tool call: name={} id={} args={}", call.name.toStdString(),
          call.id.toStdString(),
          call.arguments.left(400).toStdString()
      );

      QString output = external_tools::execute_local_tool(
          call, shared_directory, proxy
      );

      // Bound each tool result so the in-turn loop cannot grow the context
      // without limit across rounds.
      output = QString::fromStdString(
          context::clamp_tokens(output.toStdString(), cfg.tool_output_max_tokens_)
      );

      mmc::log::debug(
          "generate_reply: tool result: name={} -> {} chars", call.name.toStdString(),
          output.size()
      );

      messages.append(QJsonObject{
          {"role", "tool"},
          {"tool_call_id", call.id},
          {"content", output},
      });
    }

    // Safety net: as rounds accumulate, keep the running payload inside the
    // request budget by dropping the oldest turns (never the system prompt and
    // never orphaning a tool result).
    context::trim_to_budget(messages, context_budget);
    payload.insert("messages", messages);
  }

  mmc::log::info("generate_reply: reached the tool-call round cap ({})", max_rounds);

  return result;
}

} // namespace lnb
