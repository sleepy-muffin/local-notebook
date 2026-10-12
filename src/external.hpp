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
// model answers without requesting tools (or a round cap is reached).
ChatCompletionResult generate_reply(
    QJsonObject payload, GenerationTask& task, std::stop_token stop_token,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy,
    const std::optional<std::string>& shared_directory,
    int max_tool_rounds = 8,
    int max_tool_calls = 32
);

} // namespace lnb
