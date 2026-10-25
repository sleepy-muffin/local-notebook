#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "local-notebook-core/model.hpp"
#include "net/protocol.hpp"

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

// Estimated model-facing cost of one request: the instructions plus each
// outgoing message's text and per-message overhead. Summariser requests carry
// no tools, so tool definitions and calls are not counted.
std::size_t request_tokens(const net::Request& request);

// The verbatim window chosen for a request: the newest suffix of `messages`
// whose cost fits `window_tokens`. The bound is the **conversation alone** — the
// system prompt is never counted — so the same number means the same thing for
// the request payload, the collapse and the title bar. The newest message is
// always kept, even when it alone exceeds the window, so a request never
// degenerates into "no history".
struct Window {
  std::size_t start = 0;  // index of the first message in the window
  std::size_t tokens = 0; // estimated cost of the window's messages
};

Window plan_window(const std::vector<Message>& messages, std::size_t window_tokens);

// Automatic-collapse test: the unsummarised conversation (the messages with an
// id above `watermark`) reaches `trigger_tokens`. Only the conversation is
// counted; the system prompt (instructions and memory) is deliberately excluded
// (see architecture.md), so the trigger and the title-bar readout are the same
// quantity. The watermark is bookkeeping only (design §7) and never decides what
// is sent.

// The estimated cost of the unsummarised conversation alone: every message whose
// id is above `watermark`, with nothing else included. This is exactly the
// quantity the automatic collapse compares against the trigger and the quantity
// the title bar reports, so the two can never disagree.
inline std::size_t transcript_tokens(
    const std::vector<Message>& messages
) {
  std::size_t total = 0;

  for (const Message& message: messages) {
    total += context::message_tokens(message);
  }

  return total;
}

// The same quantity restricted to the unsummarised conversation: only messages
// whose id is above `watermark` are counted. This is the trigger's measure when
// the caller holds the full transcript rather than a pre-filtered list.
inline std::size_t transcript_tokens_after(
    const std::vector<Message>& messages, std::uint32_t watermark
) {
  std::size_t total = 0;

  for (const Message& message: messages) {
    if (message.id_ > watermark) {
      total += context::message_tokens(message);
    }
  }

  return total;
}

// The overflow to fold: messages in `[first, last)`, oldest first, bounded to
// `max_tokens`. The newest message is never a candidate. Callers advance
// `first` to fold a large overflow in several batches.
std::vector<const Message*> summary_candidates(
    const std::vector<Message>& messages, std::size_t first, std::size_t last,
    std::size_t max_tokens
);

// A self-contained, tool-free request for the internal summariser. `include_tools`
// stays false, so neither protocol advertises any tool.
net::Request session_summary_request(
    const std::string& model, const std::string& existing_memory,
    const std::vector<const Message*>& candidates, std::size_t max_tokens,
    const std::string& reasoning_effort
);

net::Request assistant_summary_request(
    const std::string& model, const std::string& existing_memory,
    const std::string& sessions_block, std::size_t max_tokens,
    const std::string& reasoning_effort
);

// Truncates `text` so it costs at most `max_tokens`, appending a short notice
// when it had to cut. Used to bound each tool result.
std::string clamp_tokens(const std::string& text, std::size_t max_tokens);

} // namespace lnb::context
