#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "local-notebook-core/api_protocol.hpp"

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

  // Which reasoning level this session asks for. Empty means "use the model's
  // own default", which is how a row written before the selector existed reads.
  // A concrete level the current model does not support is refused at send time
  // rather than silently reset (see net::reasoning_effort_supported).
  std::string reasoning_effort_;

  // Trigger bookkeeping only: the highest message id already folded. See
  // design-1.md §7 — they are never used to select what a summariser reads.
  std::uint32_t memory_watermark_ = 0;
  std::uint32_t assistant_memory_watermark_ = 0;

  std::optional<std::string> shared_directory_;

  // Pinned sessions are listed in their own sidebar category above the rest.
  // Stored in the session row (design-1.md §9).
  bool pinned_ = false;

  // The input bound, and the collapse trigger: a request carries the system
  // prompt plus the transcript, and once that reaches this many tokens the
  // older messages are folded into memory_.
  std::size_t memory_trigger_tokens_ = 16000;

  // The level the transcript collapses to when the trigger fires: the newest
  // messages kept verbatim, everything older folded. Keep it well below
  // summary_trigger_tokens_, or the collapse re-triggers straight away.
  std::size_t summary_keep_tokens_ = 4000;

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

  // The wire protocol this assistant talks. Responses is the default for every
  // assistant, including ones created before the switch existed, because it is
  // the only endpoint that supports tool calling for the GPT-6 family.
  net::ApiProtocol protocol_ = net::ApiProtocol::Responses;

  // Assistant::reasoning_effort_ is the level a new session starts on
  // and the level the assistant-level summariser runs at.
  // It is not what normal chat turns use.
  std::string reasoning_effort_;

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
