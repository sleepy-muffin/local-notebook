#pragma once

#include <string>
#include <vector>

namespace lnb::net {

// One web-search hit, independent of which provider produced it.
struct SearchResult {
  std::string title;
  std::string url;
  std::string snippet;
};

// Parses a DuckDuckGo HTML results page. Returns an empty vector when the page
// carries no recognizable result links (for example, a bot-challenge page).
std::vector<SearchResult> parse_ddg_results(const std::string& html);

// Parses a Brave HTML results page. Returns an empty vector when the page
// carries no recognizable result links.
std::vector<SearchResult> parse_brave_results(const std::string& html);

// Parses Bing's RSS result feed (`search?q=...&format=rss`). RSS is a
// machine-oriented endpoint: it needs no JavaScript and, unlike the HTML page,
// is not wrapped in a consent or challenge layer.
std::vector<SearchResult> parse_bing_rss(const std::string& xml);

// True when a DuckDuckGo response is its bot-mitigation page rather than a
// results page. DuckDuckGo answers these with HTTP 202, so the status alone is
// not enough to detect them.
bool is_ddg_challenge(const std::string& html);

} // namespace lnb::net
