#include "net/proxy.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace lnb::net {

namespace {

// ASCII lower-case for scheme comparison. Proxy schemes are ASCII, so a
// locale-independent fold is enough.
std::string to_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

// Splits `host:port` and validates the port. The host may be an IPv4 address,
// an IPv6 literal in brackets, or a host name; only the last colon is treated as
// the host/port separator, which keeps IPv6 literals intact.
void apply_host_port(
    Proxy& proxy, const std::string& spec, std::string endpoint
) {
  // Strip optional credentials ("user:password@") from the authority.
  std::string credentials;
  const std::size_t at = endpoint.rfind('@');

  if (at != std::string::npos) {
    credentials = endpoint.substr(0, at);
    endpoint = endpoint.substr(at + 1);
  }

  const std::size_t colon = endpoint.rfind(':');

  if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) {
    throw std::runtime_error("Invalid proxy address (expected host:port): " + spec);
  }

  proxy.host = endpoint.substr(0, colon);
  const std::string port_text = endpoint.substr(colon + 1);

  int port = 0;

  try {
    std::size_t consumed = 0;
    port = std::stoi(port_text, &consumed);

    if (consumed != port_text.size()) {
      throw std::invalid_argument("trailing characters");
    }
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid proxy port: " + spec);
  }

  if (port <= 0 || port > 65535) {
    throw std::runtime_error("Invalid proxy port: " + spec);
  }

  proxy.port = static_cast<std::uint16_t>(port);

  if (!credentials.empty()) {
    const std::size_t cred_colon = credentials.find(':');

    if (cred_colon == std::string::npos) {
      proxy.user = credentials;
    } else {
      proxy.user = credentials.substr(0, cred_colon);
      proxy.password = credentials.substr(cred_colon + 1);
    }
  }
}

} // namespace

Proxy parse_proxy(const std::string& spec) {
  if (spec.empty()) {
    throw std::runtime_error("Empty proxy address");
  }

  std::string scheme = "socks5";
  std::string endpoint = spec;

  const std::size_t scheme_end = spec.find("://");

  if (scheme_end != std::string::npos) {
    scheme = to_lower(spec.substr(0, scheme_end));
    endpoint = spec.substr(scheme_end + 3);
  }

  Proxy proxy;

  if (scheme == "http" || scheme == "https") {
    proxy.type = Proxy::Type::Http;
  } else if (scheme == "socks5" || scheme == "socks5h") {
    proxy.type = Proxy::Type::Socks5;
  } else {
    throw std::runtime_error("Unsupported proxy scheme '" + scheme + "': " + spec);
  }

  apply_host_port(proxy, spec, endpoint);

  return proxy;
}

} // namespace lnb::net
