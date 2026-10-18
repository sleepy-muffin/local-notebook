#include "net/http.hpp"

#include "net/proxy.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lnb::net {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

// One parsed absolute URL, in the pieces the request needs.
struct parsed_url {
  std::string scheme;
  std::string host;
  std::string port;
  std::string target;  // path + query, as sent on the request line
  bool explicit_port = false;

  [[nodiscard]] bool tls() const { return scheme == "https"; }

  [[nodiscard]] std::string host_header() const {
    // An IPv6 literal has to be bracketed in the Host header.
    std::string host_text = host;

    if (host_text.find(':') != std::string::npos &&
        (host_text.empty() || host_text.front() != '[')) {
      host_text = "[" + host_text + "]";
    }

    return explicit_port ? host_text + ":" + port : host_text;
  }

  [[nodiscard]] std::string absolute_target() const {
    return scheme + "://" + host_header() + target;
  }
};

parsed_url parse_url(const std::string& text) {
  const auto result = boost::urls::parse_uri(text);

  if (!result) {
    throw std::runtime_error("Invalid URL: " + text);
  }

  const auto view = *result;
  parsed_url url;
  url.scheme = std::string(view.scheme());

  if (url.scheme != "http" && url.scheme != "https") {
    throw std::runtime_error("Only http/https URLs are supported: " + text);
  }

  url.host = std::string(view.host());

  if (url.host.empty()) {
    throw std::runtime_error("URL has no host: " + text);
  }

  url.explicit_port = !view.port().empty();
  url.port = url.explicit_port ? std::string(view.port())
                               : (url.scheme == "https" ? "443" : "80");

  url.target = std::string(view.encoded_target());

  if (url.target.empty()) {
    url.target = "/";
  }

  return url;
}

std::string resolve_location(const std::string& base, const std::string& location) {
  if (location.starts_with("http://") || location.starts_with("https://")) {
    return location;
  }

  const parsed_url from = parse_url(base);

  if (location.starts_with("//")) {
    return from.scheme + ":" + location;
  }

  if (location.starts_with("/")) {
    return from.scheme + "://" + from.host_header() + location;
  }

  // Relative reference: resolve against the base's directory.
  std::string directory = from.target;
  const std::size_t query = directory.find('?');

  if (query != std::string::npos) {
    directory.erase(query);
  }

  const std::size_t slash = directory.rfind('/');
  directory = slash == std::string::npos ? "/" : directory.substr(0, slash + 1);

  return from.scheme + "://" + from.host_header() + directory + location;
}

bool equals_ignore_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }

  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto left = static_cast<unsigned char>(a[i]);
    const auto right = static_cast<unsigned char>(b[i]);

    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }

  return true;
}

std::string base64_encode(std::string_view input) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve((input.size() + 2) / 3 * 4);

  for (std::size_t i = 0; i < input.size(); i += 3) {
    std::uint32_t value = static_cast<unsigned char>(input[i]) << 16;

    if (i + 1 < input.size()) {
      value |= static_cast<unsigned char>(input[i + 1]) << 8;
    }

    if (i + 2 < input.size()) {
      value |= static_cast<unsigned char>(input[i + 2]);
    }

    out.push_back(kTable[(value >> 18) & 0x3F]);
    out.push_back(kTable[(value >> 12) & 0x3F]);
    out.push_back(i + 1 < input.size() ? kTable[(value >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < input.size() ? kTable[value & 0x3F] : '=');
  }

  return out;
}

// Drives an io_context in short slices so a stop request (or an inactivity
// timeout) is observed promptly. Every operation creates a fresh instance, so
// the timeout always measures one operation, not the whole request.
class pump {
public:
  pump(
      asio::io_context& ioc, std::stop_token stop, int timeout_ms,
      std::function<void()> cancel
  )
      : ioc_(ioc),
        stop_(stop),
        timeout_(std::chrono::milliseconds(timeout_ms)),
        cancel_(std::move(cancel)) {}

  template<typename Predicate>
  void run(Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + timeout_;

    while (!done()) {
      if (stop_.stop_requested()) {
        cancel_();
        run_slice();
        throw std::runtime_error("Generation cancelled");
      }

      if (std::chrono::steady_clock::now() > deadline) {
        cancel_();
        run_slice();
        throw std::runtime_error("HTTP request timed out");
      }

      run_slice();
    }
  }

private:
  // `run_for` leaves the context stopped once it runs out of work. This class is
  // used for every step of a request over one shared io_context, so without the
  // restart the second and later steps return immediately: the loop then spins
  // at 100% CPU until the deadline and reports a bogus timeout.
  void run_slice() {
    if (ioc_.stopped()) {
      ioc_.restart();
    }

    ioc_.run_for(std::chrono::milliseconds(50));
  }

private:
  asio::io_context& ioc_;
  std::stop_token stop_;
  std::chrono::milliseconds timeout_;
  std::function<void()> cancel_;
};

template<typename Stream>
std::function<void()> cancel_for(Stream& stream) {
  return [&stream] {
    if constexpr (requires { stream.next_layer().socket(); }) {
      stream.next_layer().socket().cancel();
    } else {
      stream.socket().cancel();
    }
  };
}

// Small synchronous-style helpers over the async socket, each bounded by the
// pump's timeout and the stop token.
void socks_write(tcp::socket& socket, std::string_view data, asio::io_context& ioc,
                 std::stop_token stop, int timeout_ms) {
  boost::system::error_code error;
  bool done = false;

  asio::async_write(
      socket, asio::buffer(data.data(), data.size()),
      [&](boost::system::error_code ec, std::size_t) {
        error = ec;
        done = true;
      }
  );

  pump(ioc, stop, timeout_ms, [&] { socket.cancel(); }).run([&] { return done; });

  if (error) {
    throw std::runtime_error("Proxy write failed: " + error.message());
  }
}

void socks_read(tcp::socket& socket, char* data, std::size_t size,
                asio::io_context& ioc, std::stop_token stop, int timeout_ms) {
  boost::system::error_code error;
  bool done = false;

  asio::async_read(
      socket, asio::buffer(data, size),
      [&](boost::system::error_code ec, std::size_t) {
        error = ec;
        done = true;
      }
  );

  pump(ioc, stop, timeout_ms, [&] { socket.cancel(); }).run([&] { return done; });

  if (error) {
    throw std::runtime_error("Proxy read failed: " + error.message());
  }
}

void socks5_handshake(tcp::socket& socket, const parsed_url& target,
                      const Proxy& proxy, asio::io_context& ioc,
                      std::stop_token stop, int timeout_ms) {
  std::string greeting;
  greeting.push_back(0x05);

  if (proxy.has_credentials()) {
    greeting.push_back(0x02);
    greeting.push_back(0x00);
    greeting.push_back(0x02);
  } else {
    greeting.push_back(0x01);
    greeting.push_back(0x00);
  }

  socks_write(socket, greeting, ioc, stop, timeout_ms);

  char reply[2]{};
  socks_read(socket, reply, sizeof(reply), ioc, stop, timeout_ms);

  if (reply[0] != 0x05) {
    throw std::runtime_error("Invalid SOCKS5 proxy response");
  }

  if (reply[1] == 0x02) {
    if (!proxy.has_credentials()) {
      throw std::runtime_error("The SOCKS5 proxy requires credentials");
    }

    std::string auth;
    auth.push_back(0x01);
    auth.push_back(static_cast<char>(proxy.user.size()));
    auth += proxy.user;
    auth.push_back(static_cast<char>(proxy.password.size()));
    auth += proxy.password;

    socks_write(socket, auth, ioc, stop, timeout_ms);

    char auth_reply[2]{};
    socks_read(socket, auth_reply, sizeof(auth_reply), ioc, stop, timeout_ms);

    if (auth_reply[1] != 0x00) {
      throw std::runtime_error("SOCKS5 proxy authentication failed");
    }
  } else if (reply[1] != 0x00) {
    throw std::runtime_error("The SOCKS5 proxy offered no acceptable auth method");
  }

  // CONNECT to the target. The host is sent as a domain name (ATYP 3); that also
  // works for an IP literal and keeps the DNS lookup at the proxy.
  std::string request;
  request.push_back(0x05);
  request.push_back(0x01);
  request.push_back(0x00);
  request.push_back(0x03);
  request.push_back(static_cast<char>(target.host.size()));
  request += target.host;
  request.push_back(static_cast<char>((std::stoi(target.port) >> 8) & 0xFF));
  request.push_back(static_cast<char>(std::stoi(target.port) & 0xFF));

  socks_write(socket, request, ioc, stop, timeout_ms);

  char head[4]{};
  socks_read(socket, head, sizeof(head), ioc, stop, timeout_ms);

  if (head[1] != 0x00) {
    throw std::runtime_error(
        "SOCKS5 proxy could not reach " + target.host + ":" + target.port +
        " (code " + std::to_string(head[1]) + ")"
    );
  }

  std::size_t address_size = 0;

  switch (head[3]) {
    case 0x01:
      address_size = 4;
      break;
    case 0x04:
      address_size = 16;
      break;
    case 0x03: {
      char length = 0;
      socks_read(socket, &length, 1, ioc, stop, timeout_ms);
      address_size = static_cast<unsigned char>(length);
      break;
    }
    default:
      throw std::runtime_error("Invalid SOCKS5 proxy address type");
  }

  std::string rest(address_size + 2, '\0');
  socks_read(socket, rest.data(), rest.size(), ioc, stop, timeout_ms);
}

void http_connect_handshake(tcp::socket& socket, const parsed_url& target,
                            const Proxy& proxy, asio::io_context& ioc,
                            std::stop_token stop, int timeout_ms) {
  const std::string authority = target.host + ":" + target.port;

  std::string request = "CONNECT " + authority + " HTTP/1.1\r\n";
  request += "Host: " + authority + "\r\n";

  if (proxy.has_credentials()) {
    request += "Proxy-Authorization: Basic " +
        base64_encode(proxy.user + ":" + proxy.password) + "\r\n";
  }

  request += "\r\n";

  socks_write(socket, request, ioc, stop, timeout_ms);

  std::string response;
  boost::system::error_code error;
  bool done = false;

  asio::async_read_until(
      socket, asio::dynamic_buffer(response), "\r\n\r\n",
      [&](boost::system::error_code ec, std::size_t) {
        error = ec;
        done = true;
      }
  );

  pump(ioc, stop, timeout_ms, [&] { socket.cancel(); }).run([&] { return done; });

  if (error) {
    throw std::runtime_error("Proxy CONNECT failed: " + error.message());
  }

  if (response.find(" 200 ") == std::string::npos) {
    const std::size_t line_end = response.find("\r\n");
    const std::string status = response.substr(
        0, line_end == std::string::npos ? response.size() : line_end
    );
    throw std::runtime_error("Proxy refused the tunnel: " + status);
  }
}

beast::tcp_stream connect_stream(
    asio::io_context& ioc, const parsed_url& target, const Proxy* proxy,
    std::stop_token stop, int timeout_ms
) {
  const std::string connect_host = proxy != nullptr ? proxy->host : target.host;
  const std::string connect_port =
      proxy != nullptr ? std::to_string(proxy->port) : target.port;

  tcp::resolver resolver(ioc);
  std::vector<tcp::endpoint> endpoints;
  boost::system::error_code error;
  bool done = false;

  resolver.async_resolve(
      connect_host, connect_port,
      [&](boost::system::error_code ec, tcp::resolver::results_type results) {
        error = ec;

        if (!error) {
          endpoints.assign(results.begin(), results.end());
        }

        done = true;
      }
  );

  pump(ioc, stop, timeout_ms, [&] { resolver.cancel(); })
      .run([&] { return done; });

  if (error || endpoints.empty()) {
    throw std::runtime_error(
        "Cannot resolve " + connect_host + ": " + error.message()
    );
  }

  beast::tcp_stream stream(ioc);
  done = false;

  stream.expires_after(std::chrono::milliseconds(timeout_ms));
  stream.async_connect(
      endpoints,
      [&](boost::system::error_code ec, const tcp::endpoint&) {
        error = ec;
        done = true;
      }
  );

  pump(ioc, stop, timeout_ms, [&] { stream.socket().cancel(); })
      .run([&] { return done; });

  if (error) {
    throw std::runtime_error(
        "Cannot connect to " + connect_host + ":" + connect_port + ": " +
        error.message()
    );
  }

  // `expires_after` above armed Beast's internal timer for the connect
  // operation, and Beast does not disarm it once that operation finishes. The
  // expiry is an absolute deadline, so leaving it armed would abort the first
  // read or write that happens after connect+timeout — even when the caller
  // passed a much longer read window. Every later phase is already bounded by
  // `pump`, which also observes cancellation, so disable the Beast timer now.
  stream.expires_never();

  if (proxy != nullptr) {
    if (proxy->type == Proxy::Type::Socks5) {
      socks5_handshake(stream.socket(), target, *proxy, ioc, stop, timeout_ms);
    } else if (target.tls()) {
      http_connect_handshake(stream.socket(), target, *proxy, ioc, stop, timeout_ms);
    }
  }

  return stream;
}

http::verb verb_for(std::string_view method) {
  if (method == "GET") return http::verb::get;
  if (method == "HEAD") return http::verb::head;
  if (method == "PUT") return http::verb::put;
  if (method == "DELETE") return http::verb::delete_;
  if (method == "PATCH") return http::verb::patch;
  return http::verb::post;
}

template<typename Stream>
int do_request(
    Stream& stream, const parsed_url& target, std::string_view method,
    const std::vector<HttpHeader>& headers, const std::string& body,
    bool absolute_form, std::stop_token stop, int write_timeout_ms,
    int read_timeout_ms, const ResponseHandler& on_response,
    const BodyHandler& on_body, asio::io_context& ioc
) {
  http::request<http::string_body> request;
  request.method(verb_for(method));
  request.target(absolute_form ? target.absolute_target() : target.target);
  request.version(11);
  request.set(http::field::host, target.host_header());

  for (const HttpHeader& header: headers) {
    request.set(header.name, header.value);
  }

  request.body() = body;
  request.prepare_payload();

  boost::system::error_code error;
  bool done = false;

  http::async_write(
      stream, request,
      [&](boost::system::error_code ec, std::size_t) {
        error = ec;
        done = true;
      }
  );

  pump(ioc, stop, write_timeout_ms, cancel_for(stream)).run([&] { return done; });

  if (error) {
    throw std::runtime_error("HTTP write failed: " + error.message());
  }

  http::response_parser<http::buffer_body> parser;
  parser.body_limit(boost::none);
  beast::flat_buffer buffer;

  done = false;
  error.clear();

  http::async_read_header(
      stream, buffer, parser,
      [&](boost::system::error_code ec, std::size_t) {
        error = ec;
        done = true;
      }
  );

  pump(ioc, stop, read_timeout_ms, cancel_for(stream)).run([&] { return done; });

  if (error) {
    throw std::runtime_error("HTTP read failed: " + error.message());
  }

  const int status = parser.get().result_int();

  std::vector<HttpHeader> response_headers;

  for (const auto& field: parser.get()) {
    response_headers.push_back(
        {std::string(field.name_string()), std::string(field.value())}
    );
  }

  if (on_response && !on_response(status, response_headers)) {
    return status;
  }

  std::array<char, 16384> chunk{};

  while (!parser.is_done()) {
    parser.get().body().data = chunk.data();
    parser.get().body().size = chunk.size();

    done = false;
    error.clear();

    http::async_read_some(
        stream, buffer, parser,
        [&](boost::system::error_code ec, std::size_t) {
          error = ec;
          done = true;
        }
    );

    pump(ioc, stop, read_timeout_ms, cancel_for(stream)).run([&] { return done; });

    const std::size_t produced = chunk.size() - parser.get().body().size;

    if (error && error != http::error::need_buffer) {
      if (error == asio::error::eof) {
        break;
      }

      throw std::runtime_error("HTTP read failed: " + error.message());
    }

    if (produced > 0 && on_body &&
        !on_body(std::string_view(chunk.data(), produced))) {
      break;
    }

    if (parser.is_done()) {
      break;
    }
  }

  return status;
}

} // namespace

int http_request(
    std::string_view method, const std::string& url,
    const std::optional<std::string>& proxy_spec,
    const std::vector<HttpHeader>& headers, const std::string& body,
    std::stop_token stop, const ResponseHandler& on_response,
    const BodyHandler& on_body, int inactivity_timeout_ms, int max_redirects,
    int read_timeout_ms
) {
  std::optional<Proxy> proxy;

  if (proxy_spec.has_value() && !proxy_spec->empty()) {
    proxy = parse_proxy(*proxy_spec);
  }

  // A zero read window means the caller wants the transport window for both
  // phases, preserving the previous single-timeout behaviour.
  const int response_timeout_ms =
      read_timeout_ms > 0 ? read_timeout_ms : inactivity_timeout_ms;

  std::string current = url;

  for (int redirect = 0;; ++redirect) {
    const parsed_url target = parse_url(current);

    asio::io_context ioc;
    asio::ssl::context ssl_context(asio::ssl::context::tls_client);
    ssl_context.set_default_verify_paths();
    ssl_context.set_verify_mode(asio::ssl::verify_peer);

    beast::tcp_stream plain = connect_stream(
        ioc, target, proxy.has_value() ? &*proxy : nullptr, stop,
        inactivity_timeout_ms
    );

    // An http URL through an HTTP proxy is sent in absolute form on the proxy
    // connection; every other combination is tunnelled to the origin first.
    const bool absolute_form = proxy.has_value() &&
        proxy->type == Proxy::Type::Http && !target.tls();

    int status = 0;
    std::string location;

    const ResponseHandler response_handler =
        [&](int code, const std::vector<HttpHeader>& response_headers) {
          status = code;

          for (const HttpHeader& header: response_headers) {
            if (equals_ignore_case(header.name, "Location")) {
              location = header.value;
            }
          }

          return on_response ? on_response(code, response_headers) : true;
        };

    if (target.tls()) {
      beast::ssl_stream<beast::tcp_stream> tls(std::move(plain), ssl_context);

      if (SSL_set_tlsext_host_name(tls.native_handle(), target.host.c_str()) == 0) {
        throw std::runtime_error("TLS SNI setup failed");
      }

      boost::system::error_code error;
      bool done = false;

      tls.async_handshake(
          asio::ssl::stream_base::client,
          [&](boost::system::error_code ec) {
            error = ec;
            done = true;
          }
      );

      pump(ioc, stop, inactivity_timeout_ms, cancel_for(tls))
          .run([&] { return done; });

      if (error) {
        throw std::runtime_error("TLS handshake failed: " + error.message());
      }

      status = do_request(
          tls, target, method, headers, body, false, stop,
          inactivity_timeout_ms, response_timeout_ms, response_handler, on_body,
          ioc
      );
    } else {
      status = do_request(
          plain, target, method, headers, body, absolute_form, stop,
          inactivity_timeout_ms, response_timeout_ms, response_handler, on_body,
          ioc
      );
    }

    if (max_redirects > 0 && redirect < max_redirects && status >= 300 &&
        status < 400 && !location.empty()) {
      current = resolve_location(current, location);
      continue;
    }

    return status;
  }
}

std::string http_get_body(
    const std::string& url, const std::optional<std::string>& proxy, int max_bytes,
    std::stop_token stop, const std::vector<HttpHeader>& extra_headers
) {
  std::vector<HttpHeader> headers{
      {"User-Agent", "Mozilla/5.0 (compatible; local-notebook/1.0)"},
      {"Accept", "text/html,application/xhtml+xml,text/plain"},
  };

  // A caller-supplied header replaces the default of the same name rather than
  // producing a duplicate (which some servers treat as malformed).
  for (const HttpHeader& extra: extra_headers) {
    bool replaced = false;

    for (HttpHeader& header: headers) {
      if (equals_ignore_case(header.name, extra.name)) {
        header = extra;
        replaced = true;
        break;
      }
    }

    if (!replaced) {
      headers.push_back(extra);
    }
  }

  std::string body;
  bool keep = false;
  const std::size_t limit = max_bytes > 0 ? static_cast<std::size_t>(max_bytes) : 0;

  const ResponseHandler on_response =
      [&](int status, const std::vector<HttpHeader>&) {
        keep = status >= 200 && status < 300;
        return true;
      };

  const BodyHandler on_body = [&](std::string_view chunk) {
    if (keep && body.size() < limit) {
      body.append(chunk.substr(0, limit - body.size()));
    }

    return true;
  };

  const int status = http_request(
      "GET", url, proxy, headers, "", stop, on_response, on_body, 20'000, 5
  );

  if (status < 200 || status >= 300) {
    throw std::runtime_error("HTTP request failed (HTTP " + std::to_string(status) + ")");
  }

  return body;
}

} // namespace lnb::net
