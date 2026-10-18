#include "local-notebook-core/interface.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "app/assistant_io.hpp"
#include "context/context.hpp"
#include "mmc/async.hpp"
#include "mmc/log.hpp"
#include "net/client.hpp"
#include "store/managers.hpp"
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
  using executor = mmc::async::multi_executor<void()>;

  impl() {
    // Two lanes: one for the turn, one so a summarisation pass can run without
    // the executor ever needing a thread of its own. The interface still keeps
    // at most one task in flight, so the second lane is headroom, not a licence
    // to run turns concurrently.
    //
    // Important: Since we don't block other sessions while a session is running.
    // There should be more than 2 threads so the user can launch many tasks.
    multi_executor_ = std::make_unique<executor>(5);
  }

  ~impl() {
    stop_worker();
    multi_executor_->shutdown();
  }

  void open_log(const std::string& uuid) {
    const auto path = store::paths::assistant_log(uuid);

    try {
      mmc::log::set_output_file(path);
    } catch (const std::exception& error) {
      // Logging stays on the previous sink; a bad path is never fatal.
      mmc::log::error("cannot open log file {}: {}", path.string(), error.what());
    }

    mmc::log::info("===== local-notebook session started =====");
  }

  // Cancels the in-flight turn (if any) and waits for its executor job. Called
  // before the database is replaced and from the destructor.
  void stop_worker() {
    if (task != nullptr) {
      task->cancel();
    }

    worker.request_stop();
    worker.wait();
    worker = {};
    task.reset();
  }

  // Runs `work` on the executor and publishes it as the current task. The
  // returned task is the same object the caller polls.
  template<typename Work>
  std::shared_ptr<GenerationTask> spawn_task(Work&& work) {
    stop_worker();

    auto next = std::make_shared<GenerationTask>();
    task = next;

    worker = multi_executor_->spawn(
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

    return next;
  }

  // std::unique_ptr<mmc::log::backend> logger;
  std::unique_ptr<executor> multi_executor_;

  std::string assistant_uuid;
  Assistant assistant;
  mutable std::mutex assistant_mutex;

  // The cached assistant-memory trigger. Guarded by assistant_mutex, and
  // dropped whenever a change can move it.
  mutable std::optional<bool> assistant_due_cache;

  // The manager owns the database; the interface only talks to the store
  // through it.
  std::unique_ptr<store::SessionManager> sessions;

  // The session whose prompt is waiting for start_generation().
  std::optional<std::uint32_t> pending_session_id;

  // The current (or last finished) turn. Shared with the caller so it can poll
  // after start_generation() returns.
  std::shared_ptr<GenerationTask> task;
  executor::handle worker;
};

interface::interface() : impl_(std::make_unique<impl>()) {}

interface::~interface() = default;

void interface::open_assistant(std::string uuid, Assistant assistant) {
  // The worker reads `sessions` through the impl, so it must be gone before the
  // database it is using is replaced.
  impl_->stop_worker();

  {
    std::scoped_lock lock(impl_->assistant_mutex);
    impl_->assistant_uuid = std::move(uuid);
    impl_->assistant = std::move(assistant);
    impl_->assistant_due_cache.reset();
  }

  const auto directory = store::paths::assistant_dir(impl_->assistant_uuid);
  std::error_code error;
  std::filesystem::create_directories(directory, error);

  // Everything from here on is written to this assistant's log, including the
  // startup lines buffered before an assistant was chosen.
  impl_->open_log(impl_->assistant_uuid);

  // The manager owns the database from here on: the interface only talks to the
  // store through it.
  impl_->sessions = std::make_unique<store::SessionManager>(
      store::paths::assistant_db(impl_->assistant_uuid).string()
  );

  impl_->pending_session_id.reset();

  mmc::log::info(
      "opened assistant {} at {}", impl_->assistant_uuid, directory.string()
  );
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

error_code interface::has_session_memory_trigger_met(
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
  const auto pieces = message_mgr->all_messages_after_session_memory_watermark();

  // Reject if the summarisation has reached the limit but the function is still
  // called. The caller must fold the session memory first.
  if (context::transcript_tokens(pieces) >= session->memory_trigger_tokens_) {
    return error_code::ERR_SESSION_MEMORY_TRIGGER_HAS_MET;
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
  const error_code met = has_session_memory_trigger_met(session_id);
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

  // Remember which session the next start_generation() belongs to.
  impl_->pending_session_id = session_id;

  return error_code::SUCCESS;
}

bool interface::busy() const {
  return impl_->task != nullptr && !impl_->task->is_terminal();
}

std::shared_ptr<GenerationTask> interface::current_task() const {
  return impl_->task;
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

std::shared_ptr<GenerationTask> interface::start_generation() {
  if (impl_->sessions == nullptr || !impl_->pending_session_id.has_value()) {
    return nullptr;
  }

  const std::uint32_t session_id = *impl_->pending_session_id;
  impl_->pending_session_id.reset();

  // Snapshot the assistant so the worker never reads state the UI thread may be
  // mutating.
  const Assistant snapshot = assistant();

  return impl_->spawn_task(
      [this, session_id, snapshot](GenerationTask& task) {
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
            impl_->sessions->db(), *session, remote, task, stop
        );

        if (!folded.empty()) {
          session->memory_ = folded;
        }

        net::Request request = net::build_session_request(
            snapshot, *session, message_mgr->all()
        );

        const net::ChatCompletionResult result = net::generate_reply(
            std::move(request), remote, task, stop, session->shared_directory_,
            *session
        );

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

  const Assistant snapshot = assistant();

  return impl_->spawn_task(
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
      [this, force, approver = std::move(approver), snapshot](
          GenerationTask& task
      ) {
        const net::Remote remote = remote_for(snapshot);
        const std::stop_token stop = task.stop_token();

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

void interface::cancel_generation() {
  if (impl_->task != nullptr) {
    impl_->task->cancel();
  }
}

void interface::wait_for_generation() {
  impl_->worker.wait();
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
