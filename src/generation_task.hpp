
#pragma once

#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace lnb {

class GenerationTask {
public:
    enum class Status {
        PENDING,
        RUNNING,
        COMPLETED,
        CANCELLED,
        FAILED
    };

    struct Snapshot {
        Status status = Status::PENDING;
        // Assistant text only. This is what gets persisted once the turn
        // completes.
        std::string content;
        // The assistant stream shown while the turn is in flight.
        std::string display;
        std::string operation_status;
        std::string error;
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
        return true;
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

    // Updates the transient title shown while the worker is performing a
    // background operation. It is never persisted.
    bool set_status(std::string_view status) {
        std::lock_guard lock(mutex_);

        if (status_ != Status::RUNNING) {
            return false;
        }

        operation_status_.assign(status);
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
        }

        stop_source_.request_stop();
        return true;
    }

    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);

        return Snapshot{
            status_,
            content_,
            display_,
            operation_status_,
            error_
        };
    }

    bool is_terminal() const {
        std::lock_guard lock(mutex_);
        return is_terminal(status_);
    }

private:
    static bool is_terminal(Status status) noexcept {
        return status == Status::COMPLETED ||
               status == Status::CANCELLED ||
               status == Status::FAILED;
    }

    mutable std::mutex mutex_;

    std::stop_source stop_source_;

    Status status_ = Status::PENDING;
    std::string content_;
    std::string display_;
    std::string operation_status_;
    std::string error_;
};

}
