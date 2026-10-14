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

// On-disk layout. All paths are relative to the process working directory:
//
//   data/settings.json            app-global settings
//   data/<uuid>/settings.json     assistant-level settings
//   data/<uuid>/data.db           sessions + messages
namespace paths {

[[nodiscard]] std::filesystem::path data_root();
[[nodiscard]] std::filesystem::path global_settings();
[[nodiscard]] std::filesystem::path assistant_dir(std::string_view uuid);
[[nodiscard]] std::filesystem::path assistant_settings(std::string_view uuid);
[[nodiscard]] std::filesystem::path assistant_db(std::string_view uuid);

} // namespace paths

// Directory names under data_root() that contain a settings.json, sorted.
// This is the assistant picker's registry: there is no root index file.
[[nodiscard]] std::vector<std::string> scan_assistants();

} // namespace lnb::store
