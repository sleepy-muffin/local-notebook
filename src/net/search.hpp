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
// session's shared directory, with '/' separators, for display to the model.
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

// Regular-expression search over the text files collect_files yields. Returns
// matching paths, line numbers and surrounding context, or a clear message when
// nothing matched. Matching is case-sensitive; inline flags such as (?i) opt in.
std::string search_regex(
    const std::string& pattern, const std::string& root_path,
    const std::string& relative_path, const std::string& glob
);

// Lists files under `relative_path` whose path matches the `glob` filter.
std::string find_files(
    const std::string& glob, const std::string& root_path,
    const std::string& relative_path
);

} // namespace lnb::net
