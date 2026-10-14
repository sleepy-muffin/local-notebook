#pragma once

#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <utility>

// A tiny, dependency-free logger. The legacy application used the heavy `mmc`
// library, whose static archive drags in audio codecs and OpenGL; the
// reconstructed app keeps its dependency surface to Qt and SQLite, so logging
// lives here instead. The interface mirrors mmc::log (fmt-style arguments).
namespace lnb::log {

namespace detail {

inline std::mutex& mutex() {
  static std::mutex instance;
  return instance;
}

inline void write(const char* level, const std::string& message) {
  std::lock_guard lock(mutex());
  std::fprintf(stderr, "[%s] %s\n", level, message.c_str());
  std::fflush(stderr);
}

} // namespace detail

template <typename... Args>
void debug(std::format_string<Args...> format, Args&&... args) {
  detail::write("debug", std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void info(std::format_string<Args...> format, Args&&... args) {
  detail::write("info", std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> format, Args&&... args) {
  detail::write("error", std::format(format, std::forward<Args>(args)...));
}

inline void kill() {}

} // namespace lnb::log
