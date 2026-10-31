// Regression test for the assistant connection settings.
//
// The turn and summarisation timeouts are stored per assistant in
// data/<uuid>/settings.json. They must survive a save/load round-trip, and a
// file written before the fields existed must still load with the shipped
// defaults rather than a zero window.
#include "app/assistant_io.hpp"
#include "utils/util.hpp"

#include "local_notebook/core/interface.hpp"
#include "local_notebook/core/model.hpp"

#include <filesystem>
#include <fstream>
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

void expect_equal(
    std::size_t actual, std::size_t expected, const std::string& label
) {
  if (actual == expected) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << " (got " << actual << ", expected "
            << expected << ")\n";
}

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "lnb-assistant-settings-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  lnb::api::set_data_root(root);

  lnb::Assistant assistant;
  assistant.name_ = "settings test";
  assistant.turn_timeout_seconds_ = 240;
  assistant.summary_timeout_seconds_ = 600;
  assistant.summary_reasoning_effort_ = "high";

  lnb::save_assistant("test-uuid", assistant);

  const lnb::Assistant loaded = lnb::load_assistant("test-uuid");
  expect_equal(loaded.turn_timeout_seconds_, 240, "turn timeout round-trips");
  expect_equal(
      loaded.summary_timeout_seconds_, 600, "summary timeout round-trips"
  );
  expect(
      loaded.summary_reasoning_effort_ == "high",
      "summary reasoning level round-trips"
  );

  // A settings file from before these fields existed: both must read as the
  // shipped defaults (three minutes for a turn, seven for summarisation).
  {
    const std::filesystem::path directory =
        lnb::store::paths::assistant_dir("legacy-uuid");
    std::filesystem::create_directories(directory, ignored);

    std::ofstream out(
        lnb::store::paths::assistant_settings("legacy-uuid"),
        std::ios::binary | std::ios::trunc
    );
    out << R"({"name":"legacy","api_url":"https://example.com/v1"})"
        << '\n';
  }

  const lnb::Assistant legacy = lnb::load_assistant("legacy-uuid");
  expect_equal(legacy.turn_timeout_seconds_, 180, "legacy turn default");
  expect_equal(legacy.summary_timeout_seconds_, 420, "legacy summary default");
  // A file without the key reads as the shipped default, not the empty "model
  // default" the other reasoning key uses.
  expect(
      legacy.summary_reasoning_effort_ == "none",
      "legacy summary reasoning default"
  );

  std::filesystem::remove_all(root, ignored);

  if (failures != 0) {
    std::cerr << failures << " assistant settings check(s) failed\n";
    return 1;
  }

  std::cout << "assistant settings checks passed\n";
  return 0;
}
