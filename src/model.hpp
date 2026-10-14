#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// The reconstructed domain model (design-1.md §2). One Assistant per process;
// many Sessions per assistant; all sessions of an assistant share one memory.
//
// These are plain data. The lazily-loading managers (store/managers.hpp) live
// outside the structs so the model stays copyable and cheap to pass around.
namespace lnb {

// Who wrote a message. Replaces the legacy sender_id/reader_id pair.
enum class Role : std::uint8_t {
  User = 0,
  Assistant = 1,
};

struct Message {
  std::uint32_t id_ = 0;          // primary key; monotonic per assistant directory
  std::uint32_t session_id_ = 0;  // owning session
  Role role_ = Role::User;
  std::string content_;
  std::string made_at_;           // ISO-8601 UTC; sortable, so sessions can be
                                  // interleaved by time for assistant memory
};

class Session {
public:
  std::uint32_t id_ = 0;
  std::string title_;
  std::string custom_instructions_;  // user-authored; kept separate from memory_
  std::string memory_;               // generated session summarisation

  // Trigger bookkeeping only: the highest message id already folded. See
  // design-1.md §7 — they are never used to select what a summariser reads.
  std::uint32_t memory_watermark_ = 0;
  std::uint32_t assistant_memory_watermark_ = 0;

  std::optional<std::string> shared_directory_;

  // The single session knob: automatic-summarisation trigger, verbatim window
  // and summarisation granularity all in one.
  std::size_t summary_trigger_tokens_ = 16000;
  std::size_t summary_token_budget_ = 4000;
  std::size_t summary_reasoning_headroom_tokens_ = 12000;

  // One assistant turn's tool loop.
  std::size_t max_tool_rounds_ = 8;
  std::size_t max_tool_calls_ = 32;
  std::size_t tool_output_max_tokens_ = 8000;

  // Tokens reserved for the assistant's visible answer.
  std::size_t response_token_budget_ = 8000;
};

class Assistant {
public:
  std::string name_;
  std::string custom_instructions_;
  std::string api_url_;
  std::string authentication_token_;
  std::string model_;
  std::optional<std::string> proxy_;
  std::string memory_;  // generated cross-session memory

  // Target size of the generated assistant memory_.
  std::size_t summary_token_budget_ = 4000;

  // Completion headroom for the summariser's hidden reasoning.
  std::size_t summary_reasoning_headroom_tokens_ = 12000;

  // Trigger: new unsummarised tokens across ALL sessions. Also the cap for the
  // recent-message pool gathered for the summarisation.
  std::size_t summary_trigger_tokens_ = 80000;

  // Cap for the session-summary pool in the assistant summarisation. Separate
  // from summary_trigger_tokens_ — do not mix the two pools.
  std::size_t summary_token_budget_for_session_summarization_ = 16000;
};

} // namespace lnb
