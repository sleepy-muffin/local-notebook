#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "model.hpp"

namespace lnb::utils {

// --- Process-wide storage and assistant registry ------------------------
//
// The UI needs these before an assistant is open (to load the global settings
// and list the assistants). They are free functions rather than members because
// they describe the on-disk tree, not one open assistant.

class store {

public:
  store(std::filesystem::path root) {
    g_data_root = root.empty() ? std::filesystem::path{"data"} : std::move(root);
  }

  [[nodiscard]] std::filesystem::path data_root() const;

  [[nodiscard]] std::filesystem::path global_settings_path() const;

  [[nodiscard]] std::vector<std::string> scan_assistants();
  [[nodiscard]] Assistant load_assistant(const std::string &uuid);


  [[nodiscard]] std::filesystem::path assistant_dir(std::string_view uuid) const;
  [[nodiscard]] std::filesystem::path assistant_settings(std::string_view uuid) const;
  [[nodiscard]] std::filesystem::path assistant_db(std::string_view uuid);
  [[nodiscard]] std::filesystem::path assistant_log(std::string_view uuid);

  // The instance lock held while this process has the assistant open. It is
  // deliberately never deleted; see instance_lock for why.
  [[nodiscard]] std::filesystem::path assistant_lock(std::string_view uuid);

  void save_assistant(const std::string &uuid, const Assistant &assistant);

private:
  // Mutable so main() can redirect the whole tree. Defaults to the historical
  // working-directory-relative location that the tests and the screenshot tool rely
  // on; the application overrides it before constructing the Application.
  std::filesystem::path g_data_root{"data"};

};

[[nodiscard]] std::string make_uuid();
[[nodiscard]] std::string now_iso8601_utc();



} // namespace lnb::utils
