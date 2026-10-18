// Regression test for the Beast connect timer leaking into later operations.
//
// `beast::tcp_stream::expires_after()` arms an absolute deadline. The transport
// used it to bound connecting, but did not disarm it afterwards, so every read
// that happened more than the connect window after connecting failed with
// beast::error::timeout — even when the caller asked for a much longer read
// window. This test connects with a short inactivity window and then makes the
// server stay silent for longer than that window before answering.
#include "net/http.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, const std::string& label) {
  if (condition) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << '\n';
}

// Accepts one connection, waits longer than the client's connect window, then
// answers. The delay is what exposes a connect deadline that was never cleared.
class DelayedServer {
public:
  DelayedServer() : acceptor_(ioc_, {tcp::v4(), 0}) {
    acceptor_.non_blocking(true);
    port_ = acceptor_.local_endpoint().port();

    thread_ = std::thread([this] { serve(); });
  }

  ~DelayedServer() {
    finished_ = true;
    acceptor_.close();

    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] unsigned short port() const { return port_; }

private:
  void serve() {
    const auto deadline = std::chrono::steady_clock::now() + 5s;

    while (!finished_ && std::chrono::steady_clock::now() < deadline) {
      tcp::socket socket(ioc_);
      boost::system::error_code error;
      (void)acceptor_.accept(socket, error);

      if (error) {
        error.clear();
        std::this_thread::sleep_for(1ms);
        continue;
      }

      std::string request;
      (void)boost::asio::read_until(
          socket, boost::asio::dynamic_buffer(request), "\r\n\r\n", error
      );

      if (error) {
        return;
      }

      std::this_thread::sleep_for(400ms);

      const std::string response =
          "HTTP/1.1 200 OK\r\n"
          "Content-Length: 2\r\n"
          "Connection: close\r\n"
          "\r\n"
          "ok";

      (void)boost::asio::write(socket, boost::asio::buffer(response), error);
      (void)socket.shutdown(tcp::socket::shutdown_both, error);
      return;
    }
  }

  boost::asio::io_context ioc_;
  tcp::acceptor acceptor_;
  unsigned short port_ = 0;
  std::atomic<bool> finished_{false};
  std::thread thread_;
};

} // namespace

int main() {
  DelayedServer server;

  const std::string url =
      "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/chat";

  std::string body;
  bool response_seen = false;

  const int status = lnb::net::http_request(
      "GET", url, std::nullopt, {}, "", std::stop_token{},
      [&](int, const std::vector<lnb::net::HttpHeader>&) {
        response_seen = true;
        return true;
      },
      [&](std::string_view chunk) {
        body.append(chunk.data(), chunk.size());
        return true;
      },
      200, 0, 3000
  );

  expect(status == 200, "response status should be 200");
  expect(response_seen, "response headers should have been received");
  expect(body == "ok", "response body should have been read");

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "http timeout: all expectations passed\n";
  return 0;
}
