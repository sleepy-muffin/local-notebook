// Regression test for the per-session task slots.
//
// A turn in one session must not block a turn in another. Assistant-memory
// updates are exclusive and are rejected while any session task is active.
#include "local_notebook/core/interface.hpp"

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

void expect_code(
    const std::expected<void, lnb::core::error_code>& actual,
    lnb::core::error_code expected, const std::string& label
) {
  expect(
      expected == lnb::core::error_code::SUCCESS
          ? actual.has_value()
          : !actual.has_value() && actual.error() == expected,
      label
  );
}

lnb::core::SummaryApprover approve() {
  return [](const lnb::core::SummaryPlan&) { return true; };
}

bool wait_terminal(
    const std::shared_ptr<lnb::core::GenerationTask>& task,
    std::chrono::seconds limit = std::chrono::seconds(10)
) {
  const auto deadline = std::chrono::steady_clock::now() + limit;

  while (task != nullptr && !task->is_terminal() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }

  return task != nullptr && task->is_terminal();
}

// Answers every request with a small SSE completion, holding the tail back so a
// turn stays RUNNING long enough for the test to observe the other slots.
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
    const auto deadline = std::chrono::steady_clock::now() + 20s;

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

    // Drain the request body so closing does not reset the connection first.
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

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(first.size() + rest.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + first;

    (void)boost::asio::write(socket, boost::asio::buffer(response), error);

    // Keep the turn in flight while the test checks the other task slots.
    std::this_thread::sleep_for(200ms);

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
      std::filesystem::temp_directory_path() / "lnb-concurrent-sessions-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  SseServer server;

  lnb::core::interface api(root);

  lnb::core::Assistant assistant;
  assistant.name_ = "test";
  assistant.api_url_ =
      "http://127.0.0.1:" + std::to_string(server.port()) + "/v1";
  assistant.authentication_token_ = "key";
  assistant.model_ = "test-model";
  assistant.protocol_ = lnb::core::ApiProtocol::ChatCompletions;
  api.get_store().save_assistant("concurrent-test", assistant);
  expect(api.open_assistant("concurrent-test").has_value(), "open_assistant failed");

  lnb::core::Session settings;
  settings.title_ = "Session";

  const std::uint32_t first = api.create_session(settings);
  const std::uint32_t second = api.create_session(settings);
  const std::uint32_t third = api.create_session(settings);
  expect(first != 0 && second != 0 && third != 0, "create_session failed");

  // 1. Two sessions start turns back to back. The second must not be refused or
  //    cancel the first.
  expect_code(
      api.session_prompt(first, "first"),
      lnb::core::error_code::SUCCESS, "first prompt"
  );
  const auto first_task_result = api.start_generation(first);
  expect(first_task_result.has_value(), "the first session's turn should start");
  if (!first_task_result) return 1;
  const auto first_task = *first_task_result;

  expect_code(
      api.session_prompt(second, "second"),
      lnb::core::error_code::SUCCESS, "a second session must not be blocked"
  );
  const auto second_task_result = api.start_generation(second);
  expect(second_task_result.has_value(), "the second session's turn should start");
  if (!second_task_result) return 1;
  const auto second_task = *second_task_result;

  expect(api.session_busy(first), "the first session should be busy");
  expect(api.session_busy(second), "the second session should be busy");

  // 2. Assistant memory cannot be updated while session tasks are active.
  const auto rejected_fold = api.summarize_assistant(true, approve());
  expect(
      !rejected_fold && rejected_fold.error() == lnb::core::error_code::ERR_ASSISTANT_OR_ANY_SESSION_BUSY,
      "assistant memory update should be rejected while session turns run"
  );
  expect(wait_terminal(first_task), "the first turn did not finish");
  expect(wait_terminal(second_task), "the second turn did not finish");

  // 3. Once session tasks finish, the assistant fold owns all work and rejects
  //    new session prompts.
  const auto fold_result = api.summarize_assistant(true, approve());
  expect(fold_result.has_value(), "the assistant fold should start when idle");
  if (!fold_result) return 1;
  const auto fold = *fold_result;
  expect(api.assistant_busy(), "the assistant fold should be busy");
  expect_code(
      api.session_prompt(third, "third"),
      lnb::core::error_code::ERR_ASSISTANT_BUSY,
      "a new prompt must be refused while assistant memory is being updated"
  );
  expect(wait_terminal(fold), "the assistant fold did not finish");

  expect(
      first_task->snapshot().status == lnb::core::GenerationTask::Status::COMPLETED,
      "the first turn should complete"
  );
  expect(
      second_task->snapshot().status == lnb::core::GenerationTask::Status::COMPLETED,
      "the second turn should complete"
  );
  expect(
      fold->snapshot().status == lnb::core::GenerationTask::Status::COMPLETED,
      "the assistant fold should complete"
  );
  expect(!api.assistant_busy(), "the assistant fold should be finished");

  // 3. Once the fold is gone, a new prompt is accepted again.
  expect_code(
      api.session_prompt(second, "after the fold"),
      lnb::core::error_code::SUCCESS,
      "prompts must be accepted after the assistant fold finishes"
  );

  // Both session transcripts hold their own prompt and reply.
  const std::vector<lnb::core::Message> first_messages = api.messages(first);
  const std::vector<lnb::core::Message> second_messages = api.messages(second);

  expect(
      first_messages.size() == 2, "the first transcript should hold prompt + reply"
  );
  expect(
      second_messages.size() == 3,
      "the second transcript should hold prompt + reply + the later prompt"
  );

  api.wait_for_generation();

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "concurrent sessions: all expectations passed\n";
  return 0;
}
