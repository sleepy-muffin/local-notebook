#include "store/sqlite.hpp"

#include <sqlite3.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace lnb::store {

namespace {

[[noreturn]] void raise(std::string_view context, sqlite3* db) {
  const char* message = db != nullptr ? sqlite3_errmsg(db) : "unknown SQLite error";
  throw std::runtime_error(std::string(context) + ": " + (message != nullptr ? message : "unknown"));
}

} // namespace

statement::statement(
  sqlite3* db, sqlite3_stmt* stmt, std::unique_lock<std::mutex> lock
)
  : lock_(std::move(lock)), stmt_(stmt), db_(db) {
}

statement::statement(statement&& other) noexcept
  : lock_(std::move(other.lock_)),
    stmt_(std::exchange(other.stmt_, nullptr)),
    db_(std::exchange(other.db_, nullptr)) {
}

statement& statement::operator=(statement&& other) noexcept {
  if (this != &other) {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
    // Finalize before the unique_lock move-assignment releases the old mutex.
    lock_ = std::move(other.lock_);
    stmt_ = std::exchange(other.stmt_, nullptr);
    db_ = std::exchange(other.db_, nullptr);
  }
  return *this;
}

statement::~statement() {
  if (stmt_ != nullptr) {
    sqlite3_finalize(stmt_);
  }
  // lock_ releases the mutex automatically after this destructor body.
}

void statement::bind_one(int index, std::string_view text) const {
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("Bound text is too large for sqlite3_bind_text");
  }
  const char* data = text.empty() ? "" : text.data();
  if (sqlite3_bind_text(stmt_, index, data, static_cast<int>(text.size()), SQLITE_TRANSIENT) !=
      SQLITE_OK) {
    raise("Failed to bind text", db_);
  }
}

void statement::bind_one(int index, const std::string& text) const {
  bind_one(index, std::string_view(text));
}

void statement::bind_one(int index, const char* text) const {
  bind_one(index, std::string_view(text != nullptr ? text : ""));
}

void statement::bind_one(int index, const std::optional<std::string>& text) const {
  if (text) {
    bind_one(index, std::string_view(*text));
    return;
  }
  if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
    raise("Failed to bind null", db_);
  }
}

void statement::bind_one(int index, int num) const {
  if (sqlite3_bind_int(stmt_, index, num) != SQLITE_OK) {
    raise("Failed to bind integer", db_);
  }
}

void statement::bind_one(int index, std::uint32_t num) const {
  if (sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(num)) != SQLITE_OK) {
    raise("Failed to bind integer", db_);
  }
}

bool statement::step() const {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  raise("Failed to step SQLite statement", db_);
}

void statement::execute() const {
  if (sqlite3_step(stmt_) != SQLITE_DONE) {
    raise("Failed to execute SQLite statement", db_);
  }
}

std::uint32_t statement::execute_with_last_row_id() const {
  execute();
  const sqlite3_int64 id = sqlite3_last_insert_rowid(db_);
  if (id < 0 || static_cast<std::uint64_t>(id) > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("Inserted SQLite ID is outside the uint32_t range");
  }
  return static_cast<std::uint32_t>(id);
}

int statement::column_int(int index) const {
  return sqlite3_column_int(stmt_, index);
}

std::uint32_t statement::column_uint32(int index) const {
  const sqlite3_int64 value = sqlite3_column_int64(stmt_, index);
  if (value < 0 || static_cast<std::uint64_t>(value) > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("SQLite value is outside the uint32_t range");
  }
  return static_cast<std::uint32_t>(value);
}

std::string statement::column_text(int index) const {
  const auto* value = sqlite3_column_text(stmt_, index);
  if (value == nullptr) {
    return {};
  }
  const int size = sqlite3_column_bytes(stmt_, index);
  return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(size)};
}

std::optional<std::string> statement::column_text_opt(int index) const {
  const auto* value = sqlite3_column_text(stmt_, index);
  if (value == nullptr) {
    return std::nullopt;
  }
  const int size = sqlite3_column_bytes(stmt_, index);
  return std::string(
    reinterpret_cast<const char*>(value), static_cast<std::size_t>(size)
  );
}

int statement::changes() const {
  return sqlite3_changes(db_);
}

connection::connection(std::string_view path) {
  const std::string path_string(path);
  const int rc = sqlite3_open(path_string.c_str(), &db_);
  if (rc != SQLITE_OK) {
    const std::string error = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown SQLite error";
    if (db_ != nullptr) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    throw std::runtime_error("Failed to open SQLite database '" + path_string + "': " + error);
  }

  // Foreign keys are off by default; the message table relies on ON DELETE
  // CASCADE, and WAL keeps readers and writers from blocking each other.
  exec("PRAGMA foreign_keys = ON;");
  exec("PRAGMA journal_mode = WAL;");
}

connection::~connection() {
  std::scoped_lock lock(mutex_);
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

statement connection::prepare(std::string_view sql) const {
  if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("SQL statement is too long for sqlite3_prepare_v2");
  }

  std::unique_lock<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v2(
    db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr
  );
  if (rc != SQLITE_OK) {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
    raise("Failed to prepare SQL statement", db_);
  }

  return statement(db_, stmt, std::move(lock));
}

void connection::exec(std::string_view sql) const {
  std::scoped_lock lock(mutex_);
  char* error_message = nullptr;
  const int rc = sqlite3_exec(
    db_, std::string(sql).c_str(), nullptr, nullptr, &error_message
  );
  if (rc != SQLITE_OK) {
    const std::string error = error_message != nullptr ? error_message : sqlite3_errmsg(db_);
    sqlite3_free(error_message);
    throw std::runtime_error("Failed to execute SQL: " + error);
  }
}

} // namespace lnb::store
