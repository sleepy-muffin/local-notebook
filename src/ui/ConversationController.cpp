#include "ui/ConversationController.hpp"

#include "app/application.hpp"
#include "net/client.hpp"
#include "support/i18n.hpp"
#include "support/log.hpp"
#include "ui/SummaryReviewDialog.hpp"

#include <exception>
#include <utility>

namespace ui {

ConversationController::ConversationController(
    lnb::Application& app, std::uint32_t session_id
)
    : app_(app), session_id_(session_id) {}

ConversationController::~ConversationController() {
  if (worker_.joinable()) {
    task_.cancel();
    worker_.request_stop();
    worker_.join();
  }
}

lnb::Session ConversationController::session() const {
  const auto stored = app_.db().session(session_id_);
  return stored.value_or(lnb::Session{});
}

ConversationController::Progress ConversationController::progress() const {
  const GenerationTask::Snapshot snapshot = task_.snapshot();
  return Progress{snapshot.rounds, snapshot.elapsed_ms};
}

bool ConversationController::has_finished_task() const {
  std::lock_guard lock(result_mutex_);
  return last_snapshot_.has_value();
}

lnb::GenerationTask::Snapshot ConversationController::last_snapshot() const {
  std::lock_guard lock(result_mutex_);
  return last_snapshot_.value_or(lnb::GenerationTask::Snapshot{});
}

std::string ConversationController::task_log() const {
  if (running_) {
    return task_.log_text();
  }

  std::lock_guard lock(result_mutex_);
  return last_task_log_;
}

void ConversationController::send(std::string text) {
  if (running_ || text.empty()) {
    return;
  }

  app_.messages(session_id_).append(lnb::Role::User, text);

  if (on_changed_) {
    on_changed_();
  }

  start([this](GenerationTask& task, std::stop_token stop_token) {
    // Snapshot the assistant and session so the worker never reads state the UI
    // thread may be mutating.
    const lnb::Assistant assistant = app_.assistant();
    lnb::Session current = session();

    const std::string& model = assistant.model_;
    const std::string& url = assistant.api_url_;
    const std::string& key = assistant.authentication_token_;

    // Best-effort session summarisation before the turn, so the request carries
    // the freshest memory. A failure never blocks the reply. The user reviews
    // the request first; declining leaves the memory untouched.
    const std::string folded = lnb::net::maybe_summarize_session(
        app_.db(), current, model, url, key, assistant.proxy_, task, stop_token,
        review_summary
    );
    if (!folded.empty()) {
      current.memory_ = folded;
    }

    const std::vector<lnb::Message> messages = app_.db().messages(session_id_);
    QJsonObject payload = lnb::net::build_session_payload(assistant, current, messages);

    const lnb::net::ChatCompletionResult result = lnb::net::generate_reply(
        std::move(payload), task, stop_token, url, key, assistant.proxy_,
        current.shared_directory_, current
    );

    // A cancelled turn keeps nothing; a completed one stores the reply.
    if (!stop_token.stop_requested() && !result.text.empty()) {
      app_.messages(session_id_).append(lnb::Role::Assistant, result.text);
    }

    // Assistant-level memory is shared across sessions, so it is folded after
    // the turn and applied to the assistant on the UI thread. It is reviewed
    // first, like the session pass above.
    const std::string assistant_memory = lnb::net::maybe_summarize_assistant(
        app_.db(), assistant, model, url, key, assistant.proxy_, task, stop_token,
        review_summary
    );
    if (!assistant_memory.empty()) {
      std::lock_guard lock(result_mutex_);
      pending_assistant_memory_ = assistant_memory;
    }
  });
}

void ConversationController::summarize() {
  if (running_) {
    return;
  }

  start([this](GenerationTask& task, std::stop_token stop_token) {
    // Snapshot the assistant and session so the worker never reads state the UI
    // thread may be mutating.
    const lnb::Assistant assistant = app_.assistant();
    const lnb::Session current = session();

    // A manual pass folds the overflow into the session memory and persists it.
    // It is the sole action of this turn: no chat request follows. The user
    // reviews the request first; declining leaves the memory untouched.
    lnb::net::summarize_session_now(
        app_.db(), current, assistant.model_, assistant.api_url_,
        assistant.authentication_token_, assistant.proxy_, task, stop_token,
        review_summary
    );
  });
}

void ConversationController::cancel() {
  task_.cancel();
}

void ConversationController::delete_message(std::uint32_t message_id) {
  app_.db().remove_message(message_id);
  if (on_changed_) {
    on_changed_();
  }
}

void ConversationController::edit_message(
    std::uint32_t message_id, std::string content
) {
  app_.db().update_message(message_id, content);
  if (on_changed_) {
    on_changed_();
  }
}

void ConversationController::start(
    std::function<void(GenerationTask&, std::stop_token)> work
) {
  // The previous worker must be gone before its task and stop source are reused.
  // Cancellation is prompt (the worker's heavy loops observe the stop token, and
  // reap_worker() never blocks), so this is at most a short wait.
  if (worker_.joinable()) {
    worker_.join();
  }

  {
    // A late assistant-memory result from a cancelled turn must not leak into
    // the next one; nor may the previous turn's retained outcome survive it.
    std::lock_guard lock(result_mutex_);
    pending_assistant_memory_.clear();
    last_snapshot_.reset();
    last_task_log_.clear();
  }

  worker_finished_.store(false, std::memory_order_relaxed);

  task_.reset();
  task_.start();
  running_ = true;
  last_display_.clear();
  last_status_.clear();

  worker_ = std::jthread([this, work = std::move(work)](
                             std::stop_token stop_token
                         ) {
    try {
      work(task_, stop_token);
      task_.complete();
    } catch (const std::exception& error) {
      // task_.fail is a no-op when the task was cancelled, which is what we
      // want: a cancelled turn reports nothing.
      task_.fail(error.what());
    } catch (...) {
      task_.fail(trs("The request failed for an unknown reason.").toStdString());
    }

    // The thread's last action, so reap_worker() can join it without blocking.
    worker_finished_.store(true, std::memory_order_release);
  });
}

void ConversationController::reap_worker() {
  if (worker_.joinable() && worker_finished_.load(std::memory_order_acquire)) {
    worker_.join();
  }
}

void ConversationController::poll() {
  // Complete a deferred join from a previous cancelled turn, so the thread is
  // always reclaimed even though the interface never waited for it.
  reap_worker();

  if (!running_) {
    return;
  }

  const GenerationTask::Snapshot snapshot = task_.snapshot();

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

  if (task_.is_terminal()) {
    finish_worker();
  }
}

void ConversationController::finish_worker() {
  // The worker may still be winding down after a cancel, so the join is deferred
  // to a later poll (reap_worker) instead of freezing the interface here.
  reap_worker();

  running_ = false;

  const GenerationTask::Snapshot snapshot = task_.snapshot();

  // Retain the outcome and the diagnostic log, so the tasks window can still
  // show a failed (or cancelled) turn, with its reasoning, after the worker has
  // gone and the task is about to be reused.
  {
    std::lock_guard lock(result_mutex_);
    last_snapshot_ = snapshot;
    last_task_log_ = task_.log_text();
  }

  std::string assistant_memory;
  {
    std::lock_guard lock(result_mutex_);
    assistant_memory.swap(pending_assistant_memory_);
  }

  if (!assistant_memory.empty()) {
    app_.assistant().memory_ = assistant_memory;
    app_.save_assistant();
  }

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
