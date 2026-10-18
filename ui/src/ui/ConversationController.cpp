#include "ui/ConversationController.hpp"

#include <utility>

namespace ui {

ConversationController::ConversationController(
    lnb::api::interface& api, std::uint32_t session_id
)
    : api_(api), session_id_(session_id) {}

ConversationController::~ConversationController() {
  // The interface owns the worker; cancelling here only asks it to wind down.
  if (task_ != nullptr) {
    task_->cancel();
  }
}

lnb::Session ConversationController::session() const {
  return api_.session(session_id_).value_or(lnb::Session{});
}

bool ConversationController::busy() const {
  return task_ != nullptr && !task_->is_terminal();
}

ConversationController::Progress ConversationController::progress() const {
  if (task_ == nullptr) {
    return Progress{};
  }

  const GenerationTask::Snapshot snapshot = task_->snapshot();
  return Progress{snapshot.rounds, snapshot.elapsed_ms};
}

bool ConversationController::has_finished_task() const {
  return task_ != nullptr && task_->is_terminal();
}

lnb::GenerationTask::Snapshot ConversationController::last_snapshot() const {
  return task_ != nullptr ? task_->snapshot() : lnb::GenerationTask::Snapshot{};
}

std::string ConversationController::task_log() const {
  return task_ != nullptr ? task_->log_text() : std::string{};
}

bool ConversationController::track(std::shared_ptr<GenerationTask> task) {
  if (task == nullptr) {
    return false;
  }

  task_ = std::move(task);
  reported_ = false;
  last_display_.clear();
  last_status_.clear();
  return true;
}

bool ConversationController::send(std::string text) {
  if (busy() || text.empty()) {
    return false;
  }

  // The interface owns the send policy: it validates the session and both memory
  // triggers, and persists the prompt before the turn starts, so a failed
  // generation still leaves the user's message in the transcript.
  const lnb::api::error_code status = api_.session_prompt(session_id_, text);

  if (status != lnb::api::error_code::SUCCESS) {
    return false;
  }

  if (on_changed_) {
    on_changed_();
  }

  // The prompt is stored; now start the turn. The interface keeps the task alive
  // and runs the worker, so the controller only polls it.
  return track(api_.start_generation());
}

void ConversationController::summarize() {
  if (busy()) {
    return;
  }

  // The interface resolves the level: this session's own overflow first, then
  // the shared assistant trigger.
  const lnb::api::error_code status =
      api_.has_session_memory_trigger_met(session_id_);

  if (status == lnb::api::error_code::ERR_SESSION_MEMORY_TRIGGER_HAS_MET) {
    summarize_session_now();
    return;
  }

  if (status != lnb::api::error_code::ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET) {
    return;
  }

  // The automatic assistant pass is a no-op when the trigger is no longer
  // reached, so it still completes cleanly without contacting the network.
  track(api_.summarize_assistant(false));
}

void ConversationController::summarize_session_now(
    lnb::SummaryApprover approver
) {
  if (busy()) {
    return;
  }

  // Forced, so the manual action folds the overflow regardless of the automatic
  // trigger. The interface snapshots the assistant and session on its worker.
  track(api_.summarize_session(session_id_, true, std::move(approver)));
}

void ConversationController::cancel() {
  // Mark the task terminal and request its stop; the worker observes the task's
  // stop token, which is what drives the network watchdog.
  if (task_ != nullptr) {
    task_->cancel();
  }
}

void ConversationController::delete_message(std::uint32_t message_id) {
  api_.delete_message(message_id);

  if (on_changed_) {
    on_changed_();
  }
}

void ConversationController::edit_message(
    std::uint32_t message_id, std::string content
) {
  api_.edit_message(message_id, std::move(content));

  if (on_changed_) {
    on_changed_();
  }
}

void ConversationController::poll() {
  if (task_ == nullptr) {
    return;
  }

  const GenerationTask::Snapshot snapshot = task_->snapshot();

  if (snapshot.status == GenerationTask::Status::RUNNING &&
      snapshot.display != last_display_) {
    last_display_ = snapshot.display;
    if (on_stream_) {
      on_stream_(snapshot.display);
    }
  }

  // The transient operation title ("Searching...", "Summarizing...") is shown
  // until the first assistant token arrives.
  if (snapshot.status == GenerationTask::Status::RUNNING &&
      snapshot.operation_status != last_status_) {
    last_status_ = snapshot.operation_status;
    if (on_status_) {
      on_status_(snapshot.operation_status);
    }
  }

  if (task_->is_terminal() && !reported_) {
    finish_worker();
  }
}

void ConversationController::finish_worker() {
  // Report the outcome exactly once, before the callbacks can re-enter poll().
  reported_ = true;

  const GenerationTask::Snapshot snapshot = last_snapshot();

  if (snapshot.status == GenerationTask::Status::FAILED && !snapshot.error.empty() &&
      on_error_) {
    on_error_(snapshot.error);
  }

  // A warning is reported for a turn that still completed; the reply is kept and
  // the notice is shown after it.
  if (snapshot.status == GenerationTask::Status::COMPLETED &&
      !snapshot.warning.empty() && on_warning_) {
    on_warning_(snapshot.warning);
  }

  if (on_changed_) {
    on_changed_();
  }
}

} // namespace ui
