#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "model.hpp"
#include "support/generation_task.hpp"

namespace lnb {
class Application;
class Task;
enum class TaskKind;

namespace net {
struct SummaryPlan;
}
}

namespace ui {

// The shared worker state lives in lnb; bring it in so the controller reads
// naturally.
using lnb::GenerationTask;

// One session's conversation logic. It routes the session's background work (a
// turn, a session fold) through the application's TaskManager, which owns the
// worker and the state, so the view stays passive and every task is visible to
// the tasks window. The view renders the snapshot the controller exposes and
// forwards intent through the callbacks below.
class ConversationController {
public:
  ConversationController(lnb::Application& app, std::uint32_t session_id);
  ~ConversationController();

  ConversationController(const ConversationController&) = delete;
  ConversationController& operator=(const ConversationController&) = delete;

  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }

  // The session as stored (a fresh read).
  [[nodiscard]] lnb::Session session() const;

  // True while the session's task is in flight (its worker has not exited).
  [[nodiscard]] bool busy() const;

  // Progress of the in-flight turn, for the running-task readout: the tool-call
  // round reached and how long the turn has been running. Both zero when no turn
  // is running.
  struct Progress {
    std::size_t rounds = 0;
    std::int64_t elapsed_ms = 0;
  };

  [[nodiscard]] Progress progress() const;

  // Appends the user message, then starts a turn in the background. Returns
  // false — and appends nothing — when refused: the session's task is still
  // winding down, or the manager's primary action for this session is not a plain
  // send (a fold is pending), so a message can never race a fold.
  [[nodiscard]] bool send(std::string text);

  // Resolves one summarisation level per press, the session first. Used when the
  // composer's primary action is the "summarize" variant, which replaces "Send"
  // once either the session's transcript or the assistant's cross-session trigger
  // has been reached. When this session's own trigger has been reached, the press
  // folds its overflow and persists it; otherwise the press folds the shared
  // assistant memory (its cross-session trigger was what offered the action). No
  // chat request follows, so it is the sole action of the turn. After a session
  // fold the button stays a summarize action while the assistant trigger is still
  // met, so a second press resolves the assistant level.
  void summarize();

  // Folds the session's overflow into memory on demand, forcing the pass even when
  // the automatic trigger has not been reached. Unlike summarize(), only the
  // session pass runs — the shared assistant memory is left to its own trigger —
  // which is exactly the session editor's "fold this transcript now" action. It is
  // serialised with the session's turn, so a manual fold can never run beside a
  // generation on the same session.
  //
  // `approver` reviews the request before it is sent; the editor supplies its own
  // so a declined review is a silent no-op. An absent approver reviews
  // interactively, exactly like the composer path.
  void summarize_session_now(
      std::function<bool(const lnb::net::SummaryPlan&)> approver = {}
  );

  // The last finished task's outcome and diagnostic log, retained after the worker
  // is gone so the tasks window can show a failure and dump its log.
  [[nodiscard]] bool has_finished_task() const;
  [[nodiscard]] lnb::GenerationTask::Snapshot last_snapshot() const;

  // The diagnostic log for the current task, or the last finished one when idle.
  [[nodiscard]] std::string task_log() const;

  // Requests the worker to stop. The partial reply is discarded.
  void cancel();

  void delete_message(std::uint32_t message_id);
  void edit_message(std::uint32_t message_id, std::string content);

  void set_on_changed(std::function<void()> handler) {
    on_changed_ = std::move(handler);
  }
  void set_on_stream(std::function<void(const std::string&)> handler) {
    on_stream_ = std::move(handler);
  }
  void set_on_status(std::function<void(const std::string&)> handler) {
    on_status_ = std::move(handler);
  }
  void set_on_error(std::function<void(const std::string&)> handler) {
    on_error_ = std::move(handler);
  }
  // A non-fatal notice about a turn that still completed (for example, the reply
  // was cut off). Shown like an error, but the reply itself is kept.
  void set_on_warning(std::function<void(const std::string&)> handler) {
    on_warning_ = std::move(handler);
  }

  // The UI timer calls this. It forwards streamed text and, once the task
  // finishes, persists the assistant memory and signals a reload.
  void poll();

private:
  void start(
      lnb::TaskKind kind,
      std::function<void(GenerationTask&, std::stop_token)> work
  );
  void finish_worker();

  lnb::Application& app_;
  std::uint32_t session_id_;

  // The session's current (or last finished) task, owned by the task manager so
  // the tasks window and the task indicator see it too.
  std::shared_ptr<lnb::Task> task_;
  // True once finish_worker() has run for task_, so the terminal state is
  // reported exactly once.
  bool reported_ = false;

  mutable std::mutex result_mutex_;
  std::string pending_assistant_memory_;

  std::string last_display_;
  std::string last_status_;

  std::function<void()> on_changed_;
  std::function<void(const std::string&)> on_stream_;
  std::function<void(const std::string&)> on_status_;
  std::function<void(const std::string&)> on_error_;
  std::function<void(const std::string&)> on_warning_;
};

} // namespace ui
