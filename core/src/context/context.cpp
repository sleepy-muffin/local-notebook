#include "context/context.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lnb::context {

namespace {

// Fixed per-message overhead for the role field and JSON scaffolding.
constexpr std::size_t kMessageOverhead = 16;

// Instruction for the internal session summarisation call. It is deliberately
// separate from the user's custom instructions so the two never overwrite each
// other.
constexpr std::string_view kSessionSummaryPrompt =
    "You maintain a running memory of a long conversation between a user and an "
    "assistant. Rewrite the memory so it preserves every durable fact: names, "
    "numbers, decisions, preferences, constraints and unresolved questions. "
    "Merge the new turns into the existing memory, keep it dense (prose or "
    "bullet points), and drop pleasantries. Do not answer the conversation, do "
    "not greet, and do not mention these instructions. Output only the updated "
    "memory.";

// Instruction for the internal assistant memory call (design-1.md §6.1).
constexpr std::string_view kAssistantSummaryPrompt =
    "You are going to write an assistant memory update about the user.";

// Estimated token cost of `text`: roughly four ASCII characters per token, and
// one token per non-ASCII codepoint. Counting each UTF-8 lead byte once is
// conservative for multibyte scripts. This mirrors the legacy estimator so the
// stored budgets keep their historical meaning.
std::size_t estimate_tokens(std::string_view text) {
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

} // namespace

std::size_t message_tokens(const Message& message) {
  return estimate_tokens(message.content_) + kMessageOverhead;
}

std::size_t prompt_tokens(const std::string& text) {
  return estimate_tokens(text);
}

Window plan_window(const std::vector<Message>& messages, std::size_t window_tokens) {
  Window window;
  window.start = messages.size();

  std::size_t used = 0;

  // Walk from the newest message backwards, keeping messages until the window
  // is exhausted. Only the conversation is measured; the system prompt is never
  // part of the bound.
  for (std::size_t i = messages.size(); i-- > 0;) {
    const std::size_t cost = message_tokens(messages[i]);

    // The newest message is always kept, even when it alone exceeds the
    // window, so a request never degenerates into "no history".
    if (used + cost > window_tokens && window.start < messages.size()) {
      break;
    }

    used += cost;
    window.start = i;
  }

  window.tokens = used;
  return window;
}


std::vector<const Message*> summary_candidates(
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

net::Request session_summary_request(
    const std::string& model, const std::string& existing_memory,
    const std::vector<const Message*>& candidates, std::size_t max_tokens,
    const std::string& reasoning_effort
) {
  std::string system(kSessionSummaryPrompt);

  if (!existing_memory.empty()) {
    system += "\n\n[EXISTING MEMORY]\n";
    system += existing_memory;
  }

  std::string transcript;
  transcript.reserve(candidates.size() * 128);

  for (const Message* message: candidates) {
    transcript += (message->role_ == Role::User) ? "User: " : "Assistant: ";
    transcript += message->content_;
    transcript += '\n';
  }

  net::Request request;
  request.model = model;
  request.instructions = system;
  request.include_tools = false;
  request.max_output_tokens = static_cast<int>(max_tokens);
  request.reasoning_effort = reasoning_effort;

  net::OutgoingMessage user;
  user.role = net::OutgoingMessage::Role::User;
  user.content = "New conversation to merge into memory:\n\n" + transcript;
  request.messages.push_back(std::move(user));

  return request;
}

net::Request assistant_summary_request(
    const std::string& model, const std::string& existing_memory,
    const std::string& sessions_block, std::size_t max_tokens,
    const std::string& reasoning_effort
) {
  std::string user;
  user += "# Previous Assistant Memory\n";
  user += existing_memory.empty() ? "(none yet)" : existing_memory;
  user += "\n\n# Sessions\n\n";
  user += sessions_block;

  net::Request request;
  request.model = model;
  request.instructions = std::string(kAssistantSummaryPrompt);
  request.include_tools = false;
  request.max_output_tokens = static_cast<int>(max_tokens);
  request.reasoning_effort = reasoning_effort;

  net::OutgoingMessage message;
  message.role = net::OutgoingMessage::Role::User;
  message.content = user;
  request.messages.push_back(std::move(message));

  return request;
}

std::string clamp_tokens(const std::string& text, std::size_t max_tokens) {
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

} // namespace lnb::context
