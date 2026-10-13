#pragma once

#include <QNetworkProxy>

#include <optional>
#include <string>

class QNetworkAccessManager;

namespace lnb {

// Turns a proxy specification into a Qt proxy.
//
// The scheme selects the proxy type; an omitted scheme keeps the historical
// meaning of a bare `host:port` (SOCKS5), so sessions created before schemes
// existed keep working unchanged.
//
//   host:port                        -> SOCKS5 (historical default)
//   socks5://host:port               -> SOCKS5
//   socks5h://host:port              -> SOCKS5 (DNS resolved by the proxy)
//   http://host:port                 -> HTTP CONNECT tunnel
//   https://host:port                -> HTTP CONNECT tunnel
//   scheme://user:password@host:port -> same, with credentials
//
// The scheme is case-insensitive. Throws std::runtime_error on malformed input
// or an unsupported scheme, so the caller can surface a precise message.
QNetworkProxy parse_proxy(const std::string& spec);

// Points `manager` at the session proxy, if one is configured. A missing or
// empty specification leaves the manager on the system default proxy, so an
// unproxied session behaves exactly as before.
void configure_proxy(
    QNetworkAccessManager& manager, const std::optional<std::string>& proxy
);

} // namespace lnb
