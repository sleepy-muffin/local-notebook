// Regression test for the one-instance guard on an assistant directory.
//
// Two processes may not have the same assistant open at once. The guard is an
// operating-system lock (flock/LockFileEx), not a pid check, so a lock file left
// behind by a crashed instance must never block the next one.
#include "local_notebook/core/interface.hpp"

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

lnb::Assistant make_assistant() {
  lnb::Assistant assistant;
  assistant.name_ = "test";
  assistant.api_url_ = "http://127.0.0.1:9/v1";
  assistant.authentication_token_ = "key";
  assistant.model_ = "test-model";
  return assistant;
}

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "lnb-instance-lock-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  lnb::api::set_data_root(root);

  const std::string uuid = "shared-assistant";

  // A lock file left behind by an older process: its pid is bogus and nothing
  // holds the operating-system lock. Opening the assistant must still succeed.
  {
    const std::filesystem::path directory = root / uuid;
    std::filesystem::create_directories(directory, ignored);

    std::ofstream out(directory / "lock", std::ios::binary | std::ios::trunc);
    out << "pid 999999999\nstarted 1970-01-01T00:00:00Z\n";
  }

  {
    lnb::api::interface first;
    expect(
        first.open_assistant(uuid, make_assistant()),
        "a stale lock file must not block the first open"
    );

    lnb::api::interface second;
    expect(
        !second.open_assistant(uuid, make_assistant()),
        "a second open while the first is live must be refused"
    );
    expect(!second.last_error().empty(), "the refusal should carry a reason");
    expect(
        !second.has_assistant(),
        "a refused open must not attach the assistant"
    );
  }

  // The first interface is gone, so the operating system released its lock; the
  // same directory opens again with no cleanup step of our own.
  {
    lnb::api::interface third;
    expect(
        third.open_assistant(uuid, make_assistant()),
        "the assistant must open again once the previous instance exits"
    );
  }

  // Reopening the assistant already open in one interface must not fight its own
  // lock.
  {
    lnb::api::interface reused;
    expect(reused.open_assistant(uuid, make_assistant()), "first open");
    expect(
        reused.open_assistant(uuid, make_assistant()),
        "reopening the same assistant in the same interface"
    );
  }

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "instance lock: all expectations passed\n";
  return 0;
}
