#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Small, dependency-free helpers shared by the store (design-1.md §4).
namespace lnb::store {

// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ". ISO-8601 in UTC sorts
// lexicographically in time order, which is what assistant memory relies on.
[[nodiscard]] std::string now_iso8601_utc();

// A random UUID (version 4), lowercase, 36 characters with dashes. It exists
// only to avoid directory collisions and never appears in the UI.
[[nodiscard]] std::string make_uuid();

// On-disk layout. Every path is derived from a single data root:
//
//   <root>/settings.json              app-global settings
//   <root>/<uuid>/settings.json       assistant-level settings
//   <root>/<uuid>/data.db             sessions + messages
//   <root>/<uuid>/local-notebook.log  assistant-level log
//   <root>/<uuid>/lock                one-instance guard (see utils/lock.hpp)
//
// The root defaults to the relative "data" so headless tests and the screenshot
// tool can sandbox it by changing the working directory. The application calls
// set_data_root() at startup with the per-user application data directory, or the
// --user-data override; the UI resolves it before opening the interface.
namespace paths {

// Points every derived path at `root`. main() calls this once before constructing
// the Application; an empty path restores the built-in relative default.
void set_data_root(std::filesystem::path root);

[[nodiscard]] std::filesystem::path data_root();
[[nodiscard]] std::filesystem::path global_settings();
[[nodiscard]] std::filesystem::path assistant_dir(std::string_view uuid);
[[nodiscard]] std::filesystem::path assistant_settings(std::string_view uuid);
[[nodiscard]] std::filesystem::path assistant_db(std::string_view uuid);
[[nodiscard]] std::filesystem::path assistant_log(std::string_view uuid);

// The instance lock held while this process has the assistant open. It is
// deliberately never deleted; see instance_lock for why.
[[nodiscard]] std::filesystem::path assistant_lock(std::string_view uuid);

} // namespace paths

// Directory names under data_root() that contain a settings.json, sorted.
// This is the assistant picker's registry: there is no root index file.
[[nodiscard]] std::vector<std::string> scan_assistants();

} // namespace lnb::store
