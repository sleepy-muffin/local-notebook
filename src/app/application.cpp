#include "app/application.hpp"

#include <filesystem>

#include "app/assistant_io.hpp"
#include "store/util.hpp"
#include "support/log.hpp"

namespace lnb {

Application::Application()
    : settings_(load_settings()) {}

Application::~Application() = default;

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

  db_ = std::make_unique<store::database>(
      store::paths::assistant_db(assistant_uuid_).string()
  );
  sessions_ = std::make_unique<store::SessionManager>(*db_);

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

void Application::shutdown() {
  shutting_down_ = true;
}

} // namespace lnb
