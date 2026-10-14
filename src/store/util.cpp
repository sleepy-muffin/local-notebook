#include "store/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace lnb::store {

std::string now_iso8601_utc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);

  std::tm broken_down{};
#if defined(_WIN32)
  gmtime_s(&broken_down, &seconds);
#else
  gmtime_r(&seconds, &broken_down);
#endif

  char buffer[32];
  const std::size_t written =
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &broken_down);
  return std::string(buffer, written);
}

std::string make_uuid() {
  static thread_local std::mt19937_64 engine(std::random_device{}());
  std::uniform_int_distribution<std::uint32_t> distribution(0, 0xFFFFFFFFu);

  std::uint32_t parts[4];
  for (auto& part : parts) {
    part = distribution(engine);
  }

  // Set the version (4) and variant (RFC 4122) bits so this is a valid v4 UUID.
  parts[1] = (parts[1] & 0xFFFF0FFFu) | 0x00004000u;
  parts[2] = (parts[2] & 0x3FFFFFFFu) | 0x80000000u;

  char buffer[37];
  std::snprintf(
    buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%04x%08x", parts[0],
    (parts[1] >> 16) & 0xFFFFu, parts[1] & 0xFFFFu, (parts[2] >> 16) & 0xFFFFu,
    parts[2] & 0xFFFFu, parts[3]
  );
  return buffer;
}

namespace paths {

std::filesystem::path data_root() {
  return std::filesystem::path("data");
}

std::filesystem::path global_settings() {
  return data_root() / "settings.json";
}

std::filesystem::path assistant_dir(std::string_view uuid) {
  return data_root() / std::string(uuid);
}

std::filesystem::path assistant_settings(std::string_view uuid) {
  return assistant_dir(uuid) / "settings.json";
}

std::filesystem::path assistant_db(std::string_view uuid) {
  return assistant_dir(uuid) / "data.db";
}

} // namespace paths

std::vector<std::string> scan_assistants() {
  std::vector<std::string> result;

  std::error_code error;
  const std::filesystem::path root = paths::data_root();
  if (!std::filesystem::is_directory(root, error)) {
    return result;
  }

  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    if (error) {
      break;
    }
    if (!entry.is_directory()) {
      continue;
    }
    if (std::filesystem::exists(entry.path() / "settings.json", error)) {
      result.push_back(entry.path().filename().string());
    }
  }

  std::sort(result.begin(), result.end());
  return result;
}

} // namespace lnb::store
