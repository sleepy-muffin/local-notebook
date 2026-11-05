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

void SessionManager::save(const Session &session) {
  // The active summary lives in the summary table, so the session row no longer
  // carries it. A caller that edits the summary (the session editor) still hands
  // us a Session with summary_ / summary_watermark_ set; apply that change to the
  // table when it differs from the stored version, and otherwise leave the
  // versions alone.
  const auto before = db_.session(session.id_);
  const std::string previous = before ? before->summary_ : std::string{};
  const std::uint32_t previous_watermark =
      before ? before->summary_watermark_ : std::uint32_t{0};

  db_.update_session(session);

  if (session.summary_ == previous && session.summary_watermark_ == previous_watermark) {
    return;
  }

  if (session.summary_.empty()) {
    db_.remove_summaries_from(session.id_, 0);
  } else {
    db_.set_session_summary(session.id_, session.summary_, session.summary_watermark_);
  }
}

void SessionManager::remove(std::uint32_t id) { db_.remove_session(id); }

std::vector<Summary> SessionManager::summaries(std::uint32_t session_id) const {
  return db_.summaries(session_id);
}

bool SessionManager::remove_summary(std::uint32_t summary_id) {
  return db_.remove_summary(summary_id);
}

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
  const auto session_id = db_.message_session(id);

  if (!session_id.has_value()) {
    return false;
  }

  // A message and everything after it go together: a later message without its
  // context is meaningless. The summary versions built on the removed tail are
  // revoked at the same cut, so an older version becomes active again.
  const std::size_t removed = db_.remove_messages_from(*session_id, id);

  if (removed == 0) {
    return false;
  }

  db_.remove_summaries_from(*session_id, id);
  return true;
}

std::uint32_t SessionManager::branch(
    std::uint32_t session_id, std::uint32_t message_id) {
  const auto source = db_.session(session_id);

  if (!source.has_value()) {
    return 0;
  }

  Session copy = *source;
  copy.id_ = 0;
  copy.summary_.clear();
  copy.summary_watermark_ = 0;
  copy.assistant_memory_watermark_ = 0;
  copy.pinned_ = false;

  const std::uint32_t new_id = db_.insert_session(copy);

  if (new_id == 0) {
    return 0;
  }

  // Copy the transcript up to the cut, renumbering the rows. A copied message
  // gets a fresh id, so the summary and assistant-memory watermarks must be
  // remapped through this table rather than copied raw.
  std::unordered_map<std::uint32_t, std::uint32_t> remap;
  std::uint32_t last_copied = 0;

  for (const Message &message: db_.messages(session_id)) {
    if (message_id != 0 && message.id_ > message_id) {
      break;
    }

    Message moved = message;
    moved.session_id_ = new_id;
    const std::uint32_t copied = db_.insert_message(moved);
    remap.emplace(message.id_, copied);
    last_copied = copied;
  }

  // A summary version survives the branch when it was folded from messages that
  // are all inside the copied prefix.
  for (const Summary &summary: db_.summaries(session_id)) {
    if (message_id != 0 && summary.watermark_ > message_id) {
      continue;
    }

    const auto target = remap.find(summary.watermark_);

    if (target == remap.end()) {
      continue;
    }

    Summary moved;
    moved.session_id_ = new_id;
    moved.watermark_ = target->second;
    moved.content_ = summary.content_;
    moved.made_at_ = summary.made_at_;
    (void)db_.insert_summary(moved);
  }

  // Keep the copied prefix out of the assistant fold: map the source frontier
  // onto the copy, and clamp a frontier that pointed past the cut to the last
  // copied message.
  if (source->assistant_memory_watermark_ != 0) {
    std::uint32_t watermark = 0;

    if (message_id == 0 || source->assistant_memory_watermark_ <= message_id) {
      const auto target = remap.find(source->assistant_memory_watermark_);
      watermark = target != remap.end() ? target->second : 0;
    } else {
      watermark = last_copied;
    }

    if (watermark != 0) {
      db_.set_assistant_memory_watermark(new_id, watermark);
    }
  }

  return new_id;
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
