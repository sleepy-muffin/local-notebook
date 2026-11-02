#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "local_notebook/core/model.hpp"
#include "store/database.hpp"

// Thin facades over `database`. They exist so callers speak in terms of sessions
// and messages instead of SQL, and so `Message` stamping (made_at) has exactly
// one home. They hold no cache; the database is the source of truth.
//
// `SessionManager` is the owner of the database; `MessageManager` is a stateless
// view of one session's messages.
namespace lnb::core {

class MessageManager;

class SessionManager {
public:
  // Owning constructor: the manager opens (and owns) the database at
  // `db_path`. This is the form the interface uses.
  explicit SessionManager(std::string_view db_path);

  // Non-owning constructor, kept for callers that already own a database (the
  // tests). The manager never outlives it.
  explicit SessionManager(database& db) : db_(db) {}

  [[nodiscard]] std::vector<Session> most_recent(std::size_t count) const;
  [[nodiscard]] std::optional<Session> get(std::uint32_t id) const;
  [[nodiscard]] std::optional<MessageManager> get_messages(std::uint32_t id) const;
  [[nodiscard]] std::vector<Message> most_recent_after_assistant_memory_watermark() const;
  [[nodiscard]] std::unordered_map<std::uint32_t, std::string> session_last_times() const;
  [[nodiscard]] std::optional<std::string> last_message_time(std::uint32_t id) const;
  void set_reasoning_effort(std::uint32_t id, const std::string& level);
  bool remove_message(std::uint32_t id);
  void update_message(std::uint32_t id, const std::string& content);
  [[nodiscard]] database& database_for_update();

  // Inserts `session` and returns its new id. `session.id_` is ignored.
  std::uint32_t create(const Session& session);
  void save(const Session& session);
  void remove(std::uint32_t id);

  // Do not allow Getting `database` elsewhere.
  // Add interface to this class instead.

private:
  std::unique_ptr<database> owned_;
  database& db_;
};

class MessageManager {
public:
  MessageManager(database& db, std::uint32_t session_id);

  [[nodiscard]] std::vector<Message> most_recent(std::size_t count) const;
  [[nodiscard]] std::vector<Message> all() const;
  [[nodiscard]] std::size_t count() const;

  [[nodiscard]] std::vector<Message> all_messages_after_summary_watermark() const;

  // Appends a message, stamping made_at_ with the current UTC time.
  std::uint32_t append(Role role, const std::string& content);

private:
  database& db_;
  std::uint32_t session_id_;
};

} // namespace lnb::core
