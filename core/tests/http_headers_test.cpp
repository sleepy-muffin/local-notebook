// Regression test for the extra-header support in http_get_body.
//
// The web-search tools must present a browser User-Agent, because DuckDuckGo
// serves its bot-challenge page to the default `local-notebook` agent. This test
// pins the merge rule: a caller-supplied header replaces the default of the same
// name (no duplicate), and unrelated defaults survive.
#include "net/http.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
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

void expect_contains(
    const std::string& text, const std::string& needle, const std::string& label
) {
  if (text.find(needle) != std::string::npos) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << " (missing \"" << needle << "\")\n"
            << "--- request ---\n"
            << text << "---------------\n";
}

// Accepts a fixed number of connections, records each request head, and answers
// "ok". The requests are captured before the response is written, so the caller
// can inspect them once http_get_body returns.
class RecordingServer {
public:
  explicit RecordingServer(int expected) : expected_(expected),
                                           acceptor_(ioc_, {tcp::v4(), 0}) {
    acceptor_.non_blocking(true);
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] { serve(); });
  }

  ~RecordingServer() {
    finished_ = true;
    acceptor_.close();

    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] unsigned short port() const { return port_; }

  [[nodiscard]] std::string request(int index) const {
    const std::lock_guard lock(mutex_);
    return index < static_cast<int>(requests_.size()) ? requests_[index]
                                                      : std::string{};
  }

private:
  void serve() {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int served = 0;

    while (!finished_ && served < expected_ &&
           std::chrono::steady_clock::now() < deadline) {
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

      {
        const std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
      }

      const std::string response =
          "HTTP/1.1 200 OK\r\n"
          "Content-Length: 2\r\n"
          "Connection: close\r\n"
          "\r\n"
          "ok";

      (void)boost::asio::write(socket, boost::asio::buffer(response), error);
      (void)socket.shutdown(tcp::socket::shutdown_both, error);
      ++served;
    }
  }

  int expected_ = 0;
  boost::asio::io_context ioc_;
  tcp::acceptor acceptor_;
  unsigned short port_ = 0;
  std::atomic<bool> finished_{false};
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
  std::thread thread_;
};

} // namespace

int main() {
  RecordingServer server(2);
  const std::string url =
      "http://127.0.0.1:" + std::to_string(server.port()) + "/search";

  // 1. No extra headers: the default agent is used.
  const std::string first = lnb::net::http_get_body(url, std::nullopt, 4096);
  expect(first == "ok", "default request returns the body");
  expect_contains(
      server.request(0), "User-Agent: Mozilla/5.0 (compatible; local-notebook/1.0)",
      "default User-Agent is sent"
  );

  // 2. A browser agent replaces the default instead of adding a second one.
  const std::string second = lnb::net::http_get_body(
      url, std::nullopt, 4096, std::stop_token{},
      {{"User-Agent", "BrowserTest/1.0"}, {"X-Test", "yes"}}
  );
  expect(second == "ok", "override request returns the body");

  const std::string request = server.request(1);
  expect_contains(request, "User-Agent: BrowserTest/1.0", "override is sent");
  expect_contains(request, "X-Test: yes", "extra header is sent");
  expect(
      request.find("local-notebook/1.0") == std::string::npos,
      "default User-Agent is replaced, not duplicated"
  );

  if (failures != 0) {
    std::cerr << failures << " header check(s) failed\n";
    return 1;
  }

  std::cout << "http header checks passed\n";
  return 0;
}
