#include "mmc/legacy.hpp"

#include "db.hpp"
#include "ui.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
constexpr std::string_view SQL_DB_INIT = R"sql(
CREATE TABLE IF NOT EXISTS session (
  id                   INTEGER PRIMARY KEY AUTOINCREMENT,
  title                TEXT NOT NULL,
  custom_instructions  TEXT NOT NULL DEFAULT '',
  api_url              TEXT NOT NULL,
  authentication_token TEXT NOT NULL DEFAULT '',
  model                TEXT NOT NULL DEFAULT '',
  proxy                TEXT DEFAULT NULL,
  shared_directory     TEXT DEFAULT NULL,
  memory_summary       TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS lorebook (
  id      INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  keyword TEXT NOT NULL,
  content TEXT NOT NULL,
  FOREIGN KEY (session_id) REFERENCES session(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS message (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  sender_id  INTEGER,
  reader_id  INTEGER,
  content    TEXT NOT NULL,
  created_at TEXT DEFAULT (datetime('now')),
  FOREIGN KEY (reader_id) REFERENCES session(id) ON DELETE CASCADE ON UPDATE CASCADE,
  FOREIGN KEY (sender_id) REFERENCES session(id) ON DELETE CASCADE ON UPDATE CASCADE
);
)sql";

constexpr std::string_view SQL_SESSION_MIGRATE_ADD_MODEL = R"sql(
ALTER TABLE session ADD COLUMN model TEXT NOT NULL DEFAULT ''
)sql";
constexpr std::string_view SQL_SESSION_MIGRATE_ADD_PROXY = R"sql(
ALTER TABLE session ADD COLUMN proxy TEXT DEFAULT NULL
)sql";
constexpr std::string_view SQL_SESSION_MIGRATE_ADD_SHARED_DIRECTORY = R"sql(
ALTER TABLE session ADD COLUMN shared_directory TEXT DEFAULT NULL
)sql";

constexpr std::string_view SQL_SESSION_MIGRATE_ADD_MEMORY = R"sql(
ALTER TABLE session ADD COLUMN memory_summary TEXT NOT NULL DEFAULT ''
)sql";

constexpr std::string_view SQL_SESSION_SELECT =
    "SELECT s.id, s.title, s.custom_instructions, s.api_url, s.authentication_token, s.model, s.proxy, s.shared_directory, s.memory_summary, MAX(m.created_at) FROM session s LEFT JOIN message m ON m.sender_id = s.id OR m.reader_id = s.id GROUP BY s.id ORDER BY MAX(m.created_at) IS NULL, MAX(m.created_at) DESC, s.id";
constexpr std::string_view SQL_SESSION_INSERT =
    "INSERT INTO session (title, custom_instructions, api_url, authentication_token, model, proxy, shared_directory, memory_summary) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
constexpr std::string_view SQL_SESSION_UPDATE =
    "UPDATE session SET title = ?, custom_instructions = ?, api_url = ?, authentication_token = ?, model = ?, proxy = ?, shared_directory = ?, memory_summary = ? WHERE id = ?";
constexpr std::string_view SQL_SESSION_DELETE = "DELETE FROM session WHERE id = ?";
constexpr std::string_view SQL_MESSAGE_INSERT =
    "INSERT INTO message (sender_id, reader_id, content) VALUES (?, ?, ?)";
constexpr std::string_view SQL_MESSAGE_DELETE_BY_ID =
    "DELETE FROM message WHERE id = ?;";
constexpr std::string_view SQL_MESSAGE_UPDATE =
    "UPDATE message SET content = ? WHERE id = ?;";
constexpr std::string_view SQL_MESSAGE_SELECT = R"sql(
  SELECT id, sender_id, reader_id, content, created_at
  FROM message
  WHERE (sender_id = ? AND reader_id = ?) OR (sender_id = ? AND reader_id = ?)
  ORDER BY created_at ASC, id ASC
)sql";
constexpr std::string_view SQL_MESSAGE_DELETE_LAST = R"sql(
  DELETE FROM message
  WHERE id = (
    SELECT id FROM message
    WHERE (sender_id = ? AND reader_id = ?) OR (sender_id = ? AND reader_id = ?)
    ORDER BY id DESC
    LIMIT 1
  )
)sql";
constexpr std::string_view SQL_LOREBOOK_SELECT =
    "SELECT id, keyword, content FROM lorebook WHERE session_id = ? ORDER BY id ASC";
constexpr std::string_view SQL_LOREBOOK_INSERT =
    "INSERT INTO lorebook (session_id, keyword, content) VALUES (?, ?, ?)";
constexpr std::string_view SQL_LOREBOOK_DELETE = "DELETE FROM lorebook WHERE id = ?";
constexpr std::string_view SQL_LOREBOOK_UPDATE =
    "UPDATE lorebook SET keyword = ?, content = ? WHERE id = ?";
constexpr std::string_view SQL_LOREBOOK_MIGRATE_RENAME_PEER =
    "ALTER TABLE lorebook RENAME COLUMN peer_id TO session_id";
} // namespace

// ==============================================================================================
// CONFIGURATION
// ==============================================================================================

lnb::general::general(std::string_view path)
  : path_(path) {
  try {
    QFile file(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));

    if (!file.open(QIODevice::ReadOnly)) {
      throw std::runtime_error("Unable to open configuration file");
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject content = document.object();

    // The current file nests the general settings under "general"; the older
    // flat format kept them at the top level, so fall back to the root object.
    const QJsonObject general = content.contains("general")
        ? content.value("general").toObject()
        : content;

    // Every field is optional: absent keys keep the defaults declared above.
    if (general.contains("session_id")) {
      session_id_ = static_cast<uint32_t>(general.value("session_id").toInt(session_id_));
    }
    if (general.contains("font")) {
      font_ = general.value("font").toString().toStdString();
    }
    if (general.contains("font_size")) {
      font_size_ = general.value("font_size").toInt(font_size_);
    }
    if (general.contains("ui_font")) {
      ui_font_ = general.value("ui_font").toString().toStdString();
    }
    if (general.contains("ui_font_size")) {
      ui_font_size_ = general.value("ui_font_size").toInt(ui_font_size_);
    }
    if (general.contains("mono_font")) {
      mono_font_ = general.value("mono_font").toString().toStdString();
    }
    if (general.contains("mono_font_size")) {
      mono_font_size_ = general.value("mono_font_size").toInt(mono_font_size_);
    }
    if (general.contains("max_tool_calls")) {
      max_tool_calls_ = std::max(1, general.value("max_tool_calls").toInt(max_tool_calls_));
    }
    if (general.contains("model_context_tokens")) {
      model_context_tokens_ = static_cast<std::size_t>(std::max(1024, general.value("model_context_tokens").toInt(static_cast<int>(model_context_tokens_))));
    }
    if (general.contains("response_token_budget")) {
      response_token_budget_ = static_cast<std::size_t>(std::max(128, general.value("response_token_budget").toInt(static_cast<int>(response_token_budget_))));
    }
    if (general.contains("context_safety_margin_tokens")) {
      context_safety_margin_tokens_ = static_cast<std::size_t>(std::max(0, general.value("context_safety_margin_tokens").toInt(static_cast<int>(context_safety_margin_tokens_))));
    }
    if (general.contains("summary_trigger_tokens")) {
      summary_trigger_tokens_ = static_cast<std::size_t>(std::max(256, general.value("summary_trigger_tokens").toInt(static_cast<int>(summary_trigger_tokens_))));
    }
    if (general.contains("summary_token_budget")) {
      summary_token_budget_ = static_cast<std::size_t>(std::max(128, general.value("summary_token_budget").toInt(static_cast<int>(summary_token_budget_))));
    }
    if (general.contains("max_tool_rounds")) {
      max_tool_rounds_ = std::max(1, general.value("max_tool_rounds").toInt(max_tool_rounds_));
    }
    if (general.contains("rendered_message_limit")) {
      rendered_message_limit_ =
          std::max(1, general.value("rendered_message_limit").toInt(rendered_message_limit_));
    }
    if (general.contains("history_page_size")) {
      history_page_size_ =
          std::max(1, general.value("history_page_size").toInt(history_page_size_));
    }
    if (general.contains("enter_with_modifier")) {
      enter_with_modifier_ = general.value("enter_with_modifier").toBool(enter_with_modifier_);
    }

    // The theme is a name ("Catppuccin Mocha", ...) selecting a palette from
    // theme.json. An older object-valued "theme" key is simply ignored.
    if (general.contains("theme")) {
      const QJsonValue value = general.value("theme");

      if (value.isString() && !value.toString().isEmpty()) {
        theme_ = value.toString().toStdString();
      }
    }

    // The background image path is resolved relative to the working directory.
    // "background_url" is the current key; "background" is accepted for older
    // configuration files.
    if (general.contains("background_url") || general.contains("background")) {
      const QString key =
          general.contains("background_url") ? QStringLiteral("background_url")
                                             : QStringLiteral("background");
      const QString value = general.value(key).toString();

      if (value.isEmpty()) {
        background_url_ = std::nullopt;
      } else {
        background_url_ = value.toStdString();
      }
    }

    // A present array overrides the palette backgrounds; an explicit null keeps
    // them as-is even when a background image is enabled.
//     if (general.contains("background_override")) {
//       const QJsonValue value = general.value("background_override");
//
//       if (value.isNull()) {
//         background_override_ = std::nullopt;
//       } else if (value.isArray()) {
//         const color4 fallback = background_override_.value_or(color4{0.1f, 0.1f, 0.1f, 0.9f});
//         background_override_ = load_color(general, "background_override", fallback);
//       }
//     }

    // Optional layout-metric overrides. When present they replace the values
    // loaded from theme.json for the running session.
    if (content.contains("metrics") && content.value("metrics").isObject()) {
      const QJsonObject object = content.value("metrics").toObject();
      const auto read_int = [&object](const char* key, int fallback) {
        return object.contains(key) ? object.value(key).toInt(fallback) : fallback;
      };

      ui::ThemeMetrics metrics;
      metrics.nav_width = read_int("nav_width", metrics.nav_width);
      metrics.sidebar_width = read_int("sidebar_width", metrics.sidebar_width);
      metrics.bubble_radius = read_int("bubble_radius", metrics.bubble_radius);
      metrics.message_spacing = read_int("message_spacing", metrics.message_spacing);
      metrics.page_margin = read_int("page_margin", metrics.page_margin);
      metrics.composer_height = read_int("composer_height", metrics.composer_height);
      metrics.title_font_size = read_int("title_font_size", metrics.title_font_size);
      metrics.session_item_height =
          read_int("session_item_height", metrics.session_item_height);
      metrics.line_height_other =
          std::max(100, read_int("line_height_other", metrics.line_height_other));
      metrics.line_height_self =
          std::max(100, read_int("line_height_self", metrics.line_height_self));
      metrics.paragraph_spacing =
          std::max(0, read_int("paragraph_spacing", metrics.paragraph_spacing));
      metrics.message_max_width =
          std::max(0, read_int("message_max_width", metrics.message_max_width));

      metrics_ = metrics;
    }
  } catch (const std::exception&) {
    mmc::log::debug("Configuration file cannot load properly: {}", path_);
    // Keep the defaults declared by config.
  }
}

void lnb::general::save() const {
  // The file nests general settings under "general" and the optional layout
  // metric overrides under "metrics", mirroring the loader above.
  QJsonObject general;
  general["session_id"] = static_cast<int>(session_id_);

  general["ui_font"] = QString::fromStdString(ui_font_);
  general["ui_font_size"] = ui_font_size_;
  general["font"] = QString::fromStdString(font_);
  general["font_size"] = font_size_;
  general["mono_font"] = QString::fromStdString(mono_font_);
  general["mono_font_size"] = mono_font_size_;

  general["enter_with_modifier"] = enter_with_modifier_;

  general["theme"] = QString::fromStdString(theme_);

  general["background_url"] = background_url_.has_value()
      ? QJsonValue(QString::fromStdString(*background_url_))
      : QJsonValue(QJsonValue::Null);


  // general["background_override"] = QJsonValue(QJsonValue::Null);

  // Generation/context budgets, so the file round-trips every knob.
  general["model_context_tokens"] = static_cast<double>(model_context_tokens_);
  general["response_token_budget"] = static_cast<double>(response_token_budget_);
  general["context_safety_margin_tokens"] =
      static_cast<double>(context_safety_margin_tokens_);
  general["summary_trigger_tokens"] = static_cast<double>(summary_trigger_tokens_);
  general["summary_token_budget"] = static_cast<double>(summary_token_budget_);
  general["max_tool_rounds"] = max_tool_rounds_;
  general["max_tool_calls"] = max_tool_calls_;
  general["rendered_message_limit"] = rendered_message_limit_;
  general["history_page_size"] = history_page_size_;

  QJsonObject root;
  root["general"] = general;

  if (metrics_.has_value()) {
    const ui::ThemeMetrics& m = *metrics_;
    QJsonObject metrics;
    metrics["nav_width"] = m.nav_width;
    metrics["sidebar_width"] = m.sidebar_width;
    metrics["bubble_radius"] = m.bubble_radius;
    metrics["message_spacing"] = m.message_spacing;
    metrics["page_margin"] = m.page_margin;
    metrics["composer_height"] = m.composer_height;
    metrics["title_font_size"] = m.title_font_size;
    metrics["session_item_height"] = m.session_item_height;
    metrics["line_height_other"] = m.line_height_other;
    metrics["line_height_self"] = m.line_height_self;
    metrics["paragraph_spacing"] = m.paragraph_spacing;
    metrics["message_max_width"] = m.message_max_width;
    root["metrics"] = metrics;
  }

  QFile file(QString::fromUtf8(path_.data(), static_cast<qsizetype>(path_.size())));

  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    mmc::log::debug("Configuration file cannot be written: {}", path_);
    return;
  }

  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

lnb::general::~general() {
  save();
}

// ==============================================================================================
// SESSION / SQLITE
// ==============================================================================================

lnb::session::session(
  uint32_t id, std::string title, std::string custom_instructions, std::string api_url,
  std::string authentication_token, std::string model, std::optional<std::string> proxy,
  std::optional<std::string> shared_directory,
  std::string memory_summary,
  std::optional<std::string> last_message_at
)
  : id_(id),
    title_(std::move(title)),
    custom_instructions_(std::move(custom_instructions)),
    api_url_(std::move(api_url)),
    authentication_token_(std::move(authentication_token)),
    model_(std::move(model)),
    proxy_(std::move(proxy)),
    shared_directory_(std::move(shared_directory)),
    memory_summary_(std::move(memory_summary)),
    last_message_at_(std::move(last_message_at)) {
}

lnb::session::session(
  std::string title, std::string custom_instructions, std::string api_url,
  std::string authentication_token, std::string model, std::optional<std::string> proxy,
  std::optional<std::string> shared_directory,
  std::string memory_summary,
  std::optional<std::string> last_message_at
)
  : session(
    0, std::move(title), std::move(custom_instructions), std::move(api_url),
    std::move(authentication_token), std::move(model), std::move(proxy), std::move(shared_directory),
    std::move(memory_summary), std::move(last_message_at)
  ) {
}

lnb::statement::statement(
  sqlite3* db, sqlite3_stmt* stmt, std::unique_lock<std::mutex> lock
)
  : lock_(std::move(lock)), stmt_(stmt), db_(db) {
}

lnb::statement::statement(statement&& other) noexcept
  : lock_(std::move(other.lock_)),
    stmt_(std::exchange(other.stmt_, nullptr)),
    db_(std::exchange(other.db_, nullptr)) {
}

lnb::statement& lnb::statement::operator=(statement&& other) noexcept {
  if (this != &other) {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
    // Finalize the old statement before unique_lock move-assignment unlocks its mutex.
    lock_ = std::move(other.lock_);
    stmt_ = std::exchange(other.stmt_, nullptr);
    db_ = std::exchange(other.db_, nullptr);
  }
  return *this;
}

lnb::statement::~statement() {
  if (stmt_ != nullptr) {
    sqlite3_finalize(stmt_);
  }
  // lock_ releases the mutex automatically after this destructor body.
}

void lnb::statement::sqlite_binder::operator()(std::string_view text) {
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("Bound text is too large for sqlite3_bind_text");
  }
  const char* data = text.empty() ? "" : text.data();
  mmc::checked_invoke<sqlite3_bind_text, SQLITE_OK>(
    stmt_.stmt_, ++count_, data, static_cast<int>(text.size()), SQLITE_TRANSIENT
  );
}

void lnb::statement::sqlite_binder::operator()(
  const std::optional<std::string>& text
) {
  if (text) {
    (*this)(std::string_view(*text));
  } else {
    mmc::checked_invoke<sqlite3_bind_null, SQLITE_OK>(
      stmt_.stmt_, ++count_
    );
  }
}

void lnb::statement::sqlite_binder::operator()(int num) {
  mmc::checked_invoke<sqlite3_bind_int, SQLITE_OK>(stmt_.stmt_, ++count_, num);
}

void lnb::statement::sqlite_binder::operator()(uint32_t num) {
  mmc::checked_invoke<sqlite3_bind_int64, SQLITE_OK>(
    stmt_.stmt_, ++count_, static_cast<sqlite3_int64>(num)
  );
}

bool lnb::statement::step() const {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  mmc::raise("Failed to step SQLite statement: {}", sqlite3_errmsg(db_));
  return false; // Keeps compilers happy if mmc::raise is not annotated noreturn.
}

void lnb::statement::execute() const {
  const int rc = sqlite3_step(stmt_);
  if (rc != SQLITE_DONE) {
    mmc::raise("Failed to execute SQLite statement: {}", sqlite3_errmsg(db_));
  }
}

uint32_t lnb::statement::execute_with_last_row_id() const {
  execute();
  const sqlite3_int64 id = sqlite3_last_insert_rowid(db_);
  if (id < 0 || static_cast<uint64_t>(id) > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Inserted SQLite ID is outside the uint32_t range");
  }
  return static_cast<uint32_t>(id);
}

int lnb::statement::column_int(int index) const {
  return sqlite3_column_int(stmt_, index);
}

uint32_t lnb::statement::column_uint32(int index) const {
  const sqlite3_int64 value = sqlite3_column_int64(stmt_, index);
  if (value < 0 || static_cast<uint64_t>(value) > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("SQLite value is outside the uint32_t range");
  }
  return static_cast<uint32_t>(value);
}

std::string lnb::statement::column_text(int index) const {
  const auto* value = sqlite3_column_text(stmt_, index);
  if (value == nullptr) {
    return {};
  }
  const int size = sqlite3_column_bytes(stmt_, index);
  return {reinterpret_cast<const char *>(value), static_cast<std::size_t>(size)};
}

std::optional<std::string>
lnb::statement::column_text_opt(int index) const {
  const auto* value = sqlite3_column_text(stmt_, index);

  if (value == nullptr) {
    return std::nullopt;
  }

  const int size = sqlite3_column_bytes(stmt_, index);

  return std::string(
    reinterpret_cast<const char *>(value),
    static_cast<std::size_t>(size)
  );
}


lnb::sqlite_interface::sqlite_interface(std::string_view path) {
  const std::string path_string(path);
  const int rc = sqlite3_open(path_string.c_str(), &db_);
  if (rc != SQLITE_OK) {
    const std::string error = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown SQLite error";
    if (db_ != nullptr) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    mmc::raise("Failed to open SQLite database '{}': {}", path, error);
  }

  char* error_message = nullptr;
  int exec_rc = sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, &error_message);
  if (exec_rc != SQLITE_OK) {
    const std::string error = error_message != nullptr ? error_message : sqlite3_errmsg(db_);
    sqlite3_free(error_message);
    sqlite3_close(db_);
    db_ = nullptr;
    mmc::raise("Failed to enable SQLite foreign keys: {}", error);
  }

  error_message = nullptr;
  exec_rc = sqlite3_exec(db_, SQL_DB_INIT.data(), nullptr, nullptr, &error_message);
  if (exec_rc != SQLITE_OK) {
    const std::string error = error_message != nullptr ? error_message : sqlite3_errmsg(db_);
    sqlite3_free(error_message);
    sqlite3_close(db_);
    db_ = nullptr;
    mmc::raise("Failed to initialize SQLite schema: {}", error);
  }

  migrate();

  mmc::log::info("SQLite tables are ready.");
}

lnb::sqlite_interface::~sqlite_interface() {
  std::scoped_lock lock(db_mutex_);
  if (db_ != nullptr) {
    const int rc = sqlite3_close(db_);
    if (rc == SQLITE_OK) {
      db_ = nullptr;
    } else {
      mmc::log::debug("Failed to close SQLite database: {}", sqlite3_errstr(rc));
    }
  }
}

lnb::statement lnb::sqlite_interface::prepare(std::string_view sql) {
  if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("SQL statement is too long for sqlite3_prepare_v2");
  }

  std::unique_lock<std::mutex> lock(db_mutex_);
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v2(
    db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr
  );
  if (rc != SQLITE_OK) {
    const std::string error = sqlite3_errmsg(db_);
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
    mmc::raise("Failed to prepare SQL statement: {}", error);
  }

  return statement(db_, stmt, std::move(lock));
}

void lnb::sqlite_interface::migrate() {
  auto column_exists = [this](
    std::string_view table,
    std::string_view column
  ) {
    auto stmt = this->prepare(
      std::format("PRAGMA table_info({})", table)
    );

    while (stmt.step()) {
      // PRAGMA table_info:
      // 0: cid
      // 1: name
      // 2: type
      // 3: notnull
      // 4: dflt_value
      // 5: pk

      if (stmt.column_text(1) == column) {
        return true;
      }
    }

    return false;
  };


  if (!column_exists("session", "model")) {
    prepare(
      SQL_SESSION_MIGRATE_ADD_MODEL
    ).execute();
  }
  if (!column_exists("session", "proxy")) {
    prepare(
      SQL_SESSION_MIGRATE_ADD_PROXY
    ).execute();
  }
  if (!column_exists("session", "shared_directory")) {
    prepare(
      SQL_SESSION_MIGRATE_ADD_SHARED_DIRECTORY
    ).execute();
  }

  if (!column_exists("session", "memory_summary")) {
    prepare(
      SQL_SESSION_MIGRATE_ADD_MEMORY
    ).execute();
  }

  // matches the current schema (and the SQL that queries it).
  if (column_exists("lorebook", "peer_id") &&
      !column_exists("lorebook", "session_id")) {
    prepare(
      SQL_LOREBOOK_MIGRATE_RENAME_PEER
    ).execute();
  }
}

lnb::db::db(std::string_view path)
  : sql_(path) {
}

lnb::db::~db() = default;

void lnb::db::init() {
  // Schema initialization is performed by sqlite_interface's constructor.
}

void lnb::db::update_session(const session& ses) {
  auto stmt = sql_.prepare(SQL_SESSION_UPDATE);
  stmt.bind(
    ses.title_, ses.custom_instructions_, ses.api_url_, ses.authentication_token_, ses.model_,
    ses.proxy_, ses.shared_directory_, ses.memory_summary_, ses.id_
  );
  stmt.execute();
}

uint32_t lnb::db::insert_session(const session& ses) {
  auto stmt = sql_.prepare(SQL_SESSION_INSERT);
  stmt.bind(
    ses.title_, ses.custom_instructions_, ses.api_url_, ses.authentication_token_, ses.model_,
    ses.proxy_, ses.shared_directory_, ses.memory_summary_
  );
  const uint32_t id = stmt.execute_with_last_row_id();
  return id;
}

void lnb::db::remove_session(uint32_t session_id) {
  auto stmt = sql_.prepare(SQL_SESSION_DELETE);
  stmt.bind(session_id);
  stmt.execute();
}

lnb::session_snapshot lnb::db::get_all_sessions() {
  auto stmt = sql_.prepare(SQL_SESSION_SELECT);
  auto sessions = std::make_shared<session_list>();

  while (stmt.step()) {
    sessions->emplace_back(
      stmt.column_uint32(0),
      stmt.column_text(1),
      stmt.column_text(2),
      stmt.column_text(3),
      stmt.column_text(4),
      stmt.column_text(5),
      stmt.column_text_opt(6),
      stmt.column_text_opt(7),
      stmt.column_text(8),
      stmt.column_text_opt(9)
    );
  }

  return sessions;
}

std::optional<lnb::session> lnb::db::get_session_by_id(uint32_t session_id) {
  const auto sessions = get_all_sessions();
  for (const auto& ses: *sessions) {
    if (ses.id_ == session_id) {
      return ses;
    }
  }
  return std::nullopt;
}

void lnb::db::update_session_memory(uint32_t session_id, const std::string& summary) {
  auto stmt = sql_.prepare("UPDATE session SET memory_summary = ? WHERE id = ?");
  stmt.bind(summary, session_id);
  stmt.execute();
}

uint32_t lnb::db::insert_message(
  uint32_t sender_id, uint32_t reader_id, const std::string& content
) {
  auto stmt = sql_.prepare(SQL_MESSAGE_INSERT);
  stmt.bind(sender_id, reader_id, content);
  return stmt.execute_with_last_row_id();
}

void lnb::db::update_message(uint32_t message_id, const std::string& content) {
  auto stmt = sql_.prepare(SQL_MESSAGE_UPDATE);
  stmt.bind(content, message_id);
  stmt.execute();
}

bool lnb::db::remove_message_by_id(uint32_t message_id) {
  try {
    auto stmt = sql_.prepare(SQL_MESSAGE_DELETE_BY_ID);
    stmt.bind(message_id);
    stmt.execute();

    return true;
  } catch (std::exception& e) {
    mmc::print("remove_message_by_id failed to remove: {}", e.what());
    return false;
  }
}

void lnb::db::remove_last_message(uint32_t session_id, uint32_t self_id) {
  auto stmt = sql_.prepare(SQL_MESSAGE_DELETE_LAST);
  stmt.bind(self_id, session_id, session_id, self_id);
  stmt.execute();
}

std::vector<lnb::message> lnb::db::get_messages_for_session(
  uint32_t session_id, uint32_t self_id
) {
  auto stmt = sql_.prepare(SQL_MESSAGE_SELECT);
  stmt.bind(session_id, self_id, self_id, session_id);

  std::vector<message> messages;
  while (stmt.step()) {
    message row{};
    row.id = stmt.column_uint32(0);
    row.sender_id = stmt.column_uint32(1);
    row.reader_id = stmt.column_uint32(2);
    row.content = stmt.column_text(3);
    row.created_at = stmt.column_text(4);
    messages.push_back(std::move(row));
  }
  return messages;
}

std::vector<lnb::lore_row> lnb::db::get_lorebook_for_session(uint32_t session_id) {
  auto stmt = sql_.prepare(SQL_LOREBOOK_SELECT);
  stmt.bind(session_id);

  std::vector<lore_row> rows;
  while (stmt.step()) {
    lore_row row{};
    row.id = stmt.column_int(0);
    row.keyword = stmt.column_text(1);
    row.content = stmt.column_text(2);
    rows.push_back(std::move(row));
  }
  return rows;
}

void lnb::db::insert_lorebook(
  uint32_t session_id, const std::string& keyword, const std::string& content
) {
  auto stmt = sql_.prepare(SQL_LOREBOOK_INSERT);
  stmt.bind(session_id, keyword, content);
  stmt.execute();
}

void lnb::db::delete_lorebook_entry(int id) {
  auto stmt = sql_.prepare(SQL_LOREBOOK_DELETE);
  stmt.bind(id);
  stmt.execute();
}

void lnb::db::update_lorebook_entry(
  int id, const std::string& keyword, const std::string& content
) {
  auto stmt = sql_.prepare(SQL_LOREBOOK_UPDATE);
  stmt.bind(keyword, content, id);
  stmt.execute();
}
