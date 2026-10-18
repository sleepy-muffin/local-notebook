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
#include "net/protocol.hpp"
#include "net/tools.hpp"
#include "support/generation_task.hpp"

namespace lnb {
namespace store {
class database;
} // namespace store
} // namespace lnb

namespace lnb::net {

// Where and how one request goes: the fully derived endpoint URL, the key, the
// optional proxy, the model id and the wire protocol. Grouping them keeps the
// summaries' signatures readable and lets a single value carry the protocol
// choice to every request the turn makes.
struct Remote {
  std::string url;
  std::string key;
  std::optional<std::string> proxy;
  std::string model;
  ApiProtocol protocol = ApiProtocol::Responses;
};

// The outcome of one streaming request. `assistant_message` is the exact
// assistant message (role/content/optional tool_calls) that has to be echoed
// back to the model when the tool call is answered. `response_items` carries the
// same turn in the Responses shape — the raw output items (reasoning items
// included) that must be replayed verbatim for the next round.
struct ChatCompletionResult {
  std::string text;
  std::vector<ToolCall> tool_calls;
  std::string finish_reason;
  QJsonObject assistant_message;
  std::vector<QJsonObject> response_items;
};

// Assembles the protocol-neutral request for one turn (design-1.md §4): system
// (assistant + session instructions), assistant memory, session memory, then the
// verbatim window of `messages` that fits `session.summary_trigger_tokens_`. The
// system prompt becomes `instructions` (Responses) or the leading system message
// (Chat) when the caller serialises it.
Request build_session_request(
    const Assistant& assistant, const Session& session,
    const std::vector<Message>& messages
);

// The conversation-token readout the title bar reports against. `used_tokens` is
// the estimated cost of the unsummarised conversation alone (the messages above
// the session watermark) — never the system prompt — reported without trimming;
// `limit_tokens` is `session.summary_trigger_tokens_`. `used_tokens` is exactly
// the quantity the automatic collapse compares against the trigger.
struct ContextUsage {
  std::size_t used_tokens = 0;
  std::size_t limit_tokens = 0;
};

ContextUsage context_usage(
    const Session& session, const std::vector<Message>& messages
);

// Performs one streaming request using `remote`'s protocol. Assistant text is
// streamed into `task` as it arrives. Throws std::runtime_error on failure; a
// stop request surfaces as the message "Generation cancelled".
//
// `kind` labels the request in the assistant log ("chat", "session-summary",
// "assistant-summary"); the exact request body is written there verbatim before
// it is posted.
ChatCompletionResult send_message_once(
    const QJsonObject& payload, const Remote& remote, GenerationTask& task,
    std::stop_token stop_token, std::string_view kind
);

// Runs one full assistant turn: sends the payload, executes any local tool
// calls the model requests, feeds the results back, and repeats until the model
// answers without requesting tools (or a round/call cap is reached). Tool
// rounds, total tool calls and the size of each tool result are bounded by
// `session`, so the in-turn loop can never grow the context without limit.
ChatCompletionResult generate_reply(
    Request request, const Remote& remote, GenerationTask& task, std::stop_token stop_token,
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
    store::database& db, const Session& session, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, SummaryApprover approver = {}
);

std::string summarize_session_now(
    store::database& db, const Session& session, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, SummaryApprover approver = {}
);

// Assistant summarisation across all sessions (design-1.md §6). `maybe_*` fires
// only when the automatic trigger is reached; `*_now` folds on demand. Both
// return the new assistant memory (empty when nothing happened); the caller
// assigns it to `Assistant::memory_` and persists it, because the assistant is
// passed by const reference. `approver` behaves as above.
std::string maybe_summarize_assistant(
    store::database& db, const Assistant& assistant, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, SummaryApprover approver = {}
);

std::string summarize_assistant_now(
    store::database& db, const Assistant& assistant, const Remote& remote,
    GenerationTask& task, std::stop_token stop_token, SummaryApprover approver = {}
);

// The automatic assistant trigger (design-1.md §6): the tokens of messages newer
// than each session's assistant watermark, summed across all sessions, compared
// against `assistant.summary_trigger_tokens_`. Because it reads every session's
// transcript, callers should cache the answer rather than call it per frame (the
// application keeps such a cache, and the interface reads the composer's
// summarize mode from it).
bool assistant_trigger_reached(
    store::database& db, const Assistant& assistant,
    std::stop_token stop_token = std::stop_token{}
);

} // namespace lnb::net
