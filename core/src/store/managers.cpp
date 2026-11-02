#include <shark/str.hpp>

#include "store/managers.hpp"

#include "local_notebook/core/utils.hpp"

namespace lnb::core {

SessionManager::SessionManager(std::string_view db_path)
    : owned_(std::make_unique<database>(db_path)), db_(*owned_) {}

std::vector<Session> SessionManager::most_recent(std::size_t count) const {
  return db_.sessions_by_recent(count);
}

std::optional<Session> SessionManager::get(std::uint32_t id) const { return db_.session(id); }

std::optional<MessageManager> SessionManager::get_messages(std::uint32_t id) const {
  if (!db_.session_exists(id)) {
    return {};
  }

  return MessageManager(db_, id);
}

std::uint32_t SessionManager::create(const Session &session) { return db_.insert_session(session); }

void SessionManager::save(const Session &session) { db_.update_session(session); }

void SessionManager::remove(std::uint32_t id) { db_.remove_session(id); }

std::vector<Message> SessionManager::most_recent_after_assistant_memory_watermark() const {
  return db_.messages_after_assistant_memory_watermark();
}

std::unordered_map<std::uint32_t, std::string> SessionManager::session_last_times() const {
  return db_.session_last_times();
}

std::optional<std::string> SessionManager::last_message_time(std::uint32_t id) const {
  return db_.last_message_time(id);
}

void SessionManager::set_reasoning_effort(std::uint32_t id, const std::string& level) {
  db_.set_session_reasoning_effort(id, level);
}

bool SessionManager::remove_message(std::uint32_t id) {
  return db_.remove_message(id);
}

void SessionManager::update_message(std::uint32_t id, const std::string& content) {
  db_.update_message(id, content);
}

database& SessionManager::database_for_update() { return db_; }

MessageManager::MessageManager(database &db, std::uint32_t session_id)
    : db_(db), session_id_(session_id) {}

std::vector<Message> MessageManager::most_recent(std::size_t count) const {
  return db_.recent_messages(session_id_, count);
}

std::vector<Message> MessageManager::all() const { return db_.messages(session_id_); }

std::size_t MessageManager::count() const { return db_.message_count(session_id_); }

std::vector<Message> MessageManager::all_messages_after_summary_watermark() const {
  return db_.messages_after_summary_watermark(session_id_);
}

std::uint32_t MessageManager::append(Role role, const std::string &content) {
  Message message;
  message.session_id_ = session_id_;
  message.role_ = role;
  message.content_ = content;
  message.made_at_ = shark::str::now_iso8601_utc();
  return db_.insert_message(message);
}

} // namespace lnb::core
