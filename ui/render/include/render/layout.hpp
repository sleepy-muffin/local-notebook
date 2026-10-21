#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "render/backend.hpp"
#include "render/document.hpp"
#include "render/types.hpp"

namespace render {

// A caret position in the document: a block and a UTF-16 offset into its text.
struct Position {
  std::uint32_t block = 0;
  std::uint32_t offset = 0;

  auto operator<=>(const Position&) const = default;
};

struct Selection {
  Position anchor;
  Position focus;

  [[nodiscard]] bool empty() const { return anchor == focus; }
};

// Every colour, font and spacing decision the renderer needs. The UI maps its
// theme into this once; the renderer itself never looks at Qt or the settings.
struct LayoutTheme {
  Font body;
  Font mono;

  Color text;
  Color muted_text;
  Color accent;
  Color link;

  Color user_background;
  Color user_text;
  Color assistant_text;
  Color code_background;
  Color inline_code_background;
  Color notice_background;
  Color danger_background;
  Color danger_text;
  Color border;
  Color selection;

  float bubble_radius = 8.0f;
  float code_radius = 6.0f;

  // Inline code is scaled from the surrounding text rather than from the
  // configured monospace size, so it keeps its proportions inside headings and
  // never looks oversized next to the prose.
  float inline_code_scale = 0.9f;

  // Horizontal inset shared by every message body, so prose and code line up.
  float body_side_padding = 14.0f;
  float bubble_padding_v = 8.0f;
  float code_padding_h = 10.0f;
  float code_padding_v = 8.0f;

  float paragraph_spacing = 10.0f;
  float list_spacing = 0.0f;
  float message_spacing = 12.0f;
  float actions_height = 20.0f;

  float list_indent = 18.0f;
  float quote_indent = 12.0f;

  float heading_scale[6] = {1.60f, 1.40f, 1.25f, 1.15f, 1.08f, 1.00f};

  // Minimum line advance, as a multiple of the font's line spacing.
  float line_height_other = 1.60f;
  float line_height_self = 1.35f;
};

struct LayoutOptions {
  float view_width = 0.0f;
  float left_inset = 0.0f;
  float right_inset = 0.0f;
  // 0 means unlimited.
  float content_max_width = 0.0f;
  float top_padding = 0.0f;
  float bottom_padding = 0.0f;
};

// One wrapped line of one logical line of one block.
struct LineBox {
  std::uint32_t block = 0;
  ParagraphKey key;
  std::uint32_t line_in_paragraph = 0;
  // Offset of this logical line within Block::text, so a hit can be mapped back
  // to a block offset without knowing how the block was split.
  std::uint32_t logical_begin = 0;
  // Text range within the logical line (and the paragraph handed to the backend).
  std::uint32_t begin = 0;
  std::uint32_t length = 0;

  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

struct BlockGeom {
  Rect rect;
  // The rounded fill of a code block, empty for other blocks.
  Rect background;
  // The list marker area or the quote rule, empty otherwise.
  Rect marker;
  float text_x = 0.0f;
  float gap_after = 0.0f;
  std::size_t first_line = 0;
  std::size_t line_count = 0;
};

struct MessageGeom {
  Rect rect;
  // The message body without the action strip. For a wrapped role this is also
  // the filled block; for an assistant reply it is the bare text area.
  Rect content;
  // Empty for assistant replies, which sit directly on the conversation surface.
  Rect bubble;
  // Reserved strip under the message: hover actions and the id/time/tokens line.
  Rect actions;
  std::size_t first_block = 0;
  std::size_t block_count = 0;
};

class Layout {
public:
  Layout();
  ~Layout();

  Layout(const Layout&) = delete;
  Layout& operator=(const Layout&) = delete;

  void set_backend(TextBackend* backend);

  void set_theme(LayoutTheme theme);
  [[nodiscard]] const LayoutTheme& theme() const { return theme_; }

  void set_options(LayoutOptions options);
  [[nodiscard]] const LayoutOptions& options() const { return options_; }

  void set_document(Document document);
  [[nodiscard]] const Document& document() const { return document_; }

  // Appends one message and lays out only the new tail. Used when a reply starts
  // streaming, so the rest of the transcript keeps its shaped paragraphs.
  void append_message(MessageMeta meta, std::vector<Block> blocks);

  // Replaces the blocks of the last message and re-lays out only that message.
  // A streaming reply is always the last message, so this turns each text update
  // into a tail relayout instead of a whole-document reshape.
  void replace_last_message_blocks(std::vector<Block> blocks);

  // Drops every shaped paragraph and all geometry. The document is kept.
  void clear();

  // Re-shapes the document against the current theme and width.
  void relayout();

  [[nodiscard]] float content_height() const { return content_height_; }
  [[nodiscard]] float content_left() const { return content_left_; }
  [[nodiscard]] float content_width() const { return content_width_; }

  [[nodiscard]] const std::vector<LineBox>& lines() const { return lines_; }
  [[nodiscard]] const std::vector<BlockGeom>& block_geometry() const { return blocks_; }
  [[nodiscard]] const std::vector<MessageGeom>& message_geometry() const { return messages_; }

  // Nearest caret to a document-space point.
  [[nodiscard]] std::optional<Position> hit_test(Point point) const;

  // The message whose box (including its action strip) contains `content_y`.
  [[nodiscard]] std::optional<std::size_t> message_at(float content_y) const;

  [[nodiscard]] std::optional<std::size_t> message_by_id(std::uint32_t id) const;

  // The URL under a caret, when it sits inside a link span.
  [[nodiscard]] std::optional<std::u16string> link_at(Position position) const;

  // Plain text of the selection, blocks joined with newlines. Code blocks keep
  // their own newlines and are not wrapped in fence markers.
  [[nodiscard]] std::u16string selected_text(const Selection& selection) const;

  // Draws every visible message background, block decoration and text line.
  void paint(Canvas& canvas, const Rect& clip, const Selection& selection) const;

private:
  struct ParagraphSlot {
    std::uint32_t block = 0;
    std::uint32_t logical_begin = 0;
    ParagraphKey key;
    std::size_t first_line = 0;
    std::size_t line_count = 0;
  };

  void release_paragraphs();
  [[nodiscard]] float layout_message(std::size_t message_index, std::size_t first_block, float y);
  void layout_block(std::size_t block_index, float x, float width, float y);
  [[nodiscard]] std::vector<Span>
  clipped_spans(const Block& block, std::uint32_t begin, std::uint32_t end) const;
  [[nodiscard]] Font block_font(const Block& block) const;
  [[nodiscard]] Color role_text_color(Role role) const;
  [[nodiscard]] float role_line_height(Role role) const;
  [[nodiscard]] float shape_block(
      std::size_t block_index, const Block& block, Font base, Color color, float text_x,
      float text_width, float y, float line_height_scale);

  TextBackend* backend_ = nullptr;
  LayoutTheme theme_;
  LayoutOptions options_;
  Document document_;

  std::vector<ParagraphSlot> paragraphs_;
  std::vector<LineBox> lines_;
  std::vector<BlockGeom> blocks_;
  std::vector<MessageGeom> messages_;

  float content_height_ = 0.0f;
  float content_left_ = 0.0f;
  float content_width_ = 0.0f;
};

} // namespace render
