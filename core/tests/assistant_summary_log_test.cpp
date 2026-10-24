// Regression test: an assistant-level summarisation must always leave a
// diagnostic trace, even when it sends nothing or the send fails. Before this
// was fixed the task completed with an empty log, which the Tasks window renders
// as "This task produced no log.".
#include "local-notebook-core/interface.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
  if (condition) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << '\n';
}

void expect_contains(
    const std::string& log, const std::string& needle, const std::string& label
) {
  if (log.find(needle) != std::string::npos) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << " (missing \"" << needle << "\")\n"
            << "--- log ---\n"
            << log << "-----------\n";
}

void expect_status(
    const std::shared_ptr<lnb::GenerationTask>& task,
    lnb::GenerationTask::Status expected, const std::string& label
) {
  expect(task != nullptr, label + ": task is null");

  if (task != nullptr) {
    expect(task->snapshot().status == expected, label + ": unexpected status");
  }
}

bool wait_terminal(
    const std::shared_ptr<lnb::GenerationTask>& task,
    std::chrono::seconds limit = std::chrono::seconds(10)
) {
  const auto deadline = std::chrono::steady_clock::now() + limit;

  while (task != nullptr && !task->is_terminal() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  return task != nullptr && task->is_terminal();
}

lnb::SummaryApprover approve() {
  return [](const lnb::SummaryPlan&) { return true; };
}

lnb::SummaryApprover decline() {
  return [](const lnb::SummaryPlan&) { return false; };
}

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "lnb-assistant-summary-log-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  lnb::api::set_data_root(root);

  lnb::api::interface api;

  lnb::Assistant assistant;
  assistant.name_ = "test";
  assistant.api_url_ = "http://127.0.0.1:9/v1";
  assistant.authentication_token_ = "key";
  assistant.model_ = "test-model";
  api.open_assistant("log-test", assistant);

  // 1. Nothing to merge: the pass still completes, and the log says why.
  {
    const auto task = api.summarize_assistant(true, approve());
    expect(wait_terminal(task), "no-sessions: task did not finish");
    expect_status(task, lnb::GenerationTask::Status::COMPLETED, "no-sessions");

    const std::string log = task != nullptr ? task->log_text() : std::string{};
    expect(!log.empty(), "no-sessions: log is empty");
    expect_contains(log, "[assistant-summary] starting", "no-sessions");
    expect_contains(log, "nothing to merge", "no-sessions");
  }

  lnb::Session session;
  session.title_ = "Session";
  const std::uint32_t session_id = api.create_session(session);
  expect(session_id != 0, "create_session failed");
  expect(
      api.session_prompt(session_id, "hello") == lnb::api::error_code::SUCCESS,
      "session_prompt failed"
  );

  // 2. Automatic pass with an unreached trigger: completed with an explanation.
  {
    const auto task = api.summarize_assistant(false, approve());
    expect(wait_terminal(task), "not-reached: task did not finish");
    expect_status(task, lnb::GenerationTask::Status::COMPLETED, "not-reached");

    const std::string log = task != nullptr ? task->log_text() : std::string{};
    expect(!log.empty(), "not-reached: log is empty");
    expect_contains(log, "automatic trigger not reached", "not-reached");
  }

  // 3. A declined review sends nothing but is recorded in the log.
  {
    const auto task = api.summarize_assistant(true, decline());
    expect(wait_terminal(task), "declined: task did not finish");
    expect_status(task, lnb::GenerationTask::Status::COMPLETED, "declined");

    const std::string log = task != nullptr ? task->log_text() : std::string{};
    expect(!log.empty(), "declined: log is empty");
    expect_contains(log, "review declined", "declined");
  }

  // 4. A failed send keeps the exact request body in the log.
  {
    assistant.api_url_ = "ftp://example.com/v1";
    api.set_assistant(assistant);

    const auto task = api.summarize_assistant(true, approve());
    expect(wait_terminal(task), "send-failure: task did not finish");
    expect_status(task, lnb::GenerationTask::Status::FAILED, "send-failure");

    const std::string log = task != nullptr ? task->log_text() : std::string{};
    expect(!log.empty(), "send-failure: log is empty");
    expect_contains(log, "[request assistant-summary]", "send-failure");
    expect_contains(log, "request failed", "send-failure");
  }

  api.wait_for_generation();

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "assistant summary log: all expectations passed\n";
  return 0;
}
