// Regression test: an assistant-level summarisation must always leave a
// diagnostic trace, even when it sends nothing or the send fails. Before this
// was fixed the task completed with an empty log, which the Tasks window renders
// as "This task produced no log.".
#include "local_notebook/core/interface.hpp"

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
    const std::shared_ptr<lnb::core::GenerationTask>& task,
    lnb::core::GenerationTask::Status expected, const std::string& label
) {
  expect(task != nullptr, label + ": task is null");

  if (task != nullptr) {
    expect(task->snapshot().status == expected, label + ": unexpected status");
  }
}

bool wait_terminal(
    const std::shared_ptr<lnb::core::GenerationTask>& task,
    std::chrono::seconds limit = std::chrono::seconds(10)
) {
  const auto deadline = std::chrono::steady_clock::now() + limit;

  while (task != nullptr && !task->is_terminal() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  return task != nullptr && task->is_terminal();
}

lnb::core::SummaryApprover approve() {
  return [](const lnb::core::SummaryPlan&) { return true; };
}

lnb::core::SummaryApprover decline() {
  return [](const lnb::core::SummaryPlan&) { return false; };
}

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "lnb-assistant-summary-log-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  lnb::core::interface api(root);

  lnb::core::Assistant assistant;
  assistant.name_ = "test";
  assistant.api_url_ = "http://127.0.0.1:9/v1";
  assistant.authentication_token_ = "key";
  assistant.model_ = "test-model";
  api.get_store().save_assistant("log-test", assistant);
  if (!api.open_assistant("log-test")) {
    std::cerr << "FAIL: open_assistant failed\n";
    return 1;
  }

  // 1. Nothing to merge: the pass still completes, and the log says why.
  {
    const auto result = api.summarize_assistant(true, approve());
    expect(result.has_value(), "no-sessions: summarize_assistant rejected");
    const auto task = result ? *result : nullptr;
    expect(wait_terminal(task), "no-sessions: task did not finish");
    expect_status(task, lnb::core::GenerationTask::Status::COMPLETED, "no-sessions");

    const std::string log = task != nullptr ? task->log_text() : std::string{};
    expect(!log.empty(), "no-sessions: log is empty");
    expect_contains(log, "[assistant-summary] starting", "no-sessions");
    expect_contains(log, "nothing to merge", "no-sessions");
  }

  lnb::core::Session session;
  session.title_ = "Session";
  const std::uint32_t session_id = api.create_session(session);
  expect(session_id != 0, "create_session failed");
  expect(
      api.session_prompt(session_id, "hello").has_value(),
      "session_prompt failed"
  );

  // 2. Automatic pass with an unreached trigger: completed with an explanation.
  {
    const auto result = api.summarize_assistant(false, approve());
    expect(result.has_value(), "not-reached: summarize_assistant rejected");
    const auto task = result ? *result : nullptr;
    expect(wait_terminal(task), "not-reached: task did not finish");
    expect_status(task, lnb::core::GenerationTask::Status::COMPLETED, "not-reached");

    // The worker publishes the memory budget before it decides anything, so a
    // pass that sends nothing still shows the limit it would have been charged
    // against.
    if (task != nullptr) {
      const lnb::core::GenerationTask::Snapshot snapshot = task->snapshot();
      expect(
          snapshot.output_budget == assistant.summary_token_budget_ &&
              snapshot.output_budget_kind ==
                  lnb::core::GenerationTask::BudgetKind::MEMORY,
          "not-reached: the memory budget was not published"
      );
    }

    const std::string log = task != nullptr ? task->log_text() : std::string{};
    expect(!log.empty(), "not-reached: log is empty");
    expect_contains(log, "automatic trigger not reached", "not-reached");
  }

  // 3. A declined review sends nothing but is recorded in the log.
  {
    const auto result = api.summarize_assistant(true, decline());
    expect(result.has_value(), "declined: summarize_assistant rejected");
    const auto task = result ? *result : nullptr;
    expect(wait_terminal(task), "declined: task did not finish");
    expect_status(task, lnb::core::GenerationTask::Status::COMPLETED, "declined");

    const std::string log = task != nullptr ? task->log_text() : std::string{};
    expect(!log.empty(), "declined: log is empty");
    expect_contains(log, "review declined", "declined");
  }

  // 4. A failed send keeps the exact request body in the log.
  {
    assistant.api_url_ = "ftp://example.com/v1";
    api.set_assistant(assistant);

    const auto result = api.summarize_assistant(true, approve());
    expect(result.has_value(), "send-failure: summarize_assistant rejected");
    const auto task = result ? *result : nullptr;
    expect(wait_terminal(task), "send-failure: task did not finish");
    expect_status(task, lnb::core::GenerationTask::Status::FAILED, "send-failure");

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
