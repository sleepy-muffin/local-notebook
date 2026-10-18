#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace lnb::net {

// A parsed proxy specification. The scheme selects the transport; a bare
// `host:port` keeps the historical meaning (SOCKS5), so sessions created before
// schemes existed keep working unchanged.
//
//   host:port                        -> SOCKS5 (historical default)
//   socks5://host:port               -> SOCKS5
//   socks5h://host:port              -> SOCKS5 (DNS resolved by the proxy)
//   http://host:port                 -> HTTP CONNECT tunnel
//   https://host:port                -> HTTP CONNECT tunnel
//   scheme://user:password@host:port -> same, with credentials
struct Proxy {
  enum class Type {
    Socks5,
    Http,
  };

  Type type = Type::Socks5;
  std::string host;
  std::uint16_t port = 0;
  std::string user;
  std::string password;

  [[nodiscard]] bool has_credentials() const {
    return !user.empty() || !password.empty();
  }
};

// Parses `spec`. Throws std::runtime_error on malformed input or an unsupported
// scheme, so the caller can surface a precise message.
Proxy parse_proxy(const std::string& spec);

} // namespace lnb::net
