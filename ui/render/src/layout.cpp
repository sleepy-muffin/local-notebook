#include "render/layout.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace render {

namespace {

constexpr char16_t kBullet = u'\u2022';

bool is_line_break(char16_t ch) { return ch == u'\n' || ch == u'\u2028'; }

std::u16string number_text(std::uint32_t value) {
  const std::string text = std::to_string(value);
  return std::u16string(text.begin(), text.end());
}

} // namespace

Layout::Layout() = default;

Layout::~Layout() { release_paragraphs(); }

void Layout::set_backend(TextBackend* backend) {
  if (backend_ == backend) {
    return;
  }

  release_paragraphs();
  backend_ = backend;
}

void Layout::set_theme(LayoutTheme theme) { theme_ = std::move(theme); }

void Layout::set_options(LayoutOptions options) { options_ = options; }

void Layout::set_document(Document document) {
  clear();
  document_ = std::move(document);

  if (backend_ != nullptr) {
    relayout();
  }
}

void Layout::clear() {
  release_paragraphs();
  lines_.clear();
  blocks_.clear();
  messages_.clear();
  document_ = Document{};
  content_height_ = options_.top_padding + options_.bottom_padding;
  content_left_ = options_.left_inset;
  content_width_ = 0.0f;
}

void Layout::release_paragraphs() {
  if (backend_ != nullptr) {
    for (const ParagraphSlot& slot: paragraphs_) {
      backend_->release(slot.key);
    }
  }

  paragraphs_.clear();
}

void Layout::relayout() {
  release_paragraphs();
  lines_.clear();
  blocks_.clear();
  messages_.clear();
  content_height_ = 0.0f;

  const float view_width = std::max(1.0f, options_.view_width);
  float left = options_.left_inset;
  float right = std::max(left + 1.0f, view_width - options_.right_inset);
  float column = right - left;

  if (options_.content_max_width > 0.0f && column > options_.content_max_width) {
    const float extra = (column - options_.content_max_width) * 0.5f;
    left += extra;
    column = options_.content_max_width;
  }

  content_left_ = left;
  content_width_ = column;

  if (backend_ == nullptr || document_.messages.empty()) {
    content_height_ = options_.top_padding + options_.bottom_padding;
    return;
  }

  float y = options_.top_padding;
  std::size_t block_index = 0;

  for (std::size_t m = 0; m < document_.messages.size(); ++m) {
    if (m > 0) {
      y += theme_.message_spacing;
    }

    y = layout_message(m, block_index, y);
    block_index += messages_.back().block_count;
  }

  content_height_ = y + options_.bottom_padding;
}

float Layout::layout_message(std::size_t message_index, std::size_t first_block, float y) {
  const Role role = document_.messages[message_index].role;
  const bool wrapped = role != Role::Assistant;

  MessageGeom message;
  message.first_block = first_block;

  const float message_top = y;
  const float text_x = content_left_ + theme_.body_side_padding;
  const float text_width = std::max(10.0f, content_width_ - theme_.body_side_padding * 2.0f);

  if (wrapped) {
    y += theme_.bubble_padding_v;
  }

  std::size_t block_index = first_block;

  while (block_index < document_.blocks.size() &&
         document_.blocks[block_index].message == message_index) {
    layout_block(block_index, text_x, text_width, y);
    const BlockGeom& geometry = blocks_.back();
    y = geometry.rect.bottom() + geometry.gap_after;
    ++block_index;
  }

  message.block_count = block_index - first_block;

  if (wrapped) {
    y += theme_.bubble_padding_v;
  }

  message.content = Rect{content_left_, message_top, content_width_, y - message_top};
  message.bubble = wrapped ? message.content : Rect{};
  message.actions = Rect{content_left_, y, content_width_, theme_.actions_height};
  y += theme_.actions_height;
  message.rect = Rect{content_left_, message_top, content_width_, y - message_top};
  messages_.push_back(message);

  return y;
}

void Layout::append_message(MessageMeta meta, std::vector<Block> blocks) {
  const std::size_t message_index = document_.messages.size();
  document_.messages.push_back(std::move(meta));

  if (blocks.empty()) {
    blocks.push_back(Block{});
  }

  const std::size_t first_block = document_.blocks.size();

  for (std::size_t i = 0; i < blocks.size(); ++i) {
    blocks[i].message = static_cast<std::uint32_t>(message_index);
    blocks[i].first_in_message = i == 0;
    blocks[i].last_in_message = i + 1 == blocks.size();
  }

  document_.blocks.insert(
      document_.blocks.end(), std::make_move_iterator(blocks.begin()),
      std::make_move_iterator(blocks.end()));

  if (backend_ == nullptr) {
    return;
  }

  if (messages_.size() + 1 != document_.messages.size() || content_width_ <= 0.0f) {
    relayout();
    return;
  }

  float y = content_height_ - options_.bottom_padding;

  if (message_index > 0) {
    y += theme_.message_spacing;
  }

  y = layout_message(message_index, first_block, y);
  content_height_ = y + options_.bottom_padding;
}

void Layout::replace_last_message_blocks(std::vector<Block> blocks) {
  if (document_.messages.empty()) {
    return;
  }

  const std::size_t message_index = document_.messages.size() - 1;

  // Blocks are grouped by message, so the trailing run belongs to the last one.
  std::size_t first_block = document_.blocks.size();

  while (first_block > 0 &&
         document_.blocks[first_block - 1].message == static_cast<std::uint32_t>(message_index)) {
    --first_block;
  }

  // A document laid out with a backend always has one geometry entry per
  // message. Without it (a backend attached after set_document), fall back to a
  // full relayout below.
  const bool geometry_valid =
      messages_.size() == document_.messages.size() && content_width_ > 0.0f;
  const float top = geometry_valid ? messages_[message_index].rect.y : 0.0f;

  if (geometry_valid) {
    const std::size_t first_line =
        first_block < blocks_.size() ? blocks_[first_block].first_line : lines_.size();

    std::size_t first_slot = 0;

    while (first_slot < paragraphs_.size() && paragraphs_[first_slot].block < first_block) {
      ++first_slot;
    }

    if (backend_ != nullptr) {
      for (std::size_t i = first_slot; i < paragraphs_.size(); ++i) {
        backend_->release(paragraphs_[i].key);
      }
    }

    paragraphs_.erase(
        paragraphs_.begin() + static_cast<std::ptrdiff_t>(first_slot), paragraphs_.end());
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(first_line), lines_.end());
    blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(first_block), blocks_.end());
    messages_.resize(message_index);
  } else {
    release_paragraphs();
    lines_.clear();
    blocks_.clear();
    messages_.clear();
  }

  document_.blocks.erase(
      document_.blocks.begin() + static_cast<std::ptrdiff_t>(first_block), document_.blocks.end());

  if (blocks.empty()) {
    blocks.push_back(Block{});
  }

  for (std::size_t i = 0; i < blocks.size(); ++i) {
    blocks[i].message = static_cast<std::uint32_t>(message_index);
    blocks[i].first_in_message = i == 0;
    blocks[i].last_in_message = i + 1 == blocks.size();
  }

  document_.blocks.insert(
      document_.blocks.end(), std::make_move_iterator(blocks.begin()),
      std::make_move_iterator(blocks.end()));

  if (backend_ == nullptr) {
    return;
  }

  if (!geometry_valid) {
    relayout();
    return;
  }

  const float end = layout_message(message_index, first_block, top);
  content_height_ = end + options_.bottom_padding;
}

Font Layout::block_font(const Block& block) const {
  if (block.kind == BlockKind::Code) {
    return theme_.mono;
  }

  if (block.kind == BlockKind::Heading && block.heading_level > 0) {
    const int level = std::clamp(block.heading_level, 1, 6);
    Font font = theme_.body;
    font.size *= theme_.heading_scale[level - 1];
    font.weight = std::max(font.weight, 700);
    return font;
  }

  return theme_.body;
}

Color Layout::role_text_color(Role role) const {
  switch (role) {
    case Role::User:
      return theme_.user_text;
    case Role::Assistant:
      return theme_.assistant_text;
    case Role::Notice:
      return theme_.muted_text;
    case Role::Error:
      return theme_.danger_text;
  }

  return theme_.text;
}

float Layout::role_line_height(Role role) const {
  return role == Role::Assistant ? theme_.line_height_other : theme_.line_height_self;
}

std::vector<Span>
Layout::clipped_spans(const Block& block, std::uint32_t begin, std::uint32_t end) const {
  std::vector<Span> result;

  for (const Span& span: block.spans) {
    const std::uint32_t span_begin = span.begin;
    const std::uint32_t span_end = span.begin + span.length;
    const std::uint32_t clipped_begin = std::max(span_begin, begin);
    const std::uint32_t clipped_end = std::min(span_end, end);

    if (clipped_begin >= clipped_end) {
      continue;
    }

    Span clipped = span;
    clipped.begin = clipped_begin - begin;
    clipped.length = clipped_end - clipped_begin;
    result.push_back(std::move(clipped));
  }

  return result;
}

float Layout::shape_block(
    std::size_t block_index, const Block& block, Font base, Color color, float text_x,
    float text_width, float y, float line_height_scale) {
  float cursor = y;
  std::uint32_t logical_begin = 0;

  const std::uint32_t text_size = static_cast<std::uint32_t>(block.text.size());

  // A block's text can carry explicit breaks; each logical line wraps on its
  // own, and the final one is always laid out (an empty paragraph still needs a
  // caret line).
  for (std::uint32_t index = 0; index <= text_size; ++index) {
    if (index < text_size && !is_line_break(block.text[index])) {
      continue;
    }

    const std::uint32_t logical_end = index;
    const std::vector<Span> spans = clipped_spans(block, logical_begin, logical_end);

    ParagraphSpec spec;
    spec.text = std::u16string_view(block.text).substr(logical_begin, logical_end - logical_begin);
    spec.spans = spans;
    spec.base_font = base;
    // Inline code follows the block's own font size (so headings scale it too)
    // instead of the configured monospace size.
    spec.code_font = theme_.mono;
    spec.code_font.size = base.size * theme_.inline_code_scale;
    spec.base_color = color;
    spec.link_color = theme_.link;
    spec.code_color = color;
    spec.inline_code_background = theme_.inline_code_background;
    spec.has_inline_code_background = true;
    spec.wrap_width = text_width;
    spec.line_height_scale = line_height_scale;

    std::vector<LineInfo> infos;
    const ParagraphKey key = backend_->shape(spec, infos);

    ParagraphSlot slot;
    slot.block = static_cast<std::uint32_t>(block_index);
    slot.logical_begin = logical_begin;
    slot.key = key;
    slot.first_line = lines_.size();
    slot.line_count = infos.size();
    paragraphs_.push_back(slot);

    for (std::uint32_t line = 0; line < infos.size(); ++line) {
      const LineInfo& info = infos[line];
      LineBox box;
      box.block = static_cast<std::uint32_t>(block_index);
      box.key = key;
      box.line_in_paragraph = line;
      box.logical_begin = logical_begin;
      box.begin = info.begin;
      box.length = info.length;
      box.x = text_x;
      box.y = cursor;
      box.width = info.width;
      box.height = info.height;
      lines_.push_back(box);
      cursor += info.height;
    }

    logical_begin = index + 1;
  }

  return cursor;
}

void Layout::layout_block(std::size_t block_index, float x, float width, float y) {
  const Block& block = document_.blocks[block_index];
  const Role role = document_.messages[block.message].role;

  float indent = 0.0f;

  if (block.kind == BlockKind::ListItem) {
    indent += theme_.list_indent * static_cast<float>(std::max(0, block.list_depth));
  }

  if (block.quote_depth > 0) {
    indent += theme_.quote_indent * static_cast<float>(block.quote_depth);
  }

  const float block_x = x + indent;
  const float block_width = std::max(10.0f, width - indent);

  BlockGeom geometry;
  geometry.first_line = lines_.size();

  if (block.kind == BlockKind::Rule) {
    geometry.rect = Rect{block_x, y + 8.0f, block_width, 1.0f};
    geometry.marker = geometry.rect;
    geometry.gap_after = block.last_in_message ? 0.0f : 16.0f;
    blocks_.push_back(geometry);
    return;
  }

  const Font base = block_font(block);
  const Color color = role_text_color(role);
  const float scale = role_line_height(role);

  if (block.kind == BlockKind::Code) {
    const float text_x = block_x + theme_.code_padding_h;
    const float text_width = std::max(10.0f, block_width - theme_.code_padding_h * 2.0f);
    const float cursor = shape_block(
        block_index, block, base, color, text_x, text_width, y + theme_.code_padding_v, 1.0f);

    geometry.rect = Rect{block_x, y, block_width, cursor + theme_.code_padding_v - y};
    geometry.background = geometry.rect;
    geometry.text_x = text_x;
    // A code plate carries its own padding, so it needs an explicit gap before
    // the next block; the last block of a message still sits flush.
    geometry.gap_after = block.last_in_message ? 0.0f : theme_.paragraph_spacing;
    geometry.line_count = lines_.size() - geometry.first_line;
    blocks_.push_back(geometry);
    return;
  }

  float text_x = block_x;
  float text_width = block_width;

  if (block.kind == BlockKind::ListItem) {
    geometry.marker = Rect{block_x, y, theme_.list_indent, 0.0f};
    text_x = block_x + theme_.list_indent;
    text_width = std::max(10.0f, block_width - theme_.list_indent);
  } else if (block.kind == BlockKind::Quote) {
    geometry.marker = Rect{block_x, y, 2.0f, 0.0f};
    text_x = block_x + 10.0f;
    text_width = std::max(10.0f, block_width - 10.0f);
  }

  const float cursor = shape_block(block_index, block, base, color, text_x, text_width, y, scale);

  geometry.rect = Rect{block_x, y, block_width, cursor - y};
  geometry.text_x = text_x;
  geometry.marker.height =
      block.kind == BlockKind::ListItem || block.kind == BlockKind::Quote ? cursor - y : 0.0f;

  if (block.last_in_message) {
    geometry.gap_after = 0.0f;
  } else if (block.kind == BlockKind::ListItem) {
    geometry.gap_after = theme_.list_spacing;
  } else {
    geometry.gap_after = theme_.paragraph_spacing;
  }

  geometry.line_count = lines_.size() - geometry.first_line;
  blocks_.push_back(geometry);
}

std::optional<Position> Layout::hit_test(Point point) const {
  if (lines_.empty()) {
    return std::nullopt;
  }

  const LineBox* best = nullptr;
  float best_distance = std::numeric_limits<float>::max();

  for (const LineBox& line: lines_) {
    const float top = line.y;
    const float bottom = line.y + line.height;
    float distance = 0.0f;

    if (point.y < top) {
      distance = top - point.y;
    } else if (point.y > bottom) {
      distance = point.y - bottom;
    }

    if (distance < best_distance) {
      best_distance = distance;
      best = &line;
    }

    if (distance == 0.0f) {
      break;
    }
  }

  if (best == nullptr) {
    return std::nullopt;
  }

  const float local_x = std::clamp(point.x - best->x, 0.0f, best->width);
  std::uint32_t offset = backend_->x_to_offset(best->key, best->line_in_paragraph, local_x);
  offset = std::clamp(offset, best->begin, best->begin + best->length);

  return Position{best->block, best->logical_begin + offset};
}

std::optional<std::size_t> Layout::message_at(float content_y) const {
  if (messages_.empty() || content_y < messages_.front().rect.y) {
    return std::nullopt;
  }

  for (std::size_t index = 0; index < messages_.size(); ++index) {
    if (content_y < messages_[index].rect.bottom()) {
      return index;
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> Layout::message_by_id(std::uint32_t id) const {
  for (std::size_t index = 0; index < document_.messages.size(); ++index) {
    if (document_.messages[index].id == id) {
      return index;
    }
  }

  return std::nullopt;
}

std::optional<std::u16string> Layout::link_at(Position position) const {
  if (position.block >= document_.blocks.size()) {
    return std::nullopt;
  }

  const Block& block = document_.blocks[position.block];

  for (const Span& span: block.spans) {
    if (span.link && !span.href.empty() && position.offset >= span.begin &&
        position.offset < span.begin + span.length) {
      return span.href;
    }
  }

  return std::nullopt;
}

std::u16string Layout::selected_text(const Selection& selection) const {
  Position begin = selection.anchor;
  Position end = selection.focus;

  if (end < begin) {
    std::swap(begin, end);
  }

  if (begin == end || document_.blocks.empty()) {
    return {};
  }

  begin.block = std::min(begin.block, static_cast<std::uint32_t>(document_.blocks.size() - 1));
  end.block = std::min(end.block, static_cast<std::uint32_t>(document_.blocks.size() - 1));

  const Block& first = document_.blocks[begin.block];
  const Block& last = document_.blocks[end.block];

  begin.offset = std::min(begin.offset, static_cast<std::uint32_t>(first.text.size()));
  end.offset = std::min(end.offset, static_cast<std::uint32_t>(last.text.size()));

  if (begin.block == end.block) {
    return first.text.substr(begin.offset, end.offset - begin.offset);
  }

  std::u16string result = first.text.substr(begin.offset);

  for (std::uint32_t block = begin.block + 1; block < end.block; ++block) {
    result += u'\n';
    result += document_.blocks[block].text;
  }

  result += u'\n';
  result += last.text.substr(0, end.offset);
  return result;
}

void Layout::paint(Canvas& canvas, const Rect& clip, const Selection& selection) const {
  Position selection_begin = selection.anchor;
  Position selection_end = selection.focus;

  if (selection_end < selection_begin) {
    std::swap(selection_begin, selection_end);
  }

  const bool has_selection = !selection.empty();

  for (std::size_t m = 0; m < messages_.size(); ++m) {
    const MessageGeom& message = messages_[m];

    if (!message.rect.intersects(clip)) {
      continue;
    }

    const MessageMeta& meta = document_.messages[m];

    if (!message.bubble.empty()) {
      Color background = theme_.notice_background;

      switch (meta.role) {
        case Role::User:
          background = theme_.user_background;
          break;
        case Role::Error:
          background = theme_.danger_background;
          break;
        case Role::Notice:
          background = theme_.notice_background;
          break;
        case Role::Assistant:
          break;
      }

      canvas.fill_rounded_rect(message.bubble, background, theme_.bubble_radius);

      if (meta.role == Role::User || meta.role == Role::Notice) {
        canvas.stroke_rounded_rect(message.bubble, theme_.border, theme_.bubble_radius, 1.0f);
      }
    }

    const std::size_t block_end = message.first_block + message.block_count;

    for (std::size_t index = message.first_block; index < block_end; ++index) {
      const BlockGeom& geometry = blocks_[index];

      if (!geometry.rect.intersects(clip)) {
        continue;
      }

      const Block& block = document_.blocks[index];

      if (block.kind == BlockKind::Code && !geometry.background.empty()) {
        canvas.fill_rounded_rect(geometry.background, theme_.code_background, theme_.code_radius);
      }

      if (block.kind == BlockKind::Quote && geometry.marker.height > 0.0f) {
        canvas.stroke_line(
            Point{geometry.marker.x + 2.0f, geometry.marker.y + 2.0f},
            Point{geometry.marker.x + 2.0f, geometry.marker.bottom() - 2.0f}, theme_.border, 2.0f);
      }

      if (block.kind == BlockKind::Rule) {
        canvas.stroke_line(
            Point{geometry.rect.x, geometry.rect.y + 0.5f},
            Point{geometry.rect.right(), geometry.rect.y + 0.5f}, theme_.border, 1.0f);
      }

      if (block.kind == BlockKind::ListItem && geometry.line_count > 0) {
        const LineBox& first_line = lines_[geometry.first_line];
        Rect marker_rect = geometry.marker;
        marker_rect.y = first_line.y;
        marker_rect.height = first_line.height;

        if (block.ordered) {
          marker_rect.width = std::max(0.0f, marker_rect.width - 4.0f);
          canvas.draw_text(
              number_text(block.list_number), marker_rect, block_font(block),
              role_text_color(meta.role), TextAlign::Right);
        } else {
          canvas.draw_text(
              std::u16string_view(&kBullet, 1), marker_rect, block_font(block),
              role_text_color(meta.role), TextAlign::Center);
        }
      }

      for (std::size_t line_index = geometry.first_line;
           line_index < geometry.first_line + geometry.line_count; ++line_index) {
        const LineBox& line = lines_[line_index];

        if (line.y + line.height < clip.y || line.y > clip.bottom()) {
          continue;
        }

        std::uint32_t local_selection_begin = 0;
        std::uint32_t local_selection_end = 0;

        if (has_selection && index >= selection_begin.block && index <= selection_end.block) {
          std::uint32_t selected_begin = 0;
          std::uint32_t selected_end = static_cast<std::uint32_t>(block.text.size());

          if (index == selection_begin.block) {
            selected_begin = selection_begin.offset;
          }

          if (index == selection_end.block) {
            selected_end = selection_end.offset;
          }

          if (index == selection_begin.block && index == selection_end.block) {
            selected_begin = selection_begin.offset;
            selected_end = selection_end.offset;
          }

          const std::uint32_t line_begin = line.logical_begin + line.begin;
          const std::uint32_t line_end = line_begin + line.length;
          const std::uint32_t overlap_begin = std::max(selected_begin, line_begin);
          const std::uint32_t overlap_end = std::min(selected_end, line_end);

          if (overlap_begin < overlap_end) {
            local_selection_begin = overlap_begin - line.logical_begin;
            local_selection_end = overlap_end - line.logical_begin;
          }
        }

        ParagraphPaint paint;
        paint.key = line.key;
        paint.line = line.line_in_paragraph;
        paint.origin = Point{line.x, line.y};
        paint.box_height = line.height;
        paint.selection_begin = local_selection_begin;
        paint.selection_end = local_selection_end;
        paint.selection_color = theme_.selection;
        canvas.draw_paragraph(paint);
      }
    }
  }
}

} // namespace render
