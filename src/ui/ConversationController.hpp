#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "model.hpp"
#include "support/generation_task.hpp"

namespace lnb {
class Application;
}

namespace ui {

// The shared worker state lives in lnb; bring it in so the controller reads
// naturally.
using lnb::GenerationTask;

// One session's conversation logic. It owns the background worker for the
// session's turn, so switching sessions never interrupts a generation and never
// loses the in-progress draft. The view is passive: it renders the snapshot the
// controller exposes and forwards intent through the callbacks below.
class ConversationController {
public:
  ConversationController(lnb::Application& app, std::uint32_t session_id);
  ~ConversationController();

  ConversationController(const ConversationController&) = delete;
  ConversationController& operator=(const ConversationController&) = delete;

  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }

  // The session as stored (a fresh read).
  [[nodiscard]] lnb::Session session() const;

  [[nodiscard]] bool busy() const { return running_; }

  // Progress of the in-flight turn, for the running-task readout: the tool-call
  // round reached and how long the turn has been running. Both zero when no turn
  // is running.
  struct Progress {
    std::size_t rounds = 0;
    std::int64_t elapsed_ms = 0;
  };

  [[nodiscard]] Progress progress() const;

  // Appends the user message, then starts a turn in the background.
  void send(std::string text);

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

  // The UI timer calls this. It forwards streamed text and, once the worker
  // finishes, persists the assistant memory and signals a reload.
  void poll();

private:
  void start(std::function<void(GenerationTask&, std::stop_token)> work);
  void finish_worker();

  lnb::Application& app_;
  std::uint32_t session_id_;

  GenerationTask task_;
  std::jthread worker_;
  bool running_ = false;

  std::mutex result_mutex_;
  std::string pending_assistant_memory_;

  std::string last_display_;
  std::string last_status_;

  std::function<void()> on_changed_;
  std::function<void(const std::string&)> on_stream_;
  std::function<void(const std::string&)> on_status_;
  std::function<void(const std::string&)> on_error_;
  std::function<void(const std::string&)> on_warning_;
};

} // namespace ui
