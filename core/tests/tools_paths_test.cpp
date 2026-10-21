// Regression tests for the filesystem tools' model-visible path view.
//
// The model must only ever see relative paths, whether one directory is
// configured or several. These tests build a small temporary tree and check the
// tool output never contains an actual configured path.
#include "net/tools.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int failures = 0;

void expect(bool condition, const std::string& label) {
  if (condition) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << '\n';
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

void write_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path);
  out << content;
}

std::string run(
    const std::string& name, const std::string& arguments,
    const std::vector<std::string>& roots
) {
  lnb::net::ToolCall call;
  call.name = name;
  call.arguments = arguments;
  return lnb::net::execute_local_tool(call, roots, std::nullopt);
}

std::string path_argument(const fs::path& path) {
  return "{\"path\":\"" + path.generic_string() + "\"}";
}

void test_single_directory(const fs::path& alpha, const fs::path& outside) {
  const std::vector<std::string> roots{alpha.string()};

  const std::string listing = run("list_directory", "{}", roots);
  expect(contains(listing, "notes.txt"), "single: lists files at the root");
  expect(contains(listing, "src/"), "single: marks directories with /");
  expect(!contains(listing, alpha.generic_string()), "single: no actual path");

  const std::string search = run(
      "search_code", "{\"query\":\"needle\"}", roots
  );
  expect(contains(search, "notes.txt"), "single: search finds root file");
  expect(contains(search, "src/code.cpp"), "single: search finds nested file");
  expect(!contains(search, alpha.generic_string()), "single: search hides root");

  const std::string read = run(
      "read_file", "{\"path\":\"notes.txt\",\"start_line\":1,\"end_line\":5}",
      roots
  );
  expect(contains(read, "1: hello needle"), "single: reads a relative path");

  const std::string absolute = run(
      "read_file",
      "{\"path\":\"" + (alpha / "notes.txt").generic_string() +
          "\",\"start_line\":1,\"end_line\":5}",
      roots
  );
  expect(contains(absolute, "Tool error"), "single: rejects absolute paths");
  expect(
      !contains(absolute, alpha.generic_string()),
      "single: rejection hides root"
  );

  const std::string escape = run(
      "list_directory", path_argument("../outside"), roots
  );
  expect(contains(escape, "Tool error"), "single: rejects parent traversal");
  expect(!contains(escape, outside.generic_string()), "single: traversal hides root");

  const std::string link = run(
      "read_file",
      "{\"path\":\"link/secret.txt\",\"start_line\":1,\"end_line\":5}", roots
  );
  expect(contains(link, "Tool error"), "single: rejects symlink escape");
  expect(!contains(link, outside.generic_string()), "single: symlink hides root");

  const std::string missing = run(
      "search_code", "{\"query\":\"absent-token\"}", roots
  );
  expect(contains(missing, "No matches"), "single: no-match message present");
  expect(!contains(missing, alpha.generic_string()), "single: no-match hides root");
}

void test_multiple_directories(
    const fs::path& alpha, const fs::path& beta, const fs::path& duplicate
) {
  const std::vector<std::string> roots{
      alpha.string(), beta.string(), duplicate.string()};

  const std::string listing = run("list_directory", "{}", roots);
  expect(contains(listing, "alpha/\n"), "multi: exposes first mount");
  expect(contains(listing, "beta-123/\n"), "multi: exposes second mount");
  expect(contains(listing, "alpha-2/\n"), "multi: disambiguates duplicate names");
  expect(!contains(listing, alpha.generic_string()), "multi: listing hides roots");

  const std::string read = run(
      "read_file",
      "{\"path\":\"beta-123/other.txt\",\"start_line\":1,\"end_line\":5}", roots
  );
  expect(contains(read, "1: needle elsewhere"), "multi: reads a named mount");

  const std::string search = run(
      "search_code", "{\"query\":\"needle\"}", roots
  );
  expect(contains(search, "alpha/notes.txt"), "multi: prefixes first root");
  expect(contains(search, "beta-123/other.txt"), "multi: prefixes second root");
  expect(!contains(search, alpha.generic_string()), "multi: search hides roots");

  const std::string scoped = run(
      "search_code", "{\"query\":\"needle\",\"path\":\"alpha/src\"}", roots
  );
  expect(contains(scoped, "alpha/src/code.cpp"), "multi: scopes to a mount");
  expect(!contains(scoped, "beta-123/"), "multi: path limits the search");

  const std::string files = run(
      "find_files", "{\"glob\":\"*.txt\"}", roots
  );
  expect(contains(files, "alpha/notes.txt"), "multi: find_files prefixes first");
  expect(
      contains(files, "beta-123/other.txt"), "multi: find_files prefixes second"
  );
  expect(!contains(files, alpha.generic_string()), "multi: find_files hides roots");

  const std::string unknown = run(
      "list_directory", "{\"path\":\"missing\"}", roots
  );
  expect(contains(unknown, "Unknown shared directory"), "multi: unknown mount named");
}

} // namespace

int main() {
  const fs::path base =
      fs::temp_directory_path() / "lnb-tools-paths-test";

  std::error_code error;
  fs::remove_all(base, error);

  const fs::path alpha = base / "alpha";
  const fs::path beta = base / "beta-123";
  const fs::path duplicate = base / "duplicate" / "alpha";
  const fs::path outside = base / "outside";

  fs::create_directories(alpha / "src");
  fs::create_directories(beta);
  fs::create_directories(duplicate);
  fs::create_directories(outside);

  write_file(alpha / "notes.txt", "hello needle\n");
  write_file(alpha / "src" / "code.cpp", "needle in code\n");
  write_file(beta / "other.txt", "needle elsewhere\n");
  write_file(duplicate / "also.txt", "needle here too\n");
  write_file(outside / "secret.txt", "needle secret\n");

  std::error_code link_error;
  fs::create_directory_symlink(
      outside, alpha / "link", link_error
  );

  test_single_directory(alpha, outside);
  test_multiple_directories(alpha, beta, duplicate);

  fs::remove_all(base, error);

  if (failures != 0) {
    std::cerr << failures << " tool path check(s) failed\n";
    return 1;
  }

  std::cout << "tool path checks passed\n";
  return 0;
}
