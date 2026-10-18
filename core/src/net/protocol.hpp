#pragma once

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "local-notebook-core/api_protocol.hpp"
#include "net/tools.hpp"

// The protocol-neutral request model. The protocol enum itself is public
// (local-notebook-core/api_protocol.hpp), because it is part of Assistant.
namespace lnb::net {

// One entry in the outgoing conversation, independent of the wire API. The
// builders serialise this to `messages` (Chat) or `input` (Responses), which
// differ in how a tool call and its result are represented.
struct OutgoingMessage {
  enum class Role : std::uint8_t {
    User = 0,
    Assistant = 1,
    ToolResult = 2,
  };

  Role role = Role::User;
  std::string content;
  // Assistant only: the calls the model requested, echoed back verbatim.
  std::vector<ToolCall> tool_calls;
  // ToolResult only: the id of the call this message answers.
  std::string tool_call_id;
};

// Everything one request needs besides the protocol choice.
struct Request {
  std::string model;
  // The system prompt. Chat carries it as the first `system` message; Responses
  // carries it in the top-level `instructions` field.
  std::string instructions;
  std::vector<OutgoingMessage> messages;
  // Offer the filesystem tools as well as the network tools.
  bool filesystem_tools = false;
  // False omits the `tools` array entirely. Summarisation requests carry no
  // tools, so they must not advertise them.
  bool include_tools = true;
  // Reserved for the model's visible answer (and, for a reasoning model, its
  // hidden thinking). Mapped to `max_tokens` (Chat) or `max_output_tokens`
  // (Responses). Zero omits the field.
  int max_output_tokens = 0;
  // "", "none", "minimal", "low", "medium", "high", "xhigh", "max". Empty omits
  // the parameter entirely, so the model's own default applies.
  std::string reasoning_effort;

  // Responses only: raw output items from earlier rounds of the tool loop
  // (reasoning items, `function_call` items, assistant messages), replayed
  // verbatim after `messages`. Reasoning items must travel back with the tool
  // outputs or the model loses the thread. The Chat builder ignores this.
  std::vector<boost::json::object> raw_items;
};

// Serialises `request` for `protocol`.
[[nodiscard]] boost::json::object build_request(
    ApiProtocol protocol, const Request& request
);

// The Chat Completions body (`messages`, `tools` nested under `function`,
// `max_tokens`, top-level `reasoning_effort`).
[[nodiscard]] boost::json::object build_chat_request(const Request& request);

// The Responses body (`input`, top-level `instructions`, flattened `tools`,
// `max_output_tokens`, `reasoning: {effort}`).
[[nodiscard]] boost::json::object build_responses_request(const Request& request);

// The tool schemas for `protocol`. Chat nests each schema under `function`
// (`{type, function: {...}}`); Responses flattens it
// (`{type, name, description, parameters}`).
[[nodiscard]] boost::json::array tools_for(ApiProtocol protocol, bool filesystem);

// --- Endpoints ----------------------------------------------------------

// The configured URL is a base such as "https://api.example.com/v1". This
// strips a trailing "/chat/completions", "/completions", "/responses" and any
// trailing slash, so an assistant migrated from the old full-endpoint field
// lands on the same base.
[[nodiscard]] std::string normalize_base_url(const std::string& url);

// True when `url` is a usable base: http/https, a non-empty host, no query or
// fragment, and a path ending in "/v1". Saving an assistant is refused
// otherwise.
[[nodiscard]] bool is_valid_base_url(const std::string& url);

// Appends the protocol's endpoint to a normalised base.
[[nodiscard]] std::string derive_endpoint(
    const std::string& base_url, ApiProtocol protocol
);

// --- Reasoning ----------------------------------------------------------

// The reasoning-effort values the model accepts, in the order the selector
// should show them. A model-specific list when one is known, otherwise the
// common fallback (none/low/high). Matching is case-insensitive and treats
// spaces as dashes, so "GPT-6.1 Luna" and "gpt-6.1-luna" agree.
[[nodiscard]] std::vector<std::string> reasoning_efforts_for_model(
    const std::string& model
);

// The level a fresh assistant or session starts on: the first (cheapest) entry
// of the model's list. Never empty.
[[nodiscard]] std::string default_reasoning_effort(const std::string& model);

// The model's levels, with `stored` appended when it is non-empty and not already
// present. A selector built from this can show a level the current model no
// longer supports (the usual cause is a model change), so the user sees the
// stored value and can correct it, instead of it being silently dropped.
[[nodiscard]] std::vector<std::string> reasoning_efforts_with_stored(
    const std::string& model, const std::string& stored
);

// True when `effort` is in the model's list. An empty `effort` means "the model's
// own default" and is always acceptable. Case-insensitive.
[[nodiscard]] bool reasoning_effort_supported(
    const std::string& model, const std::string& effort
);

// --- Persistence helpers ------------------------------------------------

// The stored spelling of a protocol ("responses" / "chat_completions").
[[nodiscard]] std::string protocol_name(ApiProtocol protocol);

// Parses a stored spelling. Anything unrecognised falls back to Responses, so
// an assistant written before this switch existed routes through Responses.
[[nodiscard]] ApiProtocol protocol_from_name(const std::string& name);

// Prepends "https://" when the text carries no scheme, so a base typed as
// "example.com/v1" is completed when the field loses focus. An explicit
// "http://" or "https://" is left alone.
[[nodiscard]] std::string ensure_scheme(const std::string& url);

} // namespace lnb::net
