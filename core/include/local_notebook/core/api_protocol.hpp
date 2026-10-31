#pragma once

#include <cstdint>

namespace lnb::net {

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

} // namespace lnb::net
