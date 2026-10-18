#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace lnb::net {

struct HttpHeader {
  std::string name;
  std::string value;
};

// Called once the status line and headers are parsed. Returning false stops the
// body from being read (the connection is then dropped).
using ResponseHandler =
    std::function<bool(int status, const std::vector<HttpHeader>& headers)>;

// Called for each decoded body chunk. Returning false stops reading.
using BodyHandler = std::function<bool(std::string_view chunk)>;

// Performs one HTTP(S) request over a fresh connection.
//
// `on_body` receives the decoded body as it arrives (chunked transfer encoding
// and content length are handled by the HTTP parser), which is what lets the SSE
// client show tokens while they are still being generated. Throws
// std::runtime_error on transport or TLS failures, and the message
// "Generation cancelled" when `stop` is requested.
//
// `max_redirects` is 0 by default: a POST must never be silently replayed.
//
// `inactivity_timeout_ms` bounds connecting, the TLS handshake and writing the
// request. `read_timeout_ms` bounds each wait for response headers or body
// bytes; 0 means "use `inactivity_timeout_ms`". Streaming turns pass a
// three-minute read window and summariser requests a seven-minute one, because
// a reasoning model can stream nothing for minutes before it emits its first
// event.
int http_request(
    std::string_view method,
    const std::string& url,
    const std::optional<std::string>& proxy,
    const std::vector<HttpHeader>& headers,
    const std::string& body,
    std::stop_token stop,
    const ResponseHandler& on_response,
    const BodyHandler& on_body,
    int inactivity_timeout_ms = 60'000,
    int max_redirects = 0,
    int read_timeout_ms = 0
);

// A GET that follows redirects and returns at most `max_bytes` of the body.
// Throws when the final status is not 2xx. `extra_headers` are merged over the
// defaults (a header with the same name replaces the default), which lets a
// caller present a browser User-Agent to sites that reject non-browser clients.
std::string http_get_body(
    const std::string& url,
    const std::optional<std::string>& proxy,
    int max_bytes,
    std::stop_token stop = std::stop_token{},
    const std::vector<HttpHeader>& extra_headers = {}
);

} // namespace lnb::net
