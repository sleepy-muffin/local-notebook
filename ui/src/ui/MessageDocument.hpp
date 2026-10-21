#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "render/document.hpp"

// Bridges the stored message text to the Qt-free renderer model. Markdown is
// still parsed by Qt (QTextDocument keeps the current Markdown semantics), but
// only the plain blocks, spans and colours leave this file: the layout engine,
// hit testing and painting never see a QTextDocument.
namespace ui {

render::MessageMeta make_message_meta(
    render::Role role, std::uint32_t id, std::u16string time, std::u16string tokens, bool editable,
    std::u16string raw);

// A literal message (the reader's own text, notices, streamed status text). A
// single block keeps the source line breaks as hard breaks, so paragraph
// spacing never appears between typed lines.
std::vector<render::Block> make_plain_blocks(std::string_view utf8);

// A Markdown reply: one block per paragraph, with fenced code blocks folded
// into a single block and inline emphasis mapped to spans.
std::vector<render::Block> make_markdown_blocks(std::string_view utf8);

// Appends a message and its blocks to `document`, stamping each block with the
// message index and the first/last flags. Returns the message's index.
std::size_t append_message(
    render::Document& document, render::MessageMeta meta, std::vector<render::Block> blocks);

} // namespace ui
