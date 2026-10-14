#pragma once

#include <QJsonObject>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "model.hpp"

// A pure, side-effect-free layer (design-1.md §3). It decides what fits in a
// request and what should be folded into memory; it never touches the network
// or the database, which keeps the rules testable and reusable by the
// summarisation passes.
namespace lnb::context {

// Estimated cost of one stored message as it appears in the payload: its text
// plus a small fixed overhead for the role and JSON scaffolding.
std::size_t message_tokens(const Message& message);

// Estimated cost of a system prompt.
std::size_t prompt_tokens(const std::string& text);

// The verbatim window chosen for a request: the newest suffix of `messages`
// whose tokens fit `window_tokens`. The newest message is always kept, even
// when it alone exceeds the window, so a request never degenerates into "no
// history".
struct Window {
  std::size_t start = 0;  // index of the first message in the window
  std::size_t tokens = 0; // estimated cost of the window's messages
};

Window plan_window(
    const std::vector<Message>& messages, std::size_t system_tokens,
    std::size_t window_tokens
);

// Automatic-trigger test: the system prompt plus the messages above
// `watermark` reach `trigger_tokens`. The watermark is bookkeeping only (design
// §7) and never decides what is sent.
bool should_summarize(
    const std::vector<Message>& messages, std::size_t system_tokens,
    std::uint32_t watermark, std::size_t trigger_tokens
);

// The overflow to fold: messages in `[first, last)`, oldest first, bounded to
// `max_tokens`. The newest message is never a candidate. Callers advance
// `first` to fold a large overflow in several batches.
std::vector<const Message*> summary_candidates(
    const std::vector<Message>& messages, std::size_t first, std::size_t last,
    std::size_t max_tokens
);

// A self-contained, tool-free chat payload for the internal summariser. Like the
// chat payload it requests a stream: the network client is SSE-only, so a
// non-streaming body would never carry the `[DONE]` sentinel it waits for.
QJsonObject session_summary_request(
    const std::string& model, const std::string& existing_memory,
    const std::vector<const Message*>& candidates, std::size_t max_tokens
);

QJsonObject assistant_summary_request(
    const std::string& model, const std::string& existing_memory,
    const std::string& sessions_block, std::size_t max_tokens
);

// Truncates `text` so it costs at most `max_tokens`, appending a short notice
// when it had to cut. Used to bound each tool result.
std::string clamp_tokens(const std::string& text, std::size_t max_tokens);

} // namespace lnb::context
