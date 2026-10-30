// Regression test for completions that stop at the response limit.
//
// `send_message_once` must not hand back a usable result when the provider
// reports `finish_reason == "length"`. A chat turn therefore fails and persists
// nothing, and a forced session summary fails without advancing the summary
// watermark.
#include "local-notebook-core/interface.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
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

// A one-shot HTTP server that answers with a completion cut off at the response
// limit. The stream is otherwise well-formed, so only the finish reason marks it
// as truncated.
class TruncatingServer {
public:
  TruncatingServer() : acceptor_(ioc_, {tcp::v4(), 0}) {
    acceptor_.non_blocking(true);
    port_ = acceptor_.local_endpoint().port();

    thread_ = std::thread([this] { serve(); });
  }

  ~TruncatingServer() {
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
        "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},"
        "\"finish_reason\":\"length\"}]}\n\n"
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

std::string endpoint(const TruncatingServer& server) {
  return "http://127.0.0.1:" + std::to_string(server.port()) + "/v1";
}

lnb::Assistant assistant_for(const TruncatingServer& server) {
  lnb::Assistant assistant;
  assistant.name_ = "truncation";
  assistant.api_url_ = endpoint(server);
  assistant.authentication_token_ = "key";
  assistant.model_ = "test-model";
  assistant.protocol_ = lnb::net::ApiProtocol::ChatCompletions;
  return assistant;
}

lnb::GenerationTask::Snapshot snapshot_of(
    const std::shared_ptr<lnb::GenerationTask>& task
) {
  return task != nullptr ? task->snapshot() : lnb::GenerationTask::Snapshot{};
}

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "lnb-length-truncation-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  lnb::api::set_data_root(root);

  lnb::api::interface api;

  // 1. A chat turn cut off at the limit is a failure, and its streamed text is
  //    discarded rather than persisted as the assistant reply.
  {
    TruncatingServer server;
    expect(
        api.open_assistant("length-chat", assistant_for(server)),
        "open_assistant failed for the chat case"
    );

    lnb::Session session;
    session.title_ = "Truncated chat";
    const std::uint32_t session_id = api.create_session(session);
    expect(session_id != 0, "create_session failed for the chat case");

    expect(
        api.session_prompt(session_id, "hi") == lnb::api::error_code::SUCCESS,
        "session_prompt failed for the chat case"
    );

    const auto task = api.start_generation(session_id);
    const bool finished = wait_terminal(task, 5s);

    if (!finished) {
      api.cancel_session(session_id);
    }

    api.wait_for_generation();

    const lnb::GenerationTask::Snapshot snapshot = snapshot_of(task);
    expect(finished, "the truncated chat did not finish");
    expect(
        snapshot.status == lnb::GenerationTask::Status::FAILED,
        "a truncated chat turn must fail"
    );
    expect(
        snapshot.display.empty(),
        "the streamed text of a truncated chat must be cleared"
    );
    expect(
        snapshot.error.find("cut off") != std::string::npos,
        "the chat error must explain that the reply was cut off"
    );
    expect(
        api.messages(session_id).size() == 1,
        "a truncated chat must not persist an assistant reply"
    );
  }

  // 2. A forced session summary cut off at the limit fails and leaves both the
  //    stored summary and its watermark untouched.
  {
    TruncatingServer server;
    expect(
        api.open_assistant("length-summary", assistant_for(server)),
        "open_assistant failed for the summary case"
    );

    lnb::Session session;
    session.title_ = "Truncated summary";
    // Keep only the newest message verbatim, and make every message a candidate
    // for the first batch.
    session.summary_keep_tokens_ = 1;
    session.summary_trigger_tokens_ = 1;
    session.summary_token_budget_ = 32;

    const std::uint32_t session_id = api.create_session(session);
    expect(session_id != 0, "create_session failed for the summary case");

    api.append_message(session_id, lnb::Role::User, "alpha");
    api.append_message(session_id, lnb::Role::Assistant, "beta");
    api.append_message(session_id, lnb::Role::User, "gamma");

    const auto task = api.summarize_session(session_id, true);
    const bool finished = wait_terminal(task, 5s);

    if (!finished) {
      api.cancel_session(session_id);
    }

    api.wait_for_generation();

    const lnb::GenerationTask::Snapshot snapshot = snapshot_of(task);
    expect(finished, "the truncated summary did not finish");
    expect(
        snapshot.status == lnb::GenerationTask::Status::FAILED,
        "a truncated summary must fail"
    );
    expect(
        snapshot.error.find("cut off") != std::string::npos,
        "the summary error must explain that the reply was cut off"
    );
    expect(
        snapshot.output_budget == session.summary_token_budget_ &&
            snapshot.output_budget_kind ==
                lnb::GenerationTask::BudgetKind::SUMMARIZATION,
        "a standalone summary must publish the summarization budget"
    );

    const std::optional<lnb::Session> stored = api.session(session_id);
    expect(stored.has_value(), "the summarized session disappeared");
    expect(
        stored.has_value() && stored->summary_.empty(),
        "a truncated summary must not store a summary"
    );
    expect(
        stored.has_value() && stored->summary_watermark_ == 0,
        "a truncated summary must not advance the watermark"
    );
    expect(
        api.messages(session_id).size() == 3,
        "a truncated summary must not alter the transcript"
    );
  }

  std::filesystem::remove_all(root, ignored);

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "length truncation: all expectations passed\n";
  return 0;
}
