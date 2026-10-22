// Regression tests for the filesystem tools: the model-visible path view, the
// per-session permission gates, and the sandbox on the write tools.
//
// The model must only ever see relative paths, whether one directory is
// configured or several. These tests build a small temporary tree and check the
// tool output never contains an actual configured path, that writes stay inside
// it, and that a disabled permission is refused.
#include "net/tools.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
  // Binary mode keeps the bytes identical on Windows, where text mode would
  // translate \n to \r\n and break the exact-content assertions below.
  std::ofstream out(path, std::ios::binary);
  out << content;
}

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(
      std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
  );
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

std::string run_with(
    const std::string& name, const std::string& arguments,
    const std::vector<std::string>& roots,
    const lnb::net::ToolPermissions& permissions
) {
  lnb::net::ToolCall call;
  call.name = name;
  call.arguments = arguments;
  return lnb::net::execute_local_tool(call, roots, std::nullopt, permissions);
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

void test_permissions(const fs::path& alpha) {
  const std::vector<std::string> roots{alpha.string()};

  lnb::net::ToolPermissions defaults;
  defaults.filesystem = true;

  // The default posture reads and browses online, but never writes.
  const std::string denied = run_with(
      "write_file", "{\"path\":\"denied.txt\",\"content\":\"x\"}", roots,
      defaults
  );
  expect(
      contains(denied, "file writing is disabled"),
      "permissions: writing is off by default"
  );
  expect(
      !fs::exists(alpha / "denied.txt"),
      "permissions: a denied write creates nothing"
  );

  const std::string delete_denied = run_with(
      "delete_file", "{\"path\":\"notes.txt\"}", roots, defaults
  );
  expect(
      contains(delete_denied, "file writing is disabled"),
      "permissions: deletion is off by default"
  );
  expect(
      fs::exists(alpha / "notes.txt"),
      "permissions: a denied delete removes nothing"
  );

  const std::string read = run_with(
      "read_file", "{\"path\":\"notes.txt\",\"start_line\":1,\"end_line\":5}",
      roots, defaults
  );
  expect(
      contains(read, "hello needle"), "permissions: reading is on by default"
  );

  lnb::net::ToolPermissions no_read = defaults;
  no_read.read = false;
  const std::string read_denied =
      run_with("list_directory", "{}", roots, no_read);
  expect(
      contains(read_denied, "file reading is disabled"),
      "permissions: reading can be disabled"
  );

  lnb::net::ToolPermissions no_online = defaults;
  no_online.online = false;
  const std::string online_denied = run_with(
      "fetch_url", "{\"url\":\"https://example.com\"}", roots, no_online
  );
  expect(
      contains(online_denied, "online access is disabled"),
      "permissions: online access can be disabled"
  );

  lnb::net::ToolPermissions off = defaults;
  off.enabled = false;
  const std::string master_denied =
      run_with("list_directory", "{}", roots, off);
  expect(
      contains(master_denied, "tools are disabled"),
      "permissions: the master switch disables everything"
  );
}

void test_write_tools(const fs::path& alpha, const fs::path& outside) {
  const std::vector<std::string> roots{alpha.string()};

  lnb::net::ToolPermissions permissions;
  permissions.filesystem = true;
  permissions.write = true;

  const std::string created = run_with(
      "write_file", "{\"path\":\"created.txt\",\"content\":\"fresh content\\n\"}",
      roots, permissions
  );
  expect(contains(created, "Wrote"), "write: creation is reported");
  expect(
      read_text(alpha / "created.txt") == "fresh content\n",
      "write: content is written verbatim"
  );

  run_with(
      "write_file", "{\"path\":\"created.txt\",\"content\":\"replaced\"}",
      roots, permissions
  );
  expect(
      read_text(alpha / "created.txt") == "replaced",
      "write: an existing file is overwritten"
  );

  const std::string nested = run_with(
      "write_file", "{\"path\":\"src/nested.cpp\",\"content\":\"nested\\n\"}",
      roots, permissions
  );
  expect(!contains(nested, "Tool error"), "write: writes into a subdirectory");
  expect(
      read_text(alpha / "src" / "nested.cpp") == "nested\n",
      "write: nested content is written"
  );

  const std::string escape = run_with(
      "write_file", "{\"path\":\"../outside/new.txt\",\"content\":\"x\"}",
      roots, permissions
  );
  expect(contains(escape, "Tool error"), "write: rejects parent traversal");
  expect(
      !fs::exists(outside / "new.txt"), "write: traversal creates nothing"
  );

  const std::string link = run_with(
      "write_file", "{\"path\":\"link/new.txt\",\"content\":\"x\"}", roots,
      permissions
  );
  expect(contains(link, "Tool error"), "write: rejects a symlink escape");
  expect(
      !fs::exists(outside / "new.txt"), "write: symlink escape creates nothing"
  );

  const std::string missing_parent = run_with(
      "write_file", "{\"path\":\"missing/deep.txt\",\"content\":\"x\"}", roots,
      permissions
  );
  expect(
      contains(missing_parent, "Parent directory does not exist"),
      "write: requires an existing parent"
  );

  const std::string directory = run_with(
      "write_file", "{\"path\":\"src\",\"content\":\"x\"}", roots, permissions
  );
  expect(
      contains(directory, "Path is a directory"),
      "write: refuses to overwrite a directory"
  );

  // Editing a unique occurrence.
  write_file(alpha / "editme.txt", "alpha beta\n");
  const std::string edited = run_with(
      "edit_file",
      "{\"path\":\"editme.txt\",\"old_text\":\"alpha\",\"new_text\":\"gamma\"}",
      roots, permissions
  );
  expect(
      contains(edited, "Replaced one occurrence at line 1"),
      "edit: replaces a unique occurrence"
  );
  expect(
      read_text(alpha / "editme.txt") == "gamma beta\n",
      "edit: the file is updated on disk"
  );

  write_file(alpha / "dupe.txt", "same same\n");
  const std::string ambiguous = run_with(
      "edit_file",
      "{\"path\":\"dupe.txt\",\"old_text\":\"same\",\"new_text\":\"other\"}",
      roots, permissions
  );
  expect(
      contains(ambiguous, "occurs more than once"),
      "edit: rejects an ambiguous match"
  );
  expect(
      read_text(alpha / "dupe.txt") == "same same\n",
      "edit: an ambiguous match leaves the file unchanged"
  );

  const std::string absent = run_with(
      "edit_file",
      "{\"path\":\"dupe.txt\",\"old_text\":\"absent-token\",\"new_text\":\"x\"}",
      roots, permissions
  );
  expect(
      contains(absent, "was not found"), "edit: rejects an absent match"
  );
}

void test_delete_tool(const fs::path& alpha, const fs::path& outside) {
  const std::vector<std::string> roots{alpha.string()};

  lnb::net::ToolPermissions permissions;
  permissions.filesystem = true;
  permissions.write = true;

  write_file(alpha / "delete-me.txt", "temporary\n");

  const std::string removed = run_with(
      "delete_file", "{\"path\":\"delete-me.txt\"}", roots, permissions
  );
  expect(contains(removed, "Deleted"), "delete: reports the removal");
  expect(!fs::exists(alpha / "delete-me.txt"), "delete: removes the file");

  const std::string missing = run_with(
      "delete_file", "{\"path\":\"delete-me.txt\"}", roots, permissions
  );
  expect(
      contains(missing, "Path does not exist"), "delete: missing file is reported"
  );

  const std::string directory = run_with(
      "delete_file", "{\"path\":\"src\"}", roots, permissions
  );
  expect(
      contains(directory, "not a regular file"), "delete: refuses a directory"
  );
  expect(fs::is_directory(alpha / "src"), "delete: the directory remains");

  const std::string escape = run_with(
      "delete_file", "{\"path\":\"../outside/secret.txt\"}", roots, permissions
  );
  expect(contains(escape, "Tool error"), "delete: rejects parent traversal");
  expect(
      fs::exists(outside / "secret.txt"), "delete: traversal removes nothing"
  );

  const std::string link = run_with(
      "delete_file", "{\"path\":\"link/secret.txt\"}", roots, permissions
  );
  expect(contains(link, "Tool error"), "delete: rejects a symlink escape");
  expect(
      fs::exists(outside / "secret.txt"),
      "delete: symlink escape removes nothing"
  );
}

void test_tool_definitions() {
  const auto names = [](const lnb::net::ToolPermissions& permissions) {
    std::vector<std::string> out;

    for (const boost::json::value& value:
         lnb::net::tool_definitions(permissions)) {
      out.emplace_back(
          value.as_object().at("function").as_object().at("name").as_string()
      );
    }

    return out;
  };

  const auto has = [](const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
  };

  // Without a shared directory, only the network tools are offered even when
  // the read and write switches are on.
  lnb::net::ToolPermissions no_filesystem;
  no_filesystem.filesystem = false;
  no_filesystem.read = true;
  no_filesystem.write = true;

  const std::vector<std::string> network = names(no_filesystem);
  expect(
      !has(network, "list_directory") && !has(network, "write_file"),
      "definitions: filesystem tools need a shared directory"
  );
  expect(
      has(network, "fetch_url") && has(network, "web_search"),
      "definitions: online tools are offered"
  );

  lnb::net::ToolPermissions read_only;
  read_only.filesystem = true;
  read_only.read = true;
  read_only.write = false;
  read_only.online = false;

  const std::vector<std::string> read_tools = names(read_only);
  expect(
      has(read_tools, "read_file") && has(read_tools, "search_code") &&
          has(read_tools, "list_directory"),
      "definitions: reading offers the read tools"
  );
  expect(
      !has(read_tools, "write_file") && !has(read_tools, "edit_file") &&
          !has(read_tools, "delete_file"),
      "definitions: reading alone does not offer write tools"
  );
  expect(
      !has(read_tools, "fetch_url"), "definitions: offline omits the network tools"
  );

  lnb::net::ToolPermissions write_only = read_only;
  write_only.read = false;
  write_only.write = true;

  const std::vector<std::string> write_tools = names(write_only);
  expect(
      !has(write_tools, "read_file"),
      "definitions: writing alone omits the read tools"
  );
  expect(
      has(write_tools, "write_file") && has(write_tools, "edit_file") &&
          has(write_tools, "delete_file"),
      "definitions: writing offers the write and delete tools"
  );

  lnb::net::ToolPermissions disabled;
  disabled.enabled = false;
  expect(
      names(disabled).empty(), "definitions: the master switch offers nothing"
  );
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
  test_permissions(alpha);
  test_write_tools(alpha, outside);
  test_delete_tool(alpha, outside);
  test_tool_definitions();

  fs::remove_all(base, error);

  if (failures != 0) {
    std::cerr << failures << " tool path check(s) failed\n";
    return 1;
  }

  std::cout << "tool path checks passed\n";
  return 0;
}
