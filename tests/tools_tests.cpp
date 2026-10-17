// Headless tests for the ripgrep-style search tools (net/search.*) and their
// wiring through the tool layer (net/tools.*). Dependency-free: a tiny CHECK
// macro and a main() that returns non-zero on the first failure.
//
// Run via CTest (`ctest --test-dir build/linux-x64-debug`) or directly.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <QJsonArray>
#include <QJsonValue>
#include <QString>

#include "net/search.hpp"
#include "net/tools.hpp"

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

void write_file(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}

// A fixed scratch tree, rebuilt from scratch for every run.
fs::path make_tree() {
  const fs::path root = fs::temp_directory_path() / "lnb-tools-tests";
  std::error_code error;
  fs::remove_all(root, error);
  fs::create_directories(root);
  return root;
}

bool has_relative(const std::vector<lnb::net::FoundFile>& files, const std::string& rel) {
  for (const lnb::net::FoundFile& file: files) {
    if (file.relative == rel) return true;
  }
  return false;
}

// A tree exercising every traversal rule at once.
fs::path make_fixture() {
  const fs::path root = make_tree();

  write_file(root / ".gitignore", "/build/\n*.log\n!keep.log\n");
  write_file(root / "src" / ".gitignore", "generated.hpp\n");
  write_file(root / "src" / "main.cpp", "int main() {\n  int foo = 1;\n  return foo;\n}\n");
  write_file(root / "src" / "util.hpp", "#pragma once\nint util();\n");
  write_file(root / "src" / "generated.hpp", "// generated\n");
  write_file(root / "notes.md", "# Notes\n");
  write_file(root / "build" / "output.txt", "compiled artifact\n");
  write_file(root / ".hidden" / "secret.txt", "hidden\n");
  write_file(root / "debug.log", "noise\n");
  write_file(root / "keep.log", "kept\n");
  write_file(root / ".env", "SECRET=1\n");

  return root;
}

void test_collect_files_honours_gitignore() {
  const fs::path root = make_fixture();

  const lnb::net::WalkResult all =
      lnb::net::collect_files(root.string(), ".", "", 2000);

  CHECK(has_relative(all.files, "notes.md"));
  CHECK(has_relative(all.files, "src/main.cpp"));
  CHECK(has_relative(all.files, "src/util.hpp"));
  CHECK(has_relative(all.files, "keep.log")); // "*.log" then "!keep.log"

  CHECK(!has_relative(all.files, "debug.log")); // "*.log"
  CHECK(!has_relative(all.files, "build/output.txt")); // "/build/"
  CHECK(!has_relative(all.files, "src/generated.hpp")); // nested "generated.hpp"
  CHECK(!has_relative(all.files, ".hidden/secret.txt")); // hidden directory
  CHECK(!has_relative(all.files, ".env")); // hidden file

  // A glob restricts the walk to matching paths.
  const lnb::net::WalkResult cpp =
      lnb::net::collect_files(root.string(), ".", "*.cpp", 2000);
  CHECK(cpp.files.size() == 1);
  CHECK(has_relative(cpp.files, "src/main.cpp"));

  const lnb::net::WalkResult python =
      lnb::net::collect_files(root.string(), ".", "**/*.py", 2000);
  CHECK(python.files.empty());

  // A subdirectory search still honours the parent .gitignore.
  const lnb::net::WalkResult from_src =
      lnb::net::collect_files(root.string(), "src", "", 2000);
  CHECK(has_relative(from_src.files, "src/main.cpp"));
  CHECK(!has_relative(from_src.files, "src/generated.hpp"));
}

// Regression: the walk must not stop after an arbitrary number of files, or a
// match beyond that count would be reported as "no matches" (the bug this
// replaced). The result keeps everything here, so the assertion is exact.
void test_collect_files_covers_whole_tree() {
  const fs::path root = make_tree();

  // More files than the old 2000-file cap, spread across a subdirectory and the
  // root, so a truncated walk could not see them all.
  const int count = 2200;
  for (int i = 0; i < count; ++i) {
    write_file(root / "many" / ("f" + std::to_string(i) + ".txt"), "x\n");
  }
  write_file(root / "last.txt", "the unique token ZZZNEEDLEZZZ here\n");

  const lnb::net::WalkResult all =
      lnb::net::collect_files(root.string(), ".", "", 1000000);

  CHECK(!all.truncated);
  CHECK(all.scanned == static_cast<std::size_t>(count) + 1);
  CHECK(all.files.size() == static_cast<std::size_t>(count) + 1);
  CHECK(has_relative(all.files, "many/f2199.txt"));
  CHECK(has_relative(all.files, "last.txt"));

  // The same coverage through the search entry point: the only match is in a
  // file that a capped walk would have skipped.
  const std::string found =
      lnb::net::search_regex("ZZZNEEDLEZZZ", root.string(), ".", "");
  CHECK(contains(found, "--- last.txt:1 ---"));
  CHECK(contains(found, "ZZZNEEDLEZZZ"));
}

void test_search_regex_word_boundaries() {
  const fs::path root = make_fixture();

  const std::string output =
      lnb::net::search_regex("\\bfoo\\b", root.string(), ".", "");

  CHECK(contains(output, "--- src/main.cpp:2 ---"));
  CHECK(contains(output, "int foo = 1;"));

  // "foo" inside "main" must not match, so line 1 is never a match header.
  CHECK(!contains(output, "--- src/main.cpp:1 ---"));
}

void test_search_regex_is_case_sensitive_by_default() {
  const fs::path root = make_fixture();

  CHECK(!contains(
      lnb::net::search_regex("INT", root.string(), ".", ""), "main.cpp"
  ));
  CHECK(contains(
      lnb::net::search_regex("(?i)INT", root.string(), ".", ""), "main.cpp"
  ));
}

void test_search_regex_respects_ignore_and_glob() {
  const fs::path root = make_fixture();

  // "compiled" lives only in the ignored build/ directory, so it is invisible.
  const std::string ignored =
      lnb::net::search_regex("compiled", root.string(), ".", "");
  CHECK(contains(ignored, "No matches"));
  CHECK(contains(ignored, "text files scanned"));

  // The glob keeps the search out of the header.
  const std::string only_cpp =
      lnb::net::search_regex("main", root.string(), ".", "*.cpp");
  CHECK(contains(only_cpp, "--- src/main.cpp:1 ---"));
  CHECK(!contains(only_cpp, "util.hpp"));
}

void test_search_regex_invalid_pattern_throws() {
  const fs::path root = make_fixture();

  bool threw = false;
  try {
    lnb::net::search_regex("(", root.string(), ".", "");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

void test_find_files() {
  const fs::path root = make_fixture();

  const std::string headers = lnb::net::find_files("*.hpp", root.string(), ".");
  CHECK(contains(headers, "src/util.hpp"));
  CHECK(!contains(headers, "generated.hpp")); // ignored by src/.gitignore

  const std::string nested = lnb::net::find_files("**/*.cpp", root.string(), ".");
  CHECK(contains(nested, "src/main.cpp"));

  const std::string markdown = lnb::net::find_files("*.md", root.string(), ".");
  CHECK(contains(markdown, "notes.md"));

  bool threw = false;
  try {
    lnb::net::find_files("", root.string(), ".");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

void test_tool_definitions_include_new_tools() {
  const QJsonArray with_fs = lnb::net::tool_definitions(true);

  auto has_tool = [](const QJsonArray& tools, const QString& name) {
    for (const QJsonValue& value: tools) {
      if (value.toObject().value("function").toObject().value("name").toString() == name) {
        return true;
      }
    }
    return false;
  };

  CHECK(has_tool(with_fs, QStringLiteral("search_code_regex")));
  CHECK(has_tool(with_fs, QStringLiteral("find_files")));
  CHECK(has_tool(with_fs, QStringLiteral("search_code")));

  // Without a shared directory the filesystem tools are not offered.
  const QJsonArray without_fs = lnb::net::tool_definitions(false);
  CHECK(!has_tool(without_fs, QStringLiteral("search_code_regex")));
  CHECK(!has_tool(without_fs, QStringLiteral("find_files")));
  CHECK(has_tool(without_fs, QStringLiteral("web_search")));
}

void test_execute_local_tool() {
  const fs::path root = make_fixture();
  const std::string root_str = root.string();

  lnb::net::ToolCall find_call;
  find_call.name = QStringLiteral("find_files");
  find_call.arguments = QStringLiteral("{\"glob\": \"*.cpp\"}");
  const QString found =
      lnb::net::execute_local_tool(find_call, root_str, std::nullopt);
  CHECK(found.contains(QStringLiteral("src/main.cpp")));

  lnb::net::ToolCall regex_call;
  regex_call.name = QStringLiteral("search_code_regex");
  regex_call.arguments = QStringLiteral("{\"pattern\": \"\\\\bfoo\\\\b\"}");
  const QString matched =
      lnb::net::execute_local_tool(regex_call, root_str, std::nullopt);
  CHECK(matched.contains(QStringLiteral("src/main.cpp:2")));

  // A broken pattern comes back as text the model can act on, not a crash.
  lnb::net::ToolCall bad_call;
  bad_call.name = QStringLiteral("search_code_regex");
  bad_call.arguments = QStringLiteral("{\"pattern\": \"(\"}");
  const QString bad =
      lnb::net::execute_local_tool(bad_call, root_str, std::nullopt);
  CHECK(bad.startsWith(QStringLiteral("Tool error:")));

  // Filesystem tools report a missing shared directory clearly.
  const QString no_root =
      lnb::net::execute_local_tool(find_call, std::nullopt, std::nullopt);
  CHECK(no_root.contains(QStringLiteral("no shared directory")));
}

} // namespace

int main() {
  test_collect_files_honours_gitignore();
  test_collect_files_covers_whole_tree();
  test_search_regex_word_boundaries();
  test_search_regex_is_case_sensitive_by_default();
  test_search_regex_respects_ignore_and_glob();
  test_search_regex_invalid_pattern_throws();
  test_find_files();
  test_tool_definitions_include_new_tools();
  test_execute_local_tool();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }

  std::printf("All tool tests passed.\n");
  return EXIT_SUCCESS;
}
