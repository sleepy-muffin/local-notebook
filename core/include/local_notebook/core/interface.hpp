#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "local_notebook/core/generation_task.hpp"
#include "local_notebook/core/structs.hpp"
#include "local_notebook/core/model.hpp"
#include "local_notebook/core/utils.hpp"

namespace lnb::core {
class application;


class interface {
public:
  interface() = delete;
  explicit interface(std::filesystem::path store_root);
  ~interface();

  interface(const interface &) = delete;
  interface &operator=(const interface &) = delete;

  [[nodiscard]] std::expected<void, error_code> open_assistant(std::string uuid);
  [[nodiscard]] std::vector<std::pair<std::string, Assistant>> list_assistants();
  [[nodiscard]] bool has_assistant() const;
  [[nodiscard]] std::expected<std::string, error_code> assistant_uuid() const;
  [[nodiscard]] std::expected<Assistant, error_code> assistant() const;
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
  [[nodiscard]] ContextUsage context_usage(std::uint32_t session_id) const;
  [[nodiscard]] const store &get_store() const;

  std::uint32_t create_session(const Session &session);
  void save_session(const Session &session);
  void delete_session(std::uint32_t session_id);

  // Copies the session up to and including `message_id` into a new session with
  // the same settings, and returns the new session's id (0 on failure).
  std::uint32_t branch_session(std::uint32_t session_id, std::uint32_t message_id);
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
  [[nodiscard]] std::expected<void, error_code> has_session_summary_trigger_met(std::uint32_t session_id) const;
  [[nodiscard]] std::expected<void, error_code> session_prompt(std::uint32_t session_id, const std::string &prompt);
  [[nodiscard]] std::expected<std::shared_ptr<GenerationTask>, error_code>
  start_generation(std::uint32_t session_id, SummaryApprover approver = {});
  [[nodiscard]] std::expected<std::shared_ptr<GenerationTask>, error_code>
  summarize_session(std::uint32_t session_id, bool force, SummaryApprover approver = {});
  [[nodiscard]] std::expected<std::shared_ptr<GenerationTask>, error_code>
  summarize_assistant(bool force, SummaryApprover approver = {});
  void cancel_session(std::uint32_t session_id);
  void cancel_assistant();
  void wait_for_generation();

private:
  std::unique_ptr<lnb::core::application> application_;
};

[[nodiscard]] std::string ensure_scheme(const std::string &url);
[[nodiscard]] std::string normalize_base_url(const std::string &url);
[[nodiscard]] bool is_valid_base_url(const std::string &url);
[[nodiscard]] std::string reasoning_default_for(const std::string &model);
[[nodiscard]] std::vector<std::string>
reasoning_levels_for(const std::string &model, const std::string &stored);

} // namespace lnb::core
