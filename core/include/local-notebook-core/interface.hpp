#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

} // namespace lnb::api
