#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "local_notebook/core/model.hpp"
// #include "net/protocol.hpp"

// A pure, side-effect-free layer (design-1.md §3). It decides what fits in a
// request and what should be folded into the summarization; it never touches the
// network or the database, which keeps the rules testable and reusable by the
// summarisation passes.
namespace lnb::context {

// Estimated cost of one stored message as it appears in the payload: its text
// plus a small fixed overhead for the role and JSON scaffolding.
std::size_t message_tokens(const Message& message);

// Estimated cost of a string like strings.
std::size_t estimate_tokens(const std::string& text);

// Estimated model-facing cost of one request: the instructions plus each
// outgoing message's text and per-message overhead. Summariser requests carry
// no tools, so tool definitions and calls are not counted.
// std::size_t request_tokens(const net::Request& request);

// The verbatim window chosen for a request: the newest suffix of `messages`
// whose cost fits `window_tokens`, plus the one message that would overflow it.
// Keeping that boundary message matters: it is the oldest message the next
// request sends and the oldest one a session fold reads, so trimming it would
// remove it from both at once and it would never reach a summary. The bound is
// the **conversation alone** — the system prompt is never counted — so the same
// number means the same thing for the request payload, the collapse and the
// title bar. The newest message is always kept, even when it alone exceeds the
// window, so a request never degenerates into "no history".
struct Window {
  std::size_t start = 0;  // index of the first message in the window
  std::size_t tokens = 0; // estimated cost of the window's messages
  // Running cost of the window, oldest first: entry k is the cost of messages
  // [start, start + k]. The message that overflowed the bound is included, so
  // `tokens` and the last entry can exceed `window_tokens`.
  std::vector<std::size_t> accumulated;
};

Window plan_window(const std::vector<Message>& messages, std::size_t window_tokens);

// Automatic-collapse test: the unsummarised conversation (the messages with an
// id above `watermark`) reaches `trigger_tokens`. Only the conversation is
// counted; the system prompt (instructions and summarization) is deliberately
// excluded
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

// The messages a memory pass still has to read for one session: every message
// whose id is above `watermark`, oldest first, bounded to `max_tokens`. At least
// one message is returned when any are pending, so a batch always makes
// progress. The caller must advance the watermark to the last returned id and
// must never advance past an id this did not return, or that message would be
// marked consumed even though no summariser ever saw it.
std::vector<const Message*> memory_candidates(
    const std::vector<Message>& messages, std::uint32_t watermark,
    std::size_t max_tokens
);


// Truncates `text` so it costs at most `max_tokens`, appending a short notice
// when it had to cut. Used to bound each tool result.
std::string clamp_tokens(const std::string& text, std::size_t max_tokens);

} // namespace lnb::context
