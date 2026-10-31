// Regression test for message coverage across both summarisers.
//
// Every stored message must eventually be consumed by the summarisation
// pipeline: either folded into the session summary or gathered into the
// assistant memory (raw, or through a session summary that carries it). A
// message that a watermark moves past without either is abandoned: it can never
// reach a summary or the memory again. The old plan windows did exactly that,
// because they kept only the newest suffix and the caller then advanced the
// watermark to the newest message.
//
// This drives the real fold passes through the public interface against a
// loopback server. Every pseudo message carries a unique marker, and the test
// reads the markers back out of the requests that were actually sent. Consuming
// a message twice is a warning; skipping one is an error.
#include "local_notebook/core/interface.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

int failures = 0;
int warnings = 0;

void fail(const std::string& message) {
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
}

void warn(const std::string& message) {
  ++warnings;
  std::cout << "WARN: " << message << '\n';
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

// Records every request body and answers with a fixed SSE completion, so the
// test can see exactly which messages the core sent to each summariser.
class RecordingServer {
public:
  RecordingServer() : acceptor_(ioc_, {tcp::v4(), 0}) {
    acceptor_.non_blocking(true);
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] { serve(); });
  }

  ~RecordingServer() {
    stopped_ = true;

    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] unsigned short port() const { return port_; }

  [[nodiscard]] std::vector<std::string> bodies() const {
    std::scoped_lock lock(mutex_);
    return bodies_;
  }

private:
  void serve() {
    const auto deadline = std::chrono::steady_clock::now() + 60s;

    while (!stopped_ && std::chrono::steady_clock::now() < deadline) {
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

  void respond(tcp::socket& socket) {
    boost::system::error_code error;
    std::string request;

    (void)boost::asio::read_until(
        socket, boost::asio::dynamic_buffer(request), "\r\n\r\n", error
    );

    if (error) {
      return;
    }

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
      request += body;
    }

    // Recorded before the reply, so a finished task cannot hide its request.
    {
      std::scoped_lock lock(mutex_);
      bodies_.push_back(request);
    }

    const std::string events =
        "data: {\"choices\":[{\"delta\":{\"content\":\"updated\"},"
        "\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" summary\"},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":11,"
        "\"completion_tokens\":2,\"total_tokens\":13}}\n\n"
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
  std::atomic<bool> stopped_{false};
  mutable std::mutex mutex_;
  std::vector<std::string> bodies_;
  std::thread thread_;
};

std::string marker_for(std::size_t index) {
  return "<<M" + std::to_string(index) + ">>";
}

// ASCII only, so the cost is stable: (chars + 3) / 4 + 16. With the budgets
// below each message costs 38 tokens.
std::string pseudo_message(std::size_t index) {
  return marker_for(index) + std::string(80, 'x');
}

void run_task(
    lnb::api::interface& api, std::uint32_t session_id,
    const std::shared_ptr<lnb::GenerationTask>& task, const std::string& label
) {
  if (task == nullptr) {
    fail(label + ": no task was started");
    return;
  }

  if (!wait_terminal(task, 20s)) {
    api.cancel_session(session_id);
    api.cancel_assistant();
    api.wait_for_generation();
    fail(label + ": task did not finish");
    return;
  }

  api.wait_for_generation();

  if (task->snapshot().status != lnb::GenerationTask::Status::COMPLETED) {
    fail(label + ": task did not complete");
  }
}

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "lnb-message-coverage-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  lnb::api::set_data_root(root);

  RecordingServer server;

  lnb::api::interface api;

  lnb::Assistant assistant;
  assistant.name_ = "test";
  assistant.api_url_ =
      "http://127.0.0.1:" + std::to_string(server.port()) + "/v1";
  assistant.authentication_token_ = "key";
  assistant.model_ = "test-model";
  assistant.protocol_ = lnb::net::ApiProtocol::ChatCompletions;
  // Small budgets keep the run to a handful of messages. The memory trigger
  // doubles as the per-session gather cap.
  assistant.summary_trigger_tokens_ = 200;
  assistant.summary_token_budget_ = 200;
  assistant.summary_token_budget_for_session_summarization_ = 16'000;

  if (!api.open_assistant("message-coverage-test", assistant)) {
    std::cerr << "FAIL: open_assistant failed\n";
    return 1;
  }

  lnb::Session session;
  session.title_ = "Coverage";
  session.summary_trigger_tokens_ = 100;
  session.summary_keep_tokens_ = 20;
  session.summary_token_budget_ = 200;

  const std::uint32_t session_id = api.create_session(session);

  if (session_id == 0) {
    std::cerr << "FAIL: create_session failed\n";
    return 1;
  }

  // Phase 1: one message at a time. The passes keep up, so this exercises the
  // steady state where every fold covers exactly the oldest pending message.
  constexpr std::size_t kSteadyMessages = 8;

  for (std::size_t i = 1; i <= kSteadyMessages; ++i) {
    api.append_message(session_id, lnb::Role::User, pseudo_message(i));

    run_task(
        api, session_id, api.summarize_session(session_id, false),
        "automatic session fold at message " + std::to_string(i)
    );
    run_task(
        api, session_id, api.summarize_assistant(false),
        "automatic memory pass at message " + std::to_string(i)
    );
  }

  // Phase 2: a burst arrives with no pass in between. This is the case the old
  // newest-suffix windows got wrong: the window (and the watermark it advanced
  // to) could start past the oldest pending messages, so a message that was
  // never sent to either summariser was marked consumed. The memory runs first,
  // while more than its trigger is still pending, so a capped newest-window
  // would abandon the oldest one; the session fold then has a large overflow of
  // its own. Both passes must still account for every message.
  constexpr std::size_t kBurstMessages = 7;

  for (std::size_t i = kSteadyMessages + 1;
       i <= kSteadyMessages + kBurstMessages; ++i) {
    api.append_message(session_id, lnb::Role::User, pseudo_message(i));
  }

  run_task(
      api, session_id, api.summarize_assistant(false),
      "burst memory pass (oldest pending first)"
  );
  run_task(
      api, session_id, api.summarize_session(session_id, false),
      "burst session fold (oldest pending first)"
  );

  constexpr std::size_t kMessages = kSteadyMessages + kBurstMessages;

  // Drain anything the triggers deliberately left pending. A forced pass is
  // what "summarise now" runs, so it must be lossless too.
  run_task(
      api, session_id, api.summarize_session(session_id, true),
      "forced session fold"
  );
  run_task(
      api, session_id, api.summarize_assistant(true), "forced memory pass"
  );

  const auto stored = api.session(session_id);
  const std::vector<lnb::Message> messages = api.messages(session_id);

  if (!stored.has_value()) {
    std::cerr << "FAIL: the session is unreadable\n";
    return 1;
  }

  if (messages.size() != kMessages) {
    std::cerr << "FAIL: unexpected message count "
              << messages.size() << '\n';
    return 1;
  }

  // Phase 3: a plain turn shows the verbatim window the core actually sends. The
  // newest messages fit the trigger, and the first older message that would
  // exceed it is the boundary: it must be part of the request too. Leaving it
  // out silently shrinks the context the model sees, even though the message is
  // still stored. Everything is checked through the recorded request body, so no
  // internal helper is called.
  const std::string prompt_marker = "<<PROMPT>>";
  {
    const auto prompt = api.session_prompt(session_id, prompt_marker + "hi");

    if (prompt != lnb::api::error_code::SUCCESS) {
      fail("session_prompt failed for the window check");
    } else {
      run_task(
          api, session_id, api.start_generation(session_id), "plain turn"
      );
    }
  }

  const std::vector<std::string> bodies = server.bodies();

  // The plain turn is the only non-summariser request that carries the prompt.
  bool boundary_seen = false;
  bool beyond_boundary_seen = false;

  for (const std::string& body: bodies) {
    if (body.find("running summary of a long conversation") != std::string::npos ||
        body.find("assistant memory update about the user") != std::string::npos) {
      continue;
    }

    if (body.find(prompt_marker) == std::string::npos) {
      continue;
    }

    // Trigger 100, 38-token messages and a 19-token prompt: the newest three
    // messages plus the prompt cost 133, so message 13 is the boundary (it is
    // kept even though it overflows) and message 12 is not sent at all.
    boundary_seen = boundary_seen ||
                    body.find(marker_for(kMessages - 2)) != std::string::npos;
    beyond_boundary_seen =
        beyond_boundary_seen ||
        body.find(marker_for(kMessages - 3)) != std::string::npos;
  }

  if (!boundary_seen) {
    fail("the request window dropped its boundary message: message " +
         std::to_string(kMessages - 2));
  }

  if (beyond_boundary_seen) {
    fail("the request window exceeded its trigger: message " +
         std::to_string(kMessages - 3));
  }

  std::vector<int> folded(messages.size(), 0);
  std::vector<int> gathered(messages.size(), 0);
  bool compacted_sent = false;

  for (const std::string& body: bodies) {
    const bool memory =
        body.find("assistant memory update about the user") != std::string::npos;
    const bool session_fold =
        body.find("running summary of a long conversation") != std::string::npos;

    if (!memory && !session_fold) {
      continue;
    }

    if (memory &&
        body.find("### Compacted Historical Messages") != std::string::npos) {
      compacted_sent = true;
    }

    for (std::size_t i = 0; i < messages.size(); ++i) {
      if (body.find(marker_for(i + 1)) == std::string::npos) {
        continue;
      }

      if (memory) {
        ++gathered[i];
      } else {
        ++folded[i];
      }
    }
  }

  if (!compacted_sent) {
    fail("the memory request never carried a session summary");
  }

  for (std::size_t i = 0; i < messages.size(); ++i) {
    const std::string label = "message " + std::to_string(i + 1) + " (id " +
                              std::to_string(messages[i].id_) + ")";

    // The watermark is the core's claim about what the summary covers. A
    // message below it that never appeared in a fold request is the original
    // bug: the wallet was moved past it without summarising it.
    if (messages[i].id_ <= stored->summary_watermark_ && folded[i] == 0) {
      fail("the session watermark passed an unfolded message: " + label);
    }

    // The memory watermark is the same claim for the shared memory (raw or via
    // a summary). Nothing below it may be missing from both paths.
    if (messages[i].id_ <= stored->assistant_memory_watermark_ &&
        folded[i] == 0 && gathered[i] == 0) {
      fail("the memory watermark passed an unread message: " + label);
    }

    if (folded[i] > 1) {
      warn("summarised more than once: " + label);
    }

    if (gathered[i] > 1) {
      warn("gathered by the memory more than once: " + label);
    }

    if (folded[i] > 0 && gathered[i] > 0) {
      warn("read raw by the memory and also folded: " + label);
    }
  }

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "message coverage: all expectations passed";
  if (warnings != 0) {
    std::cout << " (" << warnings << " warning(s))";
  }
  std::cout << '\n';
  return 0;
}
