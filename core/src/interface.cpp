#include "local-notebook-core/interface.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/assistant_io.hpp"
#include "context/context.hpp"
#include "shark/async.hpp"
#include "shark/log.hpp"
#include "net/client.hpp"
#include "store/managers.hpp"
#include "utils/lock.hpp"
#include "utils/util.hpp"

namespace lnb::api {

namespace {

// Every request a turn makes shares one description: the endpoint derived from
// the assistant's base URL and protocol, plus its key, proxy and model.
net::Remote remote_for(const Assistant& assistant) {
  net::Remote remote;
  remote.url = net::derive_endpoint(assistant.api_url_, assistant.protocol_);
  remote.key = assistant.authentication_token_;
  remote.proxy = assistant.proxy_;
  remote.model = assistant.model_;
  remote.protocol = assistant.protocol_;

  // The settings store seconds; the transport wants milliseconds. A missing or
  // zero value falls back to the shipped default, and the upper bound keeps a
  // hand-edited file from overflowing the conversion.
  const auto to_milliseconds = [](std::size_t seconds, std::size_t fallback) {
    constexpr std::size_t kMinimum = 30;
    constexpr std::size_t kMaximum = 24 * 60 * 60;

    if (seconds == 0) {
      seconds = fallback;
    }

    return static_cast<int>(std::clamp(seconds, kMinimum, kMaximum) * 1000);
  };

  remote.turn_read_timeout_ms =
      to_milliseconds(assistant.turn_timeout_seconds_, 180);
  remote.summary_read_timeout_ms =
      to_milliseconds(assistant.summary_timeout_seconds_, 420);

  return remote;
}

} // namespace

struct interface::impl {
  using executor = shark::async::multi_executor<void()>;

  // One task per owner: a session id, or the single shared assistant fold. A
  // slot keeps the last task alive for the tasks window, exactly as the old
  // single-slot interface did, but each session has its own.
  struct task_slot {
    std::shared_ptr<GenerationTask> task;
    executor::handle worker;

    [[nodiscard]] bool busy() const {
      return task != nullptr && !task->is_terminal();
    }
  };

  impl() {
    // One lane per concurrently running task. Sessions only ever run their own
    // turn, so five lanes comfortably cover a handful of sessions plus the
    // assistant fold; a queued job simply waits for a free thread.
    multi_executor_ = std::make_unique<executor>(5);
  }

  ~impl() {
    stop_worker();
    multi_executor_->shutdown();
  }

  void open_log(const std::string& uuid) {
    const auto path = store::paths::assistant_log(uuid);

    try {
      shark::log::set_output_file(path);
    } catch (const std::exception& error) {
      // Logging stays on the previous sink; a bad path is never fatal.
      shark::log::error("cannot open log file {}: {}", path.string(), error.what());
    }

    shark::log::info("===== local-notebook session started =====");
  }

  // Cancels every in-flight task and waits for its executor job. Called before
  // the database is replaced and from the destructor.
  void stop_worker() {
    std::vector<std::shared_ptr<GenerationTask>> tasks;
    std::vector<executor::handle> workers;

    {
      std::scoped_lock lock(tasks_mutex);

      for (auto& [id, slot] : session_tasks) {
        if (slot.task != nullptr) {
          tasks.push_back(slot.task);
        }

        workers.push_back(slot.worker);
        slot.task.reset();
        slot.worker = {};
      }

      session_tasks.clear();

      if (assistant_slot.task != nullptr) {
        tasks.push_back(assistant_slot.task);
      }

      workers.push_back(assistant_slot.worker);
      assistant_slot.task.reset();
      assistant_slot.worker = {};
    }

    // Cancel outside the lock: a worker may already be waiting for tasks_mutex
    // (the assistant fold waits for the session slots).
    for (const auto& task : tasks) {
      task->cancel();
    }

    for (auto& worker : workers) {
      worker.wait();
    }
  }

  // Drops the slots of sessions whose last task has finished and whose job has
  // exited. The interface only needs a slot to refuse concurrent work; the UI's
  // controller keeps its own reference for the tasks window, so nothing is lost.
  // Caller holds tasks_mutex.
  void prune_finished_sessions() {
    for (auto it = session_tasks.begin(); it != session_tasks.end();) {
      if (!it->second.busy() && it->second.worker.finished()) {
        it = session_tasks.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Runs `work` on the executor and publishes it in `slot`. Returns nullptr when
  // the slot is already busy; a caller never cancels another task to make room.
  //
  // The previous job is joined *outside* the lock before the slot is reused, so
  // an assistant fold that is waiting on tasks_mutex cannot deadlock against a
  // replacement.
  template<typename Work>
  std::shared_ptr<GenerationTask> spawn_task(task_slot& slot, Work&& work) {
    executor::handle previous;

    {
      std::scoped_lock lock(tasks_mutex);

      if (slot.busy()) {
        return nullptr;
      }

      previous = slot.worker;
    }

    previous.wait();

    auto next = std::make_shared<GenerationTask>();

    {
      std::scoped_lock lock(tasks_mutex);

      // Defensive: a second caller may have taken the slot while the previous
      // job was joined.
      if (slot.busy()) {
        return nullptr;
      }

      slot.task = next;
      slot.worker = multi_executor_->spawn(
          [next, work = std::forward<Work>(work)](std::stop_token) mutable {
            // A cancel that landed before the job ran leaves the task terminal;
            // start() refuses it and there is nothing to do.
            if (!next->start()) {
              return;
            }

            try {
              work(*next);
              next->complete();
            } catch (const std::exception& error) {
              next->fail(error.what());
            } catch (...) {
              next->fail("Generation failed");
            }
          }
      );
    }

    return next;
  }

  // std::unique_ptr<shark::log::backend> logger;
  std::unique_ptr<executor> multi_executor_;

  std::string assistant_uuid;
  Assistant assistant;
  mutable std::mutex assistant_mutex;

  // The single live lock for the open assistant's directory. Held until another
  // assistant is opened or the interface is destroyed.
  std::unique_ptr<store::instance_lock> assistant_lock;

  // The reason the last open_assistant() failed. Guarded by assistant_mutex.
  std::string last_error;

  // The cached assistant-memory trigger. Guarded by assistant_mutex, and
  // dropped whenever a change can move it.
  mutable std::optional<bool> assistant_due_cache;

  // The manager owns the database; the interface only talks to the store
  // through it.
  std::unique_ptr<store::SessionManager> sessions;

  // Guards the task slots below. Session tasks are keyed by session id; the
  // assistant fold has its own slot because it is shared by every session.
  mutable std::mutex tasks_mutex;
  std::unordered_map<std::uint32_t, task_slot> session_tasks;
  task_slot assistant_slot;
};

interface::interface() : impl_(std::make_unique<impl>()) {}

interface::~interface() = default;

bool interface::open_assistant(std::string uuid, Assistant assistant) {
  const std::string requested = uuid;

  // Reopening the assistant already open in this process must not try to take a
  // second exclusive lock on the same file (a second open descriptor would
  // conflict with the one this process already holds).
  bool already_held = false;

  {
    std::scoped_lock lock(impl_->assistant_mutex);
    already_held = impl_->assistant_lock != nullptr &&
        impl_->assistant_uuid == requested;
    impl_->last_error.clear();
  }

  std::unique_ptr<store::instance_lock> lock;

  if (!already_held) {
    lock = std::make_unique<store::instance_lock>();

    std::string error;

    if (!lock->acquire(store::paths::assistant_lock(requested), error)) {
      shark::log::error("cannot open assistant {}: {}", requested, error);

      std::scoped_lock guard(impl_->assistant_mutex);
      impl_->last_error = std::move(error);
      return false;
    }
  }

  // The worker reads `sessions` through the impl, so it must be gone before the
  // database it is using is replaced. `stop_worker` also clears every task slot.
  impl_->stop_worker();

  std::string opened_uuid;

  {
    std::scoped_lock guard(impl_->assistant_mutex);
    impl_->assistant_uuid = std::move(uuid);
    impl_->assistant = std::move(assistant);
    impl_->assistant_due_cache.reset();

    if (lock != nullptr) {
      impl_->assistant_lock = std::move(lock);
    }

    opened_uuid = impl_->assistant_uuid;
  }

  const auto directory = store::paths::assistant_dir(opened_uuid);
  std::error_code error;
  std::filesystem::create_directories(directory, error);

  // Everything from here on is written to this assistant's log, including the
  // startup lines buffered before an assistant was chosen.
  impl_->open_log(opened_uuid);

  // The manager owns the database from here on: the interface only talks to the
  // store through it.
  impl_->sessions = std::make_unique<store::SessionManager>(
      store::paths::assistant_db(opened_uuid).string()
  );

  shark::log::info(
      "opened assistant {} at {}", opened_uuid, directory.string()
  );

  return true;
}

std::string interface::last_error() const {
  std::scoped_lock lock(impl_->assistant_mutex);
  return impl_->last_error;
}

bool interface::has_assistant() const {
  return impl_->sessions != nullptr;
}

std::string interface::assistant_uuid() const {
  std::scoped_lock lock(impl_->assistant_mutex);
  return impl_->assistant_uuid;
}

Assistant interface::assistant() const {
  std::scoped_lock lock(impl_->assistant_mutex);
  return impl_->assistant;
}

void interface::set_assistant(Assistant assistant) {
  std::scoped_lock lock(impl_->assistant_mutex);
  impl_->assistant = std::move(assistant);
  impl_->assistant_due_cache.reset();
}

void interface::set_assistant_memory(std::string memory) {
  std::scoped_lock lock(impl_->assistant_mutex);
  impl_->assistant.memory_ = std::move(memory);
  impl_->assistant_due_cache.reset();
}

void interface::save_assistant() {
  std::string uuid;
  Assistant snapshot;

  {
    std::scoped_lock lock(impl_->assistant_mutex);

    if (impl_->assistant_uuid.empty()) {
      return;
    }

    uuid = impl_->assistant_uuid;
    snapshot = impl_->assistant;
  }

  lnb::save_assistant(uuid, snapshot);
}

std::optional<Session> interface::session(std::uint32_t session_id) const {
  if (impl_->sessions == nullptr) {
    return std::nullopt;
  }

  return impl_->sessions->get(session_id);
}

std::vector<Message> interface::messages(std::uint32_t session_id) const {
  if (impl_->sessions == nullptr) {
    return {};
  }

  const auto message_mgr = impl_->sessions->get_messages(session_id);
  return message_mgr.has_value() ? message_mgr->all() : std::vector<Message>{};
}

std::vector<Message> interface::recent_messages(
    std::uint32_t session_id, std::size_t count
) const {
  if (impl_->sessions == nullptr) {
    return {};
  }

  const auto message_mgr = impl_->sessions->get_messages(session_id);
  return message_mgr.has_value() ? message_mgr->most_recent(count)
                                 : std::vector<Message>{};
}

std::vector<Session> interface::recent_sessions(std::size_t count) const {
  if (impl_->sessions == nullptr) {
    return {};
  }

  return impl_->sessions->most_recent(count);
}

std::unordered_map<std::uint32_t, std::string>
interface::session_last_times() const {
  if (impl_->sessions == nullptr) {
    return {};
  }

  return impl_->sessions->db().session_last_times();
}

std::optional<std::string> interface::last_message_time(
    std::uint32_t session_id
) const {
  if (impl_->sessions == nullptr) {
    return std::nullopt;
  }

  return impl_->sessions->db().last_message_time(session_id);
}

ContextUsage interface::context_usage(std::uint32_t session_id) const {
  ContextUsage out;

  if (impl_->sessions == nullptr) {
    return out;
  }

  const auto session = impl_->sessions->get(session_id);
  if (!session.has_value()) {
    return out;
  }

  const auto message_mgr = impl_->sessions->get_messages(session_id);
  if (!message_mgr.has_value()) {
    return out;
  }

  const net::ContextUsage usage =
      net::context_usage(*session, message_mgr->all());
  out.used_tokens = usage.used_tokens;
  out.limit_tokens = usage.limit_tokens;
  return out;
}

std::uint32_t interface::create_session(const Session& session) {
  if (impl_->sessions == nullptr) {
    return 0;
  }

  const std::uint32_t id = impl_->sessions->create(session);
  mark_assistant_summary_stale();
  return id;
}

void interface::save_session(const Session& session) {
  if (impl_->sessions == nullptr) {
    return;
  }

  impl_->sessions->save(session);
  mark_assistant_summary_stale();
}

void interface::delete_session(std::uint32_t session_id) {
  if (impl_->sessions == nullptr) {
    return;
  }

  // Stop the session's own task before its row disappears. The worker bails at
  // its next stop check; the slot is kept so the tasks window still has its log.
  cancel_session(session_id);

  impl_->sessions->remove(session_id);
  mark_assistant_summary_stale();
}

void interface::set_session_reasoning_effort(
    std::uint32_t session_id, std::string level
) {
  if (impl_->sessions == nullptr) {
    return;
  }

  impl_->sessions->db().set_session_reasoning_effort(session_id, level);
}

void interface::append_message(
    std::uint32_t session_id, Role role, std::string content
) {
  if (impl_->sessions == nullptr) {
    return;
  }

  auto message_mgr = impl_->sessions->get_messages(session_id);

  if (!message_mgr.has_value()) {
    return;
  }

  message_mgr->append(role, std::move(content));
  mark_assistant_summary_stale();
}

void interface::delete_message(std::uint32_t message_id) {
  if (impl_->sessions == nullptr) {
    return;
  }

  (void)impl_->sessions->db().remove_message(message_id);
  mark_assistant_summary_stale();
}

void interface::edit_message(
    std::uint32_t message_id, std::string content
) {
  if (impl_->sessions == nullptr) {
    return;
  }

  impl_->sessions->db().update_message(message_id, content);
  mark_assistant_summary_stale();
}

std::size_t interface::message_tokens(const Message& message) const {
  return context::message_tokens(message);
}

std::string interface::default_reasoning_effort() const {
  const Assistant current = assistant();
  return net::default_reasoning_effort(current.model_);
}

std::vector<std::string> interface::reasoning_efforts_with_stored(
    const std::string& stored
) const {
  const Assistant current = assistant();
  return net::reasoning_efforts_with_stored(current.model_, stored);
}

error_code interface::has_session_summary_trigger_met(
    std::uint32_t session_id
) const {
  if (impl_->sessions == nullptr) {
    return error_code::ERR_INVALID_SESSION_ID;
  }

  const auto session = impl_->sessions->get(session_id);
  if (!session.has_value()) {
    return error_code::ERR_INVALID_SESSION_ID;
  }

  const auto message_mgr = impl_->sessions->get_messages(session_id);
  if (!message_mgr.has_value()) {
    return error_code::ERR_INVALID_SESSION_ID;
  }

  // The trigger is measured on the unsummarised conversation alone, never on the
  // system prompt or the memories.
  const auto pieces = message_mgr->all_messages_after_summary_watermark();

  // Reject if the summarisation has reached the limit but the function is still
  // called. The caller must fold the session summary first.
  if (context::transcript_tokens(pieces) >= session->summary_trigger_tokens_) {
    return error_code::ERR_SESSION_SUMMARY_TRIGGER_HAS_MET;
  }

  // The assistant trigger is measured across every session, against the
  // assistant's own trigger.
  const auto global_pieces =
      impl_->sessions->most_recent_after_assistant_memory_watermark();

  const Assistant current = assistant();

  if (context::transcript_tokens(global_pieces) >=
      current.summary_trigger_tokens_) {
    return error_code::ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET;
  }

  return error_code::SUCCESS;
}

error_code interface::session_prompt(
    std::uint32_t session_id, const std::string& prompt
) {
  if (impl_->sessions == nullptr) {
    return error_code::ERR_INVALID_SESSION_ID;
  }

  // A session owns one task at a time, and the shared assistant fold reads every
  // session's rows: refuse a prompt that would start a task while either is in
  // flight, instead of letting the new turn corrupt the running one's work.
  {
    std::scoped_lock lock(impl_->tasks_mutex);

    impl_->prune_finished_sessions();

    if (impl_->assistant_slot.busy()) {
      return error_code::ERR_ASSISTANT_BUSY;
    }

    const auto it = impl_->session_tasks.find(session_id);

    if (it != impl_->session_tasks.end() && it->second.busy()) {
      return error_code::ERR_SESSION_BUSY;
    }
  }

  const error_code met = has_session_summary_trigger_met(session_id);
  if (met != error_code::SUCCESS) {
    return met;
  }

  auto message_mgr = impl_->sessions->get_messages(session_id);
  if (!message_mgr.has_value()) {
    return error_code::ERR_INVALID_SESSION_ID;
  }

  // Persist the prompt before the turn starts, so a failed generation still
  // leaves the user's message in the transcript.
  message_mgr->append(Role::User, prompt);
  mark_assistant_summary_stale();

  return error_code::SUCCESS;
}

bool interface::busy() const {
  std::scoped_lock lock(impl_->tasks_mutex);

  if (impl_->assistant_slot.busy()) {
    return true;
  }

  return std::any_of(
      impl_->session_tasks.begin(), impl_->session_tasks.end(),
      [](const auto& entry) { return entry.second.busy(); }
  );
}

bool interface::session_busy(std::uint32_t session_id) const {
  std::scoped_lock lock(impl_->tasks_mutex);

  const auto it = impl_->session_tasks.find(session_id);
  return it != impl_->session_tasks.end() && it->second.busy();
}

bool interface::assistant_busy() const {
  std::scoped_lock lock(impl_->tasks_mutex);
  return impl_->assistant_slot.busy();
}

std::shared_ptr<GenerationTask> interface::assistant_task() const {
  std::scoped_lock lock(impl_->tasks_mutex);
  return impl_->assistant_slot.task;
}

bool interface::assistant_summary_due() const {
  if (impl_->sessions == nullptr) {
    return false;
  }

  std::scoped_lock lock(impl_->assistant_mutex);

  if (!impl_->assistant_due_cache.has_value()) {
    impl_->assistant_due_cache = net::assistant_trigger_reached(
        impl_->sessions->db(), impl_->assistant
    );
  }

  return *impl_->assistant_due_cache;
}

void interface::mark_assistant_summary_stale() {
  std::scoped_lock lock(impl_->assistant_mutex);
  impl_->assistant_due_cache.reset();
}

std::shared_ptr<GenerationTask> interface::start_generation(
    std::uint32_t session_id, SummaryApprover approver
) {
  if (impl_->sessions == nullptr) {
    return nullptr;
  }

  {
    std::scoped_lock lock(impl_->tasks_mutex);

    impl_->prune_finished_sessions();

    if (impl_->assistant_slot.busy()) {
      return nullptr;
    }
  }

  // Snapshot the assistant so the worker never reads state the UI thread may be
  // mutating.
  const Assistant snapshot = assistant();

  return impl_->spawn_task(
      impl_->session_tasks[session_id],
      [this, session_id, snapshot, approver = std::move(approver)](
          GenerationTask& task
      ) {
        auto session = impl_->sessions->get(session_id);
        if (!session.has_value()) {
          task.fail("Invalid Session ID");
          return;
        }

        auto message_mgr = impl_->sessions->get_messages(session_id);
        if (!message_mgr.has_value()) {
          task.fail("Invalid Session ID");
          return;
        }

        const net::Remote remote = remote_for(snapshot);
        const std::stop_token stop = task.stop_token();

        // Best effort: a failed fold must not block the reply.
        const std::string folded = net::maybe_summarize_session(
            impl_->sessions->db(), *session, remote, task, stop, approver
        );

        if (!folded.empty()) {
          session->summary_ = folded;
        }

        net::Request request = net::build_session_request(
            snapshot, *session, message_mgr->all()
        );

        const auto outcome = net::generate_reply(
            std::move(request), remote, task, stop,
            session->shared_directories_, *session
        );

        if (!outcome) {
          task.fail(outcome.error().reason);
          return;
        }

        const net::ChatCompletionResult& result = *outcome;

        // A cancelled turn keeps nothing; a completed one stores the reply.
        if (!stop.stop_requested() && !result.text.empty()) {
          message_mgr->append(Role::Assistant, result.text);
          mark_assistant_summary_stale();
        }
      }
  );
}

std::shared_ptr<GenerationTask> interface::summarize_session(
    std::uint32_t session_id, bool force, SummaryApprover approver
) {
  if (impl_->sessions == nullptr) {
    return nullptr;
  }

  {
    std::scoped_lock lock(impl_->tasks_mutex);

    impl_->prune_finished_sessions();

    if (impl_->assistant_slot.busy()) {
      return nullptr;
    }
  }

  const Assistant snapshot = assistant();

  return impl_->spawn_task(
      impl_->session_tasks[session_id],
      [this, session_id, force, approver = std::move(approver), snapshot](
          GenerationTask& task
      ) {
        const auto session = impl_->sessions->get(session_id);
        if (!session.has_value()) {
          task.fail("Invalid Session ID");
          return;
        }

        const net::Remote remote = remote_for(snapshot);
        const std::stop_token stop = task.stop_token();

        if (force) {
          net::summarize_session_now(
              impl_->sessions->db(), *session, remote, task, stop, approver
          );
        } else {
          net::maybe_summarize_session(
              impl_->sessions->db(), *session, remote, task, stop, approver
          );
        }

        // The fold advances the session watermark, so the assistant trigger
        // may have moved.
        mark_assistant_summary_stale();
      }
  );
}

std::shared_ptr<GenerationTask> interface::summarize_assistant(
    bool force, SummaryApprover approver
) {
  if (impl_->sessions == nullptr) {
    return nullptr;
  }

  const Assistant snapshot = assistant();

  return impl_->spawn_task(
      impl_->assistant_slot,
      [this, force, approver = std::move(approver), snapshot](
          GenerationTask& task
      ) {
        // The fold reads every session's rows, so it must not overlap a session
        // turn. New session tasks are already blocked (the assistant slot is
        // busy); wait for the ones that started first, without holding the task
        // lock so a finishing worker can publish its result.
        std::vector<impl::executor::handle> workers;

        {
          std::scoped_lock lock(impl_->tasks_mutex);

          workers.reserve(impl_->session_tasks.size());

          for (const auto& [id, slot] : impl_->session_tasks) {
            workers.push_back(slot.worker);
          }
        }

        if (std::any_of(workers.begin(), workers.end(), [](const auto& worker) {
              return !worker.finished();
            })) {
          task.set_status("Waiting for other tasks...");
        }

        const std::stop_token stop = task.stop_token();

        for (auto& worker : workers) {
          while (!worker.finished()) {
            if (stop.stop_requested()) {
              return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
        }

        const net::Remote remote = remote_for(snapshot);

        std::string memory;

        if (force) {
          memory = net::summarize_assistant_now(
              impl_->sessions->db(), snapshot, remote, task, stop, approver
          );
        } else {
          memory = net::maybe_summarize_assistant(
              impl_->sessions->db(), snapshot, remote, task, stop, approver
          );
        }

        // The assistant is passed by const reference to the summarisers, so the
        // interface is the one that publishes the new memory.
        if (!memory.empty()) {
          set_assistant_memory(std::move(memory));
          save_assistant();
        }

        // The fold advanced every session's assistant watermark.
        mark_assistant_summary_stale();
      }
  );
}

void interface::cancel_session(std::uint32_t session_id) {
  std::scoped_lock lock(impl_->tasks_mutex);

  const auto it = impl_->session_tasks.find(session_id);

  if (it != impl_->session_tasks.end() && it->second.task != nullptr) {
    it->second.task->cancel();
  }
}

void interface::cancel_assistant() {
  std::scoped_lock lock(impl_->tasks_mutex);

  if (impl_->assistant_slot.task != nullptr) {
    impl_->assistant_slot.task->cancel();
  }
}

void interface::wait_for_generation() {
  std::vector<impl::executor::handle> workers;

  {
    std::scoped_lock lock(impl_->tasks_mutex);

    workers.reserve(impl_->session_tasks.size() + 1);

    for (const auto& [id, slot] : impl_->session_tasks) {
      workers.push_back(slot.worker);
    }

    workers.push_back(impl_->assistant_slot.worker);
  }

  for (auto& worker : workers) {
    worker.wait();
  }
}

// --- Process-wide storage and assistant registry ------------------------

void set_data_root(std::filesystem::path root) {
  store::paths::set_data_root(std::move(root));
}

std::filesystem::path data_root() {
  return store::paths::data_root();
}

std::filesystem::path global_settings_path() {
  return store::paths::global_settings();
}

std::vector<std::string> scan_assistants() {
  return store::scan_assistants();
}

Assistant load_assistant(const std::string& uuid) {
  return ::lnb::load_assistant(uuid);
}

void save_assistant(const std::string& uuid, const Assistant& assistant) {
  ::lnb::save_assistant(uuid, assistant);
}

std::string make_uuid() {
  return store::make_uuid();
}

std::string now_iso8601_utc() {
  return store::now_iso8601_utc();
}

std::string ensure_scheme(const std::string& url) {
  return net::ensure_scheme(url);
}

std::string normalize_base_url(const std::string& url) {
  return net::normalize_base_url(url);
}

bool is_valid_base_url(const std::string& url) {
  return net::is_valid_base_url(url);
}

std::string reasoning_default_for(const std::string& model) {
  return net::default_reasoning_effort(model);
}

std::vector<std::string> reasoning_levels_for(
    const std::string& model, const std::string& stored
) {
  return net::reasoning_efforts_with_stored(model, stored);
}

} // namespace lnb::api
