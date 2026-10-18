#include "ui/ConversationController.hpp"

#include <utility>

#include "app/application.hpp"
#include "app/task_manager.hpp"
#include "net/client.hpp"
#include "net/protocol.hpp"
#include "ui/SummaryReviewDialog.hpp"

namespace ui {
namespace {

// Every request this controller makes (the turn and both folds) shares one
// description: the endpoint derived from the assistant's base URL and protocol,
// plus its key, proxy and model. Building it in one place keeps the protocol
// choice and the URL derivation identical across the turn and the summarisers.
lnb::net::Remote remote_for(const lnb::Assistant& assistant) {
  lnb::net::Remote remote;
  remote.url = lnb::net::derive_endpoint(assistant.api_url_, assistant.protocol_);
  remote.key = assistant.authentication_token_;
  remote.proxy = assistant.proxy_;
  remote.model = assistant.model_;
  remote.protocol = assistant.protocol_;
  return remote;
}

} // namespace

ConversationController::ConversationController(
    lnb::Application& app, std::uint32_t session_id
)
    : app_(app), session_id_(session_id) {}

ConversationController::~ConversationController() {
  // The worker captures `this`, so it must be gone before our members are.
  if (task_ != nullptr) {
    task_->shutdown();
  }
}

lnb::Session ConversationController::session() const {
  const auto stored = app_.db().session(session_id_);
  return stored.value_or(lnb::Session{});
}

bool ConversationController::busy() const {
  return task_ != nullptr && !task_->finished();
}

ConversationController::Progress ConversationController::progress() const {
  if (task_ == nullptr) {
    return Progress{};
  }

  const GenerationTask::Snapshot snapshot = task_->snapshot();
  return Progress{snapshot.rounds, snapshot.elapsed_ms};
}

bool ConversationController::has_finished_task() const {
  return task_ != nullptr && task_->terminal();
}

lnb::GenerationTask::Snapshot ConversationController::last_snapshot() const {
  return task_ != nullptr ? task_->snapshot() : lnb::GenerationTask::Snapshot{};
}

std::string ConversationController::task_log() const {
  return task_ != nullptr ? task_->log_text() : std::string{};
}

bool ConversationController::send(std::string text) {
  if (busy() || text.empty()) {
    return false;
  }

  // The manager owns the decision of whether a send is allowed right now: a send
  // is refused while this session's overflow, or the shared assistant memory, is
  // pending a fold. This is the last line of defence behind the composer's own
  // mode button.
  if (app_.tasks().primary_action(session_id_) != lnb::TaskManager::Action::Send) {
    return false;
  }

  app_.messages(session_id_).append(lnb::Role::User, text);

  if (on_changed_) {
    on_changed_();
  }

  start(lnb::TaskKind::Turn, [this](GenerationTask& task, std::stop_token stop_token) {
    // Snapshot the assistant and session so the worker never reads state the UI
    // thread may be mutating.
    const lnb::Assistant assistant = app_.assistant();
    lnb::Session current = session();

    const lnb::net::Remote remote = remote_for(assistant);

    // Best-effort session summarisation before the turn, so the request carries
    // the freshest memory. A failure never blocks the reply. The user reviews
    // the request first; declining leaves the memory untouched.
    const std::string folded = lnb::net::maybe_summarize_session(
        app_.db(), current, remote, task, stop_token, review_summary
    );
    if (!folded.empty()) {
      current.memory_ = folded;
    }

    const std::vector<lnb::Message> messages = app_.db().messages(session_id_);
    lnb::net::Request request =
        lnb::net::build_session_request(assistant, current, messages);

    const lnb::net::ChatCompletionResult result = lnb::net::generate_reply(
        std::move(request), remote, task, stop_token, current.shared_directory_,
        current
    );

    // A cancelled turn keeps nothing; a completed one stores the reply.
    if (!stop_token.stop_requested() && !result.text.empty()) {
      app_.messages(session_id_).append(lnb::Role::Assistant, result.text);
    }

    // Assistant-level memory is shared across sessions, so it is folded after
    // the turn and applied to the assistant on the UI thread. It is reviewed
    // first, like the session pass above. This automatic pass stays best-effort:
    // a failure never fails the user's turn.
    const std::string assistant_memory = lnb::net::maybe_summarize_assistant(
        app_.db(), assistant, remote, task, stop_token, review_summary
    );
    if (!assistant_memory.empty()) {
      std::lock_guard lock(result_mutex_);
      pending_assistant_memory_ = assistant_memory;
    }
  });

  return task_ != nullptr;
}

void ConversationController::summarize() {
  if (busy()) {
    return;
  }

  // The manager resolves the level: a session overflow (folded first) or the
  // shared assistant memory. Choosing here — rather than inside the worker — is
  // what lets the fold occupy the right slot, so the assistant level shows up in
  // the tasks window as the shared assistant task.
  const lnb::TaskManager::Action action = app_.tasks().primary_action(session_id_);

  if (action == lnb::TaskManager::Action::SummarizeSession) {
    start(
        lnb::TaskKind::SessionSummary,
        [this](GenerationTask& task, std::stop_token stop_token) {
      // Snapshot the assistant and session so the worker never reads state the UI
      // thread may be mutating.
      const lnb::Assistant assistant = app_.assistant();
      const lnb::Session current = session();

      const lnb::net::Remote remote = remote_for(assistant);

      // The user reviews the request first; declining leaves the memory untouched.
      lnb::net::summarize_session_now(
          app_.db(), current, remote, task, stop_token, review_summary
      );
    });
    return;
  }

  // No session overflow this press: the press was offered by the shared assistant
  // trigger (or a fold is already due), so fold the assistant memory.
  start(
      lnb::TaskKind::AssistantSummary,
      [this](GenerationTask& task, std::stop_token stop_token) {
    // Snapshot the assistant so the worker never reads state the UI thread may be
    // mutating.
    const lnb::Assistant assistant = app_.assistant();

    // Nothing to fold (the trigger was not reached): a clean no-op that still
    // completes, so a press with nothing due never contacts the network.
    if (!lnb::net::assistant_trigger_reached(app_.db(), assistant, stop_token)) {
      return;
    }

    const lnb::net::Remote remote = remote_for(assistant);

    // The user asked for the fold, so a failure is reported (in the tasks window)
    // rather than swallowed: unlike the automatic post-turn pass, this one is
    // strict.
    const std::string assistant_memory = lnb::net::summarize_assistant_now(
        app_.db(), assistant, remote, task, stop_token, review_summary
    );

    if (!assistant_memory.empty()) {
      std::lock_guard lock(result_mutex_);
      pending_assistant_memory_ = assistant_memory;
    }
  });
}

void ConversationController::summarize_session_now(
    std::function<bool(const lnb::net::SummaryPlan&)> approver
) {
  if (busy()) {
    return;
  }

  start(
      lnb::TaskKind::SessionSummary,
      [this, approver = std::move(approver)](
          GenerationTask& task, std::stop_token stop_token
      ) {
    // Snapshot the assistant and session so the worker never reads state the UI
    // thread may be mutating.
    const lnb::Assistant assistant = app_.assistant();
    const lnb::Session current = session();

    // An absent approver reviews interactively, exactly like the composer path;
    // the editor passes its own so it can tell a deliberate decline apart from a
    // real fold.
    lnb::net::SummaryApprover reviewer = approver;

    if (!reviewer) {
      reviewer = review_summary;
    }

    const lnb::net::Remote remote = remote_for(assistant);

    // Forced, so the manual action folds the overflow regardless of the automatic
    // trigger. Only the session memory is touched: the shared assistant memory is
    // left to its own trigger, matching the editor's intent.
    lnb::net::summarize_session_now(
        app_.db(), current, remote, task, stop_token, reviewer
    );
  });
}

void ConversationController::cancel() {
  // Mark the task terminal so the interface stops showing it as running, then
  // stop the worker thread itself. The worker observes the thread's stop token
  // (that token is what drives the network watchdog which aborts the in-flight
  // request), so requesting only the task's stop would leave the request running.
  if (task_ != nullptr) {
    task_->cancel();
  }
}

void ConversationController::delete_message(std::uint32_t message_id) {
  app_.db().remove_message(message_id);
  // Removing a message can only pull the assistant trigger back below its limit,
  // so the cached answer must be recomputed.
  app_.mark_assistant_summary_stale();
  if (on_changed_) {
    on_changed_();
  }
}

void ConversationController::edit_message(
    std::uint32_t message_id, std::string content
) {
  app_.db().update_message(message_id, content);
  // An edit can change the message's token cost, so the cached trigger may be
  // stale.
  app_.mark_assistant_summary_stale();
  if (on_changed_) {
    on_changed_();
  }
}

void ConversationController::start(
    lnb::TaskKind kind,
    std::function<void(GenerationTask&, std::stop_token)> work
) {
  // The previous task's worker must be gone before its captures (this controller)
  // could outlive it. Cancellation is prompt (the worker's heavy loops observe the
  // stop token), so this is at most a short wait.
  if (task_ != nullptr) {
    task_->shutdown();
    task_.reset();
  }

  {
    // A late assistant-memory result from a cancelled task must not leak into the
    // next one.
    std::lock_guard lock(result_mutex_);
    pending_assistant_memory_.clear();
  }

  reported_ = false;
  last_display_.clear();
  last_status_.clear();

  auto submitted = app_.tasks().submit(kind, session_id_, std::move(work));

  if (submitted != nullptr) {
    task_ = std::move(submitted);
  }
}

void ConversationController::poll() {
  if (task_ == nullptr) {
    return;
  }

  // Complete a deferred join from a previous cancelled task, so the thread is
  // always reclaimed even though the interface never waited for it.
  task_->reap();

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

  if (task_->terminal() && !reported_) {
    finish_worker();
  }
}

void ConversationController::finish_worker() {
  // Report the outcome exactly once, before the callbacks can re-enter poll().
  reported_ = true;

  // The worker may still be winding down after a cancel, so the join is deferred
  // to a later poll (reap) instead of freezing the interface here.
  task_->reap();

  const GenerationTask::Snapshot snapshot = last_snapshot();

  std::string assistant_memory;
  {
    std::lock_guard lock(result_mutex_);
    assistant_memory.swap(pending_assistant_memory_);
  }

  if (!assistant_memory.empty()) {
    app_.assistant().memory_ = assistant_memory;
    app_.save_assistant();
  }

  // The task may have added messages and advanced watermarks, so the cached
  // assistant trigger is stale. Recomputing lazily keeps this per-task step cheap.
  app_.mark_assistant_summary_stale();

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
