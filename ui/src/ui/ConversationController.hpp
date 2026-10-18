#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "local-notebook-core/generation_task.hpp"
#include "local-notebook-core/interface.hpp"
#include "local-notebook-core/model.hpp"

namespace ui {

// The shared worker state lives in lnb; bring it in so the controller reads
// naturally.
using lnb::GenerationTask;

// One session's conversation logic. The application owns a single
// lnb::api::interface, and every controller talks to it: a send is validated and
// persisted by interface::session_prompt(), then the turn itself is driven by
// interface::start_generation(). The controller only polls the task the
// interface returns, so the view stays passive.
class ConversationController {
public:
  ConversationController(lnb::api::interface& api, std::uint32_t session_id);
  ~ConversationController();

  ConversationController(const ConversationController&) = delete;
  ConversationController& operator=(const ConversationController&) = delete;

  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }

  // The task the controller is polling, or nullptr before the first send. It
  // is shared with the interface, so the window can identify which task belongs
  // to which session for the running-task readout.
  [[nodiscard]] std::shared_ptr<GenerationTask> task() const { return task_; }

  // The session as stored (a fresh read).
  [[nodiscard]] lnb::Session session() const;

  // True while the session's task is in flight.
  [[nodiscard]] bool busy() const;

  // Progress of the in-flight turn, for the running-task readout: the tool-call
  // round reached and how long the turn has been running. Both zero when no turn
  // is running.
  struct Progress {
    std::size_t rounds = 0;
    std::int64_t elapsed_ms = 0;
  };

  [[nodiscard]] Progress progress() const;

  // Persists the user message through the interface, then starts the turn. The
  // send is refused (nothing is appended) when the session is busy or the
  // interface rejects the prompt (for example, a memory trigger has been met).
  [[nodiscard]] bool send(std::string text);

  // Resolves one summarisation level per press, the session first.
  void summarize();

  // Folds the session's overflow into memory on demand, forcing the pass even
  // when the automatic trigger has not been reached.
  //
  // `approver` reviews the request before it is sent; the editor supplies its own
  // so a declined review is a silent no-op. An absent approver proceeds.
  void summarize_session_now(lnb::SummaryApprover approver = {});

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
  // finishes, signals a reload.
  void poll();

private:
  // Adopts a task returned by the interface and resets the per-task polling
  // bookkeeping. Returns false when the interface refused to start one.
  bool track(std::shared_ptr<GenerationTask> task);
  void finish_worker();

  lnb::api::interface& api_;
  std::uint32_t session_id_;

  // The turn returned by interface::start_generation(), or the summarisation
  // task the interface started.
  std::shared_ptr<GenerationTask> task_;

  // True once finish_worker() has run for task_, so the terminal state is
  // reported exactly once.
  bool reported_ = false;

  std::string last_display_;
  std::string last_status_;

  std::function<void()> on_changed_;
  std::function<void(const std::string&)> on_stream_;
  std::function<void(const std::string&)> on_status_;
  std::function<void(const std::string&)> on_error_;
  std::function<void(const std::string&)> on_warning_;
};

} // namespace ui
