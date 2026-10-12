#pragma once

#include <QJsonObject>

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "external_tools.hpp"
#include "generation_task.hpp"

namespace lnb {

struct ApplicationState;
struct general;

// The outcome of one streaming request. `assistant_message` is the exact
// assistant message (role/content/optional tool_calls) that has to be echoed
// back to the model when the tool call is answered.
struct ChatCompletionResult {
  std::string text;
  std::vector<external_tools::ToolCall> tool_calls;
  std::string finish_reason;
  QJsonObject assistant_message;
};

// Assembles the request payload for `session_id` from the stored transcript,
// the session's instructions and any matching lorebook entries. Returns
// std::nullopt (instead of throwing) when the session no longer exists.
std::optional<QJsonObject> prepare_payload(
    ApplicationState& app_state, uint32_t session_id, uint32_t self_id,
    const std::string& model
);

// Folds the oldest part of the transcript into the session's persisted memory
// summary using an internal (non-tool) LLM call, when the conversation is close
// to the model's context limit. Safe to call before every request: it is a
// no-op while the transcript still fits. Runs best-effort: a summarisation
// failure is logged and ignored rather than failing the user's turn.
void maybe_summarize(
    ApplicationState& app_state, uint32_t session_id, uint32_t self_id,
    GenerationTask& task, std::stop_token stop_token, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy
);

// Folds the whole transcript into the session's persisted memory on demand,
// ignoring both the soft trigger and the verbatim keep window (only the newest
// message stays live). Unlike maybe_summarize it propagates every failure so a
// caller can report it, and returns the resulting memory (unchanged when there
// was nothing to fold).
std::string summarize_session_now(
    ApplicationState& app_state, uint32_t session_id, uint32_t self_id,
    GenerationTask& task, std::stop_token stop_token, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy
);

// Performs one streaming chat-completions request. Assistant text is streamed
// into `task` as it arrives. Throws std::runtime_error on failure; a stop
// request surfaces as the message "Generation cancelled".
ChatCompletionResult send_message_llama_once(
    const QJsonObject& payload, GenerationTask& task, std::stop_token stop_token,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy
);

// Runs one full assistant turn: sends the payload, executes any local tool
// calls the model requests, feeds the results back, and repeats until the
// model answers without requesting tools (or a round cap is reached). Tool
// rounds, total tool calls and the size of each tool result are bounded by
// `cfg`, so the in-turn loop can never grow the context without limit.
ChatCompletionResult generate_reply(
    QJsonObject payload, GenerationTask& task, std::stop_token stop_token,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy,
    const std::optional<std::string>& shared_directory,
    const general& cfg
);

} // namespace lnb
