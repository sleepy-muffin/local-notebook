// Headless tests for the M1 store layer (design-1.md). Dependency-free: a tiny
// CHECK macro and a main() that returns non-zero on the first failure.
//
// Run via CTest (`ctest --test-dir build/linux-x64-debug`) or directly.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "context/context.hpp"
#include "model.hpp"
#include "store/database.hpp"
#include "store/managers.hpp"
#include "store/util.hpp"
#include "support/log.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

std::filesystem::path make_temp_db_path() {
  const std::filesystem::path directory =
    std::filesystem::temp_directory_path() / "lnb-store-tests";
  std::filesystem::create_directories(directory);
  return directory / (lnb::store::make_uuid() + ".db");
}

void remove_temp_db(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  std::filesystem::remove(path.string() + "-wal", error);
  std::filesystem::remove(path.string() + "-shm", error);
}

void test_util() {
  const std::string uuid = lnb::store::make_uuid();
  CHECK(uuid.size() == 36);
  CHECK(uuid[8] == '-' && uuid[13] == '-' && uuid[18] == '-' && uuid[23] == '-');
  CHECK(uuid != lnb::store::make_uuid());

  const std::string now = lnb::store::now_iso8601_utc();
  CHECK(now.size() == 20);
  CHECK(now[4] == '-' && now[7] == '-');
  CHECK(now[10] == 'T');
  CHECK(now[19] == 'Z');

  CHECK(lnb::store::paths::data_root() == std::filesystem::path("data"));
  CHECK(
    lnb::store::paths::assistant_dir("abc") ==
    std::filesystem::path("data") / "abc"
  );
  CHECK(
    lnb::store::paths::assistant_db("abc") ==
    std::filesystem::path("data") / "abc" / "data.db"
  );
  CHECK(
    lnb::store::paths::assistant_log("abc") ==
    std::filesystem::path("data") / "abc" / "local-notebook.log"
  );

  // set_data_root() redirects every derived path; an empty path restores the
  // built-in relative default.
  lnb::store::paths::set_data_root("custom-root");
  CHECK(lnb::store::paths::data_root() == std::filesystem::path("custom-root"));
  CHECK(
    lnb::store::paths::global_settings() ==
    std::filesystem::path("custom-root") / "settings.json"
  );
  CHECK(
    lnb::store::paths::assistant_dir("abc") ==
    std::filesystem::path("custom-root") / "abc"
  );
  CHECK(
    lnb::store::paths::assistant_db("abc") ==
    std::filesystem::path("custom-root") / "abc" / "data.db"
  );

  lnb::store::paths::set_data_root({});
  CHECK(lnb::store::paths::data_root() == std::filesystem::path("data"));
}

// --- Assistant log ------------------------------------------------------------

// The logger buffers lines until a sink is attached, so messages logged before an
// assistant is chosen still land at the top of that assistant's log rather than
// on the command line.
void test_log_file_sink() {
  const std::filesystem::path path =
    std::filesystem::temp_directory_path() / ("lnb-log-" + lnb::store::make_uuid() + ".log");

  // Written before any sink exists: it must be buffered, not dropped.
  lnb::log::info("before attach {}", 42);

  std::FILE* file = std::fopen(path.string().c_str(), "w");
  CHECK(file != nullptr);

  if (file != nullptr) {
    lnb::log::attach(file);
    lnb::log::info("after attach {}", 7);
    lnb::log::attach(nullptr);  // detach before closing the handle
    std::fclose(file);

    std::ifstream in(path);
    const std::string contents(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
    );

    const std::size_t buffered = contents.find("before attach 42");
    const std::size_t live = contents.find("after attach 7");

    CHECK(buffered != std::string::npos);
    CHECK(live != std::string::npos);

    // The buffered startup line is flushed ahead of the line written afterwards.
    CHECK(buffered < live);
  }

  std::error_code error;
  std::filesystem::remove(path, error);
}

void test_scan_assistants() {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path sandbox =
    std::filesystem::temp_directory_path() / ("lnb-scan-" + lnb::store::make_uuid());

  // Restore the working directory even if an assertion below fails.
  struct restore_working_directory {
    std::filesystem::path path;
    ~restore_working_directory() {
      std::error_code error;
      std::filesystem::current_path(path, error);
    }
  } guard{original};

  std::filesystem::create_directories(sandbox / "data" / "alpha");
  std::filesystem::create_directories(sandbox / "data" / "beta");
  std::filesystem::create_directories(sandbox / "data" / "gamma");  // no settings.json
  {
    std::ofstream(sandbox / "data" / "alpha" / "settings.json") << "{}";
    std::ofstream(sandbox / "data" / "beta" / "settings.json") << "{}";
  }
  std::filesystem::current_path(sandbox);

  // Sorted, and only directories that actually contain settings.json.
  const auto found = lnb::store::scan_assistants();
  CHECK(found.size() == 2);
  CHECK(found.size() == 2 && found[0] == "alpha" && found[1] == "beta");

  std::error_code error;
  std::filesystem::current_path(original, error);
  std::filesystem::remove_all(sandbox, error);
}

void test_session_round_trip(const std::filesystem::path& path) {
  lnb::store::database db(path.string());

  lnb::Session session;
  session.title_ = "Planning";
  session.custom_instructions_ = "Be terse.";
  session.memory_ = "User likes C++.";
  session.memory_watermark_ = 7;
  session.assistant_memory_watermark_ = 3;
  session.shared_directory_ = "/tmp/shared";
  session.pinned_ = true;
  session.summary_trigger_tokens_ = 111;
  session.summary_token_budget_ = 222;
  session.summary_reasoning_headroom_tokens_ = 333;
  session.max_tool_rounds_ = 4;
  session.max_tool_calls_ = 5;
  session.tool_output_max_tokens_ = 666;
  session.response_token_budget_ = 777;

  const std::uint32_t id = db.insert_session(session);
  CHECK(id > 0);

  auto loaded = db.session(id);
  CHECK(loaded.has_value());
  CHECK(loaded->id_ == id);
  CHECK(loaded->title_ == "Planning");
  CHECK(loaded->custom_instructions_ == "Be terse.");
  CHECK(loaded->memory_ == "User likes C++.");
  CHECK(loaded->memory_watermark_ == 7);
  CHECK(loaded->assistant_memory_watermark_ == 3);
  CHECK(loaded->shared_directory_.has_value());
  CHECK(*loaded->shared_directory_ == "/tmp/shared");
  CHECK(loaded->pinned_);
  CHECK(loaded->summary_trigger_tokens_ == 111);
  CHECK(loaded->summary_token_budget_ == 222);
  CHECK(loaded->summary_reasoning_headroom_tokens_ == 333);
  CHECK(loaded->max_tool_rounds_ == 4);
  CHECK(loaded->max_tool_calls_ == 5);
  CHECK(loaded->tool_output_max_tokens_ == 666);
  CHECK(loaded->response_token_budget_ == 777);

  // shared_directory_ is nullable.
  lnb::Session no_dir;
  no_dir.title_ = "No dir";
  const std::uint32_t no_dir_id = db.insert_session(no_dir);
  const auto loaded_no_dir = db.session(no_dir_id);
  CHECK(loaded_no_dir.has_value());
  CHECK(!loaded_no_dir->shared_directory_.has_value());
  CHECK(!loaded_no_dir->pinned_);  // unpinned by default

  // Update round-trips.
  loaded->title_ = "Renamed";
  loaded->memory_ = "New memory";
  loaded->pinned_ = false;
  db.update_session(*loaded);
  const auto reloaded = db.session(id);
  CHECK(reloaded->title_ == "Renamed");
  CHECK(reloaded->memory_ == "New memory");
  CHECK(!reloaded->pinned_);

  // Missing session.
  CHECK(!db.session(9999).has_value());

  db.remove_session(no_dir_id);
  CHECK(!db.session(no_dir_id).has_value());
}

void test_messages(const std::filesystem::path& path) {
  lnb::store::database db(path.string());

  lnb::Session session;
  session.title_ = "Chat";
  const std::uint32_t session_id = db.insert_session(session);

  lnb::Message first;
  first.session_id_ = session_id;
  first.role_ = lnb::Role::User;
  first.content_ = "Hello";
  first.made_at_ = "2026-01-01T00:00:00Z";
  const std::uint32_t first_id = db.insert_message(first);

  lnb::Message second;
  second.session_id_ = session_id;
  second.role_ = lnb::Role::Assistant;
  second.content_ = "Hi there";
  second.made_at_ = "2026-01-01T00:00:01Z";
  const std::uint32_t second_id = db.insert_message(second);

  CHECK(first_id > 0 && second_id > first_id);

  const auto all = db.messages(session_id);
  CHECK(all.size() == 2);
  CHECK(all[0].id_ == first_id);
  CHECK(all[0].role_ == lnb::Role::User);
  CHECK(all[0].content_ == "Hello");
  CHECK(all[0].made_at_ == "2026-01-01T00:00:00Z");
  CHECK(all[1].id_ == second_id);
  CHECK(all[1].role_ == lnb::Role::Assistant);
  CHECK(all[1].content_ == "Hi there");

  CHECK(db.message_count(session_id) == 2);

  // recent_messages returns oldest first, limited to the newest N.
  const auto recent = db.recent_messages(session_id, 1);
  CHECK(recent.size() == 1);
  CHECK(recent[0].id_ == second_id);

  // A third message so "newest two" is not the whole set.
  lnb::Message third;
  third.session_id_ = session_id;
  third.role_ = lnb::Role::User;
  third.content_ = "Third";
  third.made_at_ = "2026-01-01T00:00:02Z";
  const std::uint32_t third_id = db.insert_message(third);

  const auto recent_two = db.recent_messages(session_id, 2);
  CHECK(recent_two.size() == 2);
  CHECK(recent_two[0].content_ == "Hi there");
  CHECK(recent_two[1].content_ == "Third");

  // last_message_time is the newest made_at.
  const auto last = db.last_message_time(session_id);
  CHECK(last.has_value() && *last == "2026-01-01T00:00:02Z");

  // update_message.
  db.update_message(first_id, "Hello, edited");
  CHECK(db.messages(session_id)[0].content_ == "Hello, edited");

  // remove_message.
  CHECK(db.remove_message(third_id));
  CHECK(db.message_count(session_id) == 2);
  CHECK(!db.remove_message(999999));
}

void test_memory_bookkeeping(const std::filesystem::path& path) {
  lnb::store::database db(path.string());

  lnb::Session session;
  session.title_ = "Memory";
  const std::uint32_t session_id = db.insert_session(session);

  db.set_session_memory(session_id, "Summarised so far.", 42);
  db.set_assistant_memory_watermark(session_id, 17);

  const auto loaded = db.session(session_id);
  CHECK(loaded.has_value());
  CHECK(loaded->memory_ == "Summarised so far.");
  CHECK(loaded->memory_watermark_ == 42);
  CHECK(loaded->assistant_memory_watermark_ == 17);
}

void test_sessions_by_recent(const std::filesystem::path& path) {
  lnb::store::database db(path.string());

  lnb::Session empty_session;
  empty_session.title_ = "Empty";
  const std::uint32_t empty_id = db.insert_session(empty_session);

  lnb::Session older_session;
  older_session.title_ = "Older";
  const std::uint32_t older_id = db.insert_session(older_session);

  lnb::Session newer_session;
  newer_session.title_ = "Newer";
  const std::uint32_t newer_id = db.insert_session(newer_session);

  auto append = [&db](std::uint32_t session_id, const std::string& made_at) {
    lnb::Message message;
    message.session_id_ = session_id;
    message.role_ = lnb::Role::User;
    message.content_ = "x";
    message.made_at_ = made_at;
    (void)db.insert_message(message);
  };

  append(older_id, "2026-01-01T00:00:00Z");
  append(newer_id, "2026-01-02T00:00:00Z");

  const auto ordered = db.sessions_by_recent();
  CHECK(ordered.size() == 3);
  CHECK(ordered[0].id_ == newer_id);  // newest message first
  CHECK(ordered[1].id_ == older_id);
  CHECK(ordered[2].id_ == empty_id);  // no messages last

  const auto limited = db.sessions_by_recent(2);
  CHECK(limited.size() == 2);
  CHECK(limited[0].id_ == newer_id);
  CHECK(limited[1].id_ == older_id);

  // session_last_times gives one entry per session that has messages; the empty
  // session is absent.
  const auto times = db.session_last_times();
  CHECK(times.size() == 2);
  CHECK(times.at(newer_id) == "2026-01-02T00:00:00Z");
  CHECK(times.at(older_id) == "2026-01-01T00:00:00Z");
  CHECK(times.find(empty_id) == times.end());
}

// Pinned sessions are listed first, regardless of the time of their newest
// message; within a group the newest message still wins.
void test_pinned_ordering(const std::filesystem::path& path) {
  lnb::store::database db(path.string());

  lnb::Session pinned_old;
  pinned_old.title_ = "Pinned old";
  pinned_old.pinned_ = true;
  const std::uint32_t pinned_id = db.insert_session(pinned_old);

  lnb::Session plain_new;
  plain_new.title_ = "Plain new";
  const std::uint32_t plain_id = db.insert_session(plain_new);

  auto append = [&db](std::uint32_t session_id, const std::string& made_at) {
    lnb::Message message;
    message.session_id_ = session_id;
    message.role_ = lnb::Role::User;
    message.content_ = "x";
    message.made_at_ = made_at;
    (void)db.insert_message(message);
  };

  // The pinned session is older, but still sorts first.
  append(pinned_id, "2026-01-01T00:00:00Z");
  append(plain_id, "2026-01-02T00:00:00Z");

  const auto ordered = db.sessions_by_recent();
  CHECK(ordered.size() == 2);
  CHECK(ordered.size() == 2 && ordered[0].id_ == pinned_id);
  CHECK(ordered.size() == 2 && ordered[1].id_ == plain_id);
}

// Opening a version-1 database (no `pinned` column) migrates it forward: the
// column is added and the existing rows read as unpinned.
void test_pinned_migration(const std::filesystem::path& path) {
  {
    lnb::store::connection conn(path.string());
    conn.exec(R"sql(
PRAGMA user_version = 1;
CREATE TABLE session (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  title TEXT NOT NULL DEFAULT '',
  custom_instructions TEXT NOT NULL DEFAULT '',
  memory TEXT NOT NULL DEFAULT '',
  memory_watermark INTEGER NOT NULL DEFAULT 0,
  assistant_memory_watermark INTEGER NOT NULL DEFAULT 0,
  shared_directory TEXT,
  summary_trigger_tokens INTEGER NOT NULL DEFAULT 16000,
  summary_token_budget INTEGER NOT NULL DEFAULT 4000,
  summary_reasoning_headroom_tokens INTEGER NOT NULL DEFAULT 12000,
  max_tool_rounds INTEGER NOT NULL DEFAULT 8,
  max_tool_calls INTEGER NOT NULL DEFAULT 32,
  tool_output_max_tokens INTEGER NOT NULL DEFAULT 8000,
  response_token_budget INTEGER NOT NULL DEFAULT 8000
);
CREATE TABLE message (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL REFERENCES session(id) ON DELETE CASCADE,
  role INTEGER NOT NULL,
  content TEXT NOT NULL,
  made_at TEXT NOT NULL
);
INSERT INTO session (title) VALUES ('Legacy');
)sql");
  }

  lnb::store::database db(path.string());

  const auto ordered = db.sessions_by_recent();
  CHECK(ordered.size() == 1);
  CHECK(ordered.size() == 1 && ordered[0].title_ == "Legacy");
  CHECK(ordered.size() == 1 && !ordered[0].pinned_);

  // The migrated column is writable.
  lnb::Session session = ordered[0];
  session.pinned_ = true;
  db.update_session(session);
  CHECK(db.session(session.id_)->pinned_);
}

void test_cascade_delete(const std::filesystem::path& path) {
  lnb::store::database db(path.string());

  lnb::Session session;
  session.title_ = "Doomed";
  const std::uint32_t session_id = db.insert_session(session);

  lnb::Message message;
  message.session_id_ = session_id;
  message.role_ = lnb::Role::User;
  message.content_ = "bye";
  message.made_at_ = "2026-01-01T00:00:00Z";
  (void)db.insert_message(message);

  CHECK(db.message_count(session_id) == 1);
  db.remove_session(session_id);
  CHECK(!db.session(session_id).has_value());
  CHECK(db.message_count(session_id) == 0);
}

void test_managers(const std::filesystem::path& path) {
  lnb::store::database db(path.string());
  lnb::store::SessionManager sessions(db);

  lnb::Session session;
  session.title_ = "Managed";
  const std::uint32_t id = sessions.create(session);
  CHECK(id > 0);

  const auto fetched = sessions.get(id);
  CHECK(fetched.has_value());
  CHECK(fetched->title_ == "Managed");

  lnb::Session edited = *fetched;
  edited.title_ = "Managed v2";
  sessions.save(edited);
  CHECK(sessions.get(id)->title_ == "Managed v2");

  lnb::store::MessageManager messages(db, id);
  CHECK(messages.count() == 0);

  const std::uint32_t user_message = messages.append(lnb::Role::User, "ping");
  const std::uint32_t assistant_message =
    messages.append(lnb::Role::Assistant, "pong");
  CHECK(user_message > 0 && assistant_message > user_message);
  CHECK(messages.count() == 2);

  const auto recent = messages.most_recent(1);
  CHECK(recent.size() == 1);
  CHECK(recent[0].content_ == "pong");
  CHECK(recent[0].role_ == lnb::Role::Assistant);
  CHECK(!recent[0].made_at_.empty());

  const auto all = messages.all();
  CHECK(all.size() == 2);
  CHECK(all[0].content_ == "ping");

  sessions.remove(id);
  CHECK(!sessions.get(id).has_value());
  CHECK(messages.count() == 0);
}

// --- Summariser payloads ------------------------------------------------------

// The network client is SSE-only, so every request it sends must ask for a
// stream: a non-streaming body carries no `data: [DONE]` sentinel and would be
// rejected with "SSE stream ended before [DONE]".
void test_summary_requests_stream() {
  const QJsonObject session_request =
    lnb::context::session_summary_request("model", "memory", {}, 1000);
  CHECK(session_request.value("stream").toBool() == true);

  const QJsonObject assistant_request =
    lnb::context::assistant_summary_request("model", "memory", "## Session: x\n", 1000);
  CHECK(assistant_request.value("stream").toBool() == true);
}

} // namespace

int main() {
  // Each database test gets its own fresh file, so ordering and counts are not
  // polluted by rows other tests inserted.
  auto run = [](void (*test)(const std::filesystem::path&)) {
    const std::filesystem::path path = make_temp_db_path();
    test(path);
    remove_temp_db(path);
  };

  test_util();
  test_log_file_sink();
  test_scan_assistants();
  test_summary_requests_stream();
  run(test_session_round_trip);
  run(test_messages);
  run(test_memory_bookkeeping);
  run(test_sessions_by_recent);
  run(test_pinned_ordering);
  run(test_pinned_migration);
  run(test_cascade_delete);
  run(test_managers);

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }

  std::printf("All store tests passed.\n");
  return EXIT_SUCCESS;
}
