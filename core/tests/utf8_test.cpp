// Regression test for the UTF-8 handling that guards every request body.
//
// A fetched page can carry bytes from another encoding, and cutting text at a
// byte limit can split a multi-byte code point. Either way the request body
// stops being valid UTF-8, and a strict JSON server rejects the whole turn with
// "invalid continuation byte" — which is exactly what happened when a fetched
// page was truncated in the middle of a CJK character.
#include "utils/utf8.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
  if (condition) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << '\n';
}

bool is_valid_utf8(std::string_view text) {
  std::size_t at = 0;

  while (at < text.size()) {
    const auto byte = [&text](std::size_t index) {
      return static_cast<unsigned char>(text[index]);
    };

    const unsigned char lead = byte(at);
    std::size_t length = 0;

    if (lead < 0x80) {
      length = 1;
    } else if ((lead & 0xE0) == 0xC0) {
      length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
    } else {
      return false;
    }

    if (at + length > text.size()) {
      return false;
    }

    for (std::size_t i = 1; i < length; ++i) {
      if ((byte(at + i) & 0xC0) != 0x80) {
        return false;
      }
    }

    at += length;
  }

  return true;
}

} // namespace

int main() {
  // "感" is E6 84 9F. Cutting after two bytes must drop it entirely rather than
  // keep a broken prefix.
  const std::string page = "abc\xE6\x84\x9F";

  expect(lnb::core::truncate_utf8(page, 3) == "abc", "a cut before the CJK char keeps it whole");
  expect(lnb::core::truncate_utf8(page, 4) == "abc", "a cut inside the CJK char backs off");
  expect(lnb::core::truncate_utf8(page, 5) == "abc", "a cut inside the CJK char backs off (2)");
  expect(lnb::core::truncate_utf8(page, 6) == page, "a cut after the CJK char keeps it");
  expect(lnb::core::truncate_utf8(page, 100) == page, "a generous limit keeps everything");
  expect(lnb::core::truncate_utf8("", 10).empty(), "an empty string stays empty");

  // A truncated sequence is the exact shape of the reported failure: the first
  // two bytes of "感" reached the wire.
  const std::string broken = page.substr(0, 5);
  expect(!is_valid_utf8(broken), "the fixture really is invalid UTF-8");
  expect(is_valid_utf8(lnb::core::sanitize_utf8(broken)), "sanitising repairs a split sequence");

  // Bytes from another encoding (GBK "感觉" would be B8 D0 BE F5) must never
  // survive into a request body.
  const std::string gbk = "\xB8\xD0\xBE\xF5";
  expect(is_valid_utf8(lnb::core::sanitize_utf8(gbk)), "sanitising repairs non-UTF-8 bytes");

  // Valid text is passed through untouched, including astral code points.
  const std::string valid = "plain ASCII and \xE6\x84\x9F and \xF0\x9F\x90\xB1";
  expect(lnb::core::sanitize_utf8(valid) == valid, "valid UTF-8 is unchanged");
  expect(lnb::core::sanitize_utf8("") == "", "an empty string stays empty");

  // End to end: a serialized JSON body carrying invalid bytes becomes parseable
  // again after sanitising, which is what send_message_once() relies on.
  boost::json::object payload;
  payload["content"] = broken;
  const std::string body = boost::json::serialize(payload);

  boost::system::error_code before;
  (void)boost::json::parse(body, before);
  expect(static_cast<bool>(before), "the unsanitised body is rejected");

  boost::system::error_code after;
  const boost::json::value repaired =
      boost::json::parse(lnb::core::sanitize_utf8(body), after);
  expect(!after && repaired.is_object(), "the sanitised body parses");

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }

  std::cout << "utf8: all expectations passed\n";
  return 0;
}
