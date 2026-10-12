#include "context_manager.hpp"

#include <mmc/str.hpp>

namespace lnb::context {

namespace {
// Fixed per-message overhead for the role field and JSON scaffolding.
constexpr std::size_t kMessageOverhead = 16;

// Instruction for the internal summarisation call. It is deliberately separate
// from the user's custom instructions so the two never overwrite each other.
constexpr std::string_view kSummarySystemPrompt =
    "You maintain a running memory of a long conversation between a user and an "
    "assistant. Rewrite the memory so it preserves every durable fact: names, "
    "numbers, decisions, preferences, constraints and unresolved questions. "
    "Merge the new turns into the existing memory, keep it dense (prose or "
    "bullet points), and drop pleasantries. Do not answer the conversation, do "
    "not greet, and do not mention these instructions. Output only the updated "
    "memory.";
} // namespace

std::size_t message_tokens(const message& m) {
  return mmc::str::estimate_tokens(m.content) + kMessageOverhead;
}

std::size_t prompt_tokens(const std::string& text) {
  return mmc::str::estimate_tokens(text);
}

std::size_t request_budget(const general& cfg) {
  const std::size_t reserved =
      cfg.response_token_budget_ + cfg.context_safety_margin_tokens_;
  return cfg.model_context_tokens_ > reserved ? cfg.model_context_tokens_ - reserved : 256;
}

std::size_t summary_request_max_tokens(const general& cfg) {
  return cfg.summary_token_budget_ + cfg.summary_reasoning_headroom_tokens_;
}

std::size_t summarization_budget(const general& cfg) {
  // Reserve the whole completion (memory plus reasoning) and the safety margin:
  // the input transcript and everything the model generates share one context.
  const std::size_t reserved =
      summary_request_max_tokens(cfg) + cfg.context_safety_margin_tokens_;
  return cfg.model_context_tokens_ > reserved ? cfg.model_context_tokens_ - reserved : 256;
}

Window plan_window(
    const std::vector<message>& messages, std::size_t system_tokens,
    uint32_t watermark, std::size_t budget
) {
  Window window;
  window.start = messages.size();

  std::size_t used = system_tokens;

  // Walk from the newest message backwards, keeping messages that are not yet
  // covered by the summary until the budget is exhausted.
  for (std::size_t i = messages.size(); i-- > 0;) {
    const message& m = messages[i];

    if (m.id <= watermark) {
      // Messages are ordered by id, so everything older is covered too.
      break;
    }

    const std::size_t cost = message_tokens(m);

    // The newest message is always kept, even if it alone exceeds the budget.
    if (used + cost > budget && window.start < messages.size()) {
      break;
    }

    used += cost;
    window.start = i;
  }

  window.tokens = used - system_tokens;
  return window;
}

bool should_summarize(
    const std::vector<message>& messages, std::size_t system_tokens,
    uint32_t watermark, const general& cfg
) {
  std::size_t total = system_tokens;

  for (const auto& m: messages) {
    if (m.id <= watermark) {
      continue; // already folded into the summary
    }

    total += message_tokens(m);
  }

  return total >= cfg.summary_trigger_tokens_;
}

std::vector<const message*> summary_candidates(
    const std::vector<message>& messages, std::size_t window_start,
    uint32_t watermark, std::size_t max_tokens
) {
  std::vector<const message*> candidates;

  if (window_start == 0) {
    return candidates;
  }

  std::size_t used = 0;

  // Oldest first so the summary reads chronologically. `i < window_start`
  // guarantees the newest message (always inside the window) is never folded.
  for (std::size_t i = 0; i < window_start && i < messages.size(); ++i) {
    const message& m = messages[i];

    if (m.id <= watermark) {
      continue; // already folded into the summary
    }

    const std::size_t cost = message_tokens(m);

    if (used + cost > max_tokens && !candidates.empty()) {
      break;
    }

    used += cost;
    candidates.push_back(&m);
  }

  return candidates;
}

QJsonObject summary_request(
    const std::string& model, const std::string& existing_summary,
    const std::vector<const message*>& candidates, uint32_t self_id,
    std::size_t max_tokens
) {
  std::string system(kSummarySystemPrompt);

  if (!existing_summary.empty()) {
    system += "\n\n[EXISTING MEMORY]\n";
    system += existing_summary;
  }

  std::string transcript;
  transcript.reserve(candidates.size() * 128);

  for (const auto* m: candidates) {
    transcript += (m->sender_id == self_id) ? "User: " : "Assistant: ";
    transcript += m->content;
    transcript += '\n';
  }

  QJsonArray messages;
  messages.append(
      QJsonObject{{"role", "system"}, {"content", QString::fromStdString(system)}}
  );
  messages.append(QJsonObject{
      {"role", "user"},
      {"content", QString::fromStdString(
          "New conversation to fold into memory:\n\n" + transcript
      )},
  });

  return QJsonObject{
      {"model", QString::fromStdString(model)},
      {"messages", messages},
      {"max_tokens", static_cast<int>(max_tokens)},
      {"stream", true},
  };
}

std::string clamp_tokens(const std::string& text, std::size_t max_tokens) {
  if (max_tokens == 0 || mmc::str::estimate_tokens(text) <= max_tokens) {
    return text;
  }

  // Roughly four ASCII characters per token; the loop below then shrinks the
  // result until the estimate (which is conservative for multibyte text) fits.
  std::string result = text.substr(0, max_tokens * 4);

  while (!result.empty() && mmc::str::estimate_tokens(result) > max_tokens) {
    result.resize(result.size() * 9 / 10);
  }

  // Do not leave a half-written UTF-8 sequence at the cut.
  while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xC0) == 0x80) {
    result.pop_back();
  }

  result += "\n\n[tool output truncated to fit the context budget]";
  return result;
}

void trim_to_budget(QJsonArray& messages, std::size_t budget) {
  if (messages.isEmpty()) {
    return;
  }

  const auto cost_of = [](const QJsonObject& m) {
    return mmc::str::estimate_tokens(m.value("content").toString().toStdString()) +
           kMessageOverhead;
  };

  const auto total_cost = [&messages, &cost_of]() {
    std::size_t sum = 0;

    for (const auto& value: messages) {
      sum += cost_of(value.toObject());
    }

    return sum;
  };

  // Pin the system prompt when it is present at the front.
  const bool has_system =
      messages.first().toObject().value("role").toString() == QStringLiteral("system");
  const std::size_t first = has_system ? 1 : 0;

  // Drop whole groups from the front: an assistant message that requested tools
  // is always followed by its tool results, and dropping only one half would
  // produce an invalid request.
  while (total_cost() > budget && messages.size() > first + 1) {
    const QJsonObject head = messages.at(static_cast<int>(first)).toObject();
    const bool requested_tools = head.contains("tool_calls");

    messages.removeAt(static_cast<int>(first));

    if (requested_tools) {
      while (first < messages.size() &&
             messages.at(static_cast<int>(first)).toObject().value("role").toString() ==
                 QStringLiteral("tool")) {
        messages.removeAt(static_cast<int>(first));
      }
    }

    // Defensive: never leave a tool result without its assistant message.
    while (first < messages.size() &&
           messages.at(static_cast<int>(first)).toObject().value("role").toString() ==
               QStringLiteral("tool")) {
      messages.removeAt(static_cast<int>(first));
    }
  }
}

} // namespace lnb::context
