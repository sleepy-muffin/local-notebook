// Regression tests for branching, cascade deletion and versioned summaries.
//
// A summary is stored as its own row paired to the message it was folded up to
// (its watermark). Branching copies the session's prefix into a new session and
// remaps the summary and assistant-memory watermarks onto the copied message ids.
// Deleting a message removes it and every later message of the session, and
// revokes the summary versions built on the removed tail so an older version
// becomes active again. Reopening the database must not resurrect the revoked
// versions from the legacy session columns.
#include "store/database.hpp"
#include "store/managers.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int failures = 0;

void expect(bool condition, const std::string& label) {
  if (condition) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << '\n';
}

void test_branch_and_cascade(const fs::path& path) {
  lnb::core::database db(path.string());
  lnb::core::SessionManager sessions(db);

  lnb::core::Session session;
  session.title_ = "original";
  session.custom_instructions_ = "be concise";
  session.reasoning_effort_ = "high";
  session.summary_trigger_tokens_ = 1234;
  session.include_in_assistant_memory_ = false;

  const std::uint32_t session_id = sessions.create(session);
  expect(session_id != 0, "create_session failed");

  auto messages = sessions.get_messages(session_id);
  expect(messages.has_value(), "get_messages failed");

  if (!messages.has_value()) {
    return;
  }

  const std::uint32_t first = messages->append(lnb::core::Role::User, "one");
  const std::uint32_t second = messages->append(lnb::core::Role::Assistant, "two");
  const std::uint32_t third = messages->append(lnb::core::Role::User, "three");

  db.set_session_summary(session_id, "summary one", first);
  db.set_session_summary(session_id, "summary two", second);

  {
    const auto stored = db.session(session_id);
    expect(stored.has_value(), "the session reloads");
    expect(
        stored.has_value() && stored->summary_ == "summary two" &&
            stored->summary_watermark_ == second,
        "the newest version is the active summary"
    );
    expect(db.summaries(session_id).size() == 2, "both versions are stored");
  }

  // A branch copies the prefix up to `second`, so the copy must carry the two
  // summaries of that prefix and remap their watermarks onto new message ids.
  db.set_assistant_memory_watermark(session_id, first);

  const std::uint32_t fork_id = sessions.branch(session_id, second);
  expect(fork_id != 0, "branch failed");
  expect(fork_id != session_id, "branch created its own session");

  {
    const auto fork = db.session(fork_id);
    expect(fork.has_value(), "the branch reloads");

    if (fork.has_value()) {
      expect(fork->title_ == "original", "branch keeps the title");
      expect(
          fork->custom_instructions_ == "be concise",
          "branch keeps the custom instructions"
      );
      expect(fork->reasoning_effort_ == "high", "branch keeps the reasoning level");
      expect(
          fork->summary_trigger_tokens_ == 1234, "branch keeps the summary trigger"
      );
      expect(
          !fork->include_in_assistant_memory_,
          "branch keeps the assistant-memory membership"
      );
      expect(!fork->pinned_, "branch is not pinned");
    }

    const auto copied = db.messages(fork_id);
    expect(copied.size() == 2, "branch copies only the prefix");
    expect(
        copied.size() == 2 && copied[0].content_ == "one" &&
            copied[1].content_ == "two",
        "branch copies the prefix in order"
    );
    expect(
        copied.size() == 2 && copied[0].id_ != first && copied[1].id_ != second,
        "branch renumbers the copied messages"
    );

    expect(db.summaries(fork_id).size() == 2, "branch carries the prefix summaries");

    if (fork.has_value() && copied.size() == 2) {
      expect(
          fork->summary_ == "summary two", "the branch's newest version is active"
      );
      expect(
          fork->summary_watermark_ == copied[1].id_,
          "the branch remaps the summary watermark"
      );
      expect(
          fork->assistant_memory_watermark_ == copied[0].id_,
          "the branch remaps the assistant-memory watermark"
      );
    }
  }

  // Deleting `second` takes the tail with it and revokes the version folded up to
  // it, so the older version becomes active again.
  expect(sessions.remove_message(second), "remove_message reports a removal");

  {
    const auto stored = db.session(session_id);
    expect(stored.has_value(), "the trimmed session reloads");

    if (stored.has_value()) {
      expect(stored->summary_ == "summary one", "a revoked version falls back to the older one");
      expect(
          stored->summary_watermark_ == first, "the older version's watermark is active"
      );
    }

    const auto remaining = db.messages(session_id);
    expect(remaining.size() == 1, "the tail was removed with the message");
    expect(
        remaining.size() == 1 && remaining[0].id_ == first,
        "the earlier message survives"
    );
    expect(db.summaries(session_id).size() == 1, "the revoked version is gone");
  }

  // Deleting the remaining watermarked message revokes the last version too.
  expect(sessions.remove_message(first), "remove_message removes the first message");
  expect(db.messages(session_id).empty(), "the session is empty");

  {
    const auto stored = db.session(session_id);
    expect(
        stored.has_value() && stored->summary_.empty() && stored->summary_watermark_ == 0,
        "revoking the last version leaves no active summary"
    );
    expect(db.summaries(session_id).empty(), "no versions remain");
  }

  expect(!sessions.remove_message(999999), "an unknown message id is refused");

  // Reopening runs the legacy-column migration again. It must not copy a revoked
  // summary back in, and it must not duplicate the branch's versions either.
  {
    lnb::core::database reopened(path.string());
    expect(reopened.summaries(session_id).empty(), "reopening keeps revoked versions revoked");
    expect(reopened.summaries(fork_id).size() == 2, "reopening keeps the branch versions");
  }
}

} // namespace

int main() {
  const fs::path base = fs::temp_directory_path() / "lnb-branch-summary-test";
  std::error_code error;
  fs::remove_all(base, error);
  fs::create_directories(base);

  test_branch_and_cascade(base / "branch.db");

  fs::remove_all(base, error);

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "branch and summary versioning: all expectations passed\n";
  return 0;
}
