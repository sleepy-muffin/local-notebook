#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace lnb::core {

// Byte length of the longest prefix of `text` that is at most `max_bytes` long
// and ends on a code-point boundary, so cutting there can never split a
// well-formed UTF-8 sequence. Invalid bytes are treated as one byte each.
std::size_t utf8_cut_length(std::string_view text, std::size_t max_bytes);

// `text` with every byte that is not part of a well-formed UTF-8 sequence
// replaced by U+FFFD. Fetched pages and file reads can carry bytes from another
// encoding, and a request body containing them is rejected outright by strict
// JSON servers ("invalid continuation byte"), so anything that reaches the wire
// is passed through here.
std::string sanitize_utf8(std::string_view text);

// The longest prefix of `text` that is at most `max_bytes` long and ends on a
// code-point boundary. Unlike sanitize_utf8() it introduces no replacement
// characters; use it to truncate text that is already valid UTF-8.
std::string truncate_utf8(std::string_view text, std::size_t max_bytes);

} // namespace lnb::core
