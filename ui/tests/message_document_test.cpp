#include <algorithm>
#include <cstdio>
#include <string_view>

#include "ui/MessageDocument.hpp"

int main() {
  constexpr std::string_view markdown =
      "| Route | What you hold | Main consideration |\n"
      "|---|---|---|\n"
      "| Mainland QDII mutual fund | Units of a fund investing overseas | Fees, purchase limits, and redemption timing |\n"
      "| Direct overseas brokerage account | Stocks or funds in your account | Account eligibility and a permissible funding route |";

  const std::vector<render::Block> blocks = ui::make_markdown_blocks(markdown);
  if (blocks.size() != 3) {
    std::fprintf(stderr, "FAIL: expected three table rows, got %zu blocks\n", blocks.size());
    return 1;
  }

  constexpr std::u16string_view header =
      u"Route  |  What you hold  |  Main consideration";
  constexpr std::u16string_view first_row =
      u"Mainland QDII mutual fund  |  Units of a fund investing overseas  |  Fees, purchase limits, and redemption timing";

  if (blocks[0].text != header || blocks[1].text != first_row ||
      blocks[2].text.find(u"Direct overseas brokerage account") == std::u16string::npos) {
    std::fprintf(stderr, "FAIL: table cells were not grouped into readable rows\n");
    return 1;
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
      return 1;
    }
  }

  return 0;
}
