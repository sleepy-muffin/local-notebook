#include "context/context.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

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

// Instruction for the internal assistant summarisation call (design-1.md §6.1).
// The American spelling is intentional: this is the frozen template verbatim.
constexpr std::string_view kAssistantSummaryPrompt =
    "You are going to make an assistant memory summarization about the user.";

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

Window plan_window(
    const std::vector<Message>& messages, std::size_t system_tokens,
    std::size_t window_tokens
) {
  Window window;
  window.start = messages.size();

  std::size_t used = system_tokens;

  // Walk from the newest message backwards, keeping messages until the window
  // is exhausted.
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

  window.tokens = used - system_tokens;
  return window;
}

bool should_summarize(
    const std::vector<Message>& messages, std::size_t system_tokens,
    std::uint32_t watermark, std::size_t trigger_tokens
) {
  std::size_t total = system_tokens;

  for (const Message& message: messages) {
    if (message.id_ <= watermark) {
      continue; // already folded into the memory
    }

    total += message_tokens(message);
  }

  return total >= trigger_tokens;
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

QJsonObject session_summary_request(
    const std::string& model, const std::string& existing_memory,
    const std::vector<const Message*>& candidates, std::size_t max_tokens
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

  QJsonArray messages;
  messages.append(
      QJsonObject{{"role", "system"}, {"content", QString::fromStdString(system)}}
  );
  messages.append(QJsonObject{
      {"role", "user"},
      {"content",
       QString::fromStdString("New conversation to fold into memory:\n\n" + transcript)},
  });

  // The network client only speaks Server-Sent Events, so the summariser must
  // ask for a stream too: a non-streaming body carries no `data: [DONE]` and
  // send_message_once() would reject the response.
  return QJsonObject{
      {"model", QString::fromStdString(model)},
      {"messages", messages},
      {"max_tokens", static_cast<int>(max_tokens)},
      {"stream", true},
  };
}

QJsonObject assistant_summary_request(
    const std::string& model, const std::string& existing_memory,
    const std::string& sessions_block, std::size_t max_tokens
) {
  std::string user;
  user += "# Previous Assistant Memory\n";
  user += existing_memory.empty() ? "(none yet)" : existing_memory;
  user += "\n\n# Sessions\n\n";
  user += sessions_block;

  QJsonArray messages;
  messages.append(QJsonObject{
      {"role", "system"},
      {"content", QString::fromStdString(std::string(kAssistantSummaryPrompt))},
  });
  messages.append(
      QJsonObject{{"role", "user"}, {"content", QString::fromStdString(user)}}
  );

  // As above: stream, so the SSE-only client can parse the response.
  return QJsonObject{
      {"model", QString::fromStdString(model)},
      {"messages", messages},
      {"max_tokens", static_cast<int>(max_tokens)},
      {"stream", true},
  };
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
