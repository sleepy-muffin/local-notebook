#include "application.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "local_notebook/core/context.hpp"
#include "net/client.hpp"
#include "shark/log.hpp"

namespace {

lnb::net::Remote remote_for(const lnb::Assistant &assistant) {
  lnb::net::Remote remote;
  remote.url = lnb::net::derive_endpoint(assistant.api_url_, assistant.protocol_);
  remote.key = assistant.authentication_token_;
  remote.proxy = assistant.proxy_;
  remote.model = assistant.model_;
  remote.protocol = assistant.protocol_;

  const auto to_milliseconds = [](std::size_t seconds, std::size_t fallback) {
    constexpr std::size_t kMinimum = 30;
    constexpr std::size_t kMaximum = 24 * 60 * 60;
    if (seconds == 0) {
      seconds = fallback;
    }
    return static_cast<int>(std::clamp(seconds, kMinimum, kMaximum) * 1000);
  };

  remote.turn_read_timeout_ms = to_milliseconds(assistant.turn_timeout_seconds_, 180);
  remote.summary_read_timeout_ms = to_milliseconds(assistant.summary_timeout_seconds_, 420);
  return remote;
}

} // namespace

namespace lnb::core {

application::assistant_state::assistant_state(
    std::string uuid, Assistant assistant, const std::string &db_path,
    std::unique_ptr<store::instance_lock> filesystem_lock)
    : uuid_(std::move(uuid)), assistant_(std::move(assistant)), sessions_(db_path),
      filesystem_lock_(std::move(filesystem_lock)) {}

bool application::task_slot::busy() const { return task != nullptr && !task->is_terminal(); }

application::application(std::filesystem::path store_root)
    : multi_executor_(std::make_unique<executor>(5)), store_(std::move(store_root)) {}

application::~application() {
  stop_worker();
  multi_executor_->shutdown();
}

bool application::open_assistant(std::string uuid, Assistant assistant) {
  static std::mutex function_mutex;
  std::lock_guard function_lock(function_mutex);
  const std::string requested = uuid;

  bool already_held = false;
  if (assistant_state_ != nullptr) {
    auto state = assistant_state_->lock();
    already_held = state->filesystem_lock_ != nullptr && state->uuid_ == requested;
  }

  std::unique_ptr<store::instance_lock> filesystem_lock;
  if (!already_held) {
    filesystem_lock = std::make_unique<store::instance_lock>();
    std::string error;
    if (!filesystem_lock->acquire(store_.assistant_lock(requested), error)) {
      shark::log::error("cannot open assistant {}: {}", requested, error);
      last_error_ = std::move(error);
      return false;
    }
  } else {
    filesystem_lock = std::move(assistant_state_->lock()->filesystem_lock_);
  }

  stop_worker();
  assistant_state_ = std::make_unique<shark::synchronized<assistant_state>>(
      requested, std::move(assistant), store_.assistant_db(requested).string(),
      std::move(filesystem_lock));
  last_error_.clear();
  open_log(requested);
  shark::log::info(
      "opened assistant {} at {}", requested, store_.assistant_dir(requested).string());
  return true;
}

std::string application::last_error() const { return last_error_; }

std::vector<Assistant> application::list_assistants() {
  std::vector<Assistant> result;
  for (const auto &uuid: store_.scan_assistants()) {
    result.push_back(store_.load_assistant(uuid));
  }
  return result;
}

bool application::has_assistant() const { return assistant_state_ != nullptr; }

std::string application::assistant_uuid() const {
  return assistant_state_ == nullptr ? std::string{} : assistant_state_->lock()->uuid_;
}

Assistant application::assistant() const {
  return assistant_state_ == nullptr ? Assistant{} : assistant_state_->lock()->assistant_;
}

void application::set_assistant(Assistant assistant) {
  if (assistant_state_ == nullptr)
    return;
  auto state = assistant_state_->lock();
  state->assistant_ = std::move(assistant);
  state->assistant_due_cache_.reset();
}

void application::set_assistant_memory(std::string memory) {
  if (assistant_state_ == nullptr)
    return;
  auto state = assistant_state_->lock();
  state->assistant_.memory_ = std::move(memory);
  state->assistant_due_cache_.reset();
}

void application::save_assistant() {
  if (assistant_state_ == nullptr)
    return;
  auto state = assistant_state_->lock();
  if (!state->uuid_.empty())
    store_.save_assistant(state->uuid_, state->assistant_);
}

std::optional<Session> application::session(std::uint32_t session_id) const {
  if (assistant_state_ == nullptr)
    return std::nullopt;
  return assistant_state_->lock()->sessions_.get(session_id);
}

std::vector<Message> application::messages(std::uint32_t session_id) const {
  if (assistant_state_ == nullptr)
    return {};
  const auto manager = assistant_state_->lock()->sessions_.get_messages(session_id);
  return manager ? manager->all() : std::vector<Message>{};
}

std::vector<Message>
application::recent_messages(std::uint32_t session_id, std::size_t count) const {
  if (assistant_state_ == nullptr)
    return {};
  const auto manager = assistant_state_->lock()->sessions_.get_messages(session_id);
  return manager ? manager->most_recent(count) : std::vector<Message>{};
}

std::vector<Session> application::recent_sessions(std::size_t count) const {
  if (assistant_state_ == nullptr)
    return {};
  return assistant_state_->lock()->sessions_.most_recent(count);
}

std::unordered_map<std::uint32_t, std::string> application::session_last_times() const {
  if (assistant_state_ == nullptr)
    return {};
  return assistant_state_->lock()->sessions_.db().session_last_times();
}

std::optional<std::string> application::last_message_time(std::uint32_t session_id) const {
  if (assistant_state_ == nullptr)
    return std::nullopt;
  return assistant_state_->lock()->sessions_.db().last_message_time(session_id);
}

api::ContextUsage application::context_usage(std::uint32_t session_id) const {
  api::ContextUsage result;
  if (assistant_state_ == nullptr)
    return result;
  auto state = assistant_state_->lock();
  const auto session = state->sessions_.get(session_id);
  const auto manager = state->sessions_.get_messages(session_id);
  if (!session || !manager)
    return result;
  const auto usage = net::context_usage(*session, manager->all());
  result.used_tokens = usage.used_tokens;
  result.limit_tokens = usage.limit_tokens;
  return result;
}

std::uint32_t application::create_session(const Session &session) {
  if (assistant_state_ == nullptr)
    return 0;
  const auto id = assistant_state_->lock()->sessions_.create(session);
  mark_assistant_summary_stale();
  return id;
}

void application::save_session(const Session &session) {
  if (assistant_state_ == nullptr)
    return;
  assistant_state_->lock()->sessions_.save(session);
  mark_assistant_summary_stale();
}

void application::delete_session(std::uint32_t session_id) {
  if (assistant_state_ == nullptr)
    return;
  cancel_session(session_id);
  assistant_state_->lock()->sessions_.remove(session_id);
  mark_assistant_summary_stale();
}

void application::set_session_reasoning_effort(std::uint32_t session_id, std::string level) {
  if (assistant_state_ != nullptr) {
    assistant_state_->lock()->sessions_.db().set_session_reasoning_effort(session_id, level);
  }
}

void application::append_message(std::uint32_t session_id, Role role, std::string content) {
  if (assistant_state_ == nullptr)
    return;
  auto manager = assistant_state_->lock()->sessions_.get_messages(session_id);
  if (manager) {
    manager->append(role, content);
    mark_assistant_summary_stale();
  }
}

void application::delete_message(std::uint32_t message_id) {
  if (assistant_state_ != nullptr) {
    (void) assistant_state_->lock()->sessions_.db().remove_message(message_id);
    mark_assistant_summary_stale();
  }
}

void application::edit_message(std::uint32_t message_id, std::string content) {
  if (assistant_state_ != nullptr) {
    assistant_state_->lock()->sessions_.db().update_message(message_id, content);
    mark_assistant_summary_stale();
  }
}

std::size_t application::message_tokens(const Message &message) const {
  return context::message_tokens(message);
}

std::string application::default_reasoning_effort() const {
  return net::default_reasoning_effort(assistant().model_);
}

std::vector<std::string>
application::reasoning_efforts_with_stored(const std::string &stored) const {
  return net::reasoning_efforts_with_stored(assistant().model_, stored);
}

api::error_code application::has_session_summary_trigger_met(std::uint32_t session_id) const {
  if (assistant_state_ == nullptr)
    return api::error_code::ERR_INVALID_SESSION_ID;
  auto state = assistant_state_->lock();
  const auto session = state->sessions_.get(session_id);
  const auto manager = state->sessions_.get_messages(session_id);
  if (!session || !manager)
    return api::error_code::ERR_INVALID_SESSION_ID;

  if (context::transcript_tokens(manager->all_messages_after_summary_watermark()) >=
      session->summary_trigger_tokens_) {
    return api::error_code::ERR_SESSION_SUMMARY_TRIGGER_HAS_MET;
  }
  const auto global = state->sessions_.most_recent_after_assistant_memory_watermark();
  if (context::transcript_tokens(global) >= state->assistant_.summary_trigger_tokens_) {
    return api::error_code::ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET;
  }
  return api::error_code::SUCCESS;
}

api::error_code application::session_prompt(std::uint32_t session_id, const std::string &prompt) {
  if (assistant_state_ == nullptr)
    return api::error_code::ERR_INVALID_SESSION_ID;
  {
    std::scoped_lock lock(tasks_mutex_);
    prune_finished_sessions();
    if (assistant_slot_.busy())
      return api::error_code::ERR_ASSISTANT_BUSY;
    const auto found = session_tasks_.find(session_id);
    if (found != session_tasks_.end() && found->second.busy())
      return api::error_code::ERR_SESSION_BUSY;
  }
  const auto trigger = has_session_summary_trigger_met(session_id);
  if (trigger != api::error_code::SUCCESS)
    return trigger;
  auto manager = assistant_state_->lock()->sessions_.get_messages(session_id);
  if (!manager)
    return api::error_code::ERR_INVALID_SESSION_ID;
  manager->append(Role::User, prompt);
  mark_assistant_summary_stale();
  return api::error_code::SUCCESS;
}

bool application::busy() const {
  std::scoped_lock lock(tasks_mutex_);
  if (assistant_slot_.busy())
    return true;
  return std::any_of(session_tasks_.begin(), session_tasks_.end(), [](const auto &entry) {
    return entry.second.busy();
  });
}

bool application::session_busy(std::uint32_t session_id) const {
  std::scoped_lock lock(tasks_mutex_);
  const auto found = session_tasks_.find(session_id);
  return found != session_tasks_.end() && found->second.busy();
}

bool application::assistant_busy() const {
  std::scoped_lock lock(tasks_mutex_);
  return assistant_slot_.busy();
}

std::shared_ptr<GenerationTask> application::assistant_task() const {
  std::scoped_lock lock(tasks_mutex_);
  return assistant_slot_.task;
}

bool application::assistant_summary_due() const {
  if (assistant_state_ == nullptr)
    return false;
  auto state = assistant_state_->lock();
  if (!state->assistant_due_cache_) {
    state->assistant_due_cache_ =
        net::assistant_trigger_reached(state->sessions_.db(), state->assistant_);
  }
  return *state->assistant_due_cache_;
}

void application::mark_assistant_summary_stale() {
  if (assistant_state_ != nullptr)
    assistant_state_->lock()->assistant_due_cache_.reset();
}

std::shared_ptr<GenerationTask>
application::start_generation(std::uint32_t session_id, SummaryApprover approver) {
  if (assistant_state_ == nullptr)
    return nullptr;
  {
    std::scoped_lock lock(tasks_mutex_);
    prune_finished_sessions();
    if (assistant_slot_.busy())
      return nullptr;
  }
  const Assistant snapshot = assistant();
  return spawn_task(
      session_tasks_[session_id],
      [this, session_id, snapshot, approver = std::move(approver)](GenerationTask &task) {
        auto state = assistant_state_->lock();
        auto session = state->sessions_.get(session_id);
        if (!session) {
          task.fail("Invalid Session ID");
          return;
        }
        auto manager = state->sessions_.get_messages(session_id);
        if (!manager) {
          task.fail("Invalid Session ID");
          return;
        }
        const auto remote = remote_for(snapshot);
        const auto stop = task.stop_token();
        const auto folded = net::maybe_summarize_session(
            state->sessions_.db(), *session, remote, task, stop, approver);
        if (!folded.empty())
          session->summary_ = folded;
        auto request = net::build_session_request(snapshot, *session, manager->all());
        const auto result = net::generate_reply(
            std::move(request), remote, task, stop, session->shared_directories_, *session);
        if (!result) {
          task.fail(result.error().reason);
          return;
        }
        if (!stop.stop_requested() && !result->text.empty()) {
          manager->append(Role::Assistant, result->text);
          state->assistant_due_cache_.reset();
        }
      });
}

std::shared_ptr<GenerationTask>
application::summarize_session(std::uint32_t session_id, bool force, SummaryApprover approver) {
  if (assistant_state_ == nullptr)
    return nullptr;
  {
    std::scoped_lock lock(tasks_mutex_);
    prune_finished_sessions();
    if (assistant_slot_.busy())
      return nullptr;
  }
  const Assistant snapshot = assistant();
  return spawn_task(
      session_tasks_[session_id],
      [this, session_id, force, approver = std::move(approver), snapshot](GenerationTask &task) {
        auto state = assistant_state_->lock();
        const auto session = state->sessions_.get(session_id);
        if (!session) {
          task.fail("Invalid Session ID");
          return;
        }
        const auto remote = remote_for(snapshot);
        const auto stop = task.stop_token();
        if (force) {
          net::summarize_session_now(state->sessions_.db(), *session, remote, task, stop, approver);
        } else {
          net::maybe_summarize_session(
              state->sessions_.db(), *session, remote, task, stop, approver);
        }
        state->assistant_due_cache_.reset();
      });
}

std::shared_ptr<GenerationTask>
application::summarize_assistant(bool force, SummaryApprover approver) {
  if (assistant_state_ == nullptr)
    return nullptr;
  const Assistant snapshot = assistant();
  return spawn_task(
      assistant_slot_,
      [this, force, approver = std::move(approver), snapshot](GenerationTask &task) {
        task.set_output_budget(snapshot.summary_token_budget_, GenerationTask::BudgetKind::MEMORY);
        std::vector<executor::handle> workers;
        {
          std::scoped_lock lock(tasks_mutex_);
          workers.reserve(session_tasks_.size());
          for (const auto &[id, slot]: session_tasks_)
            workers.push_back(slot.worker);
        }
        if (std::any_of(workers.begin(), workers.end(), [](const auto &worker) {
              return !worker.finished();
            })) {
          task.set_status("Waiting for other tasks...");
        }
        const auto stop = task.stop_token();
        for (auto &worker: workers) {
          while (!worker.finished()) {
            if (stop.stop_requested())
              return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
        }
        auto state = assistant_state_->lock();
        const auto remote = remote_for(snapshot);
        std::string memory;
        if (force)
          memory = net::summarize_assistant_now(
              state->sessions_.db(), snapshot, remote, task, stop, approver);
        else
          memory = net::maybe_summarize_assistant(
              state->sessions_.db(), snapshot, remote, task, stop, approver);
        if (!memory.empty()) {
          state->assistant_.memory_ = std::move(memory);
          store_.save_assistant(state->uuid_, state->assistant_);
        }
        state->assistant_due_cache_.reset();
      });
}

void application::cancel_session(std::uint32_t session_id) {
  std::scoped_lock lock(tasks_mutex_);
  const auto found = session_tasks_.find(session_id);
  if (found != session_tasks_.end() && found->second.task != nullptr)
    found->second.task->cancel();
}

void application::cancel_assistant() {
  std::scoped_lock lock(tasks_mutex_);
  if (assistant_slot_.task != nullptr)
    assistant_slot_.task->cancel();
}

void application::wait_for_generation() {
  std::vector<executor::handle> workers;
  {
    std::scoped_lock lock(tasks_mutex_);
    workers.reserve(session_tasks_.size() + 1);
    for (const auto &[id, slot]: session_tasks_)
      workers.push_back(slot.worker);
    workers.push_back(assistant_slot_.worker);
  }
  for (auto &worker: workers)
    worker.wait();
}

void application::open_log(const std::string &uuid) {
  const auto path = store_.assistant_log(uuid);
  try {
    shark::log::set_output_file(path);
  } catch (const std::exception &error) {
    shark::log::error("cannot open log file {}: {}", path.string(), error.what());
  }
  shark::log::info("===== local-notebook session started =====");
}

void application::stop_worker() {
  std::vector<std::shared_ptr<GenerationTask>> tasks;
  std::vector<executor::handle> workers;
  {
    std::scoped_lock lock(tasks_mutex_);
    for (auto &[id, slot]: session_tasks_) {
      if (slot.task != nullptr)
        tasks.push_back(slot.task);
      workers.push_back(slot.worker);
      slot.task.reset();
      slot.worker = {};
    }
    session_tasks_.clear();
    if (assistant_slot_.task != nullptr)
      tasks.push_back(assistant_slot_.task);
    workers.push_back(assistant_slot_.worker);
    assistant_slot_.task.reset();
    assistant_slot_.worker = {};
  }
  for (const auto &task: tasks)
    task->cancel();
  for (auto &worker: workers)
    worker.wait();
}

void application::prune_finished_sessions() {
  for (auto it = session_tasks_.begin(); it != session_tasks_.end();) {
    if (!it->second.busy() && it->second.worker.finished())
      it = session_tasks_.erase(it);
    else
      ++it;
  }
}

template<typename Work>
std::shared_ptr<GenerationTask> application::spawn_task(task_slot &slot, Work &&work) {
  executor::handle previous;
  {
    std::scoped_lock lock(tasks_mutex_);
    if (slot.busy())
      return nullptr;
    previous = slot.worker;
  }
  previous.wait();
  auto next = std::make_shared<GenerationTask>();
  {
    std::scoped_lock lock(tasks_mutex_);
    if (slot.busy())
      return nullptr;
    slot.task = next;
    slot.worker =
        multi_executor_->spawn([next, work = std::forward<Work>(work)](std::stop_token) mutable {
          if (!next->start())
            return;
          try {
            work(*next);
            next->complete();
          } catch (const std::exception &error) {
            next->fail(error.what());
          } catch (...) {
            next->fail("Generation failed");
          }
        });
  }
  return next;
}

} // namespace lnb::core
