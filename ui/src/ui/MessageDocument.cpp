#include "ui/MessageDocument.hpp"

#include <QChar>
#include <QFont>
#include <QString>
#include <QStringView>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>
#include <QTextListFormat>
#include <QTextTable>

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

// Qt's Markdown importer hands raw HTML to an HTML block parser that, for a tag
// it cannot close, silently drops the rest of the document. Assistant replies
// routinely carry <br> in table cells, so one unclosed tag used to truncate the
// whole reply. HTML is disabled for the parse (MarkdownNoHTML) so no raw tag can
// ever swallow the text; the one tag with a useful meaning is turned into the
// line separator the layout engine already understands. Fenced and inline code
// are copied untouched, because a literal <br> in a listing must stay literal.
QString convert_hard_breaks_in_line(const QString& line) {
  QString out;
  out.reserve(line.size());

  qsizetype i = 0;
  while (i < line.size()) {
    const QChar ch = line[i];

    if (ch == u'`') {
      qsizetype run = 1;
      while (i + run < line.size() && line[i + run] == u'`') {
        ++run;
      }

      // A code span ends at a backtick run of the same length; without one the
      // backticks are ordinary text.
      qsizetype close = i + run;
      qsizetype end = -1;
      while (close < line.size()) {
        if (line[close] != u'`') {
          ++close;
          continue;
        }

        qsizetype closing_run = 1;
        while (close + closing_run < line.size() && line[close + closing_run] == u'`') {
          ++closing_run;
        }

        if (closing_run == run) {
          end = close + closing_run;
          break;
        }

        close += closing_run;
      }

      const qsizetype stop = end >= 0 ? end : i + run;
      out += line.mid(i, stop - i);
      i = stop;
      continue;
    }

    if (ch == u'<') {
      const QStringView rest = QStringView(line).mid(i);
      if (rest.size() >= 3 && rest[1].toLower() == u'b' && rest[2].toLower() == u'r') {
        qsizetype j = 3;
        while (j < rest.size() && (rest[j] == u' ' || rest[j] == u'\t')) {
          ++j;
        }
        if (j < rest.size() && rest[j] == u'/') {
          ++j;
        }
        if (j < rest.size() && rest[j] == u'>') {
          out += QChar(0x2028);
          i += j + 1;
          continue;
        }
      }
    }

    out += ch;
    ++i;
  }

  return out;
}

QString normalize_hard_breaks(const QString& source) {
  QString out;
  out.reserve(source.size());

  bool in_fence = false;
  QChar fence_char;
  qsizetype fence_length = 0;

  qsizetype start = 0;
  while (start <= source.size()) {
    qsizetype end = source.indexOf(u'\n', start);
    const bool has_newline = end >= 0;
    if (!has_newline) {
      end = source.size();
    }

    const QString line = source.mid(start, end - start);

    // A fence opener is up to three spaces of indentation followed by three or
    // more backticks or tildes; the closer must use the same character and be at
    // least as long.
    qsizetype probe = 0;
    int indent = 0;
    while (probe < line.size() && indent < 3 && line[probe] == u' ') {
      ++probe;
      ++indent;
    }

    QChar marker;
    qsizetype run = 0;
    if (probe < line.size() && (line[probe] == u'`' || line[probe] == u'~')) {
      marker = line[probe];
      while (probe + run < line.size() && line[probe + run] == marker) {
        ++run;
      }
    }

    if (!in_fence && run >= 3) {
      in_fence = true;
      fence_char = marker;
      fence_length = run;
      out += line;
    } else if (in_fence && run >= 3 && marker == fence_char && run >= fence_length) {
      in_fence = false;
      out += line;
    } else if (in_fence) {
      out += line;
    } else {
      out += convert_hard_breaks_in_line(line);
    }

    if (!has_newline) {
      break;
    }

    out += u'\n';
    start = end + 1;
  }

  return out;
}

render::Block table_row_from_qtext(const QTextTable& table, int row) {
  render::Block out;
  out.kind = render::BlockKind::Paragraph;
  out.table_columns = table.columns();

  for (int column = 0; column < table.columns(); ++column) {
    if (column > 0) {
      constexpr std::u16string_view separator = u"  |  ";
      out.text.append(separator);
    }

    const std::uint32_t cell_begin = static_cast<std::uint32_t>(out.text.size());
    const QTextTableCell cell = table.cellAt(row, column);
    const QTextBlock last = cell.lastCursorPosition().block();

    bool first_block = true;
    for (QTextBlock block = cell.firstCursorPosition().block(); block.isValid(); block = block.next()) {
      if (!first_block) {
        out.text.push_back(u' ');
      }
      first_block = false;

      const std::uint32_t offset = static_cast<std::uint32_t>(out.text.size());
      out.text += to_u16(block.text());

      for (render::Span span : spans_from_block(block)) {
        span.begin += offset;
        out.spans.push_back(std::move(span));
      }

      if (block == last) {
        break;
      }
    }

    out.table_cells.push_back(render::TableCellRange{
        cell_begin, static_cast<std::uint32_t>(out.text.size()) - cell_begin});
  }

  return out;
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
  // Raw HTML is parsed literally (MarkdownNoHTML): the HTML block importer
  // truncates the document at any tag it cannot close, and replies contain such
  // tags. See normalize_hard_breaks for the <br> rewrite.
  QTextDocument::MarkdownFeatures features = QTextDocument::MarkdownDialectGitHub;
  features |= QTextDocument::MarkdownNoHTML;
  document.setMarkdown(normalize_hard_breaks(source), features);

  std::vector<render::Block> blocks;
  QTextBlock block = document.begin();

  while (block.isValid()) {
    if (block.text().isEmpty() && block.next().isValid()) {
      QTextCursor next_cursor(block.next());
      if (next_cursor.currentTable()) {
        block = block.next();
        continue;
      }
    }

    QTextCursor cursor(block);

    if (QTextTable* table = cursor.currentTable()) {
      const QTextTableCell cell = table->cellAt(cursor);
      const int row = cell.row();
      blocks.push_back(table_row_from_qtext(*table, row));
      block = table->cellAt(row, table->columns() - 1).lastCursorPosition().block().next();
      continue;
    }

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
