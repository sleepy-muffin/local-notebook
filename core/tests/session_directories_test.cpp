// Regression tests for session shared-directory persistence.
//
// Version 5 stores the list as a JSON array and renames the old single-path
// column. A database written by the previous version must still read correctly.
#include "store/database.hpp"
#include "shark/sqlite3.hpp"

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

void expect_directories(
    const lnb::Session& session, const std::vector<std::string>& expected,
    const std::string& label
) {
  expect(
      session.shared_directories_ == expected,
      label + " (got " + std::to_string(session.shared_directories_.size()) +
          " entries)"
  );
}

void create_legacy_database(const fs::path& path) {
  shark::sqlite::connection connection(path.string());
  connection.exec(R"sql(
CREATE TABLE session (
  id                                INTEGER PRIMARY KEY AUTOINCREMENT,
  title                             TEXT    NOT NULL DEFAULT '',
  custom_instructions               TEXT    NOT NULL DEFAULT '',
  memory                            TEXT    NOT NULL DEFAULT '',
  memory_watermark                  INTEGER NOT NULL DEFAULT 0,
  assistant_memory_watermark        INTEGER NOT NULL DEFAULT 0,
  shared_directory                  TEXT,
  pinned                            INTEGER NOT NULL DEFAULT 0,
  summary_keep_tokens               INTEGER NOT NULL DEFAULT 4000,
  summary_trigger_tokens            INTEGER NOT NULL DEFAULT 16000,
  summary_token_budget              INTEGER NOT NULL DEFAULT 4000,
  summary_reasoning_headroom_tokens INTEGER NOT NULL DEFAULT 12000,
  max_tool_rounds                   INTEGER NOT NULL DEFAULT 8,
  max_tool_calls                    INTEGER NOT NULL DEFAULT 32,
  tool_output_max_tokens            INTEGER NOT NULL DEFAULT 8000,
  response_token_budget             INTEGER NOT NULL DEFAULT 8000,
  reasoning_effort                  TEXT    NOT NULL DEFAULT ''
);

INSERT INTO session (id, title, shared_directory)
VALUES (1, 'legacy', '/old/shared');
)sql");
  connection.exec("PRAGMA user_version = 4");
}

void test_round_trip(const fs::path& path) {
  lnb::store::database db(path.string());

  lnb::Session session;
  session.title_ = "round trip";
  session.shared_directories_ = {"/one", "/two with spaces"};

  const std::uint32_t id = db.insert_session(session);
  const std::optional<lnb::Session> loaded = db.session(id);

  expect(loaded.has_value(), "round trip: session reloads");

  if (loaded.has_value()) {
    expect_directories(*loaded, session.shared_directories_, "round trip: list preserved");
  }

  lnb::Session cleared = loaded.value_or(session);
  cleared.shared_directories_.clear();
  db.update_session(cleared);

  const std::optional<lnb::Session> empty = db.session(id);
  expect(empty.has_value(), "round trip: updated session reloads");

  if (empty.has_value()) {
    expect(empty->shared_directories_.empty(), "round trip: empty list preserved");
  }
}

void test_legacy_migration(const fs::path& path) {
  {
    create_legacy_database(path);
  }

  lnb::store::database db(path.string());
  const std::optional<lnb::Session> session = db.session(1);

  expect(session.has_value(), "migration: legacy session reloads");

  if (session.has_value()) {
    expect_directories(
        *session, {"/old/shared"}, "migration: legacy path becomes one entry"
    );
  }
}

} // namespace

int main() {
  const fs::path base = fs::temp_directory_path() / "lnb-session-directories-test";
  std::error_code error;
  fs::remove_all(base, error);
  fs::create_directories(base);

  test_round_trip(base / "round_trip.db");
  test_legacy_migration(base / "legacy.db");

  fs::remove_all(base, error);

  if (failures != 0) {
    std::cerr << failures << " session directory check(s) failed\n";
    return 1;
  }

  std::cout << "session directory checks passed\n";
  return 0;
}
