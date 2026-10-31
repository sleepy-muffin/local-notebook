// Regression test for the assistant-form URL helpers. `is_valid_base_url` used
// to parse `trim(url)` directly, which left the parsed `url_view` pointing at a
// destroyed temporary; reading scheme()/host()/path() then returned garbage and
// valid URLs could be rejected. A sanitizer build flags that read immediately,
// and the assertions below pin the accepted and rejected shapes.
#include "local_notebook/core/interface.hpp"

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

void test_valid_bases() {
  const std::string bases[] = {
      "https://example.com/v1",
      "http://example.com/v1",
      "https://io.iioooo.com/v1",
      "https://example.com/api/v1",
      "https://example.com/v1/",
      "  https://example.com/v1  ",
      "https://example.com:8443/v1",
  };

  for (const std::string& base: bases) {
    expect(lnb::api::is_valid_base_url(base), "accepted base: \"" + base + "\"");
  }
}

void test_invalid_bases() {
  const std::string bases[] = {
      "ftp://example.com/v1",
      "example.com/v1",
      "https://example.com",
      "https://example.com/v2",
      "https://example.com/v1?key=value",
      "https://example.com/v1#fragment",
      "https:///v1",
      "https://example.com/",
  };

  for (const std::string& base: bases) {
    expect(
        !lnb::api::is_valid_base_url(base), "rejected base: \"" + base + "\""
    );
  }
}

void test_helpers() {
  expect_equal(
      lnb::api::ensure_scheme("example.com/v1"), "https://example.com/v1",
      "ensure_scheme adds https"
  );
  expect_equal(
      lnb::api::ensure_scheme("http://example.com/v1"),
      "http://example.com/v1", "ensure_scheme keeps http"
  );
  expect_equal(
      lnb::api::normalize_base_url("https://example.com/v1/responses"),
      "https://example.com/v1", "normalize strips /responses"
  );
  expect_equal(
      lnb::api::normalize_base_url("https://example.com/v1/chat/completions"),
      "https://example.com/v1", "normalize strips completions"
  );
  expect_equal(
      lnb::api::normalize_base_url("https://example.com/v1///"),
      "https://example.com/v1", "normalize strips trailing slashes"
  );
}

// Repeated calls keep the parsing path hot; a sanitizer build fails on the
// dangling view even though a release build may still see the old bytes.
void test_repeated_validation() {
  for (int i = 0; i < 1000; ++i) {
    const std::string base = "https://io.iioooo.com/v1";
    expect(
        lnb::api::is_valid_base_url(base), "repeated validation stays true"
    );

    const std::string noise(
        static_cast<std::size_t>(i % 64) + 1, static_cast<char>('a' + i % 26)
    );
    (void)noise;
  }
}

} // namespace

int main() {
  test_valid_bases();
  test_invalid_bases();
  test_helpers();
  test_repeated_validation();

  if (failures != 0) {
    std::cerr << failures << " URL validation check(s) failed\n";
    return 1;
  }

  std::cout << "url validation checks passed\n";
  return 0;
}
