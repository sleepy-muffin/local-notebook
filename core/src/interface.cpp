#include "local_notebook/core/interface.hpp"

#include "application.hpp"
#include "local_notebook/core/utils.hpp"
#include "net/protocol.hpp"

namespace lnb::core {

interface::interface(std::filesystem::path store_root)
    : application_(std::make_unique<core::application>(std::move(store_root))) {}

interface::~interface() = default;

bool interface::open_assistant(std::string uuid) {
  return application_->open_assistant(std::move(uuid));
}

std::string interface::last_error() const { return application_->last_error(); }

std::vector<std::pair<std::string, Assistant>> interface::list_assistants() { return application_->list_assistants(); }

bool interface::has_assistant() const { return application_->has_assistant(); }

std::string interface::assistant_uuid() const { return application_->assistant_uuid(); }

Assistant interface::assistant() const { return application_->assistant(); }

void interface::set_assistant(Assistant assistant) {
  application_->set_assistant(std::move(assistant));
}

void interface::set_assistant_memory(std::string memory) {
  application_->set_assistant_memory(std::move(memory));
}

void interface::save_assistant() { application_->save_assistant(); }

const store& interface::get_store() const {
  return application_->get_store();
}

std::optional<Session> interface::session(std::uint32_t session_id) const {
  return application_->session(session_id);
}

std::vector<Message> interface::messages(std::uint32_t session_id) const {
  return application_->messages(session_id);
}

std::vector<Message> interface::recent_messages(std::uint32_t session_id, std::size_t count) const {
  return application_->recent_messages(session_id, count);
}

std::vector<Session> interface::recent_sessions(std::size_t count) const {
  return application_->recent_sessions(count);
}

std::unordered_map<std::uint32_t, std::string> interface::session_last_times() const {
  return application_->session_last_times();
}

std::optional<std::string> interface::last_message_time(std::uint32_t session_id) const {
  return application_->last_message_time(session_id);
}

ContextUsage interface::context_usage(std::uint32_t session_id) const {
  return application_->context_usage(session_id);
}

std::uint32_t interface::create_session(const Session &session) {
  return application_->create_session(session);
}

void interface::save_session(const Session &session) { application_->save_session(session); }

void interface::delete_session(std::uint32_t session_id) {
  application_->delete_session(session_id);
}

void interface::set_session_reasoning_effort(std::uint32_t session_id, std::string level) {
  application_->set_session_reasoning_effort(session_id, std::move(level));
}

void interface::append_message(std::uint32_t session_id, Role role, std::string content) {
  application_->append_message(session_id, role, std::move(content));
}

void interface::delete_message(std::uint32_t message_id) {
  application_->delete_message(message_id);
}

void interface::edit_message(std::uint32_t message_id, std::string content) {
  application_->edit_message(message_id, std::move(content));
}

std::size_t interface::message_tokens(const Message &message) const {
  return application_->message_tokens(message);
}

std::string interface::default_reasoning_effort() const {
  return application_->default_reasoning_effort();
}

std::vector<std::string> interface::reasoning_efforts_with_stored(const std::string &stored) const {
  return application_->reasoning_efforts_with_stored(stored);
}

bool interface::busy() const { return application_->busy(); }

bool interface::session_busy(std::uint32_t session_id) const {
  return application_->session_busy(session_id);
}

bool interface::assistant_busy() const { return application_->assistant_busy(); }

std::shared_ptr<GenerationTask> interface::assistant_task() const {
  return application_->assistant_task();
}

bool interface::assistant_summary_due() const { return application_->assistant_summary_due(); }

void interface::mark_assistant_summary_stale() { application_->mark_assistant_summary_stale(); }

error_code interface::has_session_summary_trigger_met(std::uint32_t session_id) const {
  return application_->has_session_summary_trigger_met(session_id);
}

error_code interface::session_prompt(std::uint32_t session_id, const std::string &prompt) {
  return application_->session_prompt(session_id, prompt);
}

std::shared_ptr<GenerationTask>
interface::start_generation(std::uint32_t session_id, SummaryApprover approver) {
  return application_->start_generation(session_id, std::move(approver));
}

std::shared_ptr<GenerationTask>
interface::summarize_session(std::uint32_t session_id, bool force, SummaryApprover approver) {
  return application_->summarize_session(session_id, force, std::move(approver));
}

std::shared_ptr<GenerationTask>
interface::summarize_assistant(bool force, SummaryApprover approver) {
  return application_->summarize_assistant(force, std::move(approver));
}

void interface::cancel_session(std::uint32_t session_id) {
  application_->cancel_session(session_id);
}

void interface::cancel_assistant() { application_->cancel_assistant(); }

void interface::wait_for_generation() { application_->wait_for_generation(); }

std::string reasoning_default_for(const std::string &model) {
  return default_reasoning_effort(model);
}

std::vector<std::string> reasoning_levels_for(const std::string &model, const std::string &stored) {
  return reasoning_efforts_with_stored(model, stored);
}

} // namespace lnb::core
