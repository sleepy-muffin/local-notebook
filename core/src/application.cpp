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
#include "local_notebook/core/structs.hpp"
#include "net/client.hpp"
#include "shark/legacy.hpp"
#include "shark/log.hpp"

namespace {

lnb::core::Remote remote_for(const lnb::core::Assistant &assistant) {
  lnb::core::Remote remote;
  remote.url = lnb::core::derive_endpoint(assistant.api_url_, assistant.protocol_);
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

lnb::core::application::assistant_state::assistant_state(
    std::string uuid, Assistant assistant, const std::string &db_path,
    std::unique_ptr<shark::legacy::instance_lock> filesystem_lock)
    : uuid_(std::move(uuid)), assistant_(std::move(assistant)), sessions_(db_path),
      filesystem_lock_(std::move(filesystem_lock)) {}

bool lnb::core::application::task_slot::busy() const { return task != nullptr && !task->is_terminal(); }

lnb::core::application::application(std::filesystem::path store_root)
    : multi_executor_(std::make_unique<executor>(5)), store_(std::move(store_root)) {}

lnb::core::application::~application() {
  stop_worker();
  multi_executor_->shutdown();
}


const lnb::core::store &lnb::core::application::get_store() const { return (this->store_); }

std::expected<void, lnb::core::error_code> lnb::core::application::open_assistant(const std::string &uuid) {
  static std::mutex function_mutex;
  std::lock_guard function_lock(function_mutex);

  Assistant assistant = store_.load_assistant(uuid);
  const std::string requested = uuid;

  bool already_held = false;
  if (assistant_state_ != nullptr) {
    auto state = assistant_state_->lock();
    already_held = state->filesystem_lock_ != nullptr && state->uuid_ == uuid;
  }

  std::unique_ptr<shark::legacy::instance_lock> filesystem_lock;
  if (!already_held) {
    filesystem_lock = std::make_unique<shark::legacy::instance_lock>();
    std::string error;
    if (!filesystem_lock->acquire(store_.assistant_lock(uuid), error)) {
      shark::log::error("cannot open assistant {}: {}", uuid, error);
      return std::unexpected(error_code::ERR_ASSISTANT_HAS_NOT_LOADED_YET);
    }
  } else {
    filesystem_lock = std::move(assistant_state_->lock()->filesystem_lock_);
  }

  stop_worker();
  assistant_state_ = std::make_unique<shark::synchronized<assistant_state>>(
      requested, std::move(assistant), store_.assistant_db(requested).string(), std::move(filesystem_lock));

  open_log(requested);
  shark::log::info("opened assistant {} at {}", requested, store_.assistant_dir(requested).string());
  return {};
}

std::vector<std::pair<std::string, lnb::core::Assistant>> lnb::core::application::list_assistants() {
  std::vector<std::pair<std::string, Assistant>> result;
  for (const auto &uuid: store_.scan_assistants()) {
    result.emplace_back(uuid, store_.load_assistant(uuid));
  }
  return result;
}

bool lnb::core::application::has_assistant() const { return assistant_state_ != nullptr; }

std::expected<std::string, lnb::core::error_code> lnb::core::application::assistant_uuid() const {
  if (assistant_state_ == nullptr)
    return std::unexpected(error_code::ERR_ASSISTANT_HAS_NOT_LOADED_YET);
  return assistant_state_->lock()->uuid_;
}

std::expected<lnb::core::Assistant, lnb::core::error_code> lnb::core::application::assistant() const {
  if (assistant_state_ == nullptr)
    return std::unexpected(error_code::ERR_ASSISTANT_HAS_NOT_LOADED_YET);
  return assistant_state_->lock()->assistant_;
}

void lnb::core::application::set_assistant(Assistant assistant) {
  if (assistant_state_ == nullptr)
    return;
  auto state = assistant_state_->lock();
  state->assistant_ = std::move(assistant);
  state->assistant_due_cache_.reset();
}

void lnb::core::application::set_assistant_memory(std::string memory) {
  if (assistant_state_ == nullptr)
    return;
  auto state = assistant_state_->lock();
  state->assistant_.memory_ = std::move(memory);
  state->assistant_due_cache_.reset();
}

void lnb::core::application::save_assistant() {
  if (assistant_state_ == nullptr)
    return;
  auto state = assistant_state_->lock();
  if (!state->uuid_.empty())
    store_.save_assistant(state->uuid_, state->assistant_);
}

std::optional<lnb::core::Session> lnb::core::application::session(std::uint32_t session_id) const {
  if (assistant_state_ == nullptr)
    return std::nullopt;
  return assistant_state_->lock()->sessions_.get(session_id);
}

std::vector<lnb::core::Message> lnb::core::application::messages(std::uint32_t session_id) const {
  if (assistant_state_ == nullptr)
    return {};
  const auto manager = assistant_state_->lock()->sessions_.get_messages(session_id);
  return manager ? manager->all() : std::vector<Message>{};
}

std::vector<lnb::core::Message>
lnb::core::application::recent_messages(std::uint32_t session_id, std::size_t count) const {
  if (assistant_state_ == nullptr)
    return {};
  const auto manager = assistant_state_->lock()->sessions_.get_messages(session_id);
  return manager ? manager->most_recent(count) : std::vector<Message>{};
}

std::vector<lnb::core::Session> lnb::core::application::recent_sessions(std::size_t count) const {
  if (assistant_state_ == nullptr)
    return {};
  return assistant_state_->lock()->sessions_.most_recent(count);
}

std::unordered_map<std::uint32_t, std::string> lnb::core::application::session_last_times() const {
  if (assistant_state_ == nullptr)
    return {};
  return assistant_state_->lock()->sessions_.session_last_times();
}

std::optional<std::string> lnb::core::application::last_message_time(std::uint32_t session_id) const {
  if (assistant_state_ == nullptr)
    return std::nullopt;
  return assistant_state_->lock()->sessions_.last_message_time(session_id);
}

lnb::core::ContextUsage lnb::core::application::context_usage(std::uint32_t session_id) const {
  core::ContextUsage result;
  if (assistant_state_ == nullptr)
    return result;
  auto state = assistant_state_->lock();
  const auto session = state->sessions_.get(session_id);
  const auto manager = state->sessions_.get_messages(session_id);
  if (!session || !manager)
    return result;
  const auto usage = core::context_usage(*session, manager->all());
  result.used_tokens = usage.used_tokens;
  result.limit_tokens = usage.limit_tokens;
  return result;
}

std::uint32_t lnb::core::application::create_session(const Session &session) {
  if (assistant_state_ == nullptr)
    return 0;
  const auto id = assistant_state_->lock()->sessions_.create(session);
  mark_assistant_summary_stale();
  return id;
}

void lnb::core::application::save_session(const Session &session) {
  if (assistant_state_ == nullptr)
    return;
  assistant_state_->lock()->sessions_.save(session);
  mark_assistant_summary_stale();
}

void lnb::core::application::delete_session(std::uint32_t session_id) {
  if (assistant_state_ == nullptr)
    return;
  cancel_session(session_id);
  assistant_state_->lock()->sessions_.remove(session_id);
  mark_assistant_summary_stale();
}

void lnb::core::application::set_session_reasoning_effort(std::uint32_t session_id, std::string level) {
  if (assistant_state_ != nullptr) {
    assistant_state_->lock()->sessions_.set_reasoning_effort(session_id, level);
  }
}

void lnb::core::application::append_message(std::uint32_t session_id, Role role, std::string content) {
  if (assistant_state_ == nullptr)
    return;
  auto manager = assistant_state_->lock()->sessions_.get_messages(session_id);
  if (manager) {
    manager->append(role, content);
    mark_assistant_summary_stale();
  }
}

void lnb::core::application::delete_message(std::uint32_t message_id) {
  if (assistant_state_ != nullptr) {
    (void) assistant_state_->lock()->sessions_.remove_message(message_id);
    mark_assistant_summary_stale();
  }
}

void lnb::core::application::edit_message(std::uint32_t message_id, std::string content) {
  if (assistant_state_ != nullptr) {
    assistant_state_->lock()->sessions_.update_message(message_id, content);
    mark_assistant_summary_stale();
  }
}

std::size_t lnb::core::application::message_tokens(const Message &message) const {
  return lnb::core::message_tokens(message);
}

std::string lnb::core::application::default_reasoning_effort() const {
  const auto current = assistant();
  return current ? core::default_reasoning_effort(current->model_) : std::string{};
}

std::vector<std::string> lnb::core::application::reasoning_efforts_with_stored(const std::string &stored) const {
  const auto current = assistant();
  return current ? core::reasoning_efforts_with_stored(current->model_, stored) : std::vector<std::string>{};
}

std::expected<void, lnb::core::error_code>
lnb::core::application::has_session_summary_trigger_met(std::uint32_t session_id) const {
  if (assistant_state_ == nullptr)
    return std::unexpected(error_code::ERR_ASSISTANT_HAS_NOT_LOADED_YET);
  auto state = assistant_state_->lock();
  const auto session = state->sessions_.get(session_id);
  const auto manager = state->sessions_.get_messages(session_id);
  if (!session || !manager)
    return std::unexpected(error_code::ERR_INVALID_SESSION_ID);

  if (transcript_tokens(manager->all_messages_after_summary_watermark()) >= session->summary_trigger_tokens_) {
    return std::unexpected(error_code::ERR_SESSION_SUMMARY_TRIGGER_HAS_MET);
  }
  const auto global = state->sessions_.most_recent_after_assistant_memory_watermark();
  if (transcript_tokens(global) >= state->assistant_.summary_trigger_tokens_) {
    return std::unexpected(error_code::ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET);
  }
  return {};
}

std::expected<void, lnb::core::error_code>
lnb::core::application::session_prompt(std::uint32_t session_id, const std::string &prompt) {
  if (assistant_state_ == nullptr)
    return std::unexpected(error_code::ERR_ASSISTANT_HAS_NOT_LOADED_YET);
  {
    std::scoped_lock lock(tasks_mutex_);
    prune_finished_sessions();
    if (assistant_slot_.busy())
      return std::unexpected(error_code::ERR_ASSISTANT_BUSY);
  }
  const auto trigger = has_session_summary_trigger_met(session_id);
  if (!trigger)
    return std::unexpected(trigger.error());
  auto manager = assistant_state_->lock()->sessions_.get_messages(session_id);
  if (!manager)
    return std::unexpected(error_code::ERR_INVALID_SESSION_ID);
  manager->append(Role::User, prompt);
  mark_assistant_summary_stale();
  return {};
}

/*
 * It is true whenever a slot is busy.
 * It is used to block updating assistant memory.
 */
bool lnb::core::application::any_busy() const {
  std::scoped_lock lock(tasks_mutex_);
  if (assistant_slot_.busy())
    return true;
  return std::any_of(
      session_tasks_.begin(), session_tasks_.end(), [](const auto &entry) { return entry.second.busy(); });
}

/*
 * It is true when the slot of the session is busy, it might be doing summarization or generation,
 * both of them share absolutely the same slot, though.
 * It is used to block current session only.
 */
bool lnb::core::application::session_busy(std::uint32_t session_id) const {
  std::scoped_lock lock(tasks_mutex_);
  const auto found = session_tasks_.find(session_id);
  return found != session_tasks_.end() && found->second.busy();
}

/*
 * It is used to block everything.
 * No generation tasks, new prompts are allowed when assistant memory is being updated.
 */
bool lnb::core::application::assistant_busy() const {
  std::scoped_lock lock(tasks_mutex_);
  return assistant_slot_.busy();
}

std::shared_ptr<lnb::core::GenerationTask> lnb::core::application::assistant_task() const {
  std::scoped_lock lock(tasks_mutex_);
  return assistant_slot_.task;
}

bool lnb::core::application::assistant_summary_due() const {
  if (assistant_state_ == nullptr)
    return false;
  {
    auto state = assistant_state_->lock();
    if (state->assistant_due_cache_)
      return *state->assistant_due_cache_;
  }
  const auto trigger = has_assistant_trigger_met();
  const bool due = !trigger &&
      trigger.error() == error_code::ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET;
  auto state = assistant_state_->lock();
  state->assistant_due_cache_ = due;
  return due;
}

void lnb::core::application::mark_assistant_summary_stale() {
  if (assistant_state_ != nullptr)
    assistant_state_->lock()->assistant_due_cache_.reset();
}

std::optional<lnb::core::application::session_snapshot>
lnb::core::application::snapshot_session(std::uint32_t session_id) {
  // The lock covers this read only. A worker must never hold it across the
  // network round-trip: the UI thread takes the same lock for every read and for
  // `assistant_summary_due()`, so holding it there freezes the window until the
  // model answers. `open_assistant()` stops the workers before it replaces state.
  auto state = assistant_state_->lock();
  auto stored = state->sessions_.get(session_id);
  if (!stored)
    return std::nullopt;
  return session_snapshot{.session_ = std::move(*stored), .db_ = &state->sessions_.database_for_update()};
}

std::expected<std::shared_ptr<lnb::core::GenerationTask>, lnb::core::error_code>
lnb::core::application::start_generation(std::uint32_t session_id, SummaryApprover approver) {
  if (assistant_state_ == nullptr)
    return std::unexpected(error_code::ERR_ASSISTANT_HAS_NOT_LOADED_YET);
  if (assistant_busy())
    return std::unexpected(error_code::ERR_ASSISTANT_BUSY);
  if (session_busy(session_id))
    return std::unexpected(error_code::ERR_SESSION_BUSY);
  const auto trigger = has_session_summary_trigger_met(session_id);
  if (!trigger)
    return std::unexpected(trigger.error());

  const auto current = assistant();
  if (!current)
    return std::unexpected(current.error());
  const Assistant snapshot = *current;
  return spawn_task(session_tasks_[session_id], [this, session_id, snapshot](GenerationTask &task) {
    auto input = snapshot_session(session_id);
    if (!input) {
      task.fail("Invalid Session ID");
      return;
    }
    auto &session = input->session_;
    MessageManager messages(*input->db_, session_id);
    const auto remote = remote_for(snapshot);
    const auto stop = task.stop_token();
    auto request = build_session_request(snapshot, session, messages.all());
    const auto result = generate_reply(std::move(request), remote, task, stop, session.shared_directories_, session);
    if (!result) {
      task.fail(result.error().reason);
      return;
    }
    if (!stop.stop_requested() && !result->text.empty()) {
      messages.append(Role::Assistant, result->text);
      mark_assistant_summary_stale();
    }
  });
}

std::expected<std::shared_ptr<lnb::core::GenerationTask>, lnb::core::error_code>
lnb::core::application::summarize_session(std::uint32_t session_id, bool force, SummaryApprover approver) {
  if (assistant_state_ == nullptr)
    return std::unexpected(error_code::ERR_ASSISTANT_HAS_NOT_LOADED_YET);
  if (assistant_busy())
    return std::unexpected(error_code::ERR_ASSISTANT_BUSY);
  if (session_busy(session_id))
    return std::unexpected(error_code::ERR_SESSION_BUSY);
  const auto current = assistant();
  if (!current)
    return std::unexpected(current.error());
  const Assistant snapshot = *current;
  return spawn_task(
      session_tasks_[session_id],
      [this, session_id, force, approver = std::move(approver), snapshot](GenerationTask &task) {
        auto input = snapshot_session(session_id);
        if (!input) {
          task.fail("Invalid Session ID");
          return;
        }
        const auto remote = remote_for(snapshot);
        const auto stop = task.stop_token();
        update_session_summary(*input->db_, input->session_, remote, task, stop, force, approver);
        mark_assistant_summary_stale();
      });
}

std::expected<void, lnb::core::error_code>
lnb::core::application::has_assistant_trigger_met() const {
  if (assistant_state_ == nullptr)
    return std::unexpected(error_code::ERR_ASSISTANT_HAS_NOT_LOADED_YET);
  auto state = assistant_state_->lock();
  const auto pending = state->sessions_.most_recent_after_assistant_memory_watermark();
  if (transcript_tokens(pending) >= state->assistant_.summary_trigger_tokens_)
    return std::unexpected(error_code::ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET);
  return {};
}

std::expected<std::shared_ptr<lnb::core::GenerationTask>, lnb::core::error_code>
lnb::core::application::summarize_assistant(bool force, SummaryApprover approver) {
  if (assistant_state_ == nullptr)
    return std::unexpected(error_code::ERR_ASSISTANT_HAS_NOT_LOADED_YET);
  if (any_busy())
    return std::unexpected(error_code::ERR_ASSISTANT_OR_ANY_SESSION_BUSY);
  const auto current = assistant();
  if (!current)
    return std::unexpected(current.error());
  const Assistant snapshot = *current;
  return spawn_task(assistant_slot_, [this, force, approver = std::move(approver), snapshot](GenerationTask &task) {
    task.set_output_budget(snapshot.summary_token_budget_, GenerationTask::BudgetKind::MEMORY);
    std::vector<executor::handle> workers;
    {
      std::scoped_lock lock(tasks_mutex_);
      workers.reserve(session_tasks_.size());
      for (const auto &[id, slot]: session_tasks_)
        workers.push_back(slot.worker);
    }
    if (std::any_of(workers.begin(), workers.end(), [](const auto &worker) { return !worker.finished(); })) {
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
    // The assistant fold reads the database and writes the memory, so the
    // lock is taken for those only — never across the network round.
    database *db = nullptr;
    {
      auto state = assistant_state_->lock();
      db = &state->sessions_.database_for_update();
    }
    if (!force) {
      const auto trigger = has_assistant_trigger_met();
      if (!trigger && trigger.error() != error_code::ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET) {
        task.fail("Assistant trigger could not be checked");
        return;
      }
      if (trigger.has_value()) {
        task.append_log("[assistant-summary] automatic trigger not reached; nothing was sent");
        return;
      }
    }
    const auto remote = remote_for(snapshot);
    std::string memory = update_assistant_memory(*db, snapshot, remote, task, stop, approver);
    {
      auto state = assistant_state_->lock();
      if (!memory.empty()) {
        state->assistant_.memory_ = std::move(memory);
        store_.save_assistant(state->uuid_, state->assistant_);
      }
      state->assistant_due_cache_.reset();
    }
  });
}

void lnb::core::application::cancel_session(std::uint32_t session_id) {
  std::scoped_lock lock(tasks_mutex_);
  const auto found = session_tasks_.find(session_id);
  if (found != session_tasks_.end() && found->second.task != nullptr)
    found->second.task->cancel();
}

void lnb::core::application::cancel_assistant() {
  std::scoped_lock lock(tasks_mutex_);
  if (assistant_slot_.task != nullptr)
    assistant_slot_.task->cancel();
}

void lnb::core::application::wait_for_generation() {
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

void lnb::core::application::open_log(const std::string &uuid) {
  const auto path = store_.assistant_log(uuid);
  try {
    shark::log::set_output_file(path);
  } catch (const std::exception &error) {
    shark::log::error("cannot open log file {}: {}", path.string(), error.what());
  }
  shark::log::info("===== local-notebook session started =====");
}

void lnb::core::application::stop_worker() {
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

void lnb::core::application::prune_finished_sessions() {
  for (auto it = session_tasks_.begin(); it != session_tasks_.end();) {
    if (!it->second.busy() && it->second.worker.finished())
      it = session_tasks_.erase(it);
    else
      ++it;
  }
}

template<typename Work>
std::shared_ptr<lnb::core::GenerationTask> lnb::core::application::spawn_task(task_slot &slot, Work &&work) {
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
    slot.worker = multi_executor_->spawn([next, work = std::forward<Work>(work)](std::stop_token) mutable {
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
