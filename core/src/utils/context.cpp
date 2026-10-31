#include "local_notebook/core/context.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <algorithm>
#include <string_view>
#include <vector>

// Fixed per-message overhead for the role field and JSON scaffolding.

// Instruction for the internal assistant memory call (design-1.md §6.1).

// Estimated token cost of `text`: roughly four ASCII characters per token, and
// one token per non-ASCII codepoint. Counting each UTF-8 lead byte once is
// conservative for multibyte scripts. This mirrors the legacy estimator so the
// stored budgets keep their historical meaning.
std::size_t lnb::context::estimate_tokens(const std::string& text) {
  std::size_t ascii = 0;
  std::size_t non_ascii = 0;

  for (unsigned char c: text) {
    if (c < 128) {
      ++ascii;
    } else if ((c & 0xC0) != 0x80) {
      ++non_ascii; // count each UTF-8 lead byte once
    }
  }

  return (ascii + 3) / 4 + non_ascii;
}

std::size_t lnb::context::message_tokens(const Message& message) {
  constexpr std::size_t kMessageOverhead = 16;
  return estimate_tokens(message.content_) + kMessageOverhead;
}



lnb::context::Window lnb::context::plan_window(
    const std::vector<Message>& messages, std::size_t window_tokens
) {
  Window window;
  window.start = messages.size();

  std::size_t used = 0;

  // Walk from the newest message backwards, keeping messages until the window
  // is exhausted. Only the conversation is measured; the system prompt is never
  // part of the bound.
  for (std::size_t i = messages.size(); i-- > 0;) {
    const std::size_t cost = message_tokens(messages[i]);

    // The newest message is always kept, even when it alone exceeds the window,
    // so a request never degenerates into "no history".
    //
    // The message that would overflow is kept as well and ends the walk:
    // it is the oldest message the
    // next request carries, and the session fold starts exactly there. Leaving
    // it out would drop it from the request and from the fold at the same time,
    // so it would never reach a summary or the assistant memory.
    if (used + cost > window_tokens && window.start < messages.size()) {
      window.start = i;
      used += cost;
      window.accumulated.push_back(used);
      break;
    }

    window.start = i;
    used += cost;
    window.accumulated.push_back(used);
  }

  window.tokens = used;
  std::reverse(window.accumulated.begin(), window.accumulated.end());
  return window;
}


std::vector<const lnb::Message*> lnb::context::summary_candidates(
    const std::vector<Message>& messages, std::size_t first, std::size_t last,
    std::size_t max_tokens
) {
  std::vector<const Message*> candidates;
  std::size_t used = 0;

  // Oldest first so the summariser reads chronologically. At least one message
  // is always returned, so a batch that starts with an oversized message still
  // makes progress.
  for (std::size_t i = first; i < last && i < messages.size(); ++i) {
    const std::size_t cost = message_tokens(messages[i]);

    if (used + cost > max_tokens && !candidates.empty()) {
      break;
    }

    used += cost;
    candidates.push_back(&messages[i]);
  }

  return candidates;
}

std::vector<const lnb::Message*> lnb::context::memory_candidates(
    const std::vector<Message>& messages, std::uint32_t watermark,
    std::size_t max_tokens
) {
  std::vector<const Message*> candidates;
  std::size_t used = 0;

  // Oldest pending first, so a capped batch always consumes the oldest gap and
  // the watermark it advances to can never skip a message. At least one message
  // is returned, so an oversized message still makes progress.
  for (const Message& message: messages) {
    if (message.id_ <= watermark) {
      continue;
    }

    const std::size_t cost = message_tokens(message);

    if (used + cost > max_tokens && !candidates.empty()) {
      break;
    }

    used += cost;
    candidates.push_back(&message);
  }

  return candidates;
}

std::string  lnb::context::clamp_tokens(const std::string& text, std::size_t max_tokens) {
  if (max_tokens == 0 || estimate_tokens(text) <= max_tokens) {
    return text;
  }

  // Roughly four ASCII characters per token; the loop below then shrinks the
  // result until the estimate (which is conservative for multibyte text) fits.
  std::string result = text.substr(0, max_tokens * 4);

  while (!result.empty() && estimate_tokens(result) > max_tokens) {
    result.resize(result.size() * 9 / 10);
  }

  // Do not leave a half-written UTF-8 sequence at the cut.
  while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xC0) == 0x80) {
    result.pop_back();
  }

  result += "\n\n[tool output truncated to fit the context budget]";
  return result;
}
