#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "render/document.hpp"
#include "render/types.hpp"

namespace render {

// An opaque, backend-owned shaped paragraph. The layout engine never inspects
// it; it is passed back for hit testing and painting.
struct ParagraphKey {
  std::uint32_t value = 0;

  bool operator==(const ParagraphKey&) const = default;
};

struct LineInfo {
  // Offsets into the paragraph text handed to TextBackend::shape().
  std::uint32_t begin = 0;
  std::uint32_t length = 0;
  float width = 0.0f;
  // The advance used for this line, including any extra leading the caller
  // asked for through ParagraphSpec::line_height_scale.
  float height = 0.0f;
  float ascent = 0.0f;
};

struct ParagraphSpec {
  std::u16string_view text;
  std::span<const Span> spans;
  Font base_font;
  Font code_font;
  Color base_color;
  Color link_color;
  Color code_color;
  Color inline_code_background;
  bool has_inline_code_background = false;
  // <= 0 wraps nowhere (only explicit line breaks).
  float wrap_width = 0.0f;
  // Minimum line advance as a multiple of the base font's line spacing, so the
  // reading rhythm of settings.line_height_* survives shaping.
  float line_height_scale = 1.0f;
};

struct ParagraphPaint {
  ParagraphKey key;
  std::uint32_t line = 0;
  Point origin;
  float box_height = 0.0f;
  // Selection range in the paragraph's index space; an empty range draws none.
  std::uint32_t selection_begin = 0;
  std::uint32_t selection_end = 0;
  Color selection_color{};
};

// The drawing surface the layout engine talks to. Everything is in document
// coordinates; the Qt adapter translates the painter once for the whole frame.
class Canvas {
public:
  virtual ~Canvas() = default;

  virtual void fill_rect(const Rect& rect, Color color) = 0;
  virtual void fill_rounded_rect(const Rect& rect, Color color, float radius) = 0;
  virtual void stroke_rounded_rect(
      const Rect& rect, Color color, float radius, float width
  ) = 0;
  virtual void stroke_line(Point from, Point to, Color color, float width) = 0;
  virtual void draw_text(
      std::u16string_view text, const Rect& rect, const Font& font, Color color,
      TextAlign align
  ) = 0;
  virtual void draw_paragraph(const ParagraphPaint& paint) = 0;
};

// The shaping and measurement half of the backend. The renderer owns layout,
// selection and painting order; the backend owns fonts, bidi, line breaking and
// glyph placement.
class TextBackend {
public:
  virtual ~TextBackend() = default;

  // Shapes one logical line. `lines` is replaced with one entry per laid-out
  // line; an empty paragraph still yields one empty line.
  virtual ParagraphKey shape(
      const ParagraphSpec& spec, std::vector<LineInfo>& lines
  ) = 0;

  virtual void release(ParagraphKey key) = 0;

  // Maps an x coordinate inside a line box to a text offset, and back.
  virtual std::uint32_t x_to_offset(
      ParagraphKey key, std::uint32_t line, float x
  ) = 0;
  virtual float offset_to_x(
      ParagraphKey key, std::uint32_t line, std::uint32_t offset
  ) = 0;
};

} // namespace render
