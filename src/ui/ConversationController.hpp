#pragma once

#include "../ui.hpp"
#include "../generation_task.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace ui {

class ConversationView;

// Owns the runtime state of one conversation: its transcript, the in-flight
// generation (at most one at a time) and the glue to the database and the
// worker pool.
//
// Threading: every public method, including poll(), must be called from the
// GUI thread. The generation itself runs on a worker thread and only ever
// touches the thread-safe GenerationTask, never Qt.
class ConversationController {
public:
  ConversationController(
      lnb::ApplicationState& state,
      lnb::session session_info,
      ConversationView* view
  );

  ~ConversationController();

  [[nodiscard]] uint32_t session_id() const { return session_info_.id_; }

  void set_session_info(lnb::session session_info);

  // Called with (used_tokens, limit_tokens) whenever the request's context usage
  // is known: on transcript reloads, on completion of a turn and when the
  // session's settings change. Installing a handler immediately reports the
  // current values, so a freshly created view is never left blank.
  using ContextUsageHandler = std::function<void(std::size_t, std::size_t)>;

  void set_on_context_usage(ContextUsageHandler handler);

  // Recomputes the context usage from the database and notifies the handler.
  void refresh_context_usage();

  // Loads the transcript from the database and renders it.
  void reload();

  void send(const std::string& text);
  void cancel();
  void delete_message(uint32_t message_id);
  void edit_message(uint32_t message_id, const std::string& text);

  // Advances the on-screen state; called periodically from the GUI thread.
  void poll();

  [[nodiscard]] bool busy() const { return busy_; }

private:
  lnb::ApplicationState& state_;
  lnb::session session_info_;
  ConversationView* view_ = nullptr;

  std::shared_ptr<lnb::GenerationTask> task_;
  bool busy_ = false;

  ContextUsageHandler on_context_usage_;
  std::size_t context_used_ = 0;
  std::size_t context_limit_ = 0;
};

} // namespace ui
