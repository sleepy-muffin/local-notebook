#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// Ripgrep-style code search, implemented in-process on top of the PCRE2 that
// Qt already links (QRegularExpression). The point of a separate module is that
// the interesting rules — the ignore-aware walk and the matching — stay pure and
// testable, exactly like the context layer (see docs/architecture.md).
namespace lnb::net {

// One file found by the ignore-aware walk. `relative` is the path from the
// walked root, with '/' separators, for display to the model.
struct FoundFile {
  std::filesystem::path absolute;
  std::string relative;
};

// The outcome of walking a subtree.
struct WalkResult {
  std::filesystem::path directory; // the resolved start directory
  std::size_t scanned = 0; // regular files examined after the ignore rules
  std::vector<FoundFile> files; // files whose relative path matched the glob
  bool truncated = false; // the max_files bound was reached
};

// Walks `relative_path` under `root_path`, honouring .gitignore/.ignore files
// (nearest match wins, with "!" negation), skipping hidden entries and never
// following symlinks. `glob` filters by relative path ("" keeps every file).
// The walk itself is unbounded; the result keeps at most `max_files` matching
// files, setting `truncated` if more matched.
WalkResult collect_files(
    const std::string& root_path, const std::string& relative_path,
    const std::string& glob, std::size_t max_files
);

// The result of a regular-expression search. `text` holds the formatted match
// blocks and is empty when nothing matched; the counts let the caller combine
// several roots without parsing the text. `display_prefix` is prepended to
// every model-visible path, and may be empty for a single shared directory.
struct CodeSearchResult {
  std::string text;
  int matches = 0;
  int files_scanned = 0;
  bool truncated = false;
};

// Regular-expression search over the text files collect_files yields. Matching
// is case-sensitive; inline flags such as (?i) opt in.
CodeSearchResult search_regex(
    const std::string& pattern, const std::string& root_path,
    const std::string& relative_path, const std::string& glob,
    const std::string& display_prefix
);

struct FileListResult {
  std::vector<std::string> paths; // model-visible, prefix already applied
  std::size_t scanned = 0;
  bool truncated = false;
};

// Lists files under `relative_path` whose path matches the `glob` filter.
FileListResult find_files(
    const std::string& glob, const std::string& root_path,
    const std::string& relative_path, const std::string& display_prefix
);

} // namespace lnb::net
