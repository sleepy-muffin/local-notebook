#include "app/task_manager.hpp"

#include <exception>
#include <utility>

#include "app/application.hpp"
#include "net/client.hpp"
#include "support/i18n.hpp"

namespace lnb {

Task::Task(std::uint64_t id, TaskKind kind, std::uint32_t session_id)
    : id_(id), kind_(kind), session_id_(session_id) {}

Task::~Task() {
  shutdown();
}

void Task::start(std::function<void(GenerationTask&, std::stop_token)> work) {
  state_.start();

  worker_ = std::jthread([this, work = std::move(work)](std::stop_token stop_token) {
    try {
      work(state_, stop_token);
      state_.complete();
    } catch (const std::exception& error) {
      // fail() is a no-op once the task was cancelled, which is what we want: a
      // cancelled task reports nothing.
      state_.fail(error.what());
    } catch (...) {
      state_.fail(trs("The request failed for an unknown reason.").toStdString());
    }

    // The thread's last action, so reap() can join it without blocking.
    finished_.store(true, std::memory_order_release);
  });
}

void Task::cancel() {
  state_.cancel();
  worker_.request_stop();
}

void Task::reap() {
  if (worker_.joinable() && finished_.load(std::memory_order_acquire)) {
    worker_.join();
  }
}

void Task::shutdown() {
  cancel();

  if (worker_.joinable()) {
    worker_.join();
  }
}

TaskManager::TaskManager(Application& app) : app_(app) {}

TaskManager::~TaskManager() {
  shutdown();
}

TaskManager::Action TaskManager::primary_action(std::uint32_t session_id) const {
  const auto session = app_.db().session(session_id);

  if (!session.has_value()) {
    return Action::Send;
  }

  return primary_action(*session, app_.db().messages(session_id));
}

TaskManager::Action TaskManager::primary_action(
    const Session& session, const std::vector<Message>& messages
) const {
  // context_usage reports the unsummarised conversation cost against the trigger,
  // so "used >= limit" is exactly the automatic-collapse condition.
  const net::ContextUsage usage = net::context_usage(session, messages);

  if (usage.limit_tokens > 0 && usage.used_tokens >= usage.limit_tokens) {
    return Action::SummarizeSession;
  }

  // The assistant memory is shared across every session, so its own trigger — or a
  // fold already in flight — turns the primary action into a fold even when this
  // session has not overflowed.
  if (app_.assistant_summary_due() || assistant_busy()) {
    return Action::SummarizeAssistant;
  }

  return Action::Send;
}

std::shared_ptr<Task> TaskManager::submit(
    TaskKind kind, std::uint32_t session_id, Work work
) {
  // The protection layer: a turn may only start when the central decision is a
  // plain send. This keeps a message from being sent into a session whose
  // overflow, or the shared assistant memory, is pending a fold.
  if (kind == TaskKind::Turn && primary_action(session_id) != Action::Send) {
    return nullptr;
  }

  const bool is_assistant = kind == TaskKind::AssistantSummary;
  std::shared_ptr<Task>* slot =
      is_assistant ? &assistant_ : &sessions_[session_id];

  // At most one task per slot; a slot is only reused once its worker has exited.
  if (*slot && !(*slot)->finished()) {
    return nullptr;
  }

  // Reclaim the previous slot's worker before it is dropped (a finished thread is
  // joined instantly).
  if (*slot) {
    (*slot)->reap();
  }

  auto task = std::make_shared<Task>(
      next_id_++, kind, is_assistant ? 0u : session_id
  );
  task->start(std::move(work));

  *slot = task;
  return task;
}

bool TaskManager::busy(std::uint32_t session_id) const {
  const auto it = sessions_.find(session_id);
  return it != sessions_.end() && it->second != nullptr && !it->second->finished();
}

bool TaskManager::assistant_busy() const {
  return assistant_ != nullptr && !assistant_->finished();
}

std::size_t TaskManager::running_count() const {
  std::size_t running = 0;

  for (const auto& [id, task] : sessions_) {
    if (task != nullptr && !task->finished()) {
      ++running;
    }
  }

  if (assistant_ != nullptr && !assistant_->finished()) {
    ++running;
  }

  return running;
}

std::vector<std::shared_ptr<Task>> TaskManager::active_tasks() {
  std::vector<std::shared_ptr<Task>> tasks;
  tasks.reserve(sessions_.size() + 1);

  for (auto& [id, task] : sessions_) {
    if (task == nullptr) {
      continue;
    }

    task->reap();
    tasks.push_back(task);
  }

  if (assistant_ != nullptr) {
    assistant_->reap();
    tasks.push_back(assistant_);
  }

  return tasks;
}

std::shared_ptr<Task> TaskManager::find(std::uint64_t id) const {
  for (const auto& [session_id, task] : sessions_) {
    if (task != nullptr && task->id() == id) {
      return task;
    }
  }

  if (assistant_ != nullptr && assistant_->id() == id) {
    return assistant_;
  }

  return nullptr;
}

std::string TaskManager::log_text(std::uint64_t id) const {
  const std::shared_ptr<Task> task = find(id);
  return task != nullptr ? task->log_text() : std::string{};
}

bool TaskManager::cancel(std::uint64_t id) {
  const std::shared_ptr<Task> task = find(id);

  if (task == nullptr) {
    return false;
  }

  task->cancel();
  return true;
}

void TaskManager::forget_session(std::uint32_t session_id) {
  const auto it = sessions_.find(session_id);

  if (it == sessions_.end()) {
    return;
  }

  if (it->second != nullptr) {
    it->second->shutdown();
  }

  sessions_.erase(it);
}

void TaskManager::shutdown() {
  for (auto& [id, task] : sessions_) {
    if (task != nullptr) {
      task->shutdown();
    }
  }

  sessions_.clear();

  if (assistant_ != nullptr) {
    assistant_->shutdown();
    assistant_.reset();
  }
}

} // namespace lnb
