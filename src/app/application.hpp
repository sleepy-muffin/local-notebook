#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/settings.hpp"
#include "model.hpp"
#include "store/database.hpp"
#include "store/managers.hpp"

namespace lnb {

// Owns everything the windows share: the global settings, the one assistant
// open in this process, and that assistant's database and managers. A single
// Application instance is created in main() and passed by reference.
class Application {
public:
  Application();
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  [[nodiscard]] Settings& settings() { return settings_; }
  [[nodiscard]] const Settings& settings() const { return settings_; }
  void save_settings();

  [[nodiscard]] bool has_assistant() const { return db_ != nullptr; }
  [[nodiscard]] const std::string& assistant_uuid() const {
    return assistant_uuid_;
  }
  [[nodiscard]] Assistant& assistant() { return assistant_; }
  [[nodiscard]] const Assistant& assistant() const { return assistant_; }

  [[nodiscard]] store::database& db() { return *db_; }
  [[nodiscard]] store::SessionManager& sessions() { return *sessions_; }
  [[nodiscard]] store::MessageManager messages(std::uint32_t session_id);

  // Opens the assistant directory `uuid`, creating its database when needed,
  // and records it as the last-opened assistant in the global settings.
  void open_assistant(std::string uuid, Assistant assistant);

  // Persists assistant() to data/<uuid>/settings.json.
  void save_assistant();

  void shutdown();
  [[nodiscard]] bool shutting_down() const { return shutting_down_; }

private:
  Settings settings_;
  std::string assistant_uuid_;
  Assistant assistant_;
  std::unique_ptr<store::database> db_;
  std::unique_ptr<store::SessionManager> sessions_;
  bool shutting_down_ = false;
};

} // namespace lnb
