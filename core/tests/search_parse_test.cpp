// Regression tests for the web-search HTML parsers.
//
// DuckDuckGo answers automated traffic with a bot-challenge page (HTTP 202), so
// `web_search` needs to tell that page apart from a result page and fall back to
// Brave. These tests pin the parsers against captured markup shapes, without
// touching the network.
#include "net/search_results.hpp"

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

void test_ddg_results() {
  const std::string html =
      "<html><body>"
      "<div class=\"result results_links\">"
      "<a rel=\"nofollow\" class=\"result__a\" "
      "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fwww.nasa.gov%2Fnews%2F\">"
      "All NASA News - NASA</a>"
      "<a class=\"result__snippet\" href=\"//duckduckgo.com/l/?uddg=x\">"
      "NASA will preview upcoming missions.</a>"
      "</div>"
      "<div class=\"result results_links\">"
      "<a rel=\"nofollow\" class=\"result__a\" "
      "href=\"https://www.space.com/news\">Space News</a>"
      "<a class=\"result__snippet\" href=\"https://www.space.com/news\">"
      "The latest space headlines.</a>"
      "</div>"
      "</body></html>";

  const auto results = lnb::net::parse_ddg_results(html);

  expect(results.size() == 2, "DDG parses both results");

  if (results.size() != 2) {
    return;
  }

  expect_equal(results[0].title, "All NASA News - NASA", "DDG title 0");
  expect_equal(
      results[0].url, "https://www.nasa.gov/news/", "DDG redirect decoded"
  );
  expect_equal(
      results[0].snippet, "NASA will preview upcoming missions.", "DDG snippet 0"
  );
  expect_equal(results[1].title, "Space News", "DDG title 1");
  expect_equal(results[1].url, "https://www.space.com/news", "DDG direct url");
  expect_equal(
      results[1].snippet, "The latest space headlines.", "DDG snippet 1"
  );
}

void test_ddg_challenge() {
  expect(
      lnb::net::is_ddg_challenge(
          "<html><body><div id=\"anomaly-modal\">Are you a robot?</div>"
          "</body></html>"
      ),
      "DDG anomaly modal is a challenge"
  );
  expect(
      lnb::net::is_ddg_challenge(
          "<p>Select all squares containing a duck</p>"
      ),
      "DDG image challenge is a challenge"
  );
  expect(
      !lnb::net::is_ddg_challenge(
          "<p>We detected an anomaly in the data set.</p>"
      ),
      "ordinary text mentioning anomaly is not a challenge"
  );
  expect(
      !lnb::net::is_ddg_challenge(
          "<a class=\"result__a\" href=\"https://example.com\">Example</a>"
      ),
      "result page is not a challenge"
  );
}

void test_brave_results() {
  const std::string html =
      "<html><body><main>"
      "<a href=\"https://www.nasa.gov/news/recently-published/\">"
      "<div class=\"title search-snippet-title line-clamp-1 svelte-abc\" "
      "title=\"All NASA News - NASA\">All NASA News - NASA</div>"
      "</a>"
      "<div class=\"generic-snippet svelte-def\">"
      "<div class=\"content desktop-default-regular t-primary\">"
      "June 17, 2025 - NASA will preview upcoming missions."
      "</div></div>"
      "<a href=\"https://www.space.com/astronomy\">"
      "<div class=\"title search-snippet-title svelte-abc\" "
      "title=\"Space &amp; Astronomy\">Space &amp; Astronomy</div>"
      "</a>"
      "<div class=\"generic-snippet svelte-def\">"
      "<div class=\"content desktop-default-regular t-primary\">"
      "Astronomy news today."
      "</div></div>"
      "<a href=\"https://search.brave.com/search?q=more\">"
      "<div class=\"title search-snippet-title\">More results</div>"
      "</a>"
      "</main></body></html>";

  const auto results = lnb::net::parse_brave_results(html);

  expect(results.size() == 2, "Brave skips its own internal result");

  if (results.size() != 2) {
    return;
  }

  expect_equal(results[0].title, "All NASA News - NASA", "Brave title 0");
  expect_equal(
      results[0].url, "https://www.nasa.gov/news/recently-published/",
      "Brave url 0"
  );
  expect_equal(
      results[0].snippet,
      "June 17, 2025 - NASA will preview upcoming missions.", "Brave snippet 0"
  );
  expect_equal(
      results[1].title, "Space & Astronomy", "Brave title decodes entities"
  );
  expect_equal(
      results[1].url, "https://www.space.com/astronomy", "Brave url 1"
  );
  expect_equal(
      results[1].snippet, "Astronomy news today.", "Brave snippet 1"
  );
}

void test_bing_rss() {
  const std::string xml =
      "<rss version=\"2.0\"><channel>"
      "<title>Bing: latest NASA news</title>"
      "<link>http://www.bing.com:80/search?q=latest+NASA+news</link>"
      "<item>"
      "<title>All NASA News - NASA</title>"
      "<link>https://www.nasa.gov/news/recently-published/</link>"
      "<description>NASA will preview upcoming missions.</description>"
      "</item>"
      "<item>"
      "<title><![CDATA[Space &amp; Astronomy]]></title>"
      "<link><![CDATA[https://www.space.com/astronomy?a=1&amp;b=2]]></link>"
      "<description><![CDATA[<b>Astronomy</b> news today.]]></description>"
      "</item>"
      "<item>"
      "<title>Bing navigation</title>"
      "<link>https://www.bing.com/search?q=more</link>"
      "<description>Internal link</description>"
      "</item>"
      "</channel></rss>";

  const auto results = lnb::net::parse_bing_rss(xml);

  expect(results.size() == 2, "Bing skips its own navigation item");

  if (results.size() != 2) {
    return;
  }

  expect_equal(results[0].title, "All NASA News - NASA", "Bing title 0");
  expect_equal(
      results[0].url, "https://www.nasa.gov/news/recently-published/",
      "Bing url 0"
  );
  expect_equal(
      results[0].snippet, "NASA will preview upcoming missions.", "Bing snippet 0"
  );
  expect_equal(
      results[1].title, "Space & Astronomy", "Bing CDATA title is decoded"
  );
  expect_equal(
      results[1].url, "https://www.space.com/astronomy?a=1&b=2",
      "Bing CDATA url is decoded"
  );
  expect_equal(
      results[1].snippet, "Astronomy news today.", "Bing CDATA snippet is text"
  );
}

void test_brave_without_results() {
  const std::string html = "<html><body><p>No results found.</p></body></html>";

  expect(
      lnb::net::parse_brave_results(html).empty(),
      "Brave page without result markup yields nothing"
  );
  expect(
      lnb::net::parse_ddg_results(html).empty(),
      "DDG page without result markup yields nothing"
  );
}

} // namespace

int main() {
  test_ddg_results();
  test_ddg_challenge();
  test_brave_results();
  test_bing_rss();
  test_brave_without_results();

  if (failures != 0) {
    std::cerr << failures << " search parse check(s) failed\n";
    return 1;
  }

  std::cout << "search parse checks passed\n";
  return 0;
}
