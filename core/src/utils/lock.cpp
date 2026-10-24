#include "utils/lock.hpp"

#include <fstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "utils/util.hpp"

namespace lnb::store {

namespace {

// The recorded owner, as "pid <number>", when the lock file has one. Used only
// to make the refusal message useful.
std::string recorded_pid(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);

  if (!in) {
    return {};
  }

  std::string line;

  if (!std::getline(in, line)) {
    return {};
  }

  constexpr std::string_view prefix = "pid ";

  if (line.rfind(prefix, 0) != 0) {
    return {};
  }

  const std::string pid = line.substr(prefix.size());

  // A malformed or hostile line must not end up in a dialog unchecked.
  if (pid.empty() ||
      pid.find_first_not_of("0123456789") != std::string::npos) {
    return {};
  }

  return pid;
}

std::string refusal_message(const std::filesystem::path& path) {
  std::string message =
      "This assistant is already open in another Local Notebook instance";

  const std::string pid = recorded_pid(path);

  if (pid.empty()) {
    return message + ".";
  }

  return message + " (process " + pid + ").";
}

std::string lock_contents() {
#if defined(_WIN32)
  const unsigned long pid = GetCurrentProcessId();
#else
  const long pid = static_cast<long>(::getpid());
#endif

  return "pid " + std::to_string(pid) + "\nstarted " + now_iso8601_utc() + "\n";
}

void write_contents(
#if defined(_WIN32)
    HANDLE handle
#else
    int fd
#endif
) {
  const std::string text = lock_contents();

#if defined(_WIN32)
  LARGE_INTEGER start{};
  (void)SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
  (void)SetEndOfFile(handle);

  DWORD written = 0;
  (void)WriteFile(
      handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr
  );
#else
  if (::ftruncate(fd, 0) != 0) {
    return;
  }

  (void)::lseek(fd, 0, SEEK_SET);

  std::size_t offset = 0;

  while (offset < text.size()) {
    const ssize_t written =
        ::write(fd, text.data() + offset, text.size() - offset);

    if (written <= 0) {
      return;
    }

    offset += static_cast<std::size_t>(written);
  }
#endif
}

} // namespace

instance_lock::~instance_lock() {
  release();
}

bool instance_lock::acquire(const std::filesystem::path& path, std::string& error) {
  release();

  std::error_code ignored;
  std::filesystem::create_directories(path.parent_path(), ignored);

#if defined(_WIN32)
  const HANDLE handle = CreateFileW(
      path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr
  );

  if (handle == INVALID_HANDLE_VALUE) {
    error = "Cannot open the lock file " + path.string() + ".";
    return false;
  }

  OVERLAPPED overlapped{};

  if (!LockFileEx(
          handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
          &overlapped
      )) {
    // A conflict means a live instance owns it; anything else is a real failure
    // and should not be reported as "already open".
    error = GetLastError() == ERROR_LOCK_VIOLATION
        ? refusal_message(path)
        : "Cannot lock " + path.string() + ".";
    (void)CloseHandle(handle);
    return false;
  }

  handle_ = handle;
  write_contents(handle);
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);

  if (fd < 0) {
    error = "Cannot open the lock file " + path.string() + ": " +
        std::strerror(errno) + ".";
    return false;
  }

  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int code = errno;

    // EWOULDBLOCK/EAGAIN means another process holds it; any other error is a
    // genuine filesystem problem and should be reported as such.
    error = (code == EWOULDBLOCK || code == EAGAIN)
        ? refusal_message(path)
        : "Cannot lock " + path.string() + ": " + std::strerror(code) + ".";

    (void)::close(fd);
    return false;
  }

  fd_ = fd;
  write_contents(fd);
#endif

  path_ = path;
  return true;
}

void instance_lock::release() {
#if defined(_WIN32)
  if (handle_ != nullptr) {
    // Closing the handle releases the region lock.
    (void)CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (fd_ >= 0) {
    // Closing releases the flock; the file itself is intentionally left in place
    // so every process always opens the same inode.
    (void)::close(fd_);
    fd_ = -1;
  }
#endif

  path_.clear();
}

bool instance_lock::held() const {
#if defined(_WIN32)
  return handle_ != nullptr;
#else
  return fd_ >= 0;
#endif
}

} // namespace lnb::store
