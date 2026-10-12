#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mmc/log.hpp>
#include <sqlite3.h>

#include "mmc/common.hpp"

namespace lnb {
class session {
public:
  session(uint32_t id, std::string title, std::string custom_instructions, std::string api_url,
          std::string authentication_token, std::string model, std::optional<std::string> proxy,
          std::optional<std::string> shared_directory, std::string memory_summary = {},
          std::optional<std::string> last_message_at = std::nullopt,
          uint32_t memory_watermark = 0);
  session(std::string title, std::string custom_instructions, std::string api_url,
          std::string authentication_token, std::string model, std::optional<std::string> proxy,
          std::optional<std::string> shared_directory, std::string memory_summary = {},
          std::optional<std::string> last_message_at = std::nullopt,
          uint32_t memory_watermark = 0);

  uint32_t id_;
  std::string title_;
  std::string custom_instructions_;
  std::string api_url_;
  std::string authentication_token_;
  std::string model_;
  std::optional<std::string> proxy_;
  std::optional<std::string> shared_directory_;
  std::string memory_summary_;

  // Highest message id already folded into memory_summary_. Messages with an id
  // above it are still sent verbatim, so a message is never both summarised and
  // repeated in the recent window.
  uint32_t memory_watermark_ = 0;

  std::optional<std::string> last_message_at_;
};

struct message { uint32_t id{}; uint32_t sender_id{}; uint32_t reader_id{}; std::string content; std::string created_at; };
struct lore_row { int id{}; std::string keyword; std::string content; };

class statement {
public:
  statement() = delete; statement(const statement&) = delete; statement& operator=(const statement&) = delete;
  statement(statement&& other) noexcept; statement& operator=(statement&& other) noexcept;
  statement(sqlite3* db, sqlite3_stmt* stmt, std::unique_lock<std::mutex> lock); ~statement();
  struct sqlite_binder : mmc::iter_args<sqlite_binder> {
    sqlite_binder(const sqlite_binder&) = delete; sqlite_binder& operator=(const sqlite_binder&) = delete;
    explicit sqlite_binder(const statement& stmt) : stmt_(stmt) {}
    void operator()(std::string_view text); void operator()(const std::string& text) { (*this)(std::string_view(text)); }
    void operator()(const std::optional<std::string>& text_opt); void operator()(int num); void operator()(uint32_t num);
    const statement& stmt_; int count_ = 0;
  };
  template<typename... Args> void bind(Args&&... args) const { sqlite_binder(*this).invoke(std::forward<Args>(args)...); }
  [[nodiscard]] bool step() const; void execute() const; [[nodiscard]] uint32_t execute_with_last_row_id() const;
  [[nodiscard]] int column_int(int index) const; [[nodiscard]] uint32_t column_uint32(int index) const;
  [[nodiscard]] std::string column_text(int index) const; [[nodiscard]] std::optional<std::string> column_text_opt(int index) const;
private: std::unique_lock<std::mutex> lock_; sqlite3_stmt* stmt_ = nullptr; sqlite3* db_ = nullptr;
};

class sqlite_interface {
public:
  sqlite_interface() = delete; explicit sqlite_interface(std::string_view path);
  sqlite_interface(const sqlite_interface&) = delete; sqlite_interface& operator=(const sqlite_interface&) = delete;
  sqlite_interface(sqlite_interface&&) = delete; sqlite_interface& operator=(sqlite_interface&&) = delete; ~sqlite_interface();
  [[nodiscard]] statement prepare(std::string_view sql); void migrate();
private: sqlite3* db_ = nullptr; std::mutex db_mutex_;
};
using session_list = std::vector<session>; using session_snapshot = std::shared_ptr<const session_list>;

class db {
public:
  explicit db(std::string_view path); ~db(); void init();
  uint32_t insert_session(const session& ses); void update_session(const session& ses); void remove_session(uint32_t session_id);
  void update_session_memory(uint32_t session_id, const std::string& summary, uint32_t watermark);
  std::optional<session> get_session_by_id(uint32_t session_id); session_snapshot get_all_sessions();
  uint32_t insert_message(uint32_t sender_id, uint32_t reader_id, const std::string& content);
  void update_message(uint32_t message_id, const std::string& content); bool remove_message_by_id(uint32_t message_id);
  void remove_last_message(uint32_t session_id, uint32_t self_id); std::vector<message> get_messages_for_session(uint32_t session_id, uint32_t self_id);
  std::vector<lore_row> get_lorebook_for_session(uint32_t session_id);
  void insert_lorebook(uint32_t session_id, const std::string& keyword, const std::string& content);
  void delete_lorebook_entry(int id); void update_lorebook_entry(int id, const std::string& keyword, const std::string& content);
private: sqlite_interface sql_;
};
} // namespace lnb
