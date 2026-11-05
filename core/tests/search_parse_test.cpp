// Regression tests for the web-search result parser.
//
// Search engines answer a plain HTTP client with a bot challenge, so `web_search`
// asks a SearXNG-shaped JSON API instead of scraping HTML. Both a self-hosted
// SearXNG and the local browser bridge return that shape, so one parser covers
// them. These tests pin the parser, without touching the network.
#include "net/search_results.hpp"
#include "net/tools.hpp"

#include <iostream>
#include <string>
#include <vector>

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
    const std::string& actual, const std::string& expected,
    const std::string& label
) {
  if (actual == expected) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << " (got \"" << actual << "\", expected \""
            << expected << "\")\n";
}

void test_searxng_results() {
  const std::string json = R"json({
    "query": "nasa news",
    "number_of_results": 2,
    "results": [
      {
        "title": "All NASA News - NASA",
        "url": "https://www.nasa.gov/news/",
        "content": "NASA will preview upcoming missions.",
        "engine": "brave"
      },
      {
        "title": "Space News",
        "url": "https://www.space.com/news",
        "content": "The latest space headlines.",
        "engines": ["ddg", "bing"]
      }
    ],
    "answers": [],
    "unresponsive_engines": []
  })json";

  const auto results = lnb::core::parse_searxng_results(json);

  expect(results.size() == 2, "SearXNG parses both results");

  if (results.size() != 2) {
    return;
  }

  expect_equal(results[0].title, "All NASA News - NASA", "SearXNG title 0");
  expect_equal(results[0].url, "https://www.nasa.gov/news/", "SearXNG url 0");
  expect_equal(
      results[0].snippet, "NASA will preview upcoming missions.",
      "SearXNG content becomes the snippet"
  );
  expect_equal(results[1].title, "Space News", "SearXNG title 1");
  expect_equal(results[1].url, "https://www.space.com/news", "SearXNG url 1");
}

void test_searxng_skips_incomplete_results() {
  const std::string json = R"json({
    "results": [
      {"title": "", "url": "https://example.com", "content": "no title"},
      {"title": "No URL", "url": "", "content": "no url"},
      {"title": "Good", "url": "https://example.com/good", "content": ""}
    ]
  })json";

  const auto results = lnb::core::parse_searxng_results(json);

  expect(results.size() == 1, "SearXNG skips results without a title or URL");

  if (results.size() == 1) {
    expect_equal(results[0].url, "https://example.com/good", "kept the good one");
    expect(results[0].snippet.empty(), "an empty content stays an empty snippet");
  }
}

void test_malformed_documents() {
  expect(
      lnb::core::parse_searxng_results("not json").empty(),
      "malformed JSON yields nothing"
  );
  expect(
      lnb::core::parse_searxng_results(R"json({"results": "nope"})json").empty(),
      "a non-array results field yields nothing"
  );
  expect(
      lnb::core::parse_searxng_results(R"json({"answers": []})json").empty(),
      "a document without results yields nothing"
  );
  expect(
      lnb::core::parse_searxng_results(R"json({"results": []})json").empty(),
      "an empty results array yields nothing"
  );
}

} // namespace

int main() {
  test_searxng_results();
  test_searxng_skips_incomplete_results();
  test_malformed_documents();

  if (failures != 0) {
    std::cerr << failures << " search parse check(s) failed\n";
    return 1;
  }

  std::cout << "search parse checks passed\n";
  return 0;
}
