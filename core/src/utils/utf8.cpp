#include "utils/utf8.hpp"

#include <algorithm>
#include <cstdint>

namespace lnb::core {

namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD"; // U+FFFD

// Length of the well-formed sequence starting at `at`, or 0 when the bytes
// there are not one. Overlong forms, surrogates and code points above U+10FFFF
// are rejected, so a sequence accepted here is valid UTF-8 on the wire.
std::size_t sequence_length(std::string_view text, std::size_t at) {
  const auto byte = [&text](std::size_t index) {
    return static_cast<unsigned char>(text[index]);
  };

  const unsigned char lead = byte(at);

  if (lead < 0x80) {
    return 1;
  }

  std::size_t length = 0;
  std::uint32_t code = 0;
  std::uint32_t minimum = 0;

  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    code = lead & 0x1F;
    minimum = 0x80;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    code = lead & 0x0F;
    minimum = 0x800;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    code = lead & 0x07;
    minimum = 0x10000;
  } else {
    return 0;
  }

  if (at + length > text.size()) {
    return 0;
  }

  for (std::size_t i = 1; i < length; ++i) {
    const unsigned char next = byte(at + i);

    if ((next & 0xC0) != 0x80) {
      return 0;
    }

    code = (code << 6) | (next & 0x3F);
  }

  if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
    return 0;
  }

  return length;
}

} // namespace

std::size_t utf8_cut_length(std::string_view text, std::size_t max_bytes) {
  const std::size_t limit = std::min(max_bytes, text.size());
  std::size_t at = 0;
  std::size_t cut = 0;

  while (at < limit) {
    const std::size_t length = sequence_length(text, at);

    // An invalid byte is kept as a single byte: sanitize_utf8() will turn it
    // into a replacement character, which is still better than dropping it here.
    if (length == 0) {
      ++at;
      cut = at;
      continue;
    }

    // The sequence straddles the limit; cutting before it is the whole point of
    // this function.
    if (at + length > limit) {
      break;
    }

    at += length;
    cut = at;
  }

  return cut;
}

std::string sanitize_utf8(std::string_view text) {
  std::string out;
  out.reserve(text.size());

  std::size_t at = 0;

  while (at < text.size()) {
    const std::size_t length = sequence_length(text, at);

    if (length == 0) {
      out.append(kReplacement);
      ++at;
      continue;
    }

    out.append(text.substr(at, length));
    at += length;
  }

  return out;
}

std::string truncate_utf8(std::string_view text, std::size_t max_bytes) {
  return std::string(text.substr(0, utf8_cut_length(text, max_bytes)));
}

} // namespace lnb::core
