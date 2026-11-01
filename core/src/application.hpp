#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <shark/async.hpp>
#include <shark/sync.hpp>
#include <shark/legacy.hpp>

#include "local_notebook/core/interface.hpp"
#include "local_notebook/core/utils.hpp"
#include "store/managers.hpp"

namespace lnb::core {

class application {
public:
  using executor = shark::async::multi_executor<void()>;

  struct assistant_state {

    assistant_state(
        std::string uuid, Assistant assistant, const std::string &db_path,
        std::unique_ptr<shark::legacy::instance_lock> filesystem_lock);

    std::string uuid_;
    Assistant assistant_;
    lnb::core::SessionManager sessions_;
    std::unique_ptr<shark::legacy::instance_lock> filesystem_lock_;
    mutable std::optional<bool> assistant_due_cache_;
  };

  struct task_slot {
    std::shared_ptr<GenerationTask> task;
    executor::handle worker;

    [[nodiscard]] bool busy() const;
  };

  explicit application(std::filesystem::path store_root);
  ~application();

  application(const application &) = delete;
  application &operator=(const application &) = delete;

  [[nodiscard]] bool open_assistant(const std::string& uuid);
  [[nodiscard]] std::string last_error() const;
  [[nodiscard]] std::vector<std::pair<std::string, Assistant>> list_assistants();

  [[nodiscard]] bool has_assistant() const;
  [[nodiscard]] std::string assistant_uuid() const;
  [[nodiscard]] Assistant assistant() const;
  void set_assistant(Assistant assistant);
  void set_assistant_memory(std::string memory);
  void save_assistant();

  [[nodiscard]] std::optional<Session> session(std::uint32_t session_id) const;
  [[nodiscard]] std::vector<Message> messages(std::uint32_t session_id) const;
  [[nodiscard]] std::vector<Message>
  recent_messages(std::uint32_t session_id, std::size_t count) const;
  [[nodiscard]] std::vector<Session> recent_sessions(std::size_t count) const;
  [[nodiscard]] std::unordered_map<std::uint32_t, std::string> session_last_times() const;
  [[nodiscard]] std::optional<std::string> last_message_time(std::uint32_t session_id) const;
  [[nodiscard]] core::ContextUsage context_usage(std::uint32_t session_id) const;
  std::uint32_t create_session(const Session &session);
  void save_session(const Session &session);
  void delete_session(std::uint32_t session_id);
  void set_session_reasoning_effort(std::uint32_t session_id, std::string level);
  void append_message(std::uint32_t session_id, Role role, std::string content);
  void delete_message(std::uint32_t message_id);
  void edit_message(std::uint32_t message_id, std::string content);
  [[nodiscard]] std::size_t message_tokens(const Message &message) const;
  [[nodiscard]] std::string default_reasoning_effort() const;
  [[nodiscard]] std::vector<std::string>
  reasoning_efforts_with_stored(const std::string &stored) const;

  [[nodiscard]] bool busy() const;
  [[nodiscard]] bool session_busy(std::uint32_t session_id) const;
  [[nodiscard]] bool assistant_busy() const;
  [[nodiscard]] std::shared_ptr<GenerationTask> assistant_task() const;
  [[nodiscard]] bool assistant_summary_due() const;
  void mark_assistant_summary_stale();
  [[nodiscard]] core::error_code has_session_summary_trigger_met(std::uint32_t session_id) const;
  [[nodiscard]] core::error_code session_prompt(std::uint32_t session_id, const std::string &prompt);
  [[nodiscard]] std::shared_ptr<GenerationTask>
  start_generation(std::uint32_t session_id, SummaryApprover approver = {});
  [[nodiscard]] std::shared_ptr<GenerationTask>
  summarize_session(std::uint32_t session_id, bool force, SummaryApprover approver = {});
  [[nodiscard]] std::shared_ptr<GenerationTask>
  summarize_assistant(bool force, SummaryApprover approver = {});
  void cancel_session(std::uint32_t session_id);
  void cancel_assistant();
  void wait_for_generation();

  /*
   * Utils
   */
  [[nodiscard]] const store& get_store() const;

private:
  template<typename Work>
  std::shared_ptr<GenerationTask> spawn_task(task_slot &slot, Work &&work);
  void open_log(const std::string &uuid);
  void stop_worker();
  void prune_finished_sessions();

  // The session row and the database one generation worker works from. It is
  // taken under the state lock and then used without it, so a worker never holds
  // that lock across a network round-trip (see `snapshot_session`).
  struct session_snapshot {
    Session session_;
    database *db_ = nullptr;
  };

  [[nodiscard]] std::optional<session_snapshot> snapshot_session(std::uint32_t session_id);

  std::unique_ptr<executor> multi_executor_;
  std::unique_ptr<shark::synchronized<assistant_state>> assistant_state_;
  std::string last_error_;
  mutable std::mutex tasks_mutex_;
  std::unordered_map<std::uint32_t, task_slot> session_tasks_;
  task_slot assistant_slot_;
  store store_;
};

} // namespace lnb::core
