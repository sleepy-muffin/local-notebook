#include "app/application.hpp"

#include <filesystem>

#include "app/assistant_io.hpp"
#include "net/client.hpp"
#include "store/util.hpp"
#include "support/log.hpp"

namespace lnb {

Application::Application()
    : settings_(load_settings()) {}

Application::~Application() {
  close_log();
}

void Application::open_log(const std::string& uuid) {
  close_log();

  const auto path = store::paths::assistant_log(uuid);
  std::FILE* file = std::fopen(path.string().c_str(), "a");

  if (file == nullptr) {
    // Logging stays buffered; the message below reaches the next sink.
    log::error("cannot open log file {}", path.string());
    return;
  }

  log_file_ = file;
  log::attach(log_file_);
  log::info("===== local-notebook session started =====");
}

void Application::close_log() {
  if (log_file_ == nullptr) {
    return;
  }

  // Stop the sink before closing, so no writer can touch the freed handle.
  log::attach(nullptr);
  std::fclose(log_file_);
  log_file_ = nullptr;
}

void Application::save_settings() {
  lnb::save_settings(settings_);
}

store::MessageManager Application::messages(std::uint32_t session_id) {
  return store::MessageManager{*db_, session_id};
}

void Application::open_assistant(std::string uuid, Assistant assistant) {
  assistant_uuid_ = std::move(uuid);
  assistant_ = std::move(assistant);

  const auto directory = store::paths::assistant_dir(assistant_uuid_);
  std::error_code error;
  std::filesystem::create_directories(directory, error);

  // Everything from here on is written to this assistant's log, including the
  // startup lines buffered before an assistant was chosen.
  open_log(assistant_uuid_);

  db_ = std::make_unique<store::database>(
      store::paths::assistant_db(assistant_uuid_).string()
  );
  sessions_ = std::make_unique<store::SessionManager>(*db_);

  // The freshly opened database invalidates any cached trigger from a previous
  // assistant (or the initial empty one).
  mark_assistant_summary_stale();

  log::info("opened assistant {} at {}", assistant_uuid_, directory.string());

  settings_.last_assistant = assistant_uuid_;
  save_settings();
}

void Application::save_assistant() {
  if (assistant_uuid_.empty()) {
    return;
  }

  lnb::save_assistant(assistant_uuid_, assistant_);
}

bool Application::assistant_summary_due() const {
  if (db_ == nullptr) {
    return false;
  }

  if (assistant_summary_stale_) {
    assistant_summary_due_ = net::assistant_trigger_reached(*db_, assistant_);
    assistant_summary_stale_ = false;
  }

  return assistant_summary_due_;
}

void Application::shutdown() {
  shutting_down_ = true;
}

} // namespace lnb
