// Headless tests for the transcript renderer. The fake backend treats every
// UTF-16 unit as an 8px box, so expected geometry is exact and no display
// server, font database or Qt is involved.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/layout.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

class FakeBackend : public render::TextBackend {
public:
  struct Paragraph {
    std::vector<render::LineInfo> lines;
  };

  render::ParagraphKey
  shape(const render::ParagraphSpec& spec, std::vector<render::LineInfo>& lines) override {
    constexpr float kUnit = 8.0f;
    const float height = std::max(20.0f, 20.0f * spec.line_height_scale);

    const std::size_t per_line =
        spec.wrap_width > 0.0f
            ? std::max<std::size_t>(1, static_cast<std::size_t>(spec.wrap_width / kUnit))
            : std::max<std::size_t>(1, spec.text.size());

    if (spec.text.empty()) {
      lines.push_back(render::LineInfo{0, 0, 0.0f, height, 15.0f});
    } else {
      std::size_t position = 0;

      while (position < spec.text.size()) {
        const std::size_t length = std::min(per_line, spec.text.size() - position);
        lines.push_back(render::LineInfo{
            static_cast<std::uint32_t>(position), static_cast<std::uint32_t>(length),
            static_cast<float>(length) * kUnit, height, 15.0f});
        position += length;
      }
    }

    const render::ParagraphKey key{next_key_++};
    paragraphs_[key.value] = Paragraph{lines};
    return key;
  }

  void release(render::ParagraphKey key) override { paragraphs_.erase(key.value); }

  std::uint32_t x_to_offset(render::ParagraphKey key, std::uint32_t line, float x) override {
    const auto found = paragraphs_.find(key.value);

    if (found == paragraphs_.end() || line >= found->second.lines.size()) {
      return 0;
    }

    const render::LineInfo& info = found->second.lines[line];
    const auto units = static_cast<std::uint32_t>(std::floor(std::max(0.0f, x) / 8.0f));
    return std::min(info.begin + units, info.begin + info.length);
  }

  float offset_to_x(render::ParagraphKey key, std::uint32_t line, std::uint32_t offset) override {
    const auto found = paragraphs_.find(key.value);

    if (found == paragraphs_.end() || line >= found->second.lines.size()) {
      return 0.0f;
    }

    const render::LineInfo& info = found->second.lines[line];
    const std::uint32_t local =
        offset > info.begin ? std::min(offset - info.begin, info.length) : 0;
    return static_cast<float>(local) * 8.0f;
  }

  [[nodiscard]] std::size_t paragraph_count() const { return paragraphs_.size(); }

private:
  std::uint32_t next_key_ = 1;
  std::unordered_map<std::uint32_t, Paragraph> paragraphs_;
};

class NullCanvas : public render::Canvas {
public:
  void fill_rect(const render::Rect&, render::Color) override {}
  void fill_rounded_rect(const render::Rect&, render::Color, float) override {}
  void stroke_rounded_rect(const render::Rect&, render::Color, float, float) override {}
  void stroke_line(render::Point, render::Point, render::Color, float) override {}
  void draw_text(
      std::u16string_view, const render::Rect&, const render::Font&, render::Color,
      render::TextAlign) override {}
  void draw_paragraph(const render::ParagraphPaint&) override { ++paragraphs; }

  int paragraphs = 0;
};

render::LayoutTheme test_theme() {
  render::LayoutTheme theme;
  theme.body.family = "Test";
  theme.body.size = 12.0f;
  theme.mono.family = "Test Mono";
  theme.mono.size = 11.0f;
  theme.text = render::Color{255, 255, 255, 255};
  theme.assistant_text = theme.text;
  theme.body_side_padding = 14.0f;
  theme.code_padding_h = 10.0f;
  theme.code_padding_v = 8.0f;
  theme.paragraph_spacing = 10.0f;
  theme.message_spacing = 0.0f;
  theme.actions_height = 20.0f;
  theme.line_height_other = 1.0f;
  theme.line_height_self = 1.0f;
  return theme;
}

render::Document test_document() {
  render::Document document;

  render::MessageMeta meta;
  meta.role = render::Role::Assistant;
  meta.id = 7;
  meta.raw = u"raw source";
  document.messages.push_back(meta);

  render::Block paragraph;
  paragraph.kind = render::BlockKind::Paragraph;
  paragraph.message = 0;
  paragraph.first_in_message = true;
  paragraph.text = u"hello world";

  render::Span bold;
  bold.begin = 0;
  bold.length = 5;
  bold.bold = true;
  paragraph.spans.push_back(bold);

  render::Span link;
  link.begin = 0;
  link.length = 5;
  link.link = true;
  link.href = u"https://example.com";
  paragraph.spans.push_back(link);

  render::Block code;
  code.kind = render::BlockKind::Code;
  code.message = 0;
  code.text = u"line one\nline two";
  code.last_in_message = false;

  render::Block tail;
  tail.kind = render::BlockKind::Paragraph;
  tail.message = 0;
  tail.last_in_message = true;
  tail.text = u"tail";

  document.blocks.push_back(paragraph);
  document.blocks.push_back(code);
  document.blocks.push_back(tail);
  return document;
}

void test_geometry_and_selection() {
  FakeBackend backend;
  render::Layout layout;
  layout.set_backend(&backend);
  layout.set_theme(test_theme());

  render::LayoutOptions options;
  options.view_width = 300.0f;
  options.left_inset = 10.0f;
  options.right_inset = 10.0f;
  options.top_padding = 10.0f;
  options.bottom_padding = 10.0f;
  layout.set_options(options);
  layout.set_document(test_document());

  check(layout.message_geometry().size() == 1, "one message");
  check(layout.block_geometry().size() == 3, "three blocks");
  check(layout.content_height() > 0.0f, "content has height");
  check(backend.paragraph_count() == 4, "one shaped paragraph per logical line");

  // The first line starts at the content left + body padding.
  const render::LineBox& first = layout.lines().front();
  const render::Point inside{first.x + 24.0f, first.y + 5.0f};
  const auto hit = layout.hit_test(inside);
  check(hit.has_value(), "hit test finds a caret");
  check(hit->block == 0, "hit test lands in the first block");
  check(hit->offset == 3, "hit test maps x to offset 3");

  const auto message = layout.message_at(inside.y);
  check(message.has_value() && *message == 0, "message_at finds message 0");

  const auto href = layout.link_at(render::Position{0, 2});
  check(href.has_value() && *href == u"https://example.com", "link_at finds the URL");

  const render::Selection selection{{0, 0}, {2, 2}};
  const std::u16string text = layout.selected_text(selection);
  check(
      text == u"hello world\nline one\nline two\nta",
      "selected_text joins blocks and honours the end offset");

  NullCanvas canvas;
  layout.paint(canvas, render::Rect{-1000.0f, -1000.0f, 4000.0f, 4000.0f}, selection);
  check(canvas.paragraphs > 0, "paint draws paragraph lines");

  // A relayout releases the old shaped paragraphs rather than leaking them.
  layout.relayout();
  check(backend.paragraph_count() == 4, "relayout releases the previous paragraphs");
}

void test_message_index_by_id() {
  FakeBackend backend;
  render::Layout layout;
  layout.set_backend(&backend);
  layout.set_theme(test_theme());
  layout.set_options(render::LayoutOptions{300.0f, 10.0f, 10.0f, 0.0f, 0.0f, 0.0f});
  layout.set_document(test_document());

  const auto found = layout.message_by_id(7);
  check(found.has_value() && *found == 0, "message_by_id finds the stored id");
  check(!layout.message_by_id(99).has_value(), "unknown id is absent");
}

void test_incremental_tail() {
  FakeBackend backend;
  render::Layout layout;
  layout.set_backend(&backend);
  layout.set_theme(test_theme());
  layout.set_options(render::LayoutOptions{300.0f, 10.0f, 10.0f, 0.0f, 0.0f, 0.0f});
  layout.set_document(test_document());

  const float height_before = layout.content_height();
  const render::ParagraphKey untouched = layout.lines().front().key;
  const std::size_t shaped_before = backend.paragraph_count();

  render::MessageMeta streaming;
  streaming.role = render::Role::Assistant;
  streaming.id = 0;
  streaming.streaming = true;

  render::Block first;
  first.kind = render::BlockKind::Paragraph;
  first.text = u"Hello";

  layout.append_message(streaming, {first});

  check(layout.document().messages.size() == 2, "append adds one message");
  check(layout.message_geometry().size() == 2, "append adds one message geometry");
  check(layout.content_height() > height_before, "append grows the content");
  check(layout.lines().front().key == untouched, "append does not re-shape the earlier transcript");
  check(backend.paragraph_count() == shaped_before + 1, "append shapes only the new message");

  const auto& appended = layout.document().blocks.back();
  check(appended.message == 1, "appended blocks carry the new message index");
  check(appended.first_in_message && appended.last_in_message, "single block spans the message");

  render::Block longer;
  longer.kind = render::BlockKind::Paragraph;
  longer.text = u"Hello there, this reply is longer";

  layout.replace_last_message_blocks({longer});

  check(layout.document().messages.size() == 2, "replace keeps the message count");
  check(layout.message_geometry().size() == 2, "replace keeps the geometry count");
  check(layout.document().blocks.size() == 4, "replace keeps the earlier blocks");
  check(
      layout.lines().front().key == untouched, "replace does not re-shape the earlier transcript");
  check(backend.paragraph_count() == shaped_before + 1, "replace releases the old tail paragraphs");
  check(
      layout.message_by_id(7).has_value() && *layout.message_by_id(7) == 0,
      "earlier message ids still resolve after a tail replace");

  // The tail grows on the second update; the last message geometry follows it.
  layout.replace_last_message_blocks({longer, longer});
  check(layout.document().blocks.size() == 5, "replace can add blocks");
  check(
      layout.message_geometry().back().rect.bottom() <= layout.content_height(),
      "the last message stays inside the content");
}

} // namespace

int main() {
  test_geometry_and_selection();
  test_message_index_by_id();
  test_incremental_tail();

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }

  std::printf("render layout tests passed\n");
  return 0;
}
