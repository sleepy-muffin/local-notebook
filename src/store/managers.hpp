#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "model.hpp"
#include "store/database.hpp"

// Thin, stateless facades over `database`. They exist so callers speak in terms
// of sessions and messages instead of SQL, and so `Message` stamping (made_at)
// has exactly one home. They hold no cache; the database is the source of truth.
namespace lnb::store {

class SessionManager {
public:
  explicit SessionManager(database& db) : db_(db) {}

  [[nodiscard]] std::vector<Session> most_recent(std::size_t count) const;
  [[nodiscard]] std::optional<Session> get(std::uint32_t id) const;

  // Inserts `session` and returns its new id. `session.id_` is ignored.
  std::uint32_t create(const Session& session);
  void save(const Session& session);
  void remove(std::uint32_t id);

private:
  database& db_;
};

class MessageManager {
public:
  MessageManager(database& db, std::uint32_t session_id);

  [[nodiscard]] std::vector<Message> most_recent(std::size_t count) const;
  [[nodiscard]] std::vector<Message> all() const;
  [[nodiscard]] std::size_t count() const;

  // Appends a message, stamping made_at_ with the current UTC time.
  std::uint32_t append(Role role, const std::string& content);

private:
  database& db_;
  std::uint32_t session_id_;
};

} // namespace lnb::store
