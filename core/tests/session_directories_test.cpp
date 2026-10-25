// Regression tests for session shared-directory and tool-permission persistence.
//
// Version 5 stores the directory list as a JSON array and renames the old
// single-path column; version 6 adds the tool permissions. A database written by
// an older version must still read correctly, with writing off and the other
// permissions on.
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

void expect_permissions(
    const lnb::Session& session, bool tools, bool read, bool write, bool online,
    const std::string& label
) {
  expect(
      session.tools_enabled_ == tools &&
          session.read_tools_enabled_ == read &&
          session.write_tools_enabled_ == write &&
          session.online_tools_enabled_ == online,
      label + " (got " + std::to_string(session.tools_enabled_) + ", " +
          std::to_string(session.read_tools_enabled_) + ", " +
          std::to_string(session.write_tools_enabled_) + ", " +
          std::to_string(session.online_tools_enabled_) + ")"
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
  session.tools_enabled_ = false;
  session.read_tools_enabled_ = false;
  session.write_tools_enabled_ = true;
  session.online_tools_enabled_ = false;
  session.input_token_produced_midway_ = 12345;

  const std::uint32_t id = db.insert_session(session);
  const std::optional<lnb::Session> loaded = db.session(id);

  expect(loaded.has_value(), "round trip: session reloads");

  if (loaded.has_value()) {
    expect_directories(*loaded, session.shared_directories_, "round trip: list preserved");
    expect_permissions(
        *loaded, false, false, true, false, "round trip: permissions preserved"
    );
    expect(
        loaded->input_token_produced_midway_ == 12345,
        "round trip: midway input budget preserved"
    );
  }

  lnb::Session cleared = loaded.value_or(session);
  cleared.shared_directories_.clear();
  cleared.tools_enabled_ = true;
  cleared.read_tools_enabled_ = true;
  cleared.write_tools_enabled_ = false;
  cleared.online_tools_enabled_ = true;
  db.update_session(cleared);

  const std::optional<lnb::Session> empty = db.session(id);
  expect(empty.has_value(), "round trip: updated session reloads");

  if (empty.has_value()) {
    expect(empty->shared_directories_.empty(), "round trip: empty list preserved");
    expect_permissions(
        *empty, true, true, false, true, "round trip: updated permissions preserved"
    );
  }
}

bool table_has_column(const fs::path& path, const std::string& column) {
  shark::sqlite::connection connection(path.string());
  auto stmt = connection.prepare("PRAGMA table_info(session)");

  while (stmt.step()) {
    // table_info columns: cid, name, type, notnull, dflt_value, pk.
    if (stmt.column_text(1) == column) {
      return true;
    }
  }

  return false;
}

void test_legacy_migration(const fs::path& path) {
  {
    create_legacy_database(path);
  }

  {
    lnb::store::database db(path.string());
    const std::optional<lnb::Session> session = db.session(1);

    expect(session.has_value(), "migration: legacy session reloads");

    if (session.has_value()) {
      expect_directories(
          *session, {"/old/shared"}, "migration: legacy path becomes one entry"
      );
      expect_permissions(
          *session, true, true, false, true,
          "migration: older sessions read and browse but cannot write"
      );
      // Version 8 renamed the per-result tool clamp into a cumulative per-turn
      // budget and scaled the stored default (8000) to match it.
      expect(
          session->input_token_produced_midway_ == 32000,
          "migration: old tool output cap becomes the cumulative midway budget"
      );
      expect(
          session->response_token_budget_ == 8000,
          "migration: the stored response budget is preserved"
      );
    }
  }

  // The per-session loop guards moved into the core, so their columns are gone.
  expect(
      !table_has_column(path, "max_tool_rounds"),
      "migration: max_tool_rounds column was dropped"
  );
  expect(
      !table_has_column(path, "max_tool_calls"),
      "migration: max_tool_calls column was dropped"
  );
  expect(
      !table_has_column(path, "tool_output_max_tokens"),
      "migration: old tool output column was renamed"
  );
  expect(
      table_has_column(path, "input_token_produced_midway"),
      "migration: the midway input budget column exists"
  );
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
