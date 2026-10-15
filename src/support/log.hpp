#pragma once

#include <cstddef>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// A tiny, dependency-free logger. The legacy application used the heavy `mmc`
// library, whose static archive drags in audio codecs and OpenGL; the
// reconstructed app keeps its dependency surface to Qt and SQLite, so logging
// lives here instead. The interface mirrors mmc::log (fmt-style arguments).
//
// Output is buffered in memory until a sink is attached with attach(). The
// application attaches the open assistant's log file in open_assistant(), so
// startup messages that are logged before an assistant is chosen still land at
// the top of that assistant's log rather than on the command line.
namespace lnb::log {

namespace detail {

// The mutex is deliberately leaked: logging may happen from static destructors,
// and a leaked lock can never be destroyed before its last user.
inline std::mutex& mutex() {
  static std::mutex* instance = new std::mutex();
  return *instance;
}

// The active sink. Null means "no assistant log yet", so lines are buffered.
inline std::FILE*& sink() {
  static std::FILE* file = nullptr;
  return file;
}

// Lines written before a sink existed. Bounded so a sink that never arrives (for
// example, the picker was cancelled) cannot grow this without limit.
inline std::vector<std::string>& pending() {
  static std::vector<std::string> lines;
  return lines;
}

inline constexpr std::size_t kPendingLimit = 4096;

inline void write_line(const std::string& line) {
  std::FILE* out = sink();

  if (out == nullptr) {
    if (pending().size() >= kPendingLimit) {
      pending().erase(pending().begin());
    }

    pending().push_back(line);
    return;
  }

  std::fwrite(line.data(), 1, line.size(), out);
  std::fflush(out);
}

inline void write(const char* level, const std::string& message) {
  std::lock_guard lock(mutex());
  write_line(std::format("[{}] {}\n", level, message));
}

} // namespace detail

// Redirects all subsequent log output into `file`, which the caller owns and
// must keep open until it calls attach(nullptr). Lines buffered before this call
// are written first, so a startup message is never lost. Passing nullptr resumes
// buffering.
inline void attach(std::FILE* file) {
  std::lock_guard lock(detail::mutex());

  detail::sink() = file;

  if (file != nullptr) {
    for (const std::string& line: detail::pending()) {
      std::fwrite(line.data(), 1, line.size(), file);
    }

    detail::pending().clear();
    std::fflush(file);
  }
}

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
