#include "store/managers.hpp"

#include "store/util.hpp"

namespace lnb::store {

std::vector<Session> SessionManager::most_recent(std::size_t count) const {
  return db_.sessions_by_recent(count);
}

std::optional<Session> SessionManager::get(std::uint32_t id) const {
  return db_.session(id);
}

std::uint32_t SessionManager::create(const Session& session) {
  return db_.insert_session(session);
}

void SessionManager::save(const Session& session) {
  db_.update_session(session);
}

void SessionManager::remove(std::uint32_t id) {
  db_.remove_session(id);
}

MessageManager::MessageManager(database& db, std::uint32_t session_id)
  : db_(db), session_id_(session_id) {
}

std::vector<Message> MessageManager::most_recent(std::size_t count) const {
  return db_.recent_messages(session_id_, count);
}

std::vector<Message> MessageManager::all() const {
  return db_.messages(session_id_);
}

std::size_t MessageManager::count() const {
  return db_.message_count(session_id_);
}

std::uint32_t MessageManager::append(Role role, const std::string& content) {
  Message message;
  message.session_id_ = session_id_;
  message.role_ = role;
  message.content_ = content;
  message.made_at_ = now_iso8601_utc();
  return db_.insert_message(message);
}

} // namespace lnb::store
