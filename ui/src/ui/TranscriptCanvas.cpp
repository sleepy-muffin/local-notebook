#include "ui/TranscriptCanvas.hpp"

#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QColor>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QFont>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "settings/metrics.hpp"
#include "support/i18n.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/theme.hpp"

namespace ui {

namespace {

constexpr int kActionsHeight = 20;
constexpr int kBodySidePadding = 14;
constexpr int kConversationSidePadding = 28;
constexpr float kCodeRadius = 6.0f;

// Inline code sits on its own plate, padded away from the glyphs so the mono
// run does not touch the rounded edge.
constexpr float kInlineCodeRadius = 4.0f;
constexpr float kInlineCodePadding = 3.0f;

// How close to the bottom (px) still counts as "parked at the bottom".
constexpr float kStickSlack = 4.0f;

// Dragging a selection this close to an edge scrolls the transcript.
constexpr int kAutoScrollMargin = 12;
constexpr int kAutoScrollStep = 16;

QString qstring(std::u16string_view text) {
  if (text.empty()) {
    return {};
  }

  return QString::fromUtf16(
      reinterpret_cast<const char16_t*>(text.data()), static_cast<qsizetype>(text.size()));
}

render::Color to_color(const QColor& color) {
  return render::Color{
      static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
      static_cast<std::uint8_t>(color.blue()), static_cast<std::uint8_t>(color.alpha())};
}

QColor to_qcolor(const render::Color& color) { return QColor(color.r, color.g, color.b, color.a); }

QRectF to_rect(const render::Rect& rect) { return QRectF(rect.x, rect.y, rect.width, rect.height); }

render::Font to_font(const QFont& font) {
  render::Font result;
  result.family = font.family().toStdString();
  result.size = font.pointSizeF() > 0.0 ? static_cast<float>(font.pointSizeF())
                                        : static_cast<float>(font.pixelSize());
  result.weight = static_cast<int>(font.weight());
  result.italic = font.italic();
  return result;
}

QFont to_qfont(const render::Font& font) {
  // A default-constructed QFont already carries the application's family,
  // style hint and hinting preferences, so only the renderer's overrides are
  // applied here.
  QFont result;

  if (!font.family.empty()) {
    result.setFamily(QString::fromStdString(font.family));
  }

  if (font.size > 0.0f) {
    result.setPointSizeF(font.size);
  }

  if (font.weight > 0) {
    result.setWeight(static_cast<QFont::Weight>(font.weight));
  }

  result.setItalic(font.italic);
  return result;
}

QString css_color(const QColor& color) {
  return QStringLiteral("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alpha());
}

// -----------------------------------------------------------------------------
// QtTextBackend
// -----------------------------------------------------------------------------

// Shapes paragraphs with QTextLayout and keeps the result for hit testing and
// painting. The renderer core only sees opaque ParagraphKeys, so it stays free
// of Qt types while the real font shaping, bidi handling and line breaking keep
// using Qt's engine.
class QtTextBackend final : public render::TextBackend {
public:
  render::ParagraphKey
  shape(const render::ParagraphSpec& spec, std::vector<render::LineInfo>& lines) override;

  void release(render::ParagraphKey key) override;

  std::uint32_t x_to_offset(render::ParagraphKey key, std::uint32_t line, float x) override;

  float offset_to_x(render::ParagraphKey key, std::uint32_t line, std::uint32_t offset) override;

  void paint(QPainter& painter, const render::ParagraphPaint& paint);

private:
  struct Paragraph {
    std::unique_ptr<QTextLayout> layout;
    // Each laid-out line's intrinsic height (without the reading-rhythm extra),
    // so the extra leading can be split above and below the text.
    std::vector<float> intrinsic;
    QColor color;
    // Inline code spans are painted as rounded, padded plates in paint().
    // QTextLayout's own background is a square, glyph-tight rectangle, so the
    // ranges are kept here instead of in the character formats.
    QColor code_background;
    bool has_code_background = false;
    std::vector<std::pair<int, int>> code_ranges;
  };

  std::unordered_map<std::uint32_t, std::unique_ptr<Paragraph>> paragraphs_;
  std::uint32_t next_key_ = 1;
};

render::ParagraphKey
QtTextBackend::shape(const render::ParagraphSpec& spec, std::vector<render::LineInfo>& lines) {
  lines.clear();

  auto paragraph = std::make_unique<Paragraph>();

  const QFont font = to_qfont(spec.base_font);
  const QString text = qstring(spec.text);

  paragraph->layout = std::make_unique<QTextLayout>(text, font);
  paragraph->color = to_qcolor(spec.base_color);
  paragraph->code_background = to_qcolor(spec.inline_code_background);
  paragraph->has_code_background = spec.has_inline_code_background;

  QTextOption option;
  option.setWrapMode(
      spec.wrap_width > 0.0f ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
  paragraph->layout->setTextOption(option);
  paragraph->layout->setCacheEnabled(true);

  if (!spec.spans.empty()) {
    QList<QTextLayout::FormatRange> formats;
    formats.reserve(static_cast<qsizetype>(spec.spans.size()));

    for (const render::Span& span: spec.spans) {
      if (span.length == 0) {
        continue;
      }

      QTextLayout::FormatRange range;
      range.start = static_cast<int>(span.begin);
      range.length = static_cast<int>(span.length);

      QTextCharFormat format;

      if (span.bold) {
        format.setFontWeight(QFont::Bold);
      }

      if (span.italic) {
        format.setFontItalic(true);
      }

      if (span.underline) {
        format.setFontUnderline(true);
      }

      if (span.strike) {
        format.setFontStrikeOut(true);
      }

      if (span.code) {
        const QFont mono = to_qfont(spec.code_font);
        format.setFontFamilies(mono.families());
        format.setFontFixedPitch(true);

        if (mono.pointSizeF() > 0.0) {
          format.setFontPointSize(mono.pointSizeF());
        }

        // Inline code gets its own plate; a fenced block already has one. The
        // plate is drawn in paint(), so it can be rounded and padded instead of
        // following the glyph box exactly.
        if (spec.has_inline_code_background) {
          paragraph->code_ranges.emplace_back(
              range.start, range.start + static_cast<int>(range.length));
        }
      }

      if (span.link) {
        format.setAnchor(true);
        format.setAnchorHref(qstring(span.href));
        format.setForeground(to_qcolor(spec.link_color));
        format.setFontUnderline(true);
      }

      range.format = format;
      formats.push_back(std::move(range));
    }

    paragraph->layout->setFormats(formats);
  }

  // Adjacent ranges (a code run split by an emphasis boundary, say) are one
  // visual plate; merging them also avoids a seam when they touch.
  if (paragraph->code_ranges.size() > 1) {
    std::sort(paragraph->code_ranges.begin(), paragraph->code_ranges.end());

    std::vector<std::pair<int, int>> merged;
    merged.reserve(paragraph->code_ranges.size());

    for (const auto& range: paragraph->code_ranges) {
      if (!merged.empty() && range.first <= merged.back().second) {
        merged.back().second = std::max(merged.back().second, range.second);
        continue;
      }

      merged.push_back(range);
    }

    paragraph->code_ranges = std::move(merged);
  }

  paragraph->layout->beginLayout();

  const QFontMetricsF metrics(font);
  const float minimum =
      static_cast<float>(metrics.lineSpacing()) * std::max(1.0f, spec.line_height_scale);

  while (true) {
    QTextLine line = paragraph->layout->createLine();

    if (!line.isValid()) {
      break;
    }

    if (spec.wrap_width > 0.0f) {
      line.setLineWidth(spec.wrap_width);
    }

    render::LineInfo info;
    info.begin = static_cast<std::uint32_t>(line.textStart());
    info.length = static_cast<std::uint32_t>(line.textLength());
    info.width = static_cast<float>(line.naturalTextWidth());
    info.ascent = static_cast<float>(line.ascent());
    info.height = std::max(static_cast<float>(line.height()), minimum);

    lines.push_back(info);
    paragraph->intrinsic.push_back(static_cast<float>(line.height()));
  }

  paragraph->layout->endLayout();

  // Every line is drawn on its own at a renderer-chosen origin, so the layout's
  // internal positions are all zero; QTextLine::draw adds its position to the
  // point it is given.
  for (int i = 0; i < paragraph->layout->lineCount(); ++i) {
    paragraph->layout->lineAt(i).setPosition(QPointF(0.0, 0.0));
  }

  // An empty paragraph still needs a caret line for hit testing; the layout
  // itself produced no line, and paint() simply draws nothing for it.
  if (lines.empty()) {
    render::LineInfo info;
    info.begin = 0;
    info.length = 0;
    info.width = 0.0f;
    info.ascent = static_cast<float>(metrics.ascent());
    info.height = minimum;
    lines.push_back(info);
    paragraph->intrinsic.push_back(static_cast<float>(metrics.height()));
  }

  const std::uint32_t value = next_key_++;
  paragraphs_.emplace(value, std::move(paragraph));

  return render::ParagraphKey{value};
}

void QtTextBackend::release(render::ParagraphKey key) { paragraphs_.erase(key.value); }

std::uint32_t QtTextBackend::x_to_offset(render::ParagraphKey key, std::uint32_t line, float x) {
  const auto it = paragraphs_.find(key.value);

  if (it == paragraphs_.end()) {
    return 0;
  }

  const Paragraph& paragraph = *it->second;

  if (line >= static_cast<std::uint32_t>(paragraph.layout->lineCount())) {
    return 0;
  }

  const QTextLine text_line = paragraph.layout->lineAt(static_cast<int>(line));
  const int cursor = text_line.xToCursor(x);

  return static_cast<std::uint32_t>(
      std::clamp(cursor, text_line.textStart(), text_line.textStart() + text_line.textLength()));
}

float QtTextBackend::offset_to_x(
    render::ParagraphKey key, std::uint32_t line, std::uint32_t offset) {
  const auto it = paragraphs_.find(key.value);

  if (it == paragraphs_.end()) {
    return 0.0f;
  }

  const Paragraph& paragraph = *it->second;

  if (line >= static_cast<std::uint32_t>(paragraph.layout->lineCount())) {
    return 0.0f;
  }

  const QTextLine text_line = paragraph.layout->lineAt(static_cast<int>(line));
  const int clamped = std::clamp(
      static_cast<int>(offset), text_line.textStart(),
      text_line.textStart() + text_line.textLength());

  return static_cast<float>(text_line.cursorToX(clamped));
}

void QtTextBackend::paint(QPainter& painter, const render::ParagraphPaint& paint) {
  const auto it = paragraphs_.find(paint.key.value);

  if (it == paragraphs_.end()) {
    return;
  }

  const Paragraph& paragraph = *it->second;

  if (paint.line >= static_cast<std::uint32_t>(paragraph.layout->lineCount())) {
    return;
  }

  QTextLine line = paragraph.layout->lineAt(static_cast<int>(paint.line));
  const float height = static_cast<float>(line.height());
  // The renderer's box is taller than the line when a reading rhythm is
  // configured; split the extra leading above and below instead of dumping it
  // below the glyphs.
  const float top = paint.origin.y + (paint.box_height - height) * 0.5f;

  const int text_start = line.textStart();
  const int text_end = text_start + line.textLength();

  if (paragraph.has_code_background && !paragraph.code_ranges.empty()) {
    // The plate hugs the glyphs rather than the reading-rhythm box, so a line
    // with extra leading does not turn inline code into a tall bar.
    const float intrinsic =
        paint.line < paragraph.intrinsic.size() ? paragraph.intrinsic[paint.line] : height;
    const float plate_height = std::min(intrinsic, height);
    const float plate_top = paint.origin.y + (paint.box_height - plate_height) * 0.5f;

    painter.setPen(Qt::NoPen);
    painter.setBrush(paragraph.code_background);

    // The padded plate must not run into neighbouring text. Padding is only
    // added where a space (or the line/paragraph edge) already gives it room;
    // against a glyph the plate ends exactly at the run so the two never
    // overlap.
    const QString paragraph_text = paragraph.layout->text();

    for (const auto& [range_begin, range_end]: paragraph.code_ranges) {
      const int begin = std::max(range_begin, text_start);
      const int end = std::min(range_end, text_end);

      if (begin >= end) {
        continue;
      }

      const bool pad_left = begin <= 0 || paragraph_text.at(begin - 1).isSpace();
      const bool pad_right =
          end >= paragraph_text.size() || paragraph_text.at(end).isSpace();

      const qreal x1 = line.cursorToX(begin);
      const qreal x2 = line.cursorToX(end);
      const qreal left = paint.origin.x + std::min(x1, x2) -
          (pad_left ? kInlineCodePadding : 0.0);
      const qreal right = paint.origin.x + std::max(x1, x2) +
          (pad_right ? kInlineCodePadding : 0.0);

      painter.drawRoundedRect(
          QRectF(left, plate_top, right - left, plate_height), kInlineCodeRadius,
          kInlineCodeRadius);
    }

    painter.setBrush(Qt::NoBrush);
  }

  if (paint.selection_end > paint.selection_begin) {
    const int begin = std::clamp(static_cast<int>(paint.selection_begin), text_start, text_end);
    const int end = std::clamp(static_cast<int>(paint.selection_end), text_start, text_end);

    if (begin < end) {
      const qreal x1 = line.cursorToX(begin);
      const qreal x2 = line.cursorToX(end);

      painter.fillRect(
          QRectF(paint.origin.x + std::min(x1, x2), top, std::abs(x2 - x1), height),
          to_qcolor(paint.selection_color));
    }
  }

  painter.setPen(paragraph.color);
  line.draw(&painter, QPointF(paint.origin.x, top));
}

// -----------------------------------------------------------------------------
// QtCanvasAdapter
// -----------------------------------------------------------------------------

class QtCanvasAdapter final : public render::Canvas {
public:
  QtCanvasAdapter(QPainter& painter, QtTextBackend& backend)
      : painter_(painter), backend_(backend) {}

  void fill_rect(const render::Rect& rect, render::Color color) override {
    painter_.fillRect(to_rect(rect), to_qcolor(color));
  }

  void fill_rounded_rect(const render::Rect& rect, render::Color color, float radius) override {
    painter_.setPen(Qt::NoPen);
    painter_.setBrush(to_qcolor(color));
    painter_.drawRoundedRect(to_rect(rect), radius, radius);
  }

  void stroke_rounded_rect(
      const render::Rect& rect, render::Color color, float radius, float width) override {
    QRectF outline = to_rect(rect);
    const qreal inset = width * 0.5;
    outline.adjust(inset, inset, -inset, -inset);

    painter_.setPen(QPen(to_qcolor(color), width));
    painter_.setBrush(Qt::NoBrush);
    painter_.drawRoundedRect(outline, radius, radius);
  }

  void
  stroke_line(render::Point from, render::Point to, render::Color color, float width) override {
    painter_.setPen(QPen(to_qcolor(color), width));
    painter_.setBrush(Qt::NoBrush);
    painter_.drawLine(QPointF(from.x, from.y), QPointF(to.x, to.y));
  }

  void draw_text(
      std::u16string_view text, const render::Rect& rect, const render::Font& font,
      render::Color color, render::TextAlign align) override {
    int flags = Qt::AlignVCenter;

    switch (align) {
      case render::TextAlign::Left:
        flags |= Qt::AlignLeft;
        break;
      case render::TextAlign::Center:
        flags |= Qt::AlignHCenter;
        break;
      case render::TextAlign::Right:
        flags |= Qt::AlignRight;
        break;
    }

    painter_.setFont(to_qfont(font));
    painter_.setPen(to_qcolor(color));
    painter_.drawText(to_rect(rect), flags, qstring(text));
  }

  void draw_paragraph(const render::ParagraphPaint& paint) override {
    backend_.paint(painter_, paint);
  }

private:
  QPainter& painter_;
  QtTextBackend& backend_;
};

// -----------------------------------------------------------------------------
// MessageActionsBar
// -----------------------------------------------------------------------------

// The hover strip of one message: "#id  time  tokens" plus the message actions.
// It is a single reusable widget (not one per message), moved over whichever
// message is hovered. Its slot is reserved by the renderer's layout, so showing
// it never reflows the transcript.
class MessageActionsBar final : public QWidget {
public:
  explicit MessageActionsBar(QWidget* parent = nullptr);

  void apply_style();
  void set_message(const render::MessageMeta& meta);

  void set_on_copy(std::function<void(std::uint32_t)> handler) { on_copy_ = std::move(handler); }

  void set_on_edit(std::function<void(std::uint32_t)> handler) { on_edit_ = std::move(handler); }

  void set_on_delete(std::function<void(std::uint32_t)> handler) {
    on_delete_ = std::move(handler);
  }

private:
  QToolButton*
  make_button(const QString& icon, QStyle::StandardPixmap fallback, const QString& tip);
  void apply_icon(QToolButton* button, const QString& icon, QStyle::StandardPixmap fallback);

  QLabel* id_ = nullptr;
  QLabel* time_ = nullptr;
  QLabel* tokens_ = nullptr;
  QWidget* row_ = nullptr;
  QBoxLayout* outer_ = nullptr;
  QToolButton* copy_ = nullptr;
  QToolButton* edit_ = nullptr;
  QToolButton* remove_ = nullptr;

  std::uint32_t message_id_ = 0;
  render::Role role_ = render::Role::Assistant;
  bool editable_ = false;
  bool built_ = false;

  std::function<void(std::uint32_t)> on_copy_;
  std::function<void(std::uint32_t)> on_edit_;
  std::function<void(std::uint32_t)> on_delete_;
};

MessageActionsBar::MessageActionsBar(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("MessageActionsBar"));
  setAutoFillBackground(false);
  setAttribute(Qt::WA_StyledBackground, false);
  setFixedHeight(kActionsHeight);

  outer_ = new QHBoxLayout(this);
  outer_->setContentsMargins(kBodySidePadding, 0, kBodySidePadding, 0);
  outer_->setSpacing(0);

  row_ = new QWidget(this);
  row_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  auto* row = new QHBoxLayout(row_);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(4);

  id_ = new QLabel(row_);
  time_ = new QLabel(row_);
  tokens_ = new QLabel(row_);

  copy_ = make_button(QStringLiteral("copy"), QStyle::SP_DialogSaveButton, tr("Copy message"));
  edit_ = make_button(QStringLiteral("edit"), QStyle::SP_FileIcon, tr("Edit message"));
  remove_ = make_button(QStringLiteral("delete"), QStyle::SP_TrashIcon, tr("Delete message"));

  row->addWidget(id_);
  row->addWidget(time_);
  row->addWidget(tokens_);
  row->addWidget(copy_);
  row->addWidget(edit_);
  row->addWidget(remove_);

  // A stretch on either side lets the same widget mirror the old behaviour:
  // the reader's own messages put the strip on the right, replies on the left.
  outer_->addStretch(1);
  outer_->addWidget(row_);
  outer_->addStretch(0);

  connect(copy_, &QToolButton::clicked, this, [this]() {
    if (on_copy_) {
      on_copy_(message_id_);
    }
  });

  connect(edit_, &QToolButton::clicked, this, [this]() {
    if (on_edit_) {
      on_edit_(message_id_);
    }
  });

  connect(remove_, &QToolButton::clicked, this, [this]() {
    if (on_delete_) {
      on_delete_(message_id_);
    }
  });

  apply_style();
}

QToolButton* MessageActionsBar::make_button(
    const QString& icon, QStyle::StandardPixmap fallback, const QString& tip) {
  auto* button = new QToolButton(row_);
  button->setToolButtonStyle(Qt::ToolButtonIconOnly);
  button->setIconSize(QSize(15, 15));
  button->setFixedSize(kActionsHeight, kActionsHeight);
  button->setAutoRaise(true);
  button->setCursor(Qt::PointingHandCursor);
  button->setFocusPolicy(Qt::NoFocus);
  button->setToolTip(tip);

  apply_icon(button, icon, fallback);
  return button;
}

void MessageActionsBar::apply_icon(
    QToolButton* button, const QString& icon, QStyle::StandardPixmap fallback) {
  const QColor color = theme().colors.text_muted;

  QIcon artwork = resource_icon(icon, color, color);

  if (artwork.isNull()) {
    artwork = style()->standardIcon(fallback);
  }

  button->setIcon(artwork);
}

void MessageActionsBar::apply_style() {
  const ThemeColors& colors = theme().colors;
  const QString muted = css_color(colors.text_muted);

  for (QLabel* label: {id_, time_, tokens_}) {
    label->setStyleSheet(
        QStringLiteral("QLabel { background: transparent; color: %1; }").arg(muted));
  }

  setStyleSheet(QStringLiteral("QWidget { background: transparent; }"
                               "QToolButton { background: transparent; border: none;"
                               " border-radius: 4px; padding: 0; }"
                               "QToolButton:hover { background: %1; }")
                    .arg(css_color(row_hover_background())));

  apply_icon(copy_, QStringLiteral("copy"), QStyle::SP_DialogSaveButton);
  apply_icon(edit_, QStringLiteral("edit"), QStyle::SP_FileIcon);
  apply_icon(remove_, QStringLiteral("delete"), QStyle::SP_TrashIcon);
}

void MessageActionsBar::set_message(const render::MessageMeta& meta) {
  if (built_ && meta.id == message_id_ && meta.role == role_ && meta.editable == editable_) {
    return;
  }

  built_ = true;
  message_id_ = meta.id;
  role_ = meta.role;
  editable_ = meta.editable;

  id_->setText(QStringLiteral("#%1").arg(meta.id));
  id_->setVisible(true);

  time_->setText(qstring(meta.time));
  time_->setVisible(!time_->text().isEmpty());

  tokens_->setText(qstring(meta.tokens));
  tokens_->setVisible(!tokens_->text().isEmpty());

  copy_->setVisible(meta.role == render::Role::Assistant);
  edit_->setVisible(meta.editable);
  remove_->setVisible(true);

  const bool right_aligned = meta.role == render::Role::User;
  outer_->setStretch(0, right_aligned ? 1 : 0);
  outer_->setStretch(2, right_aligned ? 0 : 1);
  outer_->invalidate();

  row_->updateGeometry();
  updateGeometry();
}

// -----------------------------------------------------------------------------
// TranscriptCanvas::Private
// -----------------------------------------------------------------------------

} // namespace

struct TranscriptCanvas::Private {
  explicit Private(TranscriptCanvas* canvas) : q(canvas) { layout.set_backend(&backend); }

  TranscriptCanvas* q;

  // Declared before `layout`: the layout's destructor releases its shaped
  // paragraphs through this backend's pointer.
  QtTextBackend backend;
  render::Layout layout;

  MessageActionsBar* actions = nullptr;

  render::Selection selection;
  std::optional<std::size_t> hovered;

  QPointF drag_point;
  bool dragging = false;
  QTimer* drag_timer = nullptr;

  bool loading_more = false;
  bool load_check_pending = false;
  bool relayouting = false;
  bool resizing = false;

  std::function<void()> on_reach_top;

  [[nodiscard]] float origin() const {
    return std::max(0.0f, static_cast<float>(q->viewport()->height()) - layout.content_height());
  }

  [[nodiscard]] float scroll() const { return static_cast<float>(q->verticalScrollBar()->value()); }

  [[nodiscard]] float content_y(float viewport_y) const { return viewport_y - origin() + scroll(); }

  [[nodiscard]] render::LayoutOptions options() const {
    const lnb::ThemeMetrics& m = metrics();
    const float side = static_cast<float>(std::max(m.page_margin, kConversationSidePadding));

    render::LayoutOptions result;
    result.view_width = static_cast<float>(q->viewport()->width());
    result.left_inset = side;
    result.right_inset = side;
    result.content_max_width =
        message_max_width() > 0 ? static_cast<float>(message_max_width()) : 0.0f;
    result.top_padding = static_cast<float>(m.page_margin);
    result.bottom_padding = static_cast<float>(m.page_margin);
    return result;
  }

  [[nodiscard]] render::LayoutTheme build_theme() const {
    const ThemeColors& c = theme().colors;
    const lnb::ThemeMetrics& m = metrics();

    render::LayoutTheme result;
    result.body = to_font(chat_font());
    result.mono = to_font(mono_font());
    result.inline_code_scale = inline_code_scale();

    result.text = to_color(c.text);
    result.muted_text = to_color(c.text_muted);
    result.accent = to_color(c.accent);
    result.link = to_color(c.accent);

    result.user_background = to_color(c.bubble_user);
    result.user_text = to_color(c.bubble_user_text);
    result.assistant_text = to_color(c.bubble_assistant_text);
    result.code_background = to_color(c.bubble_assistant_code);
    result.inline_code_background = to_color(c.bubble_assistant_code);
    result.notice_background = to_color(c.base);
    result.danger_background = to_color(c.danger);
    result.danger_text = to_color(c.danger_text);
    result.border = to_color(c.border);

    QColor selected = c.accent;
    selected.setAlpha(90);
    result.selection = to_color(selected);

    result.bubble_radius = static_cast<float>(m.bubble_radius);
    result.code_radius = kCodeRadius;
    result.body_side_padding = static_cast<float>(kBodySidePadding);
    result.bubble_padding_v = 8.0f;
    result.code_padding_h = 10.0f;
    result.code_padding_v = 8.0f;

    result.paragraph_spacing = static_cast<float>(m.paragraph_spacing);
    result.list_spacing = static_cast<float>(m.list_paragraph_spacing);
    result.message_spacing = static_cast<float>(m.message_spacing);
    result.actions_height = static_cast<float>(kActionsHeight);

    result.list_indent = 18.0f;
    result.quote_indent = 12.0f;

    result.line_height_other = static_cast<float>(m.line_height_other) / 100.0f;
    result.line_height_self = static_cast<float>(m.line_height_self) / 100.0f;

    return result;
  }

  void update_scrollbars() {
    auto* bar = q->verticalScrollBar();
    const int viewport_height = q->viewport()->height();
    const int content_height = static_cast<int>(std::ceil(layout.content_height()));

    bar->setRange(0, std::max(0, content_height - viewport_height));
    bar->setPageStep(viewport_height);
    bar->setSingleStep(32);
  }

  void restore_anchor(const TranscriptCanvas::ScrollAnchor& anchor) {
    auto* bar = q->verticalScrollBar();

    if (!anchor.valid) {
      bar->setValue(std::min(bar->value(), bar->maximum()));
      return;
    }

    const auto index = layout.message_by_id(anchor.message_id);

    if (!index.has_value() || *index >= layout.message_geometry().size()) {
      bar->setValue(std::min(bar->value(), bar->maximum()));
      return;
    }

    const float y = layout.message_geometry()[*index].rect.y;
    const int value = static_cast<int>(std::lround(y + origin() - anchor.viewport_offset));
    bar->setValue(std::clamp(value, 0, bar->maximum()));
  }

  void maybe_load_more() {
    if (loading_more || !on_reach_top) {
      return;
    }

    // A drag near the top must not page history in under the selection.
    if (dragging) {
      return;
    }

    // While the reader holds the scrollbar thumb, Qt keeps the value under the
    // pointer: a page loaded at value 0 would be scrolled straight back to 0 on
    // the next mouse move and immediately page again, so the whole history
    // would stream in. Waiting for the release means the thumb visibly moves
    // down once the page is inserted (the release handler below asks for this
    // check again).
    if (q->verticalScrollBar()->isSliderDown()) {
      return;
    }

    // Only the top of the window asks for history. When the transcript is
    // shorter than the viewport the scrollbar stays at 0, which is exactly the
    // signal that another page should fill it.
    if (q->verticalScrollBar()->value() > 0) {
      return;
    }

    loading_more = true;
    on_reach_top();
    loading_more = false;
  }

  void maybe_load_more_deferred() {
    if (load_check_pending) {
      return;
    }

    load_check_pending = true;

    QTimer::singleShot(0, q, [this]() {
      load_check_pending = false;

      if (!relayouting) {
        maybe_load_more();
      }
    });
  }

  // The viewport width changes when the vertical scrollbar appears or
  // disappears, even though the scroll area itself did not resize. Wrapping
  // depends on that width, so the document must be re-laid out.
  void on_viewport_resize() {
    const float width = static_cast<float>(q->viewport()->width());

    if (resizing || std::abs(layout.options().view_width - width) < 0.5f) {
      return;
    }

    resizing = true;

    const auto anchor = q->anchor();
    const bool follow = q->at_bottom();

    relayouting = true;
    layout.set_options(options());
    layout.relayout();
    update_scrollbars();
    relayouting = false;

    if (follow) {
      q->scroll_to_bottom();
    } else {
      restore_anchor(anchor);
    }

    resizing = false;

    position_actions();
    q->viewport()->update();
    maybe_load_more_deferred();

    // A scrollbar appearing during the relayout changes the viewport width. If
    // that resize was swallowed by the guard, re-check once the guard is down.
    QTimer::singleShot(0, q, [this]() { on_viewport_resize(); });
  }

  [[nodiscard]] std::optional<render::Position> hit(QPointF point) const {
    return layout.hit_test(
        render::Point{static_cast<float>(point.x()), content_y(static_cast<float>(point.y()))});
  }

  void update_selection_focus(QPointF point) {
    const auto position = hit(point);

    if (position.has_value()) {
      selection.focus = *position;
      q->viewport()->update();
    }
  }

  void update_hover(QPointF point) {
    const auto position = hit(point);
    const auto index = layout.message_at(content_y(static_cast<float>(point.y())));

    // A link reads as clickable; everywhere else the canvas is text.
    const bool over_link = position.has_value() && layout.link_at(*position).has_value();
    q->viewport()->setCursor(over_link ? Qt::PointingHandCursor : Qt::IBeamCursor);

    if (index != hovered) {
      hovered = index;
    }

    position_actions();
  }

  void position_actions() {
    if (actions == nullptr) {
      return;
    }

    if (!hovered.has_value() || *hovered >= layout.document().messages.size() ||
        *hovered >= layout.message_geometry().size()) {
      actions->hide();
      return;
    }

    const render::MessageMeta& meta = layout.document().messages[*hovered];

    // Streaming and notice entries have no stored identity, so they have no
    // actions either.
    if (meta.id == 0) {
      actions->hide();
      return;
    }

    const render::MessageGeom& geometry = layout.message_geometry()[*hovered];
    const float top = geometry.actions.y - scroll() + origin();
    const float viewport_height = static_cast<float>(q->viewport()->height());

    if (top + geometry.actions.height <= 0.0f || top >= viewport_height) {
      actions->hide();
      return;
    }

    actions->set_message(meta);
    actions->setGeometry(QRect(
        static_cast<int>(std::lround(geometry.actions.x)), static_cast<int>(std::lround(top)),
        static_cast<int>(std::lround(geometry.actions.width)), kActionsHeight));
    actions->show();
    actions->raise();
  }

  void pointer_left() {
    QTimer::singleShot(0, q, [this]() {
      const QPoint local = q->viewport()->mapFromGlobal(QCursor::pos());

      // The bar is a child of the viewport, so moving onto it still maps inside
      // the viewport rectangle: the strip stays while the pointer is on it.
      if (q->viewport()->rect().contains(local)) {
        return;
      }

      hovered.reset();
      actions->hide();
    });
  }

  void press(QPointF point) {
    // Starting a selection (or following a link) dismisses the hover strip; it
    // comes back on the next idle move.
    hovered.reset();
    actions->hide();

    q->setFocus(Qt::MouseFocusReason);

    const auto position = hit(point);
    const bool had_selection = !selection.empty();

    if (!position.has_value()) {
      if (had_selection) {
        selection = {};
        q->viewport()->update();
      }

      return;
    }

    if (const auto link = layout.link_at(*position); link.has_value()) {
      QDesktopServices::openUrl(QUrl(qstring(*link)));
      return;
    }

    selection.anchor = *position;
    selection.focus = *position;
    dragging = true;
    drag_point = point;
    q->viewport()->update();
  }

  void move(QPointF point) {
    if (dragging) {
      drag_point = point;
      update_selection_focus(point);

      const float height = static_cast<float>(q->viewport()->height());
      const bool near_edge =
          point.y() < kAutoScrollMargin || point.y() > height - kAutoScrollMargin;

      if (near_edge && !drag_timer->isActive()) {
        drag_timer->start(30);
      } else if (!near_edge) {
        drag_timer->stop();
      }

      return;
    }

    update_hover(point);
  }

  void release() {
    dragging = false;
    drag_timer->stop();
  }

  void double_click(QPointF point) {
    hovered.reset();
    actions->hide();

    const auto position = hit(point);

    if (!position.has_value()) {
      return;
    }

    const auto& blocks = layout.document().blocks;

    if (position->block >= blocks.size()) {
      return;
    }

    selection.anchor = render::Position{position->block, 0};
    selection.focus = render::Position{
        position->block, static_cast<std::uint32_t>(blocks[position->block].text.size())};
    dragging = false;
    q->viewport()->update();
  }

  void auto_scroll() {
    if (!dragging) {
      drag_timer->stop();
      return;
    }

    auto* bar = q->verticalScrollBar();
    const float height = static_cast<float>(q->viewport()->height());
    int delta = 0;

    if (drag_point.y() < kAutoScrollMargin) {
      delta = -kAutoScrollStep;
    } else if (drag_point.y() > height - kAutoScrollMargin) {
      delta = kAutoScrollStep;
    }

    if (delta == 0) {
      drag_timer->stop();
      return;
    }

    bar->setValue(bar->value() + delta);
    update_selection_focus(drag_point);
  }

  void copy_selection() {
    const std::u16string text = q->selected_text();

    if (!text.empty()) {
      QApplication::clipboard()->setText(qstring(text));
    }
  }

  void select_all() {
    const auto& blocks = layout.document().blocks;

    if (blocks.empty()) {
      return;
    }

    selection.anchor = render::Position{0, 0};
    selection.focus = render::Position{
        static_cast<std::uint32_t>(blocks.size() - 1),
        static_cast<std::uint32_t>(blocks.back().text.size())};
    q->viewport()->update();
  }
};

// -----------------------------------------------------------------------------
// TranscriptCanvas
// -----------------------------------------------------------------------------

TranscriptCanvas::TranscriptCanvas(QWidget* parent)
    : QAbstractScrollArea(parent), d_(std::make_unique<Private>(this)) {
  setObjectName(QStringLiteral("TranscriptCanvas"));
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  // Paint only the handle: the transcript has no background of its own, and a
  // platform style such as Breeze fills the empty track with an opaque strip.
  install_scroll_bars(this, ScrollBarTrack::Conversation);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumWidth(0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

  viewport()->setAutoFillBackground(false);
  viewport()->setAttribute(Qt::WA_NoSystemBackground, true);
  viewport()->setMouseTracking(true);
  viewport()->setCursor(Qt::IBeamCursor);
  viewport()->installEventFilter(this);

  d_->actions = new MessageActionsBar(viewport());
  d_->actions->installEventFilter(this);
  d_->actions->hide();

  d_->actions->set_on_copy([this](std::uint32_t id) {
    if (on_copy_) {
      on_copy_(id);
    }
  });

  d_->actions->set_on_edit([this](std::uint32_t id) {
    if (on_edit_) {
      on_edit_(id);
    }
  });

  d_->actions->set_on_delete([this](std::uint32_t id) {
    if (on_delete_) {
      on_delete_(id);
    }
  });

  d_->drag_timer = new QTimer(this);
  d_->drag_timer->setInterval(30);

  connect(d_->drag_timer, &QTimer::timeout, this, [this]() { d_->auto_scroll(); });

  // A drag that ended at the top deferred its page load until the thumb was
  // released, so ask for the check now: if the view is still at the top, one
  // page is inserted and the anchored restore moves the thumb down.
  connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, [this]() {
    d_->maybe_load_more_deferred();
  });

  refresh_style();
}

TranscriptCanvas::~TranscriptCanvas() = default;

void TranscriptCanvas::refresh_style() {
  const ScrollAnchor anchor = this->anchor();
  const bool follow = at_bottom();

  d_->selection = {};
  d_->layout.set_theme(d_->build_theme());
  d_->layout.set_options(d_->options());
  d_->layout.relayout();
  d_->update_scrollbars();

  if (follow) {
    scroll_to_bottom();
  } else {
    d_->restore_anchor(anchor);
  }

  d_->actions->apply_style();
  d_->position_actions();
  viewport()->update();
}

void TranscriptCanvas::set_document(
    render::Document document, const ScrollAnchor& anchor, bool follow_bottom) {
  d_->hovered.reset();
  d_->actions->hide();
  d_->selection = {};
  d_->dragging = false;
  d_->drag_timer->stop();

  d_->layout.set_document(std::move(document));
  d_->update_scrollbars();

  if (follow_bottom) {
    scroll_to_bottom();
  } else {
    d_->restore_anchor(anchor);
  }

  viewport()->update();
  d_->maybe_load_more_deferred();
}

void TranscriptCanvas::clear() {
  d_->hovered.reset();
  d_->actions->hide();
  d_->selection = {};
  d_->layout.clear();
  d_->update_scrollbars();
  viewport()->update();
}

void TranscriptCanvas::begin_streaming() {
  const bool follow = at_bottom();

  render::MessageMeta meta;
  meta.role = render::Role::Assistant;
  meta.id = 0;
  meta.streaming = true;

  d_->layout.append_message(std::move(meta), {});
  d_->update_scrollbars();

  if (follow) {
    scroll_to_bottom();
  }

  viewport()->update();
}

void TranscriptCanvas::set_streaming_blocks(std::vector<render::Block> blocks) {
  if (d_->layout.document().messages.empty()) {
    return;
  }

  // Read the reader's position before the tail grows; while they are parked at
  // the bottom the growing reply follows along, and once they scroll away it
  // stops chasing them.
  const bool follow = at_bottom();

  d_->layout.replace_last_message_blocks(std::move(blocks));
  d_->update_scrollbars();

  if (follow) {
    scroll_to_bottom();
  }

  d_->position_actions();
  viewport()->update();
}

TranscriptCanvas::ScrollAnchor TranscriptCanvas::anchor() const {
  ScrollAnchor result;

  const auto& messages = d_->layout.document().messages;
  const auto& geometry = d_->layout.message_geometry();

  if (geometry.empty()) {
    return result;
  }

  const float top = d_->scroll() - d_->origin();

  for (std::size_t i = 0; i < geometry.size() && i < messages.size(); ++i) {
    if (geometry[i].rect.bottom() <= top) {
      continue;
    }

    // A notice or a streaming reply has no stable identity to come back to.
    if (messages[i].id == 0) {
      return result;
    }

    result.valid = true;
    result.message_id = messages[i].id;
    result.viewport_offset = geometry[i].rect.y - top;
    return result;
  }

  return result;
}

bool TranscriptCanvas::at_bottom() const {
  const QScrollBar* bar = verticalScrollBar();
  return static_cast<float>(bar->value()) >= static_cast<float>(bar->maximum()) - kStickSlack;
}

void TranscriptCanvas::scroll_to_bottom() {
  verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

std::u16string TranscriptCanvas::selected_text() const {
  return d_->layout.selected_text(d_->selection);
}

void TranscriptCanvas::set_on_reach_top(std::function<void()> handler) {
  d_->on_reach_top = std::move(handler);
}

void TranscriptCanvas::set_on_copy(std::function<void(std::uint32_t)> handler) {
  on_copy_ = std::move(handler);
}

void TranscriptCanvas::set_on_edit(std::function<void(std::uint32_t)> handler) {
  on_edit_ = std::move(handler);
}

void TranscriptCanvas::set_on_delete(std::function<void(std::uint32_t)> handler) {
  on_delete_ = std::move(handler);
}

void TranscriptCanvas::paintEvent(QPaintEvent* event) {
  QPainter painter(viewport());
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);

  const float origin = d_->origin();
  const float scroll = d_->scroll();
  painter.translate(0.0, origin - scroll);

  const QRect exposed = event->rect();
  const render::Rect clip{
      static_cast<float>(exposed.x()), static_cast<float>(exposed.y()) - origin + scroll,
      static_cast<float>(exposed.width()), static_cast<float>(exposed.height())};

  QtCanvasAdapter canvas(painter, d_->backend);
  d_->layout.paint(canvas, clip, d_->selection);
}

void TranscriptCanvas::resizeEvent(QResizeEvent* event) {
  QAbstractScrollArea::resizeEvent(event);

  d_->on_viewport_resize();
  d_->update_scrollbars();
  d_->position_actions();
  viewport()->update();
  d_->maybe_load_more_deferred();
}

void TranscriptCanvas::keyPressEvent(QKeyEvent* event) {
  if (event->matches(QKeySequence::Copy)) {
    d_->copy_selection();
    event->accept();
    return;
  }

  if (event->matches(QKeySequence::SelectAll)) {
    d_->select_all();
    event->accept();
    return;
  }

  if (event->key() == Qt::Key_Escape) {
    d_->selection = {};
    d_->dragging = false;
    d_->drag_timer->stop();
    viewport()->update();
    event->accept();
    return;
  }

  QAbstractScrollArea::keyPressEvent(event);
}

void TranscriptCanvas::scrollContentsBy(int dx, int dy) {
  Q_UNUSED(dx);
  Q_UNUSED(dy);

  // The widget paints the visible slice itself, so the default (blitting the
  // viewport's pixels) is not used; a full repaint of the viewport is cheap and
  // keeps fractional line positions exact.
  d_->position_actions();
  viewport()->update();
  d_->maybe_load_more_deferred();
}

bool TranscriptCanvas::eventFilter(QObject* watched, QEvent* event) {
  if (watched == viewport()) {
    switch (event->type()) {
      case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);

        if (mouse->button() == Qt::LeftButton) {
          d_->press(mouse->position());
          return true;
        }

        break;
      }

      case QEvent::MouseMove: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        d_->move(mouse->position());
        return false;
      }

      case QEvent::MouseButtonRelease: {
        auto* mouse = static_cast<QMouseEvent*>(event);

        if (mouse->button() == Qt::LeftButton) {
          d_->release();
          return true;
        }

        break;
      }

      case QEvent::MouseButtonDblClick: {
        auto* mouse = static_cast<QMouseEvent*>(event);

        if (mouse->button() == Qt::LeftButton) {
          d_->double_click(mouse->position());
          return true;
        }

        break;
      }

      case QEvent::Leave:
        d_->pointer_left();
        break;

      case QEvent::Resize:
        d_->on_viewport_resize();
        break;

      case QEvent::ContextMenu: {
        auto* context = static_cast<QContextMenuEvent*>(event);
        QMenu menu(viewport());

        QAction* copy = menu.addAction(trs("Copy"));
        copy->setEnabled(!d_->selection.empty());

        QAction* select_all = menu.addAction(trs("Select all"));
        QAction* chosen = menu.exec(context->globalPos());

        if (chosen == copy) {
          d_->copy_selection();
        } else if (chosen == select_all) {
          d_->select_all();
        }

        return true;
      }

      default:
        break;
    }
  }

  if (watched == d_->actions && event->type() == QEvent::Leave) {
    d_->pointer_left();
  }

  return QAbstractScrollArea::eventFilter(watched, event);
}

} // namespace ui
