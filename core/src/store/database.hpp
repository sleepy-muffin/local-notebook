#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "local_notebook/core/model.hpp"
#include "shark/sqlite3.hpp"

// The reconstructed per-assistant database (design-1.md §9). One file holds all
// sessions and messages of one assistant. It owns the schema; `connection` does
// not.
namespace lnb::store {

class database {
public:
  explicit database(std::string_view path);

  // Sessions.
  [[nodiscard]] std::uint32_t insert_session(const Session& session);
  void update_session(const Session& session);
  void remove_session(std::uint32_t session_id);
  [[nodiscard]] std::optional<Session> session(std::uint32_t session_id) const;
  [[nodiscard]] bool session_exists(std::uint32_t session_id) const;

  // Ordered by the time of their newest message (newest first); sessions with no
  // messages come last. Ordering is a query, not a stored column.
  [[nodiscard]] std::vector<Session> sessions_by_recent(
    std::optional<std::size_t> limit = std::nullopt
  ) const;

  // Summarization bookkeeping: write a summarisation result and advance its
  // watermark.
  void set_session_summary(
    std::uint32_t session_id, const std::string& summary, std::uint32_t watermark
  );
  void set_assistant_memory_watermark(
    std::uint32_t session_id, std::uint32_t watermark
  );

  // The session's reasoning level, written on its own so the composer's selector
  // does not have to round-trip the whole row.
  void set_session_reasoning_effort(
    std::uint32_t session_id, const std::string& reasoning_effort
  );

  // Messages.
  [[nodiscard]] std::uint32_t insert_message(const Message& message);
  void update_message(std::uint32_t message_id, const std::string& content);
  [[nodiscard]] bool remove_message(std::uint32_t message_id);
  [[nodiscard]] std::vector<Message> messages(std::uint32_t session_id) const;

  // The unsummarised conversation: every message of the session whose id is
  // above the session's summary watermark, oldest first. This is the quantity the
  // summarisation trigger measures.
  [[nodiscard]] std::vector<Message> messages_after_summary_watermark(
    std::uint32_t session_id
  ) const;

  // The unsummarised conversation at the assistant level: every message whose id
  // is above its own session's assistant watermark, across all sessions, oldest
  // first. This is the quantity the assistant summarisation trigger measures.
  [[nodiscard]] std::vector<Message> messages_after_assistant_memory_watermark()
    const;

  // The newest `count` messages of a session, returned oldest first.
  [[nodiscard]] std::vector<Message> recent_messages(
    std::uint32_t session_id, std::size_t count
  ) const;
  [[nodiscard]] std::size_t message_count(std::uint32_t session_id) const;
  [[nodiscard]] std::optional<std::string> last_message_time(
    std::uint32_t session_id
  ) const;

  // The newest message time of every session, keyed by session id; sessions with
  // no messages are absent. One query for the whole sidebar, so listing rows does
  // not cost one query per row.
  [[nodiscard]] std::unordered_map<std::uint32_t, std::string> session_last_times()
    const;

private:
  shark::sqlite::connection connection_;
};

} // namespace lnb::store
