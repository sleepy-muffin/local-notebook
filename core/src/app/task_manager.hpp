#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

#include "local-notebook-core/model.hpp"
#include "local-notebook-core/generation_task.hpp"

namespace lnb {

class Application;

// What a background job is doing. The kind is the tasks window's label and it
// chooses the slot the task occupies: the shared assistant slot, or a per-session
// one (a turn and a session summary share a session's slot, so they can never run
// together on the same session).
enum class TaskKind {
  Turn,
  SessionSummary,
  AssistantSummary,
};

// One background job: the GenerationTask the worker publishes its progress and
// diagnostics into, plus the std::jthread that runs it. It is owned through a
// shared_ptr by the TaskManager (which keeps it until the slot is reused) and by
// the widget that started it (which watches its state). The worker's only channel
// to the interface is the GenerationTask's own locking.
class Task {
public:
  Task(std::uint64_t id, TaskKind kind, std::uint32_t session_id);
  ~Task();

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  [[nodiscard]] std::uint64_t id() const { return id_; }
  [[nodiscard]] TaskKind kind() const { return kind_; }
  // The owning session, or 0 for the shared assistant task.
  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }

  [[nodiscard]] GenerationTask::Snapshot snapshot() const { return state_.snapshot(); }
  [[nodiscard]] std::string log_text() const { return state_.log_text(); }

  // True once the task reached a terminal state (completed, cancelled or failed).
  // A cancelled task reports terminal as soon as the stop is requested, even
  // though its worker may still be winding down.
  [[nodiscard]] bool terminal() const { return state_.is_terminal(); }

  // True once the worker thread has exited. A slot is only reusable once its
  // task has finished, so "finished" is what the manager gates a new submit on.
  [[nodiscard]] bool finished() const {
    return finished_.load(std::memory_order_acquire);
  }

  // Runs `work` on a new thread, then records the outcome (complete, or fail on
  // an exception). Must be called exactly once, immediately after construction.
  void start(std::function<void(GenerationTask&, std::stop_token)> work);

  // Marks the task cancelled and asks the worker to stop. Both stop sources are
  // requested: the state's (which the network client polls) and the thread's
  // (which drives the request watchdog), so an in-flight request is aborted.
  void cancel();

  // Joins the worker, but only once it has exited, so this never blocks. Used to
  // reclaim a finished thread from a timer.
  void reap();

  // Cancels and joins unconditionally: blocks only as long as the worker takes
  // to observe the stop request.
  void shutdown();

private:
  std::uint64_t id_;
  TaskKind kind_;
  std::uint32_t session_id_;

  GenerationTask state_;
  // Set by the worker as its very last action, so reap() can tell the thread has
  // exited without blocking on it.
  std::atomic<bool> finished_{false};
};

// The single owner of every background job and the single place the "what should
// the composer's primary action be?" policy lives. Widgets never start a worker
// directly and never decide on their own whether a send is allowed: they ask the
// manager, and every task they start goes through submit(), so the tasks window
// sees them all without special cases.
//
// Everything is driven from the interface thread; the worker threads only touch
// their own task's locking state.
class TaskManager {
public:
  // The composer's one primary action for a session, decided centrally rather
  // than by each widget: fold this session's overflow, fold the shared assistant
  // memory, or send a turn.
  enum class Action {
    Send,
    SummarizeSession,
    SummarizeAssistant,
  };

  explicit TaskManager(Application& app);
  ~TaskManager();

  TaskManager(const TaskManager&) = delete;
  TaskManager& operator=(const TaskManager&) = delete;

  using Work = std::function<void(GenerationTask&, std::stop_token)>;

  // The protection layer and the single entry point every task goes through.
  // Returns the started task, or nullptr when refused:
  //   - a Turn while primary_action(session_id) != Send, so a send can never race
  //     a fold the triggers have already called for;
  //   - any task while its slot (the session's, or the shared assistant's) still
  //     holds a task that has not finished.
  [[nodiscard]] std::shared_ptr<Task> submit(
      TaskKind kind, std::uint32_t session_id, Work work
  );

  // The central decision, in priority order: this session's own transcript
  // overflow, then the shared assistant trigger (or an assistant fold already in
  // flight), then a plain send.
  [[nodiscard]] Action primary_action(std::uint32_t session_id) const;
  [[nodiscard]] Action primary_action(
      const Session& session, const std::vector<Message>& messages
  ) const;

  // True while a task is in flight for `session_id` (its worker has not exited).
  [[nodiscard]] bool busy(std::uint32_t session_id) const;
  // True while the shared assistant fold is in flight; every composer is then
  // barred from sending.
  [[nodiscard]] bool assistant_busy() const;
  // The number of tasks whose workers are still running.
  [[nodiscard]] std::size_t running_count() const;

  // One task per occupied slot (running, or the last finished one), for the tasks
  // window. Finished workers are reclaimed as a side effect.
  [[nodiscard]] std::vector<std::shared_ptr<Task>> active_tasks();

  [[nodiscard]] std::shared_ptr<Task> find(std::uint64_t id) const;
  [[nodiscard]] std::string log_text(std::uint64_t id) const;

  // Requests a stop for the task with this id. False when no such task is kept.
  bool cancel(std::uint64_t id);

  // Drops the task kept for a deleted session (cancelling and joining it).
  void forget_session(std::uint32_t session_id);

  // Cancels and joins every task. Called at shutdown.
  void shutdown();

private:
  Application& app_;

  std::uint64_t next_id_ = 1;

  // At most one task per slot: the shared assistant memory, and one per session.
  std::unordered_map<std::uint32_t, std::shared_ptr<Task>> sessions_;
  std::shared_ptr<Task> assistant_;
};

} // namespace lnb
