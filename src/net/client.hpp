#pragma once

#include <QJsonObject>

#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "model.hpp"
#include "net/tools.hpp"
#include "support/generation_task.hpp"

namespace lnb {
namespace store {
class database;
} // namespace store
} // namespace lnb

namespace lnb::net {

// The outcome of one streaming request. `assistant_message` is the exact
// assistant message (role/content/optional tool_calls) that has to be echoed
// back to the model when the tool call is answered.
struct ChatCompletionResult {
  std::string text;
  std::vector<ToolCall> tool_calls;
  std::string finish_reason;
  QJsonObject assistant_message;
};

// Assembles the payload for one prompt, in order (design-1.md §4): system
// (assistant + session instructions), assistant memory, session memory, then
// the verbatim window of `messages` that fits `session.summary_trigger_tokens_`.
QJsonObject build_session_payload(
    const Assistant& assistant, const Session& session,
    const std::vector<Message>& messages
);

// How much of the input bound the next request would occupy, in estimated
// tokens. `limit_tokens` is `session.summary_trigger_tokens_`, the single input
// bound (design-1.md §1). Used to show context usage on the title bar.
struct ContextUsage {
  std::size_t used_tokens = 0;
  std::size_t limit_tokens = 0;
};

ContextUsage context_usage(
    const Assistant& assistant, const Session& session,
    const std::vector<Message>& messages
);

// Performs one streaming chat-completions request. Assistant text is streamed
// into `task` as it arrives. Throws std::runtime_error on failure; a stop
// request surfaces as the message "Generation cancelled".
//
// `kind` labels the request in the assistant log ("chat", "session-summary",
// "assistant-summary"); the exact request body is written there verbatim before
// it is posted.
ChatCompletionResult send_message_once(
    const QJsonObject& payload, GenerationTask& task, std::stop_token stop_token,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, std::string_view kind
);

// Runs one full assistant turn: sends the payload, executes any local tool
// calls the model requests, feeds the results back, and repeats until the model
// answers without requesting tools (or a round/call cap is reached). Tool
// rounds, total tool calls and the size of each tool result are bounded by
// `session`, so the in-turn loop can never grow the context without limit.
ChatCompletionResult generate_reply(
    QJsonObject payload, GenerationTask& task, std::stop_token stop_token,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy,
    const std::optional<std::string>& shared_directory, const Session& session
);

// A description of one summarisation request, built immediately before it is
// transmitted so the interface can show the user exactly what will leave the
// machine. `request` is the exact body; `kind` names the pass; `automatic` is
// true when the token threshold triggered the pass rather than the user asking
// for it.
struct SummaryPlan {
  std::string kind;
  QJsonObject request;
  std::string watermark_text;
  bool automatic = false;
  // True when the pass continues in further requests after this one (a large
  // overflow is folded in several bounded batches).
  bool more_requests = false;
};

// Called on the worker thread immediately before a summarisation request is
// transmitted. Return true to send it, false to skip the pass. An empty
// approver always proceeds, so headless callers and tests are unaffected.
using SummaryApprover = std::function<bool(const SummaryPlan&)>;

// Session summarisation (design-1.md §5). `maybe_*` is a no-op (returns the
// unchanged memory) while the automatic trigger is not reached and swallows
// failures (best effort); the `*_now` variant ignores the trigger, folds the
// whole overflow and propagates failures. Both return the resulting session
// memory (which is also persisted to the database). `approver`, when set, is
// consulted before the request is sent and may cancel the pass.
std::string maybe_summarize_session(
    store::database& db, const Session& session, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token, SummaryApprover approver = {}
);

std::string summarize_session_now(
    store::database& db, const Session& session, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token, SummaryApprover approver = {}
);

// Assistant summarisation across all sessions (design-1.md §6). `maybe_*` fires
// only when the automatic trigger is reached; `*_now` folds on demand. Both
// return the new assistant memory (empty when nothing happened); the caller
// assigns it to `Assistant::memory_` and persists it, because the assistant is
// passed by const reference. `approver` behaves as above.
std::string maybe_summarize_assistant(
    store::database& db, const Assistant& assistant, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token, SummaryApprover approver = {}
);

std::string summarize_assistant_now(
    store::database& db, const Assistant& assistant, const std::string& model,
    const std::string& api_url, const std::string& api_key,
    const std::optional<std::string>& proxy, GenerationTask& task,
    std::stop_token stop_token, SummaryApprover approver = {}
);

} // namespace lnb::net
