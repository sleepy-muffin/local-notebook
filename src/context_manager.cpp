#include "context_manager.hpp"
#include <deque>
#include <mmc/str.hpp>
namespace lnb::context {
Selection select_recent(const std::vector<message>& messages, uint32_t self_id, const std::string& system_prompt, const std::string& summary, const general& cfg) {
  Selection result;
  std::size_t used = mmc::str::estimate_tokens(system_prompt) + mmc::str::estimate_tokens(summary);
  const std::size_t reserved = cfg.response_token_budget_ + cfg.context_safety_margin_tokens_;
  const std::size_t budget = cfg.model_context_tokens_ > reserved ? cfg.model_context_tokens_ - reserved : 256;
  std::deque<const message*> kept;
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    const auto cost = mmc::str::estimate_tokens(it->content) + 16;
    if (used + cost > budget) break;
    used += cost; kept.push_front(&*it);
  }
  for (const auto* m : kept) result.messages.append(QJsonObject{{"role", m->sender_id == self_id ? "user" : "assistant"}, {"content", QString::fromStdString(m->content)}});
  result.estimated_tokens = used;
  return result;
}
bool should_summarize(const std::vector<message>& messages, const std::string& system_prompt, const general& cfg) {
  std::size_t total = mmc::str::estimate_tokens(system_prompt);
  for (const auto& m : messages) total += mmc::str::estimate_tokens(m.content) + 16;
  return total >= cfg.summary_trigger_tokens_;
}
}
