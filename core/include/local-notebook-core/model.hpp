#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

  // Which reasoning level this session asks for on chat turns. Empty means "use
  // the model's own default", which is how a row written before the selector
  // existed reads. A concrete level the current model does not support is refused
  // at send time rather than silently reset (see net::reasoning_effort_supported).
  std::string reasoning_effort_;

  // Which reasoning level this session's summariser runs at. Kept separate from
  // reasoning_effort_ so a memory update can run without reasoning while chat
  // turns reason (or the other way round). Defaults to "none"; a level the
  // current model does not support is refused at summarisation time rather than
  // silently replaced.
  std::string summary_reasoning_effort_ = "none";

  // Trigger bookkeeping only: the highest message id already folded. See
  // design-1.md §7 — they are never used to select what a summariser reads.
  std::uint32_t memory_watermark_ = 0;
  std::uint32_t assistant_memory_watermark_ = 0;

  // Directories the filesystem tools may access. The model never sees these
  // paths: it is given a synthetic tree of relative paths, with a single
  // directory presented at the root and several presented under visible names.
  // Empty disables the filesystem tools.
  std::vector<std::string> shared_directories_;

  // Whether this session contributes to the shared assistant memory. Turning it
  // off keeps the session's own memory and transcript intact but leaves it out of
  // both the assistant trigger and the assistant fold. It is stored per session
  // (schema version 7) so a restart preserves the choice.
  bool include_in_assistant_memory_ = true;

  // Pinned sessions are listed in their own sidebar category above the rest.
  // Stored in the session row (design-1.md §9).
  bool pinned_ = false;

  // Tool permissions for this session. The master switch disables every tool
  // without changing the individual preferences. Read covers the filesystem
  // reading tools (list, search, find, read); write covers creating, editing and
  // deleting files in the shared directories; online covers web_search and
  // fetch_url. Writing is off by default, so an existing session never gains the
  // ability to modify its shared directories without the user enabling it.
  bool tools_enabled_ = true;
  bool read_tools_enabled_ = true;
  bool write_tools_enabled_ = false;
  bool online_tools_enabled_ = true;

  // The input bound, and the collapse trigger: a request carries the system
  // prompt plus the transcript, and once that reaches this many tokens the
  // older messages are folded into memory_.
  std::size_t memory_trigger_tokens_ = 16000;

  // The level the transcript collapses to when the trigger fires: the newest
  // messages kept verbatim, everything older folded. Keep it well below
  // summary_trigger_tokens_, or the collapse re-triggers straight away.
  std::size_t summary_keep_tokens_ = 4000;

  // Completion cap for the generated memory. This is an upper bound on the
  // summariser's output, not a target written into its prompt.
  std::size_t summary_token_budget_ = 4000;

  // Token budget for the tool results fed back to the model as input during one
  // turn. The tool loop's round and call counts are fixed in the core (they are
  // loop guards, not a cost knob), so only this cumulative budget is exposed.
  std::size_t input_token_produced_midway_ = 32000;

  // The whole turn's budget for produced tokens: the visible answer plus the
  // tool results fed back to the model. Each round only gets what is left.
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

  // Request windows, in seconds. A streaming turn may wait this long for the
  // next response byte before the request is abandoned; summarisation gets its
  // own, longer window because a reasoning model can stay silent while folding
  // a large transcript. Both live in the assistant's settings.json.
  std::size_t turn_timeout_seconds_ = 180;
  std::size_t summary_timeout_seconds_ = 420;

  // The wire protocol this assistant talks. Responses is the default for every
  // assistant, including ones created before the switch existed, because it is
  // the only endpoint that supports tool calling for the GPT-6 family.
  net::ApiProtocol protocol_ = net::ApiProtocol::Responses;

  // The level a new session of this assistant starts on. It is not what normal
  // chat turns use, and not what the summarisers run at.
  std::string reasoning_effort_;

  // The level the assistant-wide summariser runs at. Separate from
  // reasoning_effort_ so the shared memory can be folded at a different level
  // than a new session starts on. Defaults to "none".
  std::string summary_reasoning_effort_ = "none";

  // Completion cap for the generated assistant memory_. This is an upper bound
  // on the summariser's output, not a target written into its prompt.
  std::size_t summary_token_budget_ = 4000;

  // Trigger: new unsummarised tokens across ALL sessions. Also the cap for the
  // recent-message pool gathered for the summarisation.
  std::size_t summary_trigger_tokens_ = 80000;

  // Cap for the session-summary pool in the assistant summarisation. Separate
  // from summary_trigger_tokens_ — do not mix the two pools.
  std::size_t summary_token_budget_for_session_summarization_ = 16000;
};

} // namespace lnb
