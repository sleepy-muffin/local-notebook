#include "store/database.hpp"

#include <algorithm>

namespace lnb::store {

namespace {

// design-1.md §9. The schema is versioned via PRAGMA user_version so future
// migrations have a starting point; this is a fresh start, so there is no
// migration from the legacy database. Version 2 added `session.pinned`, version
// 3 `session.summary_keep_tokens` and version 4 `session.reasoning_effort`; an
// older file gains each column in the constructor.
constexpr std::string_view SCHEMA = R"sql(
PRAGMA user_version = 4;

CREATE TABLE IF NOT EXISTS session (
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

CREATE TABLE IF NOT EXISTS message (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL REFERENCES session(id) ON DELETE CASCADE,
  role       INTEGER NOT NULL,
  content    TEXT    NOT NULL,
  made_at    TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_message_session ON message(session_id, id);
CREATE INDEX IF NOT EXISTS idx_message_made_at ON message(made_at);
)sql";

constexpr std::string_view SESSION_COLUMNS =
  "id, title, custom_instructions, memory, memory_watermark, "
  "assistant_memory_watermark, shared_directory, pinned, summary_keep_tokens, "
  "summary_trigger_tokens, summary_token_budget, summary_reasoning_headroom_tokens, "
  "max_tool_rounds, max_tool_calls, tool_output_max_tokens, response_token_budget, "
  "reasoning_effort";

constexpr std::string_view MESSAGE_COLUMNS = "id, session_id, role, content, made_at";

Session read_session(const statement& stmt) {
  Session session;
  session.id_ = stmt.column_uint32(0);
  session.title_ = stmt.column_text(1);
  session.custom_instructions_ = stmt.column_text(2);
  session.memory_ = stmt.column_text(3);
  session.memory_watermark_ = stmt.column_uint32(4);
  session.assistant_memory_watermark_ = stmt.column_uint32(5);
  session.shared_directory_ = stmt.column_text_opt(6);
  session.pinned_ = stmt.column_int(7) != 0;
  session.summary_keep_tokens_ = static_cast<std::size_t>(stmt.column_int(8));
  session.summary_trigger_tokens_ = static_cast<std::size_t>(stmt.column_int(9));
  session.summary_token_budget_ = static_cast<std::size_t>(stmt.column_int(10));
  session.summary_reasoning_headroom_tokens_ = static_cast<std::size_t>(stmt.column_int(11));
  session.max_tool_rounds_ = static_cast<std::size_t>(stmt.column_int(12));
  session.max_tool_calls_ = static_cast<std::size_t>(stmt.column_int(13));
  session.tool_output_max_tokens_ = static_cast<std::size_t>(stmt.column_int(14));
  session.response_token_budget_ = static_cast<std::size_t>(stmt.column_int(15));
  session.reasoning_effort_ = stmt.column_text(16);
  return session;
}

Message read_message(const statement& stmt) {
  Message message;
  message.id_ = stmt.column_uint32(0);
  message.session_id_ = stmt.column_uint32(1);
  message.role_ = static_cast<Role>(stmt.column_int(2));
  message.content_ = stmt.column_text(3);
  message.made_at_ = stmt.column_text(4);
  return message;
}

// True when `table` already has a column named `column`. Used to migrate an
// older database forward: CREATE TABLE IF NOT EXISTS does not add columns to an
// existing table, so a version-1 file needs an explicit ALTER TABLE.
bool has_column(const connection& conn, std::string_view table, std::string_view column) {
  auto stmt = conn.prepare("PRAGMA table_info(" + std::string(table) + ")");

  while (stmt.step()) {
    // table_info columns: cid, name, type, notnull, dflt_value, pk.
    if (stmt.column_text(1) == column) {
      return true;
    }
  }

  return false;
}

} // namespace

database::database(std::string_view path) : connection_(path) {
  connection_.exec(SCHEMA);

  // A database created before pinning or the keep window existed gains the
  // columns here. A fresh one (created by SCHEMA above) already has them, so the
  // ALTERs are skipped.
  if (!has_column(connection_, "session", "pinned")) {
    connection_.exec("ALTER TABLE session ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0");
  }

  if (!has_column(connection_, "session", "summary_keep_tokens")) {
    connection_.exec(
      "ALTER TABLE session ADD COLUMN summary_keep_tokens INTEGER NOT NULL DEFAULT 4000"
    );
  }

  if (!has_column(connection_, "session", "reasoning_effort")) {
    connection_.exec(
      "ALTER TABLE session ADD COLUMN reasoning_effort TEXT NOT NULL DEFAULT ''"
    );
  }
}

std::uint32_t database::insert_session(const Session& session) {
  auto stmt = connection_.prepare(
    "INSERT INTO session (title, custom_instructions, memory, memory_watermark, "
    "assistant_memory_watermark, shared_directory, pinned, summary_keep_tokens, "
    "summary_trigger_tokens, summary_token_budget, summary_reasoning_headroom_tokens, "
    "max_tool_rounds, max_tool_calls, tool_output_max_tokens, response_token_budget, "
    "reasoning_effort) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
  );
  stmt.bind(
    session.title_, session.custom_instructions_, session.memory_,
    session.memory_watermark_, session.assistant_memory_watermark_,
    session.shared_directory_, session.pinned_ ? 1 : 0,
    static_cast<int>(session.summary_keep_tokens_),
    static_cast<int>(session.summary_trigger_tokens_),
    static_cast<int>(session.summary_token_budget_),
    static_cast<int>(session.summary_reasoning_headroom_tokens_),
    static_cast<int>(session.max_tool_rounds_),
    static_cast<int>(session.max_tool_calls_),
    static_cast<int>(session.tool_output_max_tokens_),
    static_cast<int>(session.response_token_budget_),
    session.reasoning_effort_
  );
  return stmt.execute_with_last_row_id();
}

void database::update_session(const Session& session) {
  auto stmt = connection_.prepare(
    "UPDATE session SET title = ?, custom_instructions = ?, memory = ?, "
    "memory_watermark = ?, assistant_memory_watermark = ?, shared_directory = ?, "
    "pinned = ?, summary_keep_tokens = ?, summary_trigger_tokens = ?, "
    "summary_token_budget = ?, summary_reasoning_headroom_tokens = ?, "
    "max_tool_rounds = ?, max_tool_calls = ?, tool_output_max_tokens = ?, "
    "response_token_budget = ?, reasoning_effort = ? "
    "WHERE id = ?"
  );
  stmt.bind(
    session.title_, session.custom_instructions_, session.memory_,
    session.memory_watermark_, session.assistant_memory_watermark_,
    session.shared_directory_, session.pinned_ ? 1 : 0,
    static_cast<int>(session.summary_keep_tokens_),
    static_cast<int>(session.summary_trigger_tokens_),
    static_cast<int>(session.summary_token_budget_),
    static_cast<int>(session.summary_reasoning_headroom_tokens_),
    static_cast<int>(session.max_tool_rounds_),
    static_cast<int>(session.max_tool_calls_),
    static_cast<int>(session.tool_output_max_tokens_),
    static_cast<int>(session.response_token_budget_), session.reasoning_effort_,
    session.id_
  );
  stmt.execute();
}

void database::remove_session(std::uint32_t session_id) {
  auto stmt = connection_.prepare("DELETE FROM session WHERE id = ?");
  stmt.bind(session_id);
  stmt.execute();
}

std::optional<Session> database::session(std::uint32_t session_id) const {
  auto stmt = connection_.prepare(
    "SELECT " + std::string(SESSION_COLUMNS) + " FROM session WHERE id = ?"
  );
  stmt.bind(session_id);
  if (stmt.step()) {
    return read_session(stmt);
  }
  return std::nullopt;
}

std::vector<Session> database::sessions_by_recent(
  std::optional<std::size_t> limit
) const {
  std::string sql =
    "SELECT " + std::string(SESSION_COLUMNS) +
    ", (SELECT MAX(made_at) FROM message WHERE message.session_id = session.id) AS last_at "
    "FROM session ORDER BY pinned DESC, last_at IS NULL, last_at DESC, id DESC";
  if (limit) {
    sql += " LIMIT ?";
  }

  auto stmt = connection_.prepare(sql);
  if (limit) {
    stmt.bind(static_cast<int>(*limit));
  }

  std::vector<Session> sessions;
  while (stmt.step()) {
    sessions.push_back(read_session(stmt));
  }
  return sessions;
}

void database::set_session_memory(
  std::uint32_t session_id, const std::string& memory, std::uint32_t watermark
) {
  auto stmt = connection_.prepare(
    "UPDATE session SET memory = ?, memory_watermark = ? WHERE id = ?"
  );
  stmt.bind(memory, watermark, session_id);
  stmt.execute();
}

void database::set_assistant_memory_watermark(
  std::uint32_t session_id, std::uint32_t watermark
) {
  auto stmt = connection_.prepare(
    "UPDATE session SET assistant_memory_watermark = ? WHERE id = ?"
  );
  stmt.bind(watermark, session_id);
  stmt.execute();
}

void database::set_session_reasoning_effort(
  std::uint32_t session_id, const std::string& reasoning_effort
) {
  auto stmt = connection_.prepare(
    "UPDATE session SET reasoning_effort = ? WHERE id = ?"
  );
  stmt.bind(reasoning_effort, session_id);
  stmt.execute();
}

std::uint32_t database::insert_message(const Message& message) {
  auto stmt = connection_.prepare(
    "INSERT INTO message (session_id, role, content, made_at) VALUES (?, ?, ?, ?)"
  );
  stmt.bind(
    message.session_id_, static_cast<int>(message.role_), message.content_,
    message.made_at_
  );
  return stmt.execute_with_last_row_id();
}

void database::update_message(std::uint32_t message_id, const std::string& content) {
  auto stmt = connection_.prepare("UPDATE message SET content = ? WHERE id = ?");
  stmt.bind(content, message_id);
  stmt.execute();
}

bool database::remove_message(std::uint32_t message_id) {
  auto stmt = connection_.prepare("DELETE FROM message WHERE id = ?");
  stmt.bind(message_id);
  stmt.execute();
  return stmt.changes() > 0;
}

std::vector<Message> database::messages(std::uint32_t session_id) const {
  auto stmt = connection_.prepare(
    "SELECT " + std::string(MESSAGE_COLUMNS) +
    " FROM message WHERE session_id = ? ORDER BY id ASC"
  );
  stmt.bind(session_id);

  std::vector<Message> messages;
  while (stmt.step()) {
    messages.push_back(read_message(stmt));
  }
  return messages;
}

std::vector<Message> database::recent_messages(
  std::uint32_t session_id, std::size_t count
) const {
  auto stmt = connection_.prepare(
    "SELECT " + std::string(MESSAGE_COLUMNS) +
    " FROM message WHERE session_id = ? ORDER BY id DESC LIMIT ?"
  );
  stmt.bind(session_id, static_cast<int>(count));

  std::vector<Message> messages;
  while (stmt.step()) {
    messages.push_back(read_message(stmt));
  }
  std::reverse(messages.begin(), messages.end());
  return messages;
}

std::size_t database::message_count(std::uint32_t session_id) const {
  auto stmt = connection_.prepare(
    "SELECT COUNT(*) FROM message WHERE session_id = ?"
  );
  stmt.bind(session_id);
  if (stmt.step()) {
    return static_cast<std::size_t>(stmt.column_int(0));
  }
  return 0;
}

std::optional<std::string> database::last_message_time(
  std::uint32_t session_id
) const {
  auto stmt = connection_.prepare(
    "SELECT MAX(made_at) FROM message WHERE session_id = ?"
  );
  stmt.bind(session_id);
  if (stmt.step()) {
    return stmt.column_text_opt(0);
  }
  return std::nullopt;
}

std::unordered_map<std::uint32_t, std::string> database::session_last_times() const {
  auto stmt = connection_.prepare(
    "SELECT session_id, MAX(made_at) FROM message GROUP BY session_id"
  );

  std::unordered_map<std::uint32_t, std::string> times;
  while (stmt.step()) {
    const std::optional<std::string> made_at = stmt.column_text_opt(1);
    if (made_at.has_value()) {
      times.emplace(stmt.column_uint32(0), *made_at);
    }
  }
  return times;
}

} // namespace lnb::store
