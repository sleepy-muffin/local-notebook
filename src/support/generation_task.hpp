#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace lnb {

// The shared state of one background generation, updated by the worker thread
// and polled by the UI thread. It is the only channel between the two, so every
// accessor is guarded by a mutex.
class GenerationTask {
public:
  enum class Status {
    PENDING,
    RUNNING,
    COMPLETED,
    CANCELLED,
    FAILED,
  };

  struct Snapshot {
    Status status = Status::PENDING;
    // Assistant text only. This is what gets persisted once the turn completes.
    std::string content;
    // The assistant stream shown while the turn is in flight.
    std::string display;
    std::string operation_status;
    std::string error;
    // A non-fatal notice to show after a completed turn (for example, the reply
    // was cut off). Never persisted.
    std::string warning;
    // Progress of a running turn: the tool-call round reached and how long it has
    // been running. Both are zero for a task that has not started.
    std::size_t rounds = 0;
    std::int64_t elapsed_ms = 0;
  };

  GenerationTask() = default;

  GenerationTask(const GenerationTask&) = delete;
  GenerationTask& operator=(const GenerationTask&) = delete;

  std::stop_token stop_token() const noexcept {
    return stop_source_.get_token();
  }

  bool start() {
    std::lock_guard lock(mutex_);

    if (status_ != Status::PENDING) {
      return false;
    }

    status_ = Status::RUNNING;
    started_at_ = std::chrono::steady_clock::now();
    rounds_ = 0;
    return true;
  }

  // Returns the task to its initial PENDING state so it can be reused for the
  // next turn. A previously requested stop is cleared with the stop source.
  void reset() {
    std::lock_guard lock(mutex_);

    status_ = Status::PENDING;
    content_.clear();
    display_.clear();
    operation_status_.clear();
    error_.clear();
    warning_.clear();
    rounds_ = 0;
    stop_source_ = std::stop_source{};
  }

  bool append_content(std::string_view content) {
    std::lock_guard lock(mutex_);

    if (status_ != Status::RUNNING) {
      return false;
    }

    content_.append(content);
    display_.append(content);
    operation_status_.clear();
    return true;
  }

  // Updates the transient title shown while the worker performs a background
  // operation. It is never persisted.
  bool set_status(std::string_view status) {
    std::lock_guard lock(mutex_);

    if (status_ != Status::RUNNING) {
      return false;
    }

    operation_status_.assign(status);
    return true;
  }

  // Records the tool-call round the worker has reached, for the running-task
  // readout. Ignored unless the task is running.
  bool set_rounds(std::size_t rounds) {
    std::lock_guard lock(mutex_);

    if (status_ != Status::RUNNING) {
      return false;
    }

    rounds_ = rounds;
    return true;
  }

  bool complete() {
    std::lock_guard lock(mutex_);

    if (status_ != Status::RUNNING) {
      return false;
    }

    status_ = Status::COMPLETED;
    return true;
  }

  // Records a non-fatal notice about a turn that still completes successfully
  // (e.g. the reply was cut off). The UI shows it after the reply, like a
  // generation error, but the reply itself is kept.
  bool set_warning(std::string warning) {
    std::lock_guard lock(mutex_);

    if (status_ != Status::RUNNING) {
      return false;
    }

    warning_ = std::move(warning);
    return true;
  }

  bool cancel() {
    {
      std::lock_guard lock(mutex_);

      if (is_terminal(status_)) {
        return false;
      }

      status_ = Status::CANCELLED;
      content_.clear();
      display_.clear();
      error_.clear();
      warning_.clear();
    }

    stop_source_.request_stop();
    return true;
  }

  bool fail(std::string error) {
    {
      std::lock_guard lock(mutex_);

      if (is_terminal(status_)) {
        return false;
      }

      status_ = Status::FAILED;
      content_.clear();
      display_.clear();
      error_ = std::move(error);
      warning_.clear();
    }

    stop_source_.request_stop();
    return true;
  }

  Snapshot snapshot() const {
    std::lock_guard lock(mutex_);

    // Elapsed time is only meaningful while the turn is in flight; a finished or
    // not-yet-started task reports zero.
    std::int64_t elapsed_ms = 0;

    if (status_ == Status::RUNNING) {
      elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - started_at_
                   )
                       .count();
    }

    return Snapshot{
      status_,
      content_,
      display_,
      operation_status_,
      error_,
      warning_,
      rounds_,
      elapsed_ms,
    };
  }

  bool is_terminal() const {
    std::lock_guard lock(mutex_);
    return is_terminal(status_);
  }

private:
  static bool is_terminal(Status status) noexcept {
    return status == Status::COMPLETED || status == Status::CANCELLED ||
           status == Status::FAILED;
  }

  mutable std::mutex mutex_;

  std::stop_source stop_source_;

  Status status_ = Status::PENDING;
  std::string content_;
  std::string display_;
  std::string operation_status_;
  std::string error_;
  std::string warning_;

  // Progress bookkeeping for the running-task readout.
  std::chrono::steady_clock::time_point started_at_ = std::chrono::steady_clock::now();
  std::size_t rounds_ = 0;
};

} // namespace lnb
