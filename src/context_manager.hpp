#pragma once

#include "db.hpp"
#include "ui.hpp"

#include <QJsonArray>
#include <QJsonObject>

#include <cstddef>
#include <string>
#include <vector>

// Context management is a pure, side-effect-free layer: it decides what fits in
// a request and what should be folded into the session memory. Keeping it out
// of the networking code (and out of run_agent_loop / generate_reply) makes the
// budgeting rules testable and lets the summarisation pass reuse them.
namespace lnb::context {

// Estimated cost of one stored message as it appears in the payload: its text
// plus a small fixed overhead for the role and JSON scaffolding.
std::size_t message_tokens(const message& m);

// Estimated cost of a system prompt.
std::size_t prompt_tokens(const std::string& text);

// Token budget left for the conversation history: the model context minus the
// reserved response budget and the safety margin. Never below 256.
std::size_t request_budget(const general& cfg);

// Token budget for the transcript sent to the summariser: the model context
// minus the summary output budget and the safety margin. Never below 256.
std::size_t summarization_budget(const general& cfg);

// The verbatim window chosen for a request.
struct Window {
  std::size_t start = 0; // index of the first message to send
  std::size_t tokens = 0; // estimated cost of the window's messages
};

// Plans the newest suffix of `messages` that fits `budget` and lies above
// `watermark`. The newest message is always kept, even when it alone exceeds
// the budget, so a request never degenerates into "no history".
Window plan_window(
    const std::vector<message>& messages, std::size_t system_tokens,
    uint32_t watermark, std::size_t budget
);

// True when the request is large enough to be worth folding. Only messages
// that actually reach the model count: the system prompt plus the messages
// above `watermark`. Messages already folded into the summary are excluded, so
// the trigger reflects the real request size instead of the whole history.
bool should_summarize(
    const std::vector<message>& messages, std::size_t system_tokens,
    uint32_t watermark, const general& cfg
);

// Older-than-window messages not yet covered by the summary, oldest first,
// capped to `max_tokens`. The newest message is never a candidate.
std::vector<const message*> summary_candidates(
    const std::vector<message>& messages, std::size_t window_start,
    uint32_t watermark, std::size_t max_tokens
);

// Builds a self-contained chat-completions payload for summarising the
// conversation. It carries no tools, so the summariser cannot recurse.
QJsonObject summary_request(
    const std::string& model, const std::string& existing_summary,
    const std::vector<const message*>& candidates, uint32_t self_id,
    std::size_t max_tokens
);

// Truncates `text` so it costs at most `max_tokens`, appending a short notice
// when it had to cut. Used to bound each tool result.
std::string clamp_tokens(const std::string& text, std::size_t max_tokens);

// Drops the oldest non-system messages until `messages` fits `budget`. The
// system prompt is pinned and a tool result is never orphaned from the
// assistant message that requested it.
void trim_to_budget(QJsonArray& messages, std::size_t budget);

} // namespace lnb::context
