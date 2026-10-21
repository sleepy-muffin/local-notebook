#include "ui/MessageDocument.hpp"

#include <QFont>
#include <QString>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>
#include <QTextListFormat>

#include <algorithm>
#include <utility>
#include <vector>

namespace ui {

namespace {

std::u16string to_u16(const QString& text) {
  return std::u16string(
      reinterpret_cast<const char16_t*>(text.utf16()), static_cast<std::size_t>(text.size()));
}

bool same_style(const render::Span& a, const render::Span& b) {
  return a.bold == b.bold && a.italic == b.italic && a.code == b.code &&
         a.underline == b.underline && a.strike == b.strike && a.link == b.link && a.href == b.href;
}

// QTextLayout applies overlapping formats last-wins, while the renderer stacks
// spans (bold inside a link, code inside emphasis). Splitting at every boundary
// and merging adjacent identical runs reproduces the visual result with a
// non-overlapping span list.
std::vector<render::Span>
normalize_spans(const std::vector<render::Span>& spans, std::uint32_t text_size) {
  std::vector<render::Span> clipped;

  for (const render::Span& span: spans) {
    const std::uint32_t begin = std::min(span.begin, text_size);
    const std::uint32_t end = std::min(span.begin + span.length, text_size);

    if (begin >= end) {
      continue;
    }

    render::Span value = span;
    value.begin = begin;
    value.length = end - begin;
    clipped.push_back(std::move(value));
  }

  if (clipped.empty()) {
    return {};
  }

  std::vector<std::uint32_t> bounds;
  bounds.reserve(clipped.size() * 2 + 2);
  bounds.push_back(0);
  bounds.push_back(text_size);

  for (const render::Span& span: clipped) {
    bounds.push_back(span.begin);
    bounds.push_back(span.begin + span.length);
  }

  std::sort(bounds.begin(), bounds.end());
  bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

  std::vector<render::Span> result;

  for (std::size_t i = 0; i + 1 < bounds.size(); ++i) {
    const std::uint32_t begin = bounds[i];
    const std::uint32_t end = bounds[i + 1];

    if (begin >= end) {
      continue;
    }

    render::Span run;
    run.begin = begin;
    run.length = end - begin;
    bool styled = false;

    for (const render::Span& span: clipped) {
      if (span.begin > begin || span.begin + span.length < end) {
        continue;
      }

      run.bold = run.bold || span.bold;
      run.italic = run.italic || span.italic;
      run.code = run.code || span.code;
      run.underline = run.underline || span.underline;
      run.strike = run.strike || span.strike;
      run.link = run.link || span.link;
      styled = styled || span.bold || span.italic || span.code || span.underline || span.strike ||
               span.link;

      if (span.link && run.href.empty()) {
        run.href = span.href;
      }
    }

    if (!styled) {
      continue;
    }

    if (!result.empty() && same_style(result.back(), run) &&
        result.back().begin + result.back().length == run.begin) {
      result.back().length += run.length;
      continue;
    }

    result.push_back(std::move(run));
  }

  return result;
}

bool is_ordered_style(QTextListFormat::Style style) {
  switch (style) {
    case QTextListFormat::ListDecimal:
    case QTextListFormat::ListLowerAlpha:
    case QTextListFormat::ListUpperAlpha:
    case QTextListFormat::ListLowerRoman:
    case QTextListFormat::ListUpperRoman:
      return true;
    default:
      return false;
  }
}

std::vector<render::Span> spans_from_block(const QTextBlock& block) {
  const QString text = block.text();
  const int base = block.position();

  std::vector<render::Span> spans;

  for (auto it = block.begin(); !it.atEnd(); ++it) {
    const QTextFragment fragment = it.fragment();

    if (!fragment.isValid() || fragment.length() <= 0) {
      continue;
    }

    const QTextCharFormat format = fragment.charFormat();

    render::Span span;
    span.begin = static_cast<std::uint32_t>(fragment.position() - base);
    span.length = static_cast<std::uint32_t>(fragment.length());
    span.bold = format.fontWeight() > static_cast<int>(QFont::Normal);
    span.italic = format.fontItalic();
    span.underline = format.fontUnderline();
    span.strike = format.fontStrikeOut();
    span.code =
        format.fontFixedPitch() ||
        format.font().family().compare(QStringLiteral("monospace"), Qt::CaseInsensitive) == 0;
    span.link = format.isAnchor() && !format.anchorHref().isEmpty();

    if (span.link) {
      span.href = to_u16(format.anchorHref());
    }

    spans.push_back(std::move(span));
  }

  return normalize_spans(spans, static_cast<std::uint32_t>(text.size()));
}

render::Block block_from_qtext(const QTextBlock& block) {
  render::Block out;

  const QTextBlockFormat format = block.blockFormat();
  const int quote_depth = format.intProperty(QTextFormat::BlockQuoteLevel);
  const int heading_level = format.headingLevel();

  out.quote_depth = std::max(0, quote_depth);

  if (format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
    out.kind = render::BlockKind::Rule;
  } else if (heading_level > 0) {
    out.kind = render::BlockKind::Heading;
    out.heading_level = heading_level;
  } else if (const QTextList* list = block.textList()) {
    out.kind = render::BlockKind::ListItem;
    // Qt counts list indentation from 1, the renderer from 0.
    out.list_depth = std::max(0, list->format().indent() - 1);
    out.ordered = is_ordered_style(list->format().style());

    if (out.ordered) {
      const int number = list->format().start() + list->itemNumber(block);
      out.list_number = static_cast<std::uint32_t>(std::max(0, number));
    }
  } else if (quote_depth > 0) {
    out.kind = render::BlockKind::Quote;
  }

  out.text = to_u16(block.text());
  out.spans = spans_from_block(block);
  return out;
}

} // namespace

render::MessageMeta make_message_meta(
    render::Role role, std::uint32_t id, std::u16string time, std::u16string tokens, bool editable,
    std::u16string raw) {
  render::MessageMeta meta;
  meta.role = role;
  meta.id = id;
  meta.time = std::move(time);
  meta.tokens = std::move(tokens);
  meta.editable = editable;
  meta.raw = std::move(raw);
  return meta;
}

std::vector<render::Block> make_plain_blocks(std::string_view utf8) {
  QString text = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
  // Stored text is normally LF-only, but a pasted CRLF must not draw as a stray
  // glyph. A block keeps its breaks either way; they are just normalised.
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

  render::Block block;
  block.kind = render::BlockKind::Paragraph;
  block.text = to_u16(text);

  return {std::move(block)};
}

std::vector<render::Block> make_markdown_blocks(std::string_view utf8) {
  const QString source = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));

  QTextDocument document;
  document.setMarkdown(source);

  std::vector<render::Block> blocks;
  QTextBlock block = document.begin();

  while (block.isValid()) {
    if (block.blockFormat().nonBreakableLines()) {
      // A fenced (or indented) code block is imported as one QTextBlock per
      // line. Folding them back together gives the renderer one block, so its
      // rounded background covers the whole listing and cross-block selection
      // sees normal text.
      render::Block code;
      code.kind = render::BlockKind::Code;

      const QString language = block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage);

      QString text;
      bool first = true;

      while (block.isValid() && block.blockFormat().nonBreakableLines()) {
        if (!first) {
          text += QLatin1Char('\n');
        }

        text += block.text();
        first = false;
        block = block.next();
      }

      while (text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);
      }

      code.code_language = to_u16(language);
      code.text = to_u16(text);
      blocks.push_back(std::move(code));
      continue;
    }

    blocks.push_back(block_from_qtext(block));
    block = block.next();
  }

  while (!blocks.empty() && blocks.back().kind == render::BlockKind::Paragraph &&
         blocks.back().text.empty()) {
    blocks.pop_back();
  }

  if (blocks.empty()) {
    blocks.push_back(render::Block{});
  }

  return blocks;
}

std::size_t append_message(
    render::Document& document, render::MessageMeta meta, std::vector<render::Block> blocks) {
  const std::size_t index = document.messages.size();
  document.messages.push_back(std::move(meta));

  if (blocks.empty()) {
    blocks.push_back(render::Block{});
  }

  for (std::size_t i = 0; i < blocks.size(); ++i) {
    blocks[i].message = static_cast<std::uint32_t>(index);
    blocks[i].first_in_message = i == 0;
    blocks[i].last_in_message = i + 1 == blocks.size();
  }

  document.blocks.insert(
      document.blocks.end(), std::make_move_iterator(blocks.begin()),
      std::make_move_iterator(blocks.end()));

  return index;
}

} // namespace ui
