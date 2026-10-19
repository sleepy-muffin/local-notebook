#include "shark/log.hpp"

#include "net/client.hpp"

#include "context/context.hpp"
#include "net/http.hpp"
#include "store/database.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

const boost::json::object* object_at(
    const boost::json::object& object, std::string_view key
) {
  const boost::json::value* value = object.if_contains(key);
  return value != nullptr && value->is_object() ? &value->as_object() : nullptr;
}

const boost::json::array* array_at(
    const boost::json::object& object, std::string_view key
) {
  const boost::json::value* value = object.if_contains(key);
  return value != nullptr && value->is_array() ? &value->as_array() : nullptr;
}

std::string string_at(
    const boost::json::object& object, std::string_view key
) {
  const boost::json::value* value = object.if_contains(key);

  return value != nullptr && value->is_string()
      ? std::string(value->as_string())
      : std::string{};
}

int int_at(const boost::json::object& object, std::string_view key, int fallback) {
  const boost::json::value* value = object.if_contains(key);

  if (value == nullptr || !value->is_number()) {
    return fallback;
  }

  if (value->is_int64()) {
    return static_cast<int>(value->as_int64());
  }

  if (value->is_uint64()) {
    return static_cast<int>(value->as_uint64());
  }

  return static_cast<int>(value->as_double());
}

// A human-readable status for the tool the model just requested. Kept out of the
// loop so the mapping reads as a table.
std::string tool_status(const ToolCall& call) {
  if (call.name == "read_file") return "Reading files...";
  if (call.name == "search_code") return "Searching code...";
  if (call.name == "search_code_regex") return "Searching code...";
  if (call.name == "find_files") return "Finding files...";
  if (call.name == "list_directory") return "Listing files...";
  if (call.name == "fetch_url") return "Fetching page...";
  if (call.name == "web_search") return "Searching the web...";
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

// Refuses a reasoning level the model does not offer. The user chose it
// explicitly (or it is the level a session was created with), so a mismatch —
// usually after the model was changed — fails loudly with a message naming the
// fix, rather than being silently downgraded to a level the model accepts.
void require_supported_reasoning(
    const std::string& model, const std::string& effort
) {
  if (!reasoning_effort_supported(model, effort)) {
    throw std::runtime_error(
        "Reasoning level \"" + effort + "\" is not available for model \"" +
        model + "\". Choose another level for this session or assistant."
    );
  }
}

// The reasoning level to actually send: the stored level when it is set, else
// the model's first (cheapest) level. Validated, so an unsupported level throws.
std::string resolve_reasoning_effort(
    const std::string& model, const std::string& stored
) {
  const std::string effort =
      stored.empty() ? default_reasoning_effort(model) : stored;
  require_supported_reasoning(model, effort);
  return effort;
}

// Incrementally decodes an OpenAI-compatible Server-Sent Events stream.
//
// It never throws: a failure is recorded in `failed_`/`error_` and reported as
// "finished" so the caller can stop reading and then act on the recorded state.
class SseStream {
public:
  SseStream(GenerationTask& task, std::stop_token stop_token)
      : task_(task), stop_token_(stop_token) {}

  // Feeds a chunk of raw bytes. Returns false once no more input is needed
  // (either [DONE] arrived or a fatal error was recorded).
  bool feed(std::string_view chunk) {
    buffer_.append(chunk.data(), chunk.size());

    std::size_t newline = std::string::npos;

    while ((newline = buffer_.find('\n')) != std::string::npos) {
      const std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);

      if (!handle_line(line)) {
        return false;
      }
    }

    return true;
  }

  // Processes a trailing record that was not terminated by a newline.
  void flush() {
    if (finished_ || buffer_.empty()) {
      return;
    }

    std::string line = std::move(buffer_);
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

  [[nodiscard]] boost::json::object assistant_message() const {
    boost::json::object message;
    message["role"] = "assistant";
    message["content"] = full_reply_;

    if (!tool_calls_.empty()) {
      boost::json::array calls;

      for (const auto& [index, call]: tool_calls_) {
        boost::json::object function;
        function["name"] = call.name;
        function["arguments"] = call.arguments;

        boost::json::object item;
        item["id"] = call.id;
        item["type"] = "function";
        item["function"] = std::move(function);

        calls.emplace_back(std::move(item));
      }

      message["tool_calls"] = std::move(calls);
    }

    return message;
  }

private:
  // Handles a single SSE line. Returns false when processing should stop.
  bool handle_line(std::string_view raw_line) {
    if (stop_token_.stop_requested()) {
      finished_ = true;
      return false;
    }

    if (raw_line.ends_with('\r')) {
      raw_line.remove_suffix(1);
    }

    // Only `data:` records carry payload; comments and keep-alives are ignored.
    if (!raw_line.starts_with("data:")) {
      return true;
    }

    std::string_view data = raw_line.substr(5);

    if (data.starts_with(' ')) {
      data.remove_prefix(1);
    }

    if (data == "[DONE]") {
      finished_ = true;
      return false;
    }

    boost::system::error_code parse_error;
    const boost::json::value document = boost::json::parse(data, parse_error);

    if (parse_error || !document.is_object()) {
      // A malformed keep-alive must not abort the whole stream.
      shark::log::debug("SSE JSON parse failed: {}", parse_error.message());
      return true;
    }

    const boost::json::object& root = document.as_object();
    const boost::json::array* choices = array_at(root, "choices");

    if (choices == nullptr || choices->empty() || !choices->front().is_object()) {
      return true;
    }

    const boost::json::object& choice = choices->front().as_object();

    if (const boost::json::value* reason = choice.if_contains("finish_reason");
        reason != nullptr && reason->is_string()) {
      finish_reason_ = std::string(reason->as_string());
    }

    const boost::json::object* delta = object_at(choice, "delta");

    if (delta == nullptr) {
      return true;
    }

    if (const boost::json::value* content = delta->if_contains("content");
        content != nullptr && content->is_string()) {
      const std::string_view token = content->as_string();

      if (!task_.append_content(token)) {
        failed_ = true;
        error_ = "Generation is no longer active";
        finished_ = true;
        return false;
      }

      full_reply_.append(token);
    }

    // Hidden reasoning (a reasoning model's "thinking") is kept out of the reply
    // but recorded on the task, so the tasks window can show what the model was
    // working through when a turn failed.
    const boost::json::value* reasoning = delta->if_contains("reasoning_content");

    if (reasoning == nullptr) {
      reasoning = delta->if_contains("reasoning");
    }

    if (reasoning != nullptr && reasoning->is_string()) {
      task_.append_thinking(reasoning->as_string());
    }

    // Tool calls arrive split across chunks; merge them per `index`.
    if (const boost::json::array* calls = array_at(*delta, "tool_calls");
        calls != nullptr) {
      for (const boost::json::value& item: *calls) {
        if (!item.is_object()) {
          continue;
        }

        const boost::json::object& part = item.as_object();
        const int index =
            int_at(part, "index", static_cast<int>(tool_calls_.size()));

        auto& call = tool_calls_[index];
        call.index = index;

        const std::string id = string_at(part, "id");
        if (!id.empty()) {
          call.id = id;
        }

        if (const boost::json::object* function = object_at(part, "function");
            function != nullptr) {
          call.name += string_at(*function, "name");
          call.arguments += string_at(*function, "arguments");
        }
      }
    }

    return true;
  }

  GenerationTask& task_;
  std::stop_token stop_token_;

  std::string buffer_;
  std::string full_reply_;
  std::string finish_reason_;
  std::map<int, ToolCall> tool_calls_;

  bool finished_ = false;
  bool failed_ = false;
  std::string error_;
};

// Incrementally decodes the Responses API's Server-Sent Events stream. The
// vocabulary is the `response.*` event family and the terminal event is
// `response.completed` (with `response.failed` / `response.incomplete` as the
// other endings); there is no `[DONE]` sentinel. Like SseStream it never throws:
// a failure is recorded and reported as "finished".
class ResponsesStream {
public:
  ResponsesStream(GenerationTask& task, std::stop_token stop_token)
      : task_(task), stop_token_(stop_token) {}

  bool feed(std::string_view chunk) {
    buffer_.append(chunk.data(), chunk.size());

    std::size_t newline = std::string::npos;

    while ((newline = buffer_.find('\n')) != std::string::npos) {
      const std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);

      if (!handle_line(line)) {
        return false;
      }
    }

    return true;
  }

  void flush() {
    if (finished_ || buffer_.empty()) {
      return;
    }

    std::string line = std::move(buffer_);
    buffer_.clear();
    handle_line(line);
  }

  [[nodiscard]] bool finished() const { return finished_; }
  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] const std::string& text() const { return full_reply_; }
  [[nodiscard]] const std::string& finish_reason() const { return finish_reason_; }

  // The tool calls the model requested, in output order. `id` is the Responses
  // `call_id`, which is exactly what a function_call_output must quote back.
  [[nodiscard]] std::vector<ToolCall> tool_calls() const {
    std::vector<ToolCall> calls;

    for (const auto& [index, item]: items_) {
      if (string_at(item, "type") != "function_call") {
        continue;
      }

      ToolCall call;
      call.index = index;
      call.id = string_at(item, "call_id");
      if (call.id.empty()) {
        call.id = string_at(item, "id");
      }
      call.name = string_at(item, "name");
      call.arguments = string_at(item, "arguments");
      calls.push_back(std::move(call));
    }

    return calls;
  }

  // Every output item the turn produced (reasoning, function_call, message), in
  // order, so the next round can replay them verbatim — reasoning items must
  // travel back with the tool outputs.
  [[nodiscard]] std::vector<boost::json::object> output_items() const {
    std::vector<boost::json::object> items;

    for (const auto& [index, item]: items_) {
      items.push_back(item);
    }

    return items;
  }

private:
  bool handle_line(std::string_view raw_line) {
    if (stop_token_.stop_requested()) {
      finished_ = true;
      return false;
    }

    if (raw_line.ends_with('\r')) {
      raw_line.remove_suffix(1);
    }

    // Only `data:` records carry payload; comments and keep-alives are ignored.
    if (!raw_line.starts_with("data:")) {
      return true;
    }

    std::string_view data = raw_line.substr(5);

    if (data.starts_with(' ')) {
      data.remove_prefix(1);
    }

    // A gateway may still append the OpenAI sentinel; treat it as an ending.
    if (data == "[DONE]") {
      finish_if_open("stop");
      return false;
    }

    boost::system::error_code parse_error;
    const boost::json::value document = boost::json::parse(data, parse_error);

    if (parse_error || !document.is_object()) {
      shark::log::debug(
          "Responses SSE JSON parse failed: {}", parse_error.message()
      );
      return true;
    }

    return handle_event(document.as_object());
  }

  bool handle_event(const boost::json::object& root) {
    const std::string type = string_at(root, "type");

    if (type == "response.output_text.delta") {
      return append_text(string_at(root, "delta"));
    }

    // Hidden reasoning is only exposed as a summary, and only when one was
    // requested. When present it is recorded on the task, never the reply.
    if (type == "response.reasoning_summary_text.delta" ||
        type == "response.reasoning_text.delta") {
      task_.append_thinking(string_at(root, "delta"));
      return true;
    }

    if (type == "response.output_item.done") {
      // The finished item is authoritative: it carries the complete argument
      // string (and any encrypted reasoning content) the deltas spelled out.
      const int index = int_at(root, "output_index", static_cast<int>(items_.size()));

      if (const boost::json::object* item = object_at(root, "item");
          item != nullptr && !item->empty()) {
        items_[index] = *item;
      }

      return true;
    }

    if (type == "response.completed") {
      const boost::json::object* response = object_at(root, "response");

      if (response != nullptr) {
        adopt_output(*response);
        finish_if_open(
            string_at(*response, "status") == "incomplete" ? "length" : "stop"
        );
      } else {
        finish_if_open("stop");
      }

      return false;
    }

    if (type == "response.incomplete") {
      if (const boost::json::object* response = object_at(root, "response");
          response != nullptr) {
        adopt_output(*response);
      }

      finish_if_open("length");
      return false;
    }

    if (type == "response.failed") {
      error_ = "The response failed";

      if (const boost::json::object* response = object_at(root, "response");
          response != nullptr) {
        if (const boost::json::object* error = object_at(*response, "error");
            error != nullptr) {
          const std::string message = string_at(*error, "message");
          if (!message.empty()) {
            error_ = message;
          }
        }
      }

      failed_ = true;
      finished_ = true;
      return false;
    }

    return true;
  }

  // The completed event carries the whole response; its `output` array, when
  // present, is the authoritative item list and supersedes what the item events
  // gathered.
  void adopt_output(const boost::json::object& response) {
    const boost::json::array* output = array_at(response, "output");

    if (output == nullptr || output->empty()) {
      return;
    }

    std::map<int, boost::json::object> authoritative;

    for (std::size_t i = 0; i < output->size(); ++i) {
      if ((*output)[i].is_object()) {
        authoritative[static_cast<int>(i)] = (*output)[i].as_object();
      }
    }

    items_ = std::move(authoritative);

    // Fall back to the output's own message text when no text deltas arrived.
    if (full_reply_.empty()) {
      for (const auto& [index, item]: items_) {
        if (string_at(item, "type") == "message") {
          append_text(message_text(item));
        }
      }
    }
  }

  static std::string message_text(const boost::json::object& item) {
    std::string text;

    if (const boost::json::array* content = array_at(item, "content");
        content != nullptr) {
      for (const boost::json::value& part: *content) {
        if (!part.is_object()) {
          continue;
        }

        const boost::json::object& entry = part.as_object();

        if (string_at(entry, "type") == "output_text") {
          text += string_at(entry, "text");
        }
      }
    }

    return text;
  }

  bool append_text(std::string_view delta) {
    if (delta.empty()) {
      return true;
    }

    if (!task_.append_content(delta)) {
      failed_ = true;
      error_ = "Generation is no longer active";
      finished_ = true;
      return false;
    }

    full_reply_.append(delta);
    return true;
  }

  void finish_if_open(std::string_view reason) {
    if (finish_reason_.empty()) {
      finish_reason_ = std::string(reason);
    }

    finished_ = true;
  }

  GenerationTask& task_;
  std::stop_token stop_token_;

  std::string buffer_;
  std::string full_reply_;
  std::string finish_reason_;
  std::map<int, boost::json::object> items_;

  bool finished_ = false;
  bool failed_ = false;
  std::string error_;
};

// Assembles the system prompt (design-1.md §4): the assistant and session
// instructions first, each under its own header, then the shared assistant
// memory and the session memory. Used by build_session_request(); the memory it
// carries is deliberately never part of any conversation-token count.
std::string build_system_prompt(
    const Assistant& assistant, const Session& session
) {
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

} // namespace

Request build_session_request(
    const Assistant& assistant, const Session& session,
    const std::vector<Message>& messages
) {
  const std::string system_prompt = build_system_prompt(assistant, session);

  // The request carries at most the session's input bound: the newest messages
  // that fit `memory_trigger_tokens_`, the boundary message kept whole. The
  // bound is the conversation alone, so the system prompt never eats into it.
  const context::Window window =
      context::plan_window(messages, session.memory_trigger_tokens_);

  Request request;
  request.model = assistant.model_;
  request.instructions = system_prompt;
  request.filesystem_tools = session.shared_directory_.has_value();
  request.include_tools = true;
  request.max_output_tokens = static_cast<int>(session.response_token_budget_);
  // The level is resolved here so an unsupported one fails the turn with a
  // message naming the fix, rather than being corrected behind the user's back.
  request.reasoning_effort =
      resolve_reasoning_effort(assistant.model_, session.reasoning_effort_);

  for (std::size_t i = window.start; i < messages.size(); ++i) {
    const Message& message = messages[i];
    OutgoingMessage outgoing;
    outgoing.role = message.role_ == Role::User ? OutgoingMessage::Role::User
                                                : OutgoingMessage::Role::Assistant;
    outgoing.content = message.content_;
    request.messages.push_back(std::move(outgoing));
  }

  return request;
}

ContextUsage context_usage(
    const Session& session, const std::vector<Message>& messages
) {
  ContextUsage usage;

  // Report the unsummarised conversation alone — never the system prompt — so
  // the readout is exactly the quantity the automatic collapse compares against
  // the trigger. Trimming to the trigger made the readout shrink whenever the
  // trigger was lowered, which contradicts the collapse rule.
  usage.used_tokens =
      context::transcript_tokens_after(messages, session.memory_watermark_);
  usage.limit_tokens = session.memory_trigger_tokens_;

  return usage;
}

ChatCompletionResult send_message_once(
    const boost::json::object& payload, const Remote& remote, GenerationTask& task,
    std::stop_token stop_token, std::string_view kind
) {
  auto throw_if_stopped = [&stop_token]() {
    if (stop_token.stop_requested()) {
      throw std::runtime_error("Generation cancelled");
    }
  };

  throw_if_stopped();

  // Record the exact request body: these are byte-for-byte the bytes posted, so
  // a surprising reply (or a failed summary) can always be traced back to what
  // the model was actually sent.
  const std::string body = boost::json::serialize(payload);
  shark::log::info("request {}: {}", kind, body);

  // The same body goes into the task's own log, so the tasks window can dump the
  // exact bytes that were posted when a turn is inspected. The endpoint is
  // included (it carries no credentials) so a timeout can be attributed to the
  // exact host that was contacted.
  task.append_log(
      "[request " + std::string(kind) + "] POST " + remote.url + "\n" + body
  );

  std::vector<HttpHeader> headers{
      {"Content-Type", "application/json"},
      {"Accept", "text/event-stream"},
  };

  if (!remote.key.empty()) {
    headers.push_back({"Authorization", "Bearer " + remote.key});
  }

  // Both parsers share the same surface; only the protocol decides which event
  // vocabulary is decoded. The lambdas below keep the surrounding code identical
  // for either.
  std::optional<SseStream> chat_stream;
  std::optional<ResponsesStream> responses_stream;

  if (remote.protocol == ApiProtocol::Responses) {
    responses_stream.emplace(task, stop_token);
  } else {
    chat_stream.emplace(task, stop_token);
  }

  const auto feed = [&](std::string_view chunk) -> bool {
    return chat_stream ? chat_stream->feed(chunk) : responses_stream->feed(chunk);
  };
  const auto flush = [&]() {
    if (chat_stream) {
      chat_stream->flush();
    } else {
      responses_stream->flush();
    }
  };
  const auto stream_finished = [&]() {
    return chat_stream ? chat_stream->finished() : responses_stream->finished();
  };
  const auto stream_failed = [&]() {
    return chat_stream ? chat_stream->failed() : responses_stream->failed();
  };
  const auto stream_error = [&]() -> const std::string& {
    return chat_stream ? chat_stream->error() : responses_stream->error();
  };

  // Keep the first bytes of the response around so an HTTP error body can be
  // reported; on success the captured prefix is simply ignored.
  std::string captured;
  constexpr std::size_t CAPTURE_LIMIT = 2048;

  const ResponseHandler on_response =
      [](int, const std::vector<HttpHeader>&) { return true; };

  const BodyHandler on_body = [&](std::string_view chunk) {
    if (captured.size() < CAPTURE_LIMIT) {
      captured.append(chunk.substr(0, CAPTURE_LIMIT - captured.size()));
    }

    return feed(chunk);
  };

  int status = 0;

  // A reasoning model may stay silent for a while before it emits its first
  // streamed event, so the read window comes from the assistant's connection
  // settings: a chat turn and a summariser pass can have different windows.
  // Connecting, the TLS handshake and the write keep the 60 s transport window,
  // and the stop token is still observed every 50 ms, so cancelling stays
  // responsive regardless of the read window.
  const bool summary =
      kind == "session-summary" || kind == "assistant-summary";

  const int read_timeout_ms = summary ? remote.summary_read_timeout_ms
                                      : remote.turn_read_timeout_ms;

  try {
    status = http_request(
        "POST", remote.url, remote.proxy, headers, body, stop_token, on_response,
        on_body, 60'000, 0, read_timeout_ms
    );
  } catch (const std::exception&) {
    flush();

    if (stop_token.stop_requested()) {
      throw std::runtime_error("Generation cancelled");
    }

    throw;
  }

  flush();

  if (stop_token.stop_requested()) {
    throw std::runtime_error("Generation cancelled");
  }

  if (stream_failed()) {
    throw std::runtime_error(stream_error());
  }

  if (status < 200 || status >= 300) {
    std::string message =
        "HTTP request failed (HTTP " + std::to_string(status) + ")";

    if (!captured.empty()) {
      message += ": " + captured;
    }

    throw std::runtime_error(message);
  }

  if (!stream_finished()) {
    // The stream closed without a `[DONE]` sentinel. The usual cause is a
    // non-streaming response — a plain JSON completion or an error body — which
    // carries no `data:` lines to parse; showing the start of the body makes
    // that immediately obvious.
    std::string message = "SSE stream ended before [DONE]";

    if (!captured.empty()) {
      message += ": ";
      message += captured.substr(0, 400);
    }

    throw std::runtime_error(message);
  }

  ChatCompletionResult result;

  if (chat_stream) {
    result.text = chat_stream->text();
    result.finish_reason = chat_stream->finish_reason();
    result.tool_calls = chat_stream->tool_calls();
    result.assistant_message = chat_stream->assistant_message();
  } else {
    result.text = responses_stream->text();
    result.finish_reason = responses_stream->finish_reason();
    result.tool_calls = responses_stream->tool_calls();
    result.response_items = responses_stream->output_items();
  }

  shark::log::info(
      "send_message_once protocol={} tool_calls={}, finish_reason={}",
      protocol_name(remote.protocol), result.tool_calls.size(), result.finish_reason
  );

  task.append_log(
      "[response " + std::string(kind) + "] finish_reason=" +
      (result.finish_reason.empty() ? std::string("none") : result.finish_reason) +
      " tool_calls=" + std::to_string(result.tool_calls.size())
  );

  return result;
}

ChatCompletionResult generate_reply(
    Request request, const Remote& remote, GenerationTask& task,
    std::stop_token stop_token,
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

    shark::log::info(
        "generate_reply round={} protocol={}", round, protocol_name(remote.protocol)
    );

    task.append_log("generate_reply round=" + std::to_string(round));

    // Publish the round so the running-task readout can show progress.
    task.set_rounds(round);

    // Rebuilt each round from the accumulated request, so the body reflects the
    // tool history: appended messages for Chat, replayed raw items for Responses.
    const boost::json::object payload = build_request(remote.protocol, request);

    result = send_message_once(payload, remote, task, stop_token, "chat");

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

    if (total_calls + result.tool_calls.size() > max_calls) {
      throw std::runtime_error("Tool-call budget exhausted");
    }
    total_calls += result.tool_calls.size();

    // Echo the assistant turn back in the shape the protocol expects, before
    // the results, so the model can match them by id.
    if (remote.protocol == ApiProtocol::Responses) {
      // The raw output items (reasoning included) replay verbatim; the service
      // discards any reasoning it cannot use.
      for (const boost::json::object& item: result.response_items) {
        request.raw_items.push_back(item);
      }
    } else {
      OutgoingMessage assistant;
      assistant.role = OutgoingMessage::Role::Assistant;
      assistant.content = result.text;
      assistant.tool_calls = result.tool_calls;
      request.messages.push_back(std::move(assistant));
    }

    for (const ToolCall& call: result.tool_calls) {
      task.set_status(tool_status(call));

      // Filesystem tools need a shared directory; network tools do not, so the
      // gate lives inside execute_local_tool instead of around the call.
      //
      // Log the call and its result at debug level so a failing search can be
      // diagnosed: the exact arguments the model sent (and the resolved size of
      // what came back) are otherwise invisible, since tool calls and results
      // are not persisted.
      shark::log::debug(
          "generate_reply: tool call: name={} id={} args={}", call.name, call.id,
          call.arguments.substr(0, 400)
      );

      task.append_log(
          "tool call: " + call.name + "(" + call.arguments.substr(0, 2000) + ")"
      );

      std::string output = execute_local_tool(call, shared_directory, remote.proxy);

      // Bound each tool result so the in-turn loop cannot grow the context
      // without limit across rounds.
      output = context::clamp_tokens(output, session.tool_output_max_tokens_);

      shark::log::debug(
          "generate_reply: tool result: name={} -> {} chars", call.name,
          output.size()
      );

      task.append_log(
          "tool result: " + call.name + " -> " + output.substr(0, 8000)
      );

      if (remote.protocol == ApiProtocol::Responses) {
        boost::json::object item;
        item["type"] = "function_call_output";
        item["call_id"] = call.id;
        item["output"] = output;
        request.raw_items.push_back(std::move(item));
      } else {
        OutgoingMessage tool_result;
        tool_result.role = OutgoingMessage::Role::ToolResult;
        tool_result.tool_call_id = call.id;
        tool_result.content = output;
        request.messages.push_back(std::move(tool_result));
      }
    }
  }

  shark::log::info("generate_reply: reached the tool-call round cap ({})", max_rounds);

  note_length_warning(result, task);
  return result;
}

namespace {

// Folds the session's overflow — everything older than the verbatim window —
// into `session.memory_` and persists the result. `force` ignores the automatic
// trigger (a manual "summarise now"). Returns the resulting memory, which is
// the unchanged input when there was nothing to fold.
std::string fold_session_memory(
    store::database& db, const Session& session, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, bool force,
    const SummaryApprover& approver
) {
  // A cancelled turn must not keep the interface waiting while a large gather
  // runs, so the pass bails out as soon as a stop is requested.
  if (stop_token.stop_requested()) {
    throw std::runtime_error("Generation cancelled");
  }

  const std::vector<Message> messages = db.messages(session.id_);

  // Nothing to fold until there is an actual exchange: the newest message is
  // never a candidate, so a single message has no overflow.
  if (messages.size() < 2) {
    return session.memory_;
  }

  std::string memory = session.memory_;

  // The collapse retains the newest conversation messages that fit the keep
  // window; only the conversation is measured, so the memory's own size never
  // shrinks the retained transcript. Everything before the window floor is the
  // overflow to fold.
  const context::Window window =
      context::plan_window(messages, session.summary_keep_tokens_);

  if (!force &&
      context::transcript_tokens_after(messages, session.memory_watermark_) <
          session.memory_trigger_tokens_) {
    return memory;
  }

  // Nothing has overflowed the retained window, so there is nothing to fold and
  // nothing to persist (leaving the stored watermark untouched).
  if (window.start == 0) {
    return memory;
  }

  // The completion cap reserves room for the summariser's hidden reasoning.
  const std::size_t max_tokens =
      session.summary_token_budget_ + session.summary_reasoning_headroom_tokens_;

  // The summariser inherits the session's reasoning level: an unsupported level
  // is refused here with the same message the turn itself would raise, so a
  // manual summarisation cannot quietly pick a different level.
  const std::string effort =
      resolve_reasoning_effort(remote.model, session.reasoning_effort_);

  // Review the pass before anything leaves the machine. The plan describes the
  // first request; a very large overflow continues in further batches, which is
  // noted for the user. Declining returns the memory untouched, because nothing
  // has been sent yet.
  if (approver && window.start > 0) {
    const std::vector<const Message*> first_batch = context::summary_candidates(
        messages, 0, window.start, session.memory_trigger_tokens_
    );

    SummaryPlan plan;
    plan.kind = "session memory";
    plan.request = build_request(
        remote.protocol,
        context::session_summary_request(
            remote.model, memory, first_batch, max_tokens, effort
        )
    );
    plan.automatic = !force;
    plan.more_requests = first_batch.size() < window.start;
    plan.watermark_text =
        "Session memory watermark: " + std::to_string(session.memory_watermark_) +
        " -> " + std::to_string(messages[window.start - 1].id_);

    if (!approver(plan)) {
      return session.memory_;
    }
  }

  // The watermark advances only once the whole overflow has been folded; a pass
  // that stops early leaves the stored memory and its watermark exactly as they
  // were, so a failed (or empty) job never regresses the frontier.
  std::uint32_t watermark = session.memory_watermark_;
  std::size_t first = 0;
  bool complete = true;

  // Fold the overflow oldest-first in batches bounded by the trigger, advancing
  // `first` until the whole prefix is consumed. Batching keeps a large overflow
  // from overflowing one summariser call, and the loop condition guarantees
  // nothing is silently dropped.
  while (first < window.start) {
    if (stop_token.stop_requested()) {
      throw std::runtime_error("Generation cancelled");
    }

    const std::vector<const Message*> batch = context::summary_candidates(
        messages, first, window.start, session.memory_trigger_tokens_
    );

    if (batch.empty()) {
      complete = false;
      break; // defensive: no progress is possible
    }

    task.set_status("Summarizing conversation...");

    const boost::json::object request = build_request(
        remote.protocol,
        context::session_summary_request(
            remote.model, memory, batch, max_tokens, effort
        )
    );

    // A silent, throwaway task: the summariser's text is internal and must not
    // be streamed into the visible transcript. The outer stop token is forwarded
    // so cancelling the turn also cancels summarisation.
    GenerationTask silent_task;
    silent_task.start();

    ChatCompletionResult result;

    try {
      result = send_message_once(
          request, remote, silent_task, stop_token, "session-summary"
      );
    } catch (const std::exception& error) {
      // Keep the request that was already recorded even when the send fails:
      // the exact bytes are the most useful part of a failed pass.
      task.append_log(silent_task.log_text());
      task.append_log(
          "[session-summary] request failed: " + std::string(error.what())
      );
      throw;
    }

    // The summariser's own diagnostics (the exact request and response) are
    // copied onto the visible task, so the tasks window shows what was sent
    // instead of an empty log.
    task.append_log(silent_task.log_text());

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

      complete = false;
      break;
    }

    memory = text;
    watermark = batch.back()->id_;
    first += batch.size();
  }

  // Persist only when every batch succeeded. A partial pass is discarded: the
  // local `memory` is dropped and the caller keeps the stored one, so memory and
  // watermark can never disagree.
  if (!complete) {
    return session.memory_;
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
    store::database& db, const Assistant& assistant, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, bool strict,
    const SummaryApprover& approver
) {
  // Every heavy loop below bails out the moment a stop is requested, so a
  // cancelled turn never pins the interface thread on the join that follows it.
  const auto throw_if_stopped = [&stop_token]() {
    if (stop_token.stop_requested()) {
      throw std::runtime_error("Generation cancelled");
    }
  };

  // Always leave a trace: an assistant pass can legitimately end without a
  // request (nothing to fold, a declined review, an unreached trigger), and an
  // empty log makes those outcomes indistinguishable from a broken task.
  task.append_log("[assistant-summary] starting");

  const std::vector<Session> sessions = db.sessions_by_recent();
  task.append_log(
      "[assistant-summary] sessions=" + std::to_string(sessions.size())
  );

  // Load each session's messages once; the gathered pool points into these.
  std::vector<std::vector<Message>> session_messages;
  session_messages.reserve(sessions.size());

  for (const Session& session: sessions) {
    throw_if_stopped();
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
    throw_if_stopped();

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
    throw_if_stopped();

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

  task.append_log(
      "[assistant-summary] recent messages=" +
      std::to_string(gather.recent.size()) + ", compacted sessions=" +
      std::to_string(gather.compacted.size())
  );

  const std::string sessions_block =
      build_sessions_block(sessions, recent_by_session, gather.compacted);

  if (sessions_block.empty()) {
    task.append_log(
        "[assistant-summary] nothing to fold: no session memory or recent messages"
    );
    return assistant.memory_;
  }

  task.set_status("Summarizing assistant memory...");

  // The summariser inherits the assistant's reasoning level; an unsupported
  // level is refused rather than silently replaced (see resolve_reasoning_effort).
  const std::string effort =
      resolve_reasoning_effort(remote.model, assistant.reasoning_effort_);

  const boost::json::object request = build_request(
      remote.protocol,
      context::assistant_summary_request(
          remote.model, assistant.memory_, sessions_block,
          assistant.summary_token_budget_ + assistant.summary_reasoning_headroom_tokens_,
          effort
      )
  );

  // Review the single request before it leaves the machine. Declining returns
  // the memory untouched, because nothing has been sent yet.
  if (approver) {
    SummaryPlan plan;
    plan.kind = "assistant memory";
    plan.request = request;
    plan.automatic = !strict;
    plan.more_requests = false;

    std::string watermarks;

    for (const Session& session: sessions) {
      const auto it = gather.gathered_ids.find(session.id_);

      if (it == gather.gathered_ids.end()) {
        continue;
      }

      watermarks += "Session ";
      watermarks += std::to_string(session.id_);

      if (!session.title_.empty()) {
        watermarks += " (";
        watermarks += session.title_;
        watermarks += ")";
      }

      watermarks += " assistant watermark: ";
      watermarks += std::to_string(session.assistant_memory_watermark_);
      watermarks += " -> ";
      watermarks += std::to_string(it->second);
      watermarks += '\n';
    }

    plan.watermark_text = watermarks;

    if (!approver(plan)) {
      task.append_log("[assistant-summary] review declined; nothing was sent");
      return assistant.memory_;
    }
  }

  GenerationTask silent_task;
  silent_task.start();

  ChatCompletionResult result;

  try {
    result = send_message_once(
        request, remote, silent_task, stop_token, "assistant-summary"
    );
  } catch (const std::exception& error) {
    // Keep the request that was already recorded even when the send fails: the
    // exact bytes are the most useful part of a failed pass.
    task.append_log(silent_task.log_text());
    task.append_log(
        "[assistant-summary] request failed: " + std::string(error.what())
    );
    throw;
  }

  // The summariser's own diagnostics (the exact request and response) are copied
  // onto the visible task, so the tasks window shows what was sent instead of an
  // empty log.
  task.append_log(silent_task.log_text());

  const std::string memory = trimmed(result.text);

  if (memory.empty()) {
    const std::string reason =
        result.finish_reason.empty() ? "unknown" : result.finish_reason;

    task.append_log(
        "[assistant-summary] summariser returned no text (finish_reason=" +
        reason + ")"
    );

    if (strict) {
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

} // namespace

// The automatic assistant trigger (design-1.md §6): the tokens of messages newer
// than each session's assistant watermark, summed across all sessions. Public so
// the interface can cache it.
bool assistant_trigger_reached(
    store::database& db, const Assistant& assistant, std::stop_token stop_token
) {
  std::size_t new_tokens = 0;

  for (const Session& session: db.sessions_by_recent()) {
    if (stop_token.stop_requested()) {
      return false;
    }

    for (const Message& message: db.messages(session.id_)) {
      if (message.id_ > session.assistant_memory_watermark_) {
        new_tokens += context::message_tokens(message);
      }
    }
  }

  return new_tokens >= assistant.summary_trigger_tokens_;
}

std::string maybe_summarize_session(
    store::database& db, const Session& session, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, SummaryApprover approver
) {
  try {
    return fold_session_memory(db, session, remote, task, stop_token, false, approver);
  } catch (const std::exception& error) {
    // Best effort: an automatic summarisation must never fail the user's turn,
    // so an unchanged memory is returned instead of propagating.
    shark::log::info(
        "maybe_summarize_session: skipped for session {}: {}", session.id_,
        error.what()
    );
    task.append_log(
        "[session-summary] automatic pass skipped: " + std::string(error.what())
    );
    return session.memory_;
  }
}

std::string summarize_session_now(
    store::database& db, const Session& session, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, SummaryApprover approver
) {
  return fold_session_memory(db, session, remote, task, stop_token, true, approver);
}

std::string maybe_summarize_assistant(
    store::database& db, const Assistant& assistant, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, SummaryApprover approver
) {
  try {
    // Empty when the automatic trigger is not reached, so the caller only
    // overwrites Assistant::memory_ when there is a new memory to persist.
    if (!assistant_trigger_reached(db, assistant, stop_token)) {
      task.append_log(
          "[assistant-summary] automatic trigger not reached; nothing was sent"
      );
      return {};
    }

    return fold_assistant_memory(
        db, assistant, remote, task, stop_token, false, approver
    );
  } catch (const std::exception& error) {
    shark::log::info("maybe_summarize_assistant: skipped: {}", error.what());
    task.append_log(
        "[assistant-summary] automatic pass skipped: " + std::string(error.what())
    );
    return {};
  }
}

std::string summarize_assistant_now(
    store::database& db, const Assistant& assistant, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, SummaryApprover approver
) {
  return fold_assistant_memory(
      db, assistant, remote, task, stop_token, true, approver
  );
}

} // namespace lnb::net
