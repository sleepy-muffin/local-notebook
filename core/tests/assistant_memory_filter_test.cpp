// Regression test for the per-session opt-out from the shared assistant memory.
//
// The flag is stored in the session row and must keep the session out of both
// paths the assistant trigger uses: the cross-session watermark query and the
// direct scan in assistant_trigger_reached().
#include "local-notebook-core/interface.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
  if (condition) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << '\n';
}

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "lnb-assistant-memory-filter-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  lnb::api::set_data_root(root);

  lnb::api::interface api;

  lnb::Assistant assistant;
  assistant.name_ = "test";
  assistant.api_url_ = "http://127.0.0.1:9/v1";
  assistant.authentication_token_ = "key";
  assistant.model_ = "test-model";
  // Any new token is enough to reach the shared trigger; this test checks which
  // sessions count, not the threshold arithmetic.
  assistant.summary_trigger_tokens_ = 1;
  expect(api.open_assistant("filter-test", assistant), "open_assistant failed");

  // A fresh session includes itself by default.
  lnb::Session defaults;
  expect(defaults.include_in_assistant_memory_, "a new session must be included");

  lnb::Session session;
  session.title_ = "Opted out";
  session.summary_trigger_tokens_ = 2'000'000'000;  // keep the session trigger clear
  session.include_in_assistant_memory_ = false;

  const std::uint32_t session_id = api.create_session(session);
  expect(session_id != 0, "create_session failed");

  const auto stored = api.session(session_id);
  expect(stored.has_value(), "the session should be readable");
  expect(
      stored.has_value() && !stored->include_in_assistant_memory_,
      "the opt-out flag must round-trip through the database"
  );

  api.append_message(session_id, lnb::Role::User, "a message that counts");

  // Only the opted-out session has new content, so neither assistant path may
  // see it: the session itself stays below its own trigger.
  expect(
      api.has_session_summary_trigger_met(session_id) ==
          lnb::api::error_code::SUCCESS,
      "an opted-out session must not reach the assistant trigger"
  );
  expect(
      !api.assistant_summary_due(),
      "an opted-out session must not make the assistant summary due"
  );

  // Opting the same session back in makes the same transcript count.
  lnb::Session opted_in = *stored;
  opted_in.include_in_assistant_memory_ = true;
  api.save_session(opted_in);

  expect(
      api.has_session_summary_trigger_met(session_id) ==
          lnb::api::error_code::ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET,
      "an opted-in session must reach the assistant trigger"
  );
  expect(
      api.assistant_summary_due(),
      "an opted-in session must make the assistant summary due"
  );

  api.wait_for_generation();

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "assistant memory filter: all expectations passed\n";
  return 0;
}
