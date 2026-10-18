// Regression test for the HTTP transport's io_context handling.
//
// `io_context::run_for()` leaves the context stopped once it runs out of work.
// The request code reuses one io_context for every step (resolve, connect,
// handshake, write, header read, body reads); without a restart, every step
// after the first returned immediately and the pump busy-spun at 100% CPU until
// its deadline, reporting a bogus timeout even though the server answered.
//
// This drives a real generation against a loopback SSE server, which exercises
// the whole chain of pumps.
#include "local-notebook-core/interface.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

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

bool wait_terminal(
    const std::shared_ptr<lnb::GenerationTask>& task, std::chrono::seconds limit
) {
  const auto deadline = std::chrono::steady_clock::now() + limit;

  while (task != nullptr && !task->is_terminal() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }

  return task != nullptr && task->is_terminal();
}

// A one-shot HTTP server that answers the first request with a small SSE
// completion. It polls a non-blocking acceptor so the test can stop it even
// when the client never manages to connect.
class SseServer {
public:
  SseServer() : acceptor_(ioc_, {tcp::v4(), 0}) {
    acceptor_.non_blocking(true);
    port_ = acceptor_.local_endpoint().port();

    thread_ = std::thread([this] { serve(); });
  }

  ~SseServer() {
    finished_ = true;

    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] unsigned short port() const { return port_; }

private:
  void serve() {
    const auto deadline = std::chrono::steady_clock::now() + 15s;

    while (!finished_ && std::chrono::steady_clock::now() < deadline) {
      tcp::socket socket(ioc_);
      boost::system::error_code error;
      (void)acceptor_.accept(socket, error);

      if (error) {
        error.clear();
        std::this_thread::sleep_for(1ms);
        continue;
      }

      respond(socket);
      return;
    }
  }

  static void respond(tcp::socket& socket) {
    boost::system::error_code error;
    std::string request;

    (void)boost::asio::read_until(
        socket, boost::asio::dynamic_buffer(request), "\r\n\r\n", error
    );

    if (error) {
      return;
    }

    // Drain the request body so closing the socket does not reset the
    // connection before the response is read.
    const std::size_t header_end = request.find("\r\n\r\n") + 4;
    std::size_t body_left = 0;
    const std::string field = "Content-Length:";

    if (const std::size_t at = request.find(field); at != std::string::npos) {
      body_left = static_cast<std::size_t>(
          std::stoul(request.substr(at + field.size()))
      );
    }

    const std::size_t already = request.size() - header_end;
    body_left = body_left > already ? body_left - already : 0;

    if (body_left > 0) {
      std::string body(body_left, '\0');
      (void)boost::asio::read(socket, boost::asio::buffer(body), error);
    }

    const std::string events =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},"
        "\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(events.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + events;

    (void)boost::asio::write(socket, boost::asio::buffer(response), error);
    (void)socket.shutdown(tcp::socket::shutdown_both, error);
  }

  boost::asio::io_context ioc_;
  tcp::acceptor acceptor_;
  unsigned short port_ = 0;
  std::atomic<bool> finished_{false};
  std::thread thread_;
};

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "lnb-http-generation-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  lnb::api::set_data_root(root);

  SseServer server;

  lnb::api::interface api;

  lnb::Assistant assistant;
  assistant.name_ = "test";
  assistant.api_url_ =
      "http://127.0.0.1:" + std::to_string(server.port()) + "/v1";
  assistant.authentication_token_ = "key";
  assistant.model_ = "test-model";
  assistant.protocol_ = lnb::net::ApiProtocol::ChatCompletions;
  api.open_assistant("http-generation-test", assistant);

  lnb::Session session;
  session.title_ = "Generation";
  const std::uint32_t session_id = api.create_session(session);
  expect(session_id != 0, "create_session failed");

  expect(
      api.session_prompt(session_id, "hi") == lnb::api::error_code::SUCCESS,
      "session_prompt failed"
  );

  const auto task = api.start_generation();
  const bool finished = wait_terminal(task, 5s);

  if (!finished) {
    api.cancel_generation();
  }

  api.wait_for_generation();

  expect(finished, "generation did not finish within 5s");
  expect(
      task != nullptr &&
          task->snapshot().status == lnb::GenerationTask::Status::COMPLETED,
      "generation did not complete"
  );

  const std::vector<lnb::Message> messages = api.messages(session_id);
  expect(messages.size() == 2, "transcript should hold the prompt and the reply");
  expect(
      !messages.empty() && messages.back().content_ == "Hello world",
      "assistant reply was not stored"
  );

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "http generation: all expectations passed\n";
  return 0;
}
