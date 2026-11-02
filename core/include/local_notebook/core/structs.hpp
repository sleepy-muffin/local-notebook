#pragma once

#include <string>
#include <cstdint>

#include <boost/json/object.hpp>

namespace lnb::core {

// The wire protocol an assistant talks to. Both shapes are OpenAI's; the two
// differ in the request body, the tool-loop round-trip and the SSE vocabulary,
// so everything above this module works with the protocol-neutral `Request`
// below and lets the builders and the stream parsers absorb the difference.
//
// Responses is the only endpoint that supports tool calling for the GPT-6
// family, and it is what OpenAI now recommends for new integrations. Chat
// Completions stays available for OpenAI-compatible servers (vLLM, llama.cpp,
// ...) that expose only that shape.
enum class ApiProtocol : std::uint8_t {
  ChatCompletions = 0,
  Responses = 1,
};


// The conversation-token readout the title bar reports against. `used_tokens` is
// the estimated cost of the unsummarised conversation alone (the messages above
// the session watermark) — never the system prompt — reported without trimming;
// `limit_tokens` is `session.summary_trigger_tokens_`. `used_tokens` is exactly
// the quantity the automatic collapse compares against the trigger.
struct ContextUsage {
  std::size_t used_tokens = 0;
  std::size_t limit_tokens = 0;
};


enum class error_code {
  SUCCESS,


  ERR_INVALID_SESSION_ID = 100,
  ERR_SESSION_SUMMARY_TRIGGER_HAS_MET = 200,
  ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET = 201,

  // Another task already owns the session (one turn or fold at a time), or the
  // shared assistant fold is running and would read this session's rows.
  ERR_SESSION_BUSY = 202,
  ERR_ASSISTANT_BUSY = 203,
  ERR_ASSISTANT_HAS_NOT_LOADED_YET = 204,
  ERR_ASSISTANT_OR_ANY_SESSION_BUSY = 205,


};


// A description of one summarisation request, built immediately before it is
// transmitted so the UI can show the user exactly what will leave the machine.
// `request` is the exact body; `kind` names the pass; `automatic` is true when
// the token threshold triggered the pass rather than the user asking for it.
//
// Always strict.
struct SummaryPlan {
  std::string kind;
  boost::json::object request;
  std::string watermark_text;

  // Approximate model-facing input size of `request`, using the same
  // character-based estimate as the rest of the app.
  std::size_t estimated_input_tokens = 0;

  // True when the pass continues in further requests after this one (a large
  // overflow is folded in several bounded batches).
  bool more_requests = false;
};

// Called on the worker thread immediately before a summarisation request is
// transmitted. Return true to send it, false to skip the pass. An empty
// approver always proceeds, so headless callers and tests are unaffected.
using SummaryApprover = std::function<bool(const SummaryPlan&)>;


} // namespace lnb::core
