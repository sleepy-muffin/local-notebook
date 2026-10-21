#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/types.hpp"

namespace render {

// A styled range of a block's text. Offsets are UTF-16 code units into
// Block::text, which is the same index space QString and QTextLayout use, so a
// backend never has to remap them.
struct Span {
  std::uint32_t begin = 0;
  std::uint32_t length = 0;
  bool bold = false;
  bool italic = false;
  bool code = false;
  bool underline = false;
  bool strike = false;
  bool link = false;
  std::u16string href;
};

// One paragraph-like unit. A Block may contain explicit line breaks ('\n' and
// U+2028); the layout engine splits those into logical lines, and each logical
// line wraps independently. A fenced code block is a single Block so its
// background and rounded corners cover the whole fence.
struct Block {
  BlockKind kind = BlockKind::Paragraph;
  int heading_level = 0;
  int list_depth = 0;
  bool ordered = false;
  std::uint32_t list_number = 0;
  int quote_depth = 0;
  std::uint32_t message = 0;
  bool first_in_message = false;
  bool last_in_message = false;
  std::u16string code_language;
  std::u16string text;
  std::vector<Span> spans;
};

// Everything the transcript needs to draw a message's metadata strip and to
// carry out its hover actions. The renderer never looks at the database.
struct MessageMeta {
  Role role = Role::Assistant;
  std::uint32_t id = 0;
  std::u16string time;
  std::u16string tokens;
  bool editable = false;
  bool streaming = false;
  std::u16string raw;
};

// A complete transcript window, in visual order.
struct Document {
  std::vector<MessageMeta> messages;
  std::vector<Block> blocks;
};

} // namespace render
