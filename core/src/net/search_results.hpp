#pragma once

#include <string>
#include <vector>

namespace lnb::core {

// One web-search hit, independent of which provider produced it.
struct SearchResult {
  std::string title;
  std::string url;
  std::string snippet;
};

// Parses a SearXNG-style JSON response (`{"results": [...]}`), which is also the
// shape the local browser bridge returns. Each hit contributes `title`, `url`
// and `content`; malformed or hostless entries are skipped. Returns an empty
// vector when the document carries no results.
std::vector<SearchResult> parse_searxng_results(const std::string& json);

} // namespace lnb::core
