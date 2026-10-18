// Regression test for the UI send path.
//
// The send button does exactly what ConversationController::send() does:
// interface::session_prompt() persists the user turn, then
// interface::start_generation() starts the worker. This drives that path against
// a loopback SSE server, so a regression in either half is caught without a
// display server.
#include "ui/ConversationController.hpp"

#include "local-notebook-core/interface.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
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

    const std::string first =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},"
        "\"finish_reason\":null}]}\n\n";
    const std::string rest =
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    const std::string events = first + rest;

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(events.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + first;

    (void)boost::asio::write(socket, boost::asio::buffer(response), error);

    // Hold the rest back briefly so the controller's poll has a chance to see
    // the turn while it is still RUNNING; an instant completion would never
    // exercise the streaming callback.
    std::this_thread::sleep_for(150ms);

    (void)boost::asio::write(socket, boost::asio::buffer(rest), error);
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
      std::filesystem::temp_directory_path() / "lnb-controller-test";
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
  api.open_assistant("controller-test", assistant);

  lnb::Session session;
  session.title_ = "Controller";
  const std::uint32_t session_id = api.create_session(session);
  expect(session_id != 0, "create_session failed");

  ui::ConversationController controller(api, session_id);

  int changed = 0;
  std::string streamed;
  std::string error;

  controller.set_on_changed([&changed] { ++changed; });
  controller.set_on_stream([&streamed](const std::string& display) {
    streamed = display;
  });
  controller.set_on_error([&error](const std::string& message) {
    error = message;
  });

  // An empty composer must not persist anything or start a turn.
  expect(!controller.send(""), "empty send should be refused");
  expect(api.messages(session_id).empty(), "empty send must not store a message");
  expect(controller.task() == nullptr, "empty send must not start a task");

  // The real send path: persist the prompt first, then start the worker.
  expect(controller.send("hi"), "send should be accepted");
  expect(controller.task() != nullptr, "send should start a task");

  // session_prompt() persists synchronously, so the user turn is visible before
  // the worker has produced anything.
  {
    const std::vector<lnb::Message> stored = api.messages(session_id);
    expect(stored.size() == 1, "the prompt should be stored before the turn");
    expect(
        !stored.empty() && stored.back().content_ == "hi",
        "the stored prompt should be the composer text"
    );
  }

  const auto deadline = std::chrono::steady_clock::now() + 5s;

  while (controller.busy() && std::chrono::steady_clock::now() < deadline) {
    controller.poll();
    std::this_thread::sleep_for(10ms);
  }

  if (controller.busy()) {
    controller.cancel();
  }

  api.wait_for_generation();

  // Deliver the terminal snapshot to the controller's callbacks.
  controller.poll();

  expect(!controller.busy(), "the turn should be finished");
  expect(
      controller.has_finished_task(), "the finished task should be retained"
  );
  expect(
      controller.last_snapshot().status ==
          lnb::GenerationTask::Status::COMPLETED,
      "the turn should complete"
  );
  expect(error.empty(), "the turn should not report an error");
  // The controller forwards text only while the turn is RUNNING, so the final
  // chunk may arrive after the terminal snapshot. A non-empty prefix proves the
  // streaming callback ran; the full reply is asserted from the transcript.
  expect(!streamed.empty(), "streaming should reach the view");
  expect(
      streamed == "Hello" || streamed == "Hello world",
      "streamed text should be a prefix of the reply"
  );

  {
    const std::vector<lnb::Message> stored = api.messages(session_id);
    expect(stored.size() == 2, "the transcript should hold the prompt and reply");
    expect(
        !stored.empty() && stored.back().content_ == "Hello world",
        "the assistant reply should be stored"
    );
  }

  // One on_changed for the persisted prompt, one when the worker finishes.
  expect(changed >= 2, "the view should be told to reload after the turn");

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "conversation controller: all expectations passed\n";
  return 0;
}
