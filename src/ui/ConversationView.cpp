#include "ui/ConversationView.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "app/application.hpp"
#include "ui/theme.hpp"

namespace ui {

namespace {

// Formats a QColor for use inside a stylesheet, alpha included.
QString css_color(const QColor& color) {
  return QStringLiteral("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alpha());
}

QString from_utf8(const std::string& value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// Fixed height of every action strip, so revealing the buttons never reflows the
// transcript.
constexpr int kActionsHeight = 20;

// Inner left/right inset shared by every message body, so the assistant's text
// lines up with the user's block text.
constexpr int kBodySidePadding = 14;

// Minimum left/right inset of the conversation flow. The page margin alone is
// too tight, so blocks never crowd the pane edges even when the flow width is
// unlimited or the pane is narrow.
constexpr int kConversationSidePadding = 28;

// Streaming Markdown is re-rendered at most this often (ms), so a long reply is
// not re-parsed on every poll tick.
constexpr int kStreamRenderMs = 50;

// How close (in pixels) the scrollbar must be to the maximum for the reader to
// count as "at the bottom". Absorbs sub-pixel layout rounding so the transcript
// keeps following content while parked at the bottom.
constexpr int kStickToBottomSlack = 4;

// The generic family Qt's Markdown importer assigns to code spans and blocks.
const QString kMonospaceFamily = QStringLiteral("monospace");

// Builds a selectable, wrapping label that grows in height when its text wraps.
QLabel* make_label(QWidget* parent, Qt::TextFormat format) {
  auto* label = new QLabel(parent);
  label->setWordWrap(true);
  label->setTextFormat(format);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
  label->setOpenExternalLinks(true);

  QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Minimum);
  policy.setHeightForWidth(true);
  label->setSizePolicy(policy);
  label->setMinimumWidth(0);

  return label;
}

// A proportional line height is drawn with the extra leading *below* the text,
// so a solid block (a code block) looks cramped against the paragraph that
// follows it. Padding the block by that same extra leading restores balance.
int code_block_bottom_gap() {
  const int line_spacing = QFontMetrics(chat_font()).lineSpacing();
  return line_spacing * std::max(0, line_height_other() - 100) / 100;
}

// The stylesheet of a fenced code block. It is rebuilt (rather than cached) so a
// theme change always re-derives the background from the current colours.
QString code_block_stylesheet() {
  return QStringLiteral("QFrame#CodeBlock { background: %1; border-radius: 6px; }")
      .arg(css_color(theme().colors.bubble_assistant_code));
}

// Renders Markdown into rich text for the assistant label. Code blocks get a
// darker background and every code span uses the configured monospace font.
QString render_markdown(const QString& markdown) {
  const QColor code_background = theme().colors.bubble_assistant_code;
  const QFont mono = mono_font();

  QTextDocument document;
  document.setDefaultFont(chat_font());
  document.setMarkdown(markdown);

  const QTextBlock last = document.lastBlock();

  for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
    // A fenced code block is imported as one block per line with unbreakable
    // lines; give each of them the code background.
    const bool code_block = block.blockFormat().nonBreakableLines();
    const bool last_block = block == last;
    // List items belong to a QTextList; they get their own, usually tighter,
    // paragraph gap so a list does not read as widely spaced as prose.
    const bool list_block = block.textList() != nullptr;

    QTextBlockFormat format = block.blockFormat();
    format.setLineHeight(line_height_other(), QTextBlockFormat::ProportionalHeight);
    // Space only *between* paragraphs: keeping the first and last edges flush
    // balances the gaps at the top and bottom of the message.
    format.setTopMargin(0);
    format.setBottomMargin(
        (code_block || last_block)
            ? 0
            : (list_block ? list_paragraph_spacing() : paragraph_spacing())
    );

    if (code_block) {
      format.setBackground(code_background);
    }

    QTextCursor block_cursor(block);
    block_cursor.setBlockFormat(format);

    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const QTextFragment fragment = it.fragment();

      if (!fragment.isValid() ||
          fragment.charFormat().font().family() != kMonospaceFamily) {
        continue;
      }

      QTextCharFormat char_format = fragment.charFormat();
      char_format.setFontFamilies(mono.families());
      char_format.setFontFixedPitch(true);

      if (mono.pointSize() > 0) {
        char_format.setFontPointSize(mono.pointSize());
      }

      // Inline code needs its own background; inside a code block the block
      // background already covers it.
      if (!code_block) {
        char_format.setBackground(code_background);
      }

      QTextCursor cursor(block);
      cursor.setPosition(fragment.position());
      cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
      cursor.setCharFormat(char_format);
    }
  }

  return document.toHtml();
}

// Renders plain text (user and notice messages) with the same reading rhythm as
// replies. Each source line stays its own block, so the line height applies but
// no paragraph gap is inserted between lines.
QString render_plain(const QString& text) {
  QTextDocument document;
  document.setDefaultFont(chat_font());

  QTextCursor cursor(&document);
  const QStringList lines = text.split(QLatin1Char('\n'));

  for (qsizetype i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      cursor.insertBlock();
    }
    cursor.insertText(lines.at(i));
  }

  for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
    QTextBlockFormat format = block.blockFormat();
    format.setLineHeight(line_height_self(), QTextBlockFormat::ProportionalHeight);
    QTextCursor block_cursor(block);
    block_cursor.setBlockFormat(format);
  }

  return document.toHtml();
}

} // namespace

// -----------------------------------------------------------------------------
// MessageBubble
// -----------------------------------------------------------------------------

MessageBubble::MessageBubble(Role role, QWidget* parent)
    : QFrame(parent),
      role_(role),
      markdown_(role == Role::Assistant) {
  // Give the bubble its own object name: apply_style() uses a type-selector
  // stylesheet, which would otherwise leak into descendant frames (the code
  // blocks) and clobber their styling.
  setObjectName(QStringLiteral("MessageBubble"));

  const bool wrapped = role_ != Role::Assistant;

  body_layout_ = new QVBoxLayout(this);
  body_layout_->setContentsMargins(
      wrapped ? kBodySidePadding : kBodySidePadding,
      wrapped ? 8 : 8,
      kBodySidePadding,
      // A proportional line height already leaves trailing space under the last
      // line, so the bottom inset stays small to keep the block balanced.
      wrapped ? 2 : 2
  );
  body_layout_->setSpacing(6);

  setMinimumWidth(0);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

  // An assistant reply is split into text and code blocks by set_markdown(), so
  // its body starts empty. Every other role is a single text label holding
  // rendered (escaped) plain text, so the reading rhythm applies to it too.
  if (!markdown_) {
    text_label_ = make_label(this, Qt::RichText);
    text_label_->setMinimumSize(0, 0);
    text_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    text_label_->setFont(chat_font());
    body_layout_->addWidget(text_label_);
  }

  apply_style();
}

void MessageBubble::apply_style() {
  const ThemeColors& colors = theme().colors;
  const int radius = metrics().bubble_radius;

  QColor background;
  QColor foreground;
  int effective_radius = radius;
  QString border_rule;

  switch (role_) {
    case Role::User:
      background = colors.bubble_user;
      foreground = colors.bubble_user_text;
      // The same hairline outline as the composer field, so the two read as the
      // same surface.
      border_rule = QStringLiteral("border:1px solid %1;").arg(css_color(colors.border));
      break;
    case Role::Assistant:
      background = QColor(Qt::transparent);
      foreground = colors.bubble_assistant_text;
      effective_radius = 0;
      break;
    case Role::Notice:
      background = colors.base;
      foreground = colors.text_muted;
      border_rule = QStringLiteral("border:1px solid %1;").arg(css_color(colors.border));
      break;
    case Role::Error:
      background = colors.danger;
      foreground = colors.danger_text;
      break;
  }

  // The selectors are scoped to this bubble so the styling does not leak into
  // descendant code blocks (which are QFrames with their own look).
  setStyleSheet(
      QString(
          "QFrame#MessageBubble{background-color:%1;%2border-radius:%3px;}"
          "QFrame#MessageBubble QLabel{color:%4;background:transparent;}"
      )
          .arg(css_color(background))
          .arg(border_rule)
          .arg(effective_radius)
          .arg(css_color(foreground))
  );
}

void MessageBubble::set_text(const QString& text) {
  // Skip redundant updates: the streaming bubble is refreshed on every poll
  // tick, but its text only changes when a new token arrives.
  if (source_ == text) {
    return;
  }

  if (markdown_) {
    set_markdown(text);
    return;
  }

  source_ = text;

  if (text_label_ != nullptr) {
    text_label_->setText(render_plain(text));
  }
}

void MessageBubble::refresh_style() {
  apply_style();

  // Re-render so the code background (embedded in the rendered HTML) follows the
  // new theme.
  const QString current = source_;
  source_.clear();
  set_text(current);

  // A fenced code block keeps its stylesheet outside the rendered HTML, so it
  // must be restyled explicitly; an in-place text update never rebuilds it.
  refresh_code_blocks();
}

void MessageBubble::refresh_code_blocks() {
  for (const Block& block : blocks_) {
    if (block.code && block.frame != nullptr) {
      block.frame->setStyleSheet(code_block_stylesheet());
    }
  }
}

// Splits raw Markdown into alternating prose and fenced-code segments. A fence
// is a line starting with ``` or ~~~; everything between two fences becomes a
// single code segment and the language tag of the opening fence is dropped.
std::vector<MessageBubble::Segment> MessageBubble::parse_segments(
    const QString& markdown
) const {
  std::vector<Segment> segments;

  QString text_buffer;
  QString code_buffer;
  bool in_code = false;

  // Flushes the accumulated prose, dropping it when it is only whitespace.
  const auto flush_text = [&segments, &text_buffer]() {
    const QString trimmed = text_buffer.trimmed();
    text_buffer.clear();

    if (!trimmed.isEmpty()) {
      segments.push_back(Segment{false, trimmed});
    }
  };

  const QStringList lines = markdown.split(QLatin1Char('\n'));

  for (const QString& line : lines) {
    const QString trimmed = line.trimmed();
    const bool fence = trimmed.startsWith(QStringLiteral("```")) ||
        trimmed.startsWith(QStringLiteral("~~~"));

    if (fence) {
      if (!in_code) {
        flush_text();
        in_code = true;
      } else {
        segments.push_back(Segment{true, code_buffer.trimmed()});
        code_buffer.clear();
        in_code = false;
      }
      continue;
    }

    if (in_code) {
      if (!code_buffer.isEmpty()) {
        code_buffer += QLatin1Char('\n');
      }
      code_buffer += line;
      continue;
    }

    if (!text_buffer.isEmpty()) {
      text_buffer += QLatin1Char('\n');
    }
    text_buffer += line;
  }

  if (in_code) {
    // Unterminated fence: still show the collected lines as code.
    segments.push_back(Segment{true, code_buffer.trimmed()});
  } else {
    flush_text();
  }

  return segments;
}

void MessageBubble::set_markdown(const QString& markdown) {
  if (source_ == markdown) {
    return;
  }
  source_ = markdown;

  const std::vector<Segment> segments = parse_segments(markdown);

  // While the segment shape is unchanged (the common streaming case) update the
  // existing labels in place so the widgets do not flicker. Rebuild only when a
  // fence opens or closes and the block layout actually changes.
  if (segments.size() == blocks_.size()) {
    bool same_shape = true;

    for (std::size_t i = 0; i < segments.size(); ++i) {
      if (segments[i].code != blocks_[i].code) {
        same_shape = false;
        break;
      }
    }

    if (same_shape) {
      for (std::size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].code) {
          blocks_[i].label->setText(segments[i].text);
        } else {
          blocks_[i].label->setText(render_markdown(segments[i].text));
        }
      }
      return;
    }
  }

  rebuild_blocks(segments);
}

void MessageBubble::rebuild_blocks(const std::vector<Segment>& segments) {
  for (const Block& block : blocks_) {
    body_layout_->removeWidget(block.container);
    block.container->deleteLater();
  }
  blocks_.clear();

  for (std::size_t i = 0; i < segments.size(); ++i) {
    const Segment& segment = segments[i];

    if (!segment.code) {
      QLabel* label = make_label(this, Qt::RichText);
      label->setMinimumSize(0, 0);
      label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
      label->setFont(chat_font());
      label->setText(render_markdown(segment.text));

      body_layout_->addWidget(label);
      blocks_.push_back(Block{false, label, label});
      continue;
    }

    // A fenced code block becomes a rounded frame wrapping a monospace label,
    // because rich text cannot round the corners of a block background.
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("CodeBlock"));
    frame->setAttribute(Qt::WA_StyledBackground, true);
    frame->setMinimumSize(0, 0);
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    frame->setStyleSheet(code_block_stylesheet());
    auto* frame_layout = new QVBoxLayout(frame);
    frame_layout->setContentsMargins(10, 8, 10, 8);
    QLabel* label = make_label(frame, Qt::PlainText);
    label->setMinimumSize(0, 0);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    label->setFont(mono_font());
    label->setText(segment.text);
    frame_layout->addWidget(label);

    // The frame lives inside a container so the extra gap below can be removed
    // and rebuilt together with the block. No gap is added after the last block,
    // where it would only pad the bottom of the message.
    const int bottom_gap = i + 1 < segments.size() ? code_block_bottom_gap() : 0;
    auto* container = new QWidget(this);
    container->setMinimumSize(0, 0);
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto* container_layout = new QVBoxLayout(container);
    container_layout->setContentsMargins(0, 0, 0, bottom_gap);
    container_layout->setSpacing(0);
    container_layout->addWidget(frame);

    body_layout_->addWidget(container);
    blocks_.push_back(Block{true, container, label, frame});
  }
}

// -----------------------------------------------------------------------------
// MessageRow
// -----------------------------------------------------------------------------

MessageRow::MessageRow(MessageBubble::Role role, QWidget* parent)
    : QWidget(parent),
      role_(role) {
  auto* row_layout = new QHBoxLayout(this);
  row_layout->setContentsMargins(0, 0, 0, 0);
  row_layout->setSpacing(0);

  auto* column = new QWidget(this);
  column->setMinimumWidth(0);
  column->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
  auto* column_layout = new QVBoxLayout(column);
  column_layout->setContentsMargins(0, 0, 0, 0);
  column_layout->setSpacing(2);

  bubble_ = new MessageBubble(role, column);
  column_layout->addWidget(bubble_);

  actions_ = new QWidget(column);
  actions_->setObjectName(QStringLiteral("MessageActions"));
  // A constant height keeps the transcript stable while the buttons appear.
  actions_->setFixedHeight(kActionsHeight);

  auto* action_layout = new QHBoxLayout(actions_);
  action_layout->setContentsMargins(kBodySidePadding, 0, kBodySidePadding, 0);
  action_layout->setSpacing(4);

  // A single stretch keeps the id + buttons group on the block's side: it leads
  // for user messages (group right-aligned) and trails for the others
  // (left-aligned). The id label is created here so add_action can insert the
  // buttons next to it in either arrangement.
  message_id_label_ = new QLabel(actions_);
  message_id_label_->setObjectName(QStringLiteral("MessageId"));
  message_id_label_->setStyleSheet(
      QStringLiteral("QLabel#MessageId { color: %1; }")
          .arg(css_color(theme().colors.text_muted))
  );
  message_id_label_->setVisible(false);

  if (role_ == MessageBubble::Role::User) {
    action_layout->addStretch(1);
    action_layout->addWidget(message_id_label_);
  } else {
    action_layout->addWidget(message_id_label_);
    action_layout->addStretch(1);
  }

  column_layout->addWidget(actions_);

  row_layout->addWidget(column, 1);
}

QToolButton* MessageRow::add_action(
    const QString& icon_name, QStyle::StandardPixmap fallback_icon, const QString& tip
) {
  auto* button = new QToolButton(actions_);
  button->setObjectName(QStringLiteral("MessageAction"));
  button->setToolButtonStyle(Qt::ToolButtonIconOnly);
  button->setIconSize(QSize(15, 15));
  button->setFixedSize(20, 20);
  button->setAutoRaise(true);
  button->setCursor(Qt::PointingHandCursor);
  button->setFocusPolicy(Qt::NoFocus);
  button->setToolTip(tip);

  action_icons_.push_back(ActionIcon{button, icon_name, fallback_icon});
  apply_action_icon(action_icons_.back());

  auto* layout = static_cast<QHBoxLayout*>(actions_->layout());

  if (role_ == MessageBubble::Role::User) {
    layout->addWidget(button);
  } else {
    // Insert before the trailing stretch so the buttons stay left-aligned.
    layout->insertWidget(layout->count() - 1, button);
  }

  button->setVisible(actions_visible_);

  return button;
}

void MessageRow::apply_action_icon(ActionIcon& entry) {
  const QColor colour = theme().colors.text_muted;

  // The bundled artwork is recoloured to the theme; a missing resource falls back
  // to a built-in standard icon.
  QIcon icon = resource_icon(entry.name, colour, colour);

  if (icon.isNull()) {
    icon = style()->standardIcon(entry.fallback);
  }

  entry.button->setIcon(icon);
}

void MessageRow::refresh_action_icons() {
  for (ActionIcon& entry : action_icons_) {
    apply_action_icon(entry);
  }
}

void MessageRow::set_actions_visible(bool visible) {
  if (actions_visible_ == visible) {
    return;
  }

  actions_visible_ = visible;

  for (QToolButton* button : actions_->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly)) {
    button->setVisible(visible);
  }

  if (message_id_label_ != nullptr) {
    message_id_label_->setVisible(visible);
  }
}

void MessageRow::set_message_id(std::uint32_t id) {
  message_id_ = id;

  if (message_id_label_ == nullptr) {
    return;
  }

  message_id_label_->setText(id == 0 ? QString{} : QStringLiteral("#%1").arg(id));
}

void MessageRow::enterEvent(QEnterEvent* event) {
  QWidget::enterEvent(event);
  set_actions_visible(true);
}

void MessageRow::leaveEvent(QEvent* event) {
  QWidget::leaveEvent(event);

  // The row also receives a Leave when the pointer moves onto one of its
  // children (the bubble or the action strip), so only hide the buttons once the
  // pointer has really left the entry.
  if (actions_visible_ && !rect().contains(mapFromGlobal(QCursor::pos()))) {
    set_actions_visible(false);
  }
}

// -----------------------------------------------------------------------------
// ConversationView
// -----------------------------------------------------------------------------

ConversationView::ConversationView(
    lnb::Application& app, std::uint32_t session_id, QWidget* parent
)
    : QWidget(parent),
      app_(app),
      session_id_(session_id),
      controller_(app, session_id) {
  const lnb::ThemeMetrics& m = metrics();

  // The pane paints its own background (the conversation surface), so the
  // stylesheet must be allowed to fill it.
  setObjectName(QStringLiteral("ConversationView"));
  setAttribute(Qt::WA_StyledBackground, true);

  setMinimumWidth(0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  scroll_ = new QScrollArea(this);
  scroll_->setObjectName(QStringLiteral("Transcript"));
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setMinimumWidth(0);
  scroll_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll_->setStyleSheet(
      QStringLiteral("QScrollArea#Transcript { background: transparent; border: none; }")
  );
  scroll_->viewport()->setAutoFillBackground(false);

  transcript_ = new QWidget(scroll_);
  transcript_->setObjectName(QStringLiteral("TranscriptInner"));
  transcript_->setMinimumWidth(0);
  transcript_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  transcript_layout_ = new QVBoxLayout(transcript_);
  transcript_layout_->setContentsMargins(
      m.page_margin, m.page_margin, m.page_margin, m.page_margin
  );
  transcript_->setMinimumSize(0, 0);
  transcript_layout_->setSpacing(m.message_spacing);

  // A leading stretch pins the transcript to the bottom of the viewport. With
  // the default top alignment the (viewport-filling) transcript widget leaves a
  // large empty gap below the last message whenever a conversation is short.
  transcript_layout_->addStretch(1);

  // Reveals older messages on demand. It is inserted directly after the leading
  // stretch, so it stays pinned above every rendered row; it is hidden while the
  // whole history is already on screen.
  load_older_ = new QPushButton(QStringLiteral("Load earlier messages"), transcript_);
  load_older_->setObjectName(QStringLiteral("Secondary"));
  load_older_->setVisible(false);
  transcript_layout_->insertWidget(1, load_older_, 0, Qt::AlignHCenter);

  connect(load_older_, &QPushButton::clicked, this, [this]() { load_older(); });

  // Paging in older history is deliberate: the "Load earlier messages" button is
  // the only way to reveal it, so reaching the top never triggers a load on its
  // own. This handler only tracks whether the reader is parked at the bottom:
  // auto-scroll follows new content while they are, and resumes once they scroll
  // back down.
  auto* scrollbar = scroll_->verticalScrollBar();

  connect(
      scrollbar, &QScrollBar::valueChanged, this,
      [this, scrollbar](int value) {
        stick_to_bottom_ = value >= scrollbar->maximum() - kStickToBottomSlack;
      }
  );

  // Following the range instead of a timer is what keeps the follow smooth: the
  // range only changes once the layout has settled, so the view tracks the true
  // bottom instead of jumping to a stale maximum while Markdown reflows.
  connect(
      scrollbar, &QScrollBar::rangeChanged, this,
      [this](int, int) {
        if (stick_to_bottom_ && !updating_) {
          pin_to_bottom();
        }
      }
  );

  scroll_->setWidget(transcript_);

  // QScrollArea::setWidget() force-enables the scrolled widget's auto-fill with
  // QPalette::Window, which would paint an opaque band over the pane's own
  // conversation surface. Undo it so the surface shows through -- most visibly on
  // a translucent theme, where the tinted background image must reach the
  // transcript.
  transcript_->setAutoFillBackground(false);

  // The transcript column is re-inset whenever the viewport is resized so it
  // stays centred once the pane is wider than the configured flow width.
  scroll_->viewport()->installEventFilter(this);
  update_content_margins();

  auto* composer = new QWidget(this);
  composer->setObjectName(QStringLiteral("ComposerBar"));
  composer->setAttribute(Qt::WA_StyledBackground, true);
  composer->setAutoFillBackground(false);
  composer_bar_ = composer;

  auto* composer_layout = new QHBoxLayout(composer);
  composer_layout->setContentsMargins(
      m.page_margin, m.page_margin / 2, m.page_margin, m.page_margin
  );
  composer_layout->setSpacing(8);

  input_ = new QPlainTextEdit(composer);
  input_->setObjectName(QStringLiteral("Composer"));
  input_->setPlaceholderText(
      app_.settings().enter_with_modifier
          ? QStringLiteral("Type a message...  (Ctrl+Enter to send, Enter for a new line)")
          : QStringLiteral("Type a message...  (Enter to send, Shift+Enter for a new line)")
  );
  input_->setTabChangesFocus(true);
  input_->setFont(chat_font());
  input_->installEventFilter(this);

  auto* buttons = new QVBoxLayout();
  buttons->setSpacing(4);

  // The expand toggle sits at the field's top-right; the send and cancel actions
  // at the block's bottom-right, the way Zed places its composer action. All three
  // are icon-only, flat, and share the send button's 28x28 footprint.
  expand_button_ = new QToolButton(composer);
  expand_button_->setObjectName(QStringLiteral("ComposerExpand"));
  expand_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  expand_button_->setIconSize(QSize(16, 16));
  expand_button_->setFixedSize(28, 28);
  expand_button_->setAutoRaise(true);
  expand_button_->setFocusPolicy(Qt::NoFocus);
  expand_button_->setCursor(Qt::PointingHandCursor);

  send_button_ = new QToolButton(composer);
  send_button_->setObjectName(QStringLiteral("SendButton"));
  send_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  send_button_->setIconSize(QSize(16, 16));
  send_button_->setFixedSize(28, 28);
  send_button_->setAutoRaise(true);
  send_button_->setFocusPolicy(Qt::NoFocus);
  send_button_->setCursor(Qt::PointingHandCursor);

  // Cancel replaces the send action while a turn runs, so it is icon-only too.
  cancel_button_ = new QToolButton(composer);
  cancel_button_->setObjectName(QStringLiteral("CancelButton"));
  cancel_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  cancel_button_->setIconSize(QSize(16, 16));
  cancel_button_->setFixedSize(28, 28);
  cancel_button_->setAutoRaise(true);
  cancel_button_->setFocusPolicy(Qt::NoFocus);
  cancel_button_->setCursor(Qt::PointingHandCursor);
  cancel_button_->setVisible(false);

  buttons->addWidget(expand_button_, 0, Qt::AlignTop | Qt::AlignRight);
  buttons->addStretch(1);
  buttons->addWidget(send_button_, 0, Qt::AlignBottom | Qt::AlignRight);
  buttons->addWidget(cancel_button_, 0, Qt::AlignBottom | Qt::AlignRight);

  composer_layout->addWidget(input_, 1);
  composer_layout->addLayout(buttons);

  layout->addWidget(scroll_, 1);
  layout->addWidget(composer);

  apply_composer_style();
  apply_composer_height();

  // Coalesces streaming updates so Markdown is re-rendered at a steady rate
  // instead of once per token.
  stream_timer_ = new QTimer(this);
  stream_timer_->setSingleShot(true);
  stream_timer_->setInterval(kStreamRenderMs);

  connect(stream_timer_, &QTimer::timeout, this, [this]() { flush_streaming_text(); });

  connect(expand_button_, &QToolButton::clicked, this, [this]() { toggle_composer_height(); });

  connect(send_button_, &QToolButton::clicked, this, [this]() { submit(); });

  connect(cancel_button_, &QToolButton::clicked, this, [this]() { controller_.cancel(); });

  // The controller reports the turn's progress; the view renders it.
  controller_.set_on_changed([this]() {
    reload();
    if (on_changed_) {
      on_changed_();
    }
  });

  controller_.set_on_stream([this](const std::string& text) {
    // Create the reply row on the first token so the reply has a place to land.
    if (streaming_row_ == nullptr) {
      streaming_row_ = add_row(MessageBubble::Role::Assistant, 0, false);
    }

    pending_stream_text_ = text.empty() ? streaming_status_ : from_utf8(text);
    has_pending_stream_ = true;

    if (!stream_timer_->isActive()) {
      stream_timer_->start();
    }
  });

  controller_.set_on_status([this](const std::string& status) {
    streaming_status_ = from_utf8(status);

    if (streaming_row_ == nullptr) {
      streaming_row_ = add_row(MessageBubble::Role::Assistant, 0, false);
    }

    if (pending_stream_text_.isEmpty() && streaming_row_->bubble()->text().isEmpty()) {
      pending_stream_text_ = streaming_status_;
      has_pending_stream_ = true;

      if (!stream_timer_->isActive()) {
        stream_timer_->start();
      }
    }
  });

  controller_.set_on_error([this](const std::string& text) {
    pending_error_ = from_utf8(text);
  });

  controller_.set_on_warning([this](const std::string& text) {
    pending_warning_ = from_utf8(text);
  });

  reload();
}

void ConversationView::reload() {
  updating_ = true;
  clear_transcript();

  messages_ = app_.messages(session_id_).all();

  // Render only the tail window. The rest stays in memory but out of the widget
  // tree, so the per-reload cost (widget creation plus Markdown rendering) is
  // bounded no matter how long the conversation grows.
  const std::size_t limit = static_cast<std::size_t>(
      std::max(1, app_.settings().rendered_message_limit)
  );
  rendered_count_ = std::min(limit, messages_.size());

  const std::size_t start = messages_.size() - rendered_count_;

  // Layout index 0 is the leading stretch and index 1 is the load-older button,
  // so the rendered rows start at index 2.
  for (std::size_t k = 0; k < rendered_count_; ++k) {
    render_message_at(start + k, 2 + static_cast<int>(k));
  }

  // Notices from the last finished turn are rendered after the transcript, then
  // cleared so a later reload does not show them again.
  if (!pending_error_.isEmpty()) {
    auto* row = add_row(MessageBubble::Role::Error, 0, false);
    row->bubble()->set_text(pending_error_);
    pending_error_.clear();
  }

  if (!pending_warning_.isEmpty()) {
    auto* row = add_row(MessageBubble::Role::Notice, 0, false);
    row->bubble()->set_text(pending_warning_);
    pending_warning_.clear();
  }

  update_load_older_visibility();
  scroll_to_bottom();
  set_busy(controller_.busy());
  updating_ = false;
}

void ConversationView::refresh_style() {
  apply_composer_style();
  content_side_margin_ = -1;
  update_content_margins();

  for (MessageRow* row : rows_) {
    if (row != nullptr) {
      row->bubble()->refresh_style();
      row->refresh_action_icons();
    }
  }
}

void ConversationView::render_message_at(std::size_t index, int layout_index) {
  const lnb::Message& message = messages_[index];
  const bool from_self = message.role_ == lnb::Role::User;

  auto* row = make_row(
      from_self ? MessageBubble::Role::User : MessageBubble::Role::Assistant, message.id_,
      from_self
  );

  row->bubble()->set_text(from_utf8(message.content_));

  transcript_layout_->insertWidget(layout_index, row);
}

void ConversationView::load_older() {
  if (rendered_count_ >= messages_.size()) {
    return;
  }

  updating_ = true;

  // Paging through history means the reader has left the live bottom, so stop
  // following new content. The deferred scroll restore recomputes the flag from
  // the final position, so landing back at the bottom re-arms it.
  stick_to_bottom_ = false;

  const std::size_t remaining = messages_.size() - rendered_count_;
  const std::size_t page = static_cast<std::size_t>(
      std::max(1, app_.settings().history_page_size)
  );
  const std::size_t additional = std::min(page, remaining);

  // The newly revealed rows are the block immediately above the current window.
  // Inserting them (oldest first) at the top of the rendered rows keeps the
  // transcript chronological.
  const std::size_t start = remaining - additional;

  auto* bar = scroll_->verticalScrollBar();
  const int old_value = bar->value();

  // Anchor on the current first row: after prepending, its position shifts down
  // by exactly the height that was added, so offsetting the scrollbar by the
  // same amount keeps the reader on the same message.
  QWidget* anchor = nullptr;

  if (auto* item = transcript_layout_->itemAt(2)) {
    anchor = item->widget();
  }

  const int anchor_before = anchor != nullptr ? anchor->y() : 0;

  for (std::size_t k = 0; k < additional; ++k) {
    render_message_at(start + k, 2 + static_cast<int>(k));
  }

  rendered_count_ += additional;
  update_load_older_visibility();

  if (anchor == nullptr) {
    updating_ = false;
    return;
  }

  // Defer until the layout has processed the new rows so anchor->y() reflects
  // the final position.
  QTimer::singleShot(0, this, [this, anchor, anchor_before, old_value]() {
    auto* deferred_bar = scroll_->verticalScrollBar();
    deferred_bar->setValue(old_value + (anchor->y() - anchor_before));
  });

  updating_ = false;
}

void ConversationView::update_load_older_visibility() {
  if (load_older_ == nullptr) {
    return;
  }

  const std::size_t hidden = messages_.size() - rendered_count_;
  load_older_->setVisible(hidden > 0);

  if (hidden > 0) {
    load_older_->setText(QStringLiteral("Load earlier messages (%1)").arg(hidden));
  }
}

void ConversationView::flush_streaming_text() {
  if (!has_pending_stream_ || streaming_row_ == nullptr) {
    return;
  }

  has_pending_stream_ = false;
  streaming_row_->bubble()->set_text(pending_stream_text_);
  // No explicit scroll here: growing the bubble changes the scrollbar range,
  // which the rangeChanged handler turns into a smooth follow when the reader is
  // at the bottom. Scrolling here too would fight that and cause the jitter.
}

void ConversationView::poll() {
  controller_.poll();
  set_busy(controller_.busy());
}

void ConversationView::set_busy(bool busy) {
  if (busy_ == busy) {
    return;
  }

  busy_ = busy;
  send_button_->setEnabled(!busy);
  cancel_button_->setVisible(busy);
}

bool ConversationView::eventFilter(QObject* watched, QEvent* event) {
  if (scroll_ != nullptr && watched == scroll_->viewport() &&
      event->type() == QEvent::Resize) {
    update_content_margins();
  }

  if (watched == input_ && event->type() == QEvent::KeyPress) {
    auto* key_event = static_cast<QKeyEvent*>(event);

    const bool is_enter =
        key_event->key() == Qt::Key_Return || key_event->key() == Qt::Key_Enter;

    if (app_.settings().enter_with_modifier) {
      if (is_enter && (key_event->modifiers() & Qt::ControlModifier)) {
        submit();
        return true;
      }
    } else {
      if (is_enter && !(key_event->modifiers() & Qt::ShiftModifier)) {
        submit();
        return true;
      }
    }
  }

  return QWidget::eventFilter(watched, event);
}

MessageRow* ConversationView::make_row(
    MessageBubble::Role role, std::uint32_t message_id, bool editable
) {
  auto* row = new MessageRow(role, transcript_);
  row->set_message_id(message_id);
  rows_.push_back(row);

  // Only stored messages (id != 0) get actions; the streaming and notice rows
  // stay passive.
  if (message_id != 0) {
    // Assistant replies can be copied verbatim: bubble()->text() returns the raw
    // Markdown source, not the rendered rich text.
    if (role == MessageBubble::Role::Assistant) {
      auto* copy = row->add_action(
          QStringLiteral("copy"), QStyle::SP_DialogSaveButton,
          QStringLiteral("Copy message")
      );

      connect(
          copy, &QToolButton::clicked, this,
          [row]() { QApplication::clipboard()->setText(row->bubble()->text()); }
      );
    }

    if (editable) {
      auto* edit = row->add_action(
          QStringLiteral("edit"), QStyle::SP_FileIcon,
          QStringLiteral("Edit message")
      );

      connect(
          edit, &QToolButton::clicked, this,
          [this, message_id, row]() { prompt_edit(message_id, row->bubble()->text()); }
      );
    }

    auto* remove = row->add_action(
        QStringLiteral("delete"), QStyle::SP_TrashIcon,
        QStringLiteral("Delete message")
    );

    connect(
        remove, &QToolButton::clicked, this,
        [this, message_id]() { controller_.delete_message(message_id); }
    );
  }

  return row;
}

MessageRow* ConversationView::add_row(
    MessageBubble::Role role, std::uint32_t message_id, bool editable
) {
  auto* row = make_row(role, message_id, editable);
  transcript_layout_->addWidget(row);

  return row;
}

void ConversationView::prompt_edit(std::uint32_t message_id, const QString& current) {
  bool accepted = false;

  const QString edited = QInputDialog::getMultiLineText(
      this, QStringLiteral("Edit message"), QStringLiteral("Message:"), current, &accepted
  );

  if (accepted && edited != current) {
    controller_.edit_message(message_id, edited.toStdString());
  }
}

void ConversationView::clear_transcript() {
  if (stream_timer_ != nullptr) {
    stream_timer_->stop();
  }

  has_pending_stream_ = false;
  pending_stream_text_.clear();
  streaming_row_ = nullptr;
  streaming_status_.clear();
  rows_.clear();

  for (int i = transcript_layout_->count() - 1; i >= 0; --i) {
    auto* item = transcript_layout_->itemAt(i);

    // The leading stretch (a spacer item, not a widget) must be kept so the
    // transcript stays bottom-aligned after a reload, and the load-older button
    // is a permanent fixture that is only shown or hidden.
    if (auto* widget = item->widget()) {
      if (widget == load_older_) {
        continue;
      }

      transcript_layout_->takeAt(i);
      widget->deleteLater();
    }
  }
}

void ConversationView::scroll_to_bottom() {
  // Force-follow new content (session load, notice bubble) even if the reader
  // had scrolled away, then pin once the layout has processed the change.
  stick_to_bottom_ = true;

  QTimer::singleShot(0, this, [this]() {
    if (stick_to_bottom_) {
      pin_to_bottom();
    }
  });
}

void ConversationView::pin_to_bottom() {
  auto* bar = scroll_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void ConversationView::update_content_margins() {
  const lnb::ThemeMetrics& m = metrics();
  const int max_width = message_max_width();

  // Keep a comfortable minimum inset so the flow never touches the pane edges.
  int side = m.page_margin;

  if (kConversationSidePadding > side) {
    side = kConversationSidePadding;
  }

  // When a flow width cap is configured and the viewport is wider than it, grow
  // both side insets equally so the content column is centred and the blocks
  // pull away from the left and right edges.
  if (max_width > 0 && scroll_ != nullptr && scroll_->viewport() != nullptr) {
    const int extra = (scroll_->viewport()->width() - max_width) / 2;

    if (extra > side) {
      side = extra;
    }
  }

  if (side == content_side_margin_) {
    return;
  }

  content_side_margin_ = side;
  transcript_layout_->setContentsMargins(
      side, m.page_margin, side, m.page_margin
  );
}

void ConversationView::apply_composer_style() {
  const ThemeColors& colors = theme().colors;

  // The conversation surface. A theme with a background image keeps it fully
  // transparent, so the tinted image shows through; a plain theme uses its
  // window colour.
  setStyleSheet(
      QStringLiteral("QWidget#ConversationView { background: %1; }")
          .arg(css_color(colors.conversation_background))
  );

  // Zed-like composer: the field is bare text on the conversation surface, and
  // the whole block is set off from the transcript by a single hairline. No field
  // chrome and no rounded corners. The field's own background is themeable.
  input_->setStyleSheet(
      QStringLiteral(
          "QPlainTextEdit#Composer { background: %1; border: none;"
          " padding: 4px 2px; color: %2; }"
      )
          .arg(css_color(colors.input_background), css_color(colors.text))
  );

  if (composer_bar_ != nullptr) {
    // The action buttons are flat and borderless; a hover fill is the only chrome
    // they carry, the way Zed's composer actions behave.
    composer_bar_->setStyleSheet(
        QStringLiteral(
            "QWidget#ComposerBar { background: transparent;"
            " border-top: 1px solid %1; }"
            "QToolButton#SendButton, QToolButton#ComposerExpand,"
            "QToolButton#CancelButton {"
            "  background: transparent; border: none; border-radius: 4px; padding: 0; }"
            "QToolButton#SendButton:hover, QToolButton#ComposerExpand:hover,"
            "QToolButton#CancelButton:hover {"
            "  background: %2; }"
        )
            .arg(css_color(colors.border), css_color(row_hover_background()))
    );
  }

  apply_composer_icons();
}

void ConversationView::apply_composer_icons() {
  const ThemeColors& colors = theme().colors;

  if (send_button_ != nullptr) {
    // The send arrow takes the accent colour; a busy composer mutes it through
    // the icon's disabled pixmap.
    QIcon icon = resource_icon(QStringLiteral("send"), colors.accent, colors.text_muted);

    if (icon.isNull()) {
      icon = style()->standardIcon(QStyle::SP_ArrowUp);
    }

    send_button_->setIcon(icon);
    send_button_->setToolTip(QStringLiteral("Send"));
  }

  if (expand_button_ != nullptr) {
    const bool expanded = composer_expanded_;
    QIcon icon = resource_icon(
        expanded ? QStringLiteral("collapse") : QStringLiteral("expand"), colors.text_muted,
        colors.text_muted
    );

    if (icon.isNull()) {
      icon = style()->standardIcon(expanded ? QStyle::SP_ArrowDown : QStyle::SP_ArrowUp);
    }

    expand_button_->setIcon(icon);
    expand_button_->setToolTip(
        expanded ? QStringLiteral("Shrink the composer")
                 : QStringLiteral("Expand the composer")
    );
  }

  if (cancel_button_ != nullptr) {
    // The stop square shares the muted colour of the other chrome actions.
    QIcon icon =
        resource_icon(QStringLiteral("stop"), colors.text_muted, colors.text_muted);

    if (icon.isNull()) {
      icon = style()->standardIcon(QStyle::SP_MediaStop);
    }

    cancel_button_->setIcon(icon);
    cancel_button_->setToolTip(QStringLiteral("Cancel"));
  }
}

void ConversationView::apply_composer_height() {
  if (input_ != nullptr) {
    const lnb::ThemeMetrics& m = metrics();
    input_->setFixedHeight(
        composer_expanded_ ? m.composer_height_expanded : m.composer_height
    );
  }

  apply_composer_icons();
}

void ConversationView::toggle_composer_height() {
  composer_expanded_ = !composer_expanded_;
  apply_composer_height();
}

void ConversationView::submit() {
  if (controller_.busy()) {
    return;
  }

  const QString text = input_->toPlainText().trimmed();

  if (text.isEmpty()) {
    return;
  }

  input_->clear();
  controller_.send(text.toStdString());
  set_busy(true);
  scroll_to_bottom();
}

} // namespace ui
