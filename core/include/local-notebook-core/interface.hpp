#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "local-notebook-core/error_code.hpp"
#include "local-notebook-core/generation_task.hpp"
#include "local-notebook-core/model.hpp"
#include "local-notebook-core/summary.hpp"

// The one public entry point into the application core.
//
// Everything the UI needs lives here and nowhere else: `core/src` is private and
// is never on the UI's include path. The implementation is a pimpl, so this
// header carries no store, network or executor types.
namespace lnb::api {

// The title-bar readout for one session: the estimated cost of the
// unsummarised conversation alone, against the session's trigger. It is exactly
// the quantity the automatic collapse compares against.
struct ContextUsage {
  std::size_t used_tokens = 0;
  std::size_t limit_tokens = 0;
};

class interface {
public:
  interface();
  ~interface();

  interface(const interface&) = delete;
  interface& operator=(const interface&) = delete;

  // Opens the assistant's directory and the database it owns. Any turn still in
  // flight is cancelled and joined before the old database is replaced.
  void open_assistant(std::string uuid, Assistant assistant);

  [[nodiscard]] bool has_assistant() const;
  [[nodiscard]] std::string assistant_uuid() const;

  // A copy, taken under the interface's lock, so the UI can read it from any
  // thread while a worker is running.
  [[nodiscard]] Assistant assistant() const;

  // Replaces the in-memory assistant. The next turn and every summariser
  // snapshot it, so a caller can edit a copy and publish it atomically.
  void set_assistant(Assistant assistant);

  void set_assistant_memory(std::string memory);

  // Persists assistant() to data/<uuid>/settings.json.
  void save_assistant();

  // --- Store queries -----------------------------------------------------

  [[nodiscard]] std::optional<Session> session(std::uint32_t session_id) const;

  // The whole transcript of a session, oldest first.
  [[nodiscard]] std::vector<Message> messages(std::uint32_t session_id) const;

  // The newest `count` messages of a session, oldest first.
  [[nodiscard]] std::vector<Message> recent_messages(
      std::uint32_t session_id, std::size_t count
  ) const;

  // Sessions ordered by their newest message, newest first.
  [[nodiscard]] std::vector<Session> recent_sessions(std::size_t count) const;

  // The newest message time of every session, keyed by session id. One query
  // for the whole sidebar, so listing rows does not cost one query per row.
  [[nodiscard]] std::unordered_map<std::uint32_t, std::string>
  session_last_times() const;

  // The newest message time of one session, when it has any messages.
  [[nodiscard]] std::optional<std::string> last_message_time(
      std::uint32_t session_id
  ) const;

  // The conversation-token readout for the title bar.
  [[nodiscard]] ContextUsage context_usage(std::uint32_t session_id) const;

  // Inserts `session` and returns its new id. `session.id_` is ignored.
  std::uint32_t create_session(const Session& session);
  void save_session(const Session& session);
  void delete_session(std::uint32_t session_id);

  void set_session_reasoning_effort(
      std::uint32_t session_id, std::string level
  );
  void delete_message(std::uint32_t message_id);
  void edit_message(std::uint32_t message_id, std::string content);

  // The estimated token cost of one stored message.
  [[nodiscard]] std::size_t message_tokens(const Message& message) const;

  // Reasoning levels for the open assistant's model.
  [[nodiscard]] std::string default_reasoning_effort() const;
  [[nodiscard]] std::vector<std::string> reasoning_efforts_with_stored(
      const std::string& stored
  ) const;

  // --- Turns -------------------------------------------------------------

  // True while a turn or summarisation is in flight. The interface runs one
  // task at a time, so a caller can use this to refuse a send that would
  // otherwise cancel another session's work.
  [[nodiscard]] bool busy() const;

  // The current (or last finished) task, shared with the interface. nullptr
  // before any task has been started.
  [[nodiscard]] std::shared_ptr<GenerationTask> current_task() const;

  // True when the shared assistant-memory trigger is reached. The answer is
  // cached; mark_assistant_summary_stale() drops the cache after a change that
  // can move it (a new message, a fold, an edited budget).
  [[nodiscard]] bool assistant_summary_due() const;
  void mark_assistant_summary_stale();

  // SUCCESS when a prompt may be sent; otherwise the first trigger that is due.
  [[nodiscard]] error_code has_session_memory_trigger_met(
      std::uint32_t session_id
  ) const;

  // Validates the session and both memory triggers, then persists the user
  // message. The turn itself is started by start_generation().
  [[nodiscard]] error_code session_prompt(
      std::uint32_t session_id, const std::string& prompt
  );

  // Starts the turn for the prompt session_prompt() just stored. Returns nullptr
  // when no prompt is pending or no assistant is open.
  //
  // The returned task is shared with the interface, which keeps it alive while
  // the executor runs; the caller only polls its snapshot.
  [[nodiscard]] std::shared_ptr<GenerationTask> start_generation();

  // Folds a session's overflow into memory. `force` ignores the automatic
  // trigger. The task is polled exactly like a turn.
  [[nodiscard]] std::shared_ptr<GenerationTask> summarize_session(
      std::uint32_t session_id, bool force, SummaryApprover approver = {}
  );

  // Folds every session's contribution into the assistant memory. The interface
  // persists the resulting memory itself.
  [[nodiscard]] std::shared_ptr<GenerationTask> summarize_assistant(
      bool force, SummaryApprover approver = {}
  );

  // Requests the in-flight turn to stop. The partial reply is discarded.
  void cancel_generation();

  // Waits for the executor job to exit. The destructor calls this, so an
  // interface is never torn down while its worker still reads the database.
  void wait_for_generation();

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

// --- Process-wide storage and assistant registry ------------------------
//
// The UI needs these before an assistant is open (to load the global settings
// and list the assistants). They are free functions rather than members because
// they describe the on-disk tree, not one open assistant.

void set_data_root(std::filesystem::path root);
[[nodiscard]] std::filesystem::path data_root();
[[nodiscard]] std::filesystem::path global_settings_path();

[[nodiscard]] std::vector<std::string> scan_assistants();
[[nodiscard]] Assistant load_assistant(const std::string& uuid);
void save_assistant(const std::string& uuid, const Assistant& assistant);

[[nodiscard]] std::string make_uuid();
[[nodiscard]] std::string now_iso8601_utc();

// --- Assistant-form helpers ---------------------------------------------

// Completes a scheme-less URL with https:// and normalises a base URL.
[[nodiscard]] std::string ensure_scheme(const std::string& url);
[[nodiscard]] std::string normalize_base_url(const std::string& url);
[[nodiscard]] bool is_valid_base_url(const std::string& url);

// The reasoning level a fresh assistant or session starts on, and the model's
// levels with a stored value kept visible when the model no longer offers it.
[[nodiscard]] std::string reasoning_default_for(const std::string& model);
[[nodiscard]] std::vector<std::string> reasoning_levels_for(
    const std::string& model, const std::string& stored
);

} // namespace lnb::api
