#include <algorithm>
#include <cstdio>
#include <string_view>

#include "ui/MessageDocument.hpp"

namespace {

bool contains(const std::u16string& text, std::u16string_view needle) {
  return text.find(needle) != std::u16string::npos;
}

bool table_round_trip() {
  constexpr std::string_view markdown =
      "| Route | What you hold | Main consideration |\n"
      "|---|---|---|\n"
      "| Mainland QDII mutual fund | Units of a fund investing overseas | Fees, purchase limits, and redemption timing |\n"
      "| Direct overseas brokerage account | Stocks or funds in your account | Account eligibility and a permissible funding route |";

  const std::vector<render::Block> blocks = ui::make_markdown_blocks(markdown);
  if (blocks.size() != 3) {
    std::fprintf(stderr, "FAIL: expected three table rows, got %zu blocks\n", blocks.size());
    return false;
  }

  constexpr std::u16string_view header =
      u"Route  |  What you hold  |  Main consideration";
  constexpr std::u16string_view first_row =
      u"Mainland QDII mutual fund  |  Units of a fund investing overseas  |  Fees, purchase limits, and redemption timing";

  if (blocks[0].text != header || blocks[1].text != first_row ||
      blocks[2].text.find(u"Direct overseas brokerage account") == std::u16string::npos) {
    std::fprintf(stderr, "FAIL: table cells were not grouped into readable rows\n");
    return false;
  }

  constexpr std::u16string_view header_cells[] = {
      u"Route", u"What you hold", u"Main consideration"};
  for (const std::u16string_view cell : header_cells) {
    const std::size_t begin = blocks[0].text.find(cell);
    const std::size_t end = begin + cell.size();
    const bool bold = std::any_of(
        blocks[0].spans.begin(), blocks[0].spans.end(), [begin, end](const render::Span& span) {
          return span.bold && span.begin <= begin && span.begin + span.length >= end;
        });
    if (begin == std::u16string::npos || !bold) {
      std::fprintf(stderr, "FAIL: header cell bold formatting was not preserved\n");
      return false;
    }
  }

  return true;
}

// A raw <br> in a cell used to truncate the whole reply: Qt's HTML block
// importer drops everything after a tag it cannot close. It must instead become
// a line break, and the rows after it must survive.
bool hard_break_in_table_cell() {
  constexpr std::string_view markdown =
      "| a | b |\n"
      "|---|---|\n"
      "| x<br>y | z |\n"
      "| p | q |\n"
      "\n"
      "after\n";

  const std::vector<render::Block> blocks = ui::make_markdown_blocks(markdown);
  if (blocks.size() < 4) {
    std::fprintf(stderr, "FAIL: expected the rows after a <br> to survive, got %zu blocks\n", blocks.size());
    return false;
  }

  if (blocks[1].text != u"x\u2028y  |  z") {
    std::fprintf(stderr, "FAIL: <br> in a cell did not become a line separator\n");
    return false;
  }

  const bool second_row =
      std::any_of(blocks.begin(), blocks.end(), [](const render::Block& block) { return block.text == u"p  |  q"; });
  if (!second_row || blocks.back().text != u"after") {
    std::fprintf(stderr, "FAIL: text after a <br> was dropped\n");
    return false;
  }

  return true;
}

// Any other raw tag is shown literally rather than swallowing the rest of the
// document.
bool unclosed_html_is_literal() {
  constexpr std::string_view markdown = "first <div>second\n\nthird\n";

  const std::vector<render::Block> blocks = ui::make_markdown_blocks(markdown);
  if (blocks.size() != 2) {
    std::fprintf(stderr, "FAIL: expected two paragraphs, got %zu blocks\n", blocks.size());
    return false;
  }

  if (blocks[0].text != u"first <div>second" || blocks[1].text != u"third") {
    std::fprintf(stderr, "FAIL: unclosed raw HTML was not kept as text\n");
    return false;
  }

  return true;
}

// A <br> that is code is data, not markup, so it stays literal.
bool br_in_code_is_literal() {
  constexpr std::string_view inline_code = "see `<br>` here\n\nafter\n";
  constexpr std::string_view fenced_code = "```\nx<br>y\n```\n\nafter\n";

  const std::vector<render::Block> inline_blocks = ui::make_markdown_blocks(inline_code);
  if (inline_blocks.size() != 2 || !contains(inline_blocks[0].text, u"<br>") ||
      inline_blocks[1].text != u"after") {
    std::fprintf(stderr, "FAIL: <br> inside inline code was rewritten\n");
    return false;
  }

  const std::vector<render::Block> fenced_blocks = ui::make_markdown_blocks(fenced_code);
  if (fenced_blocks.size() != 2 || fenced_blocks[0].kind != render::BlockKind::Code ||
      fenced_blocks[0].text != u"x<br>y" || fenced_blocks[1].text != u"after") {
    std::fprintf(stderr, "FAIL: <br> inside a fenced code block was rewritten\n");
    return false;
  }

  return true;
}

} // namespace

int main() {
  bool ok = true;
  ok = table_round_trip() && ok;
  ok = hard_break_in_table_cell() && ok;
  ok = unclosed_html_is_literal() && ok;
  ok = br_in_code_is_literal() && ok;
  return ok ? 0 : 1;
}
