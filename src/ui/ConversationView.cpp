#include "ConversationView.hpp"

#include "theme.hpp"

#include <QCursor>
#include <QApplication>
#include <QClipboard>
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
#include <QSize>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <qnamespace.h>

#include <algorithm>

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

// Fixed height of every action strip, so showing the buttons never reflows the
// transcript.
constexpr int kActionsHeight = 24;

// Minimum left/right inset of the conversation flow. The page margin alone is
// too tight, so bubbles never crowd the pane edges even when the message width
// is unlimited or the pane is narrow.
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
// The caller is responsible for the font, the text format and the text itself.
QLabel* make_label(QWidget* parent, Qt::TextFormat format) {
  auto* label = new QLabel(parent);
  label->setWordWrap(true);
  label->setTextFormat(format);
  label->setTextInteractionFlags(
      Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse
  );

  QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Minimum);
  policy.setHeightForWidth(true);
  label->setSizePolicy(policy);
  label->setMinimumWidth(0);

  return label;
}

// Caps a message body to the effective maximum width. A maximum of 0 means the
// width is unlimited, so nothing is applied.
void apply_max_width(QWidget* widget) {
  const int max_width = message_max_width();

  if (max_width > 0) {
    widget->setMaximumWidth(max_width);
  }
}

// A proportional line height is drawn with the extra leading *below* the text,
// so the last line of a paragraph leaves a larger trailing gap than the leading
// above the first line of the next paragraph. A code block is a solid frame, so
// the paragraph before it looks well spaced while the paragraph after it looks
// cramped. Padding the block by that same extra leading restores the balance.
int code_block_bottom_gap() {
  const int line_spacing = QFontMetrics(chat_font()).lineSpacing();
  return line_spacing * std::max(0, line_height_other() - 100) / 100;
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
        (code_block || last_block) ? 0
                                   : (list_block ? list_paragraph_spacing()
                                                 : paragraph_spacing())
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

      QTextCharFormat format = fragment.charFormat();
      format.setFontFamilies(mono.families());
      format.setFontFixedPitch(true);

      if (mono.pointSize() > 0) {
        format.setFontPointSize(mono.pointSize());
      }

      // Inline code needs its own background; inside a code block the block
      // background already covers it.
      if (!code_block) {
        format.setBackground(code_background);
      }

      QTextCursor cursor(block);
      cursor.setPosition(fragment.position());
      cursor.setPosition(
          fragment.position() + fragment.length(),
          QTextCursor::KeepAnchor
      );
      cursor.setCharFormat(format);
    }
  }

  return document.toHtml();
}

// Renders plain text (user and error messages) with the same reading rhythm as
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

  // Only bubbles (user and error messages) need inner padding; the bare
  // assistant text hugs the transcript margin instead.
  const bool wrapped = role_ != Role::Assistant;

  body_layout_ = new QVBoxLayout(this);
  body_layout_->setContentsMargins(
      wrapped ? 16 : 2,
      wrapped ? 8 : 2,
      wrapped ? 16 : 2,
      role_ == Role::User ? 4 : wrapped ? 8 : 2
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
  const Theme& current = theme();

  QColor background;
  QColor foreground;
  int radius = current.metrics.bubble_radius;

  // Only the user bubble gets a hairline outline (like the composer field); the
  // assistant and error bubbles stay borderless.
  QString border_rule;

  switch (role_) {
    case Role::User:
      background = current.colors.bubble_user;
      foreground = current.colors.bubble_user_text;
      border_rule = QStringLiteral("border:1px solid %1;")
                        .arg(css_color(current.colors.border));
      break;
    case Role::Assistant:
      // background = current.colors.bubble_assistant;
      background = QColor(Qt::transparent);
      foreground = current.colors.bubble_assistant_text;
      radius = 0;
      break;
    case Role::System:
      background = current.colors.danger;
      foreground = current.colors.danger_text;
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
          .arg(radius)
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
  text_label_->setText(render_plain(text));
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
      label->setOpenExternalLinks(true);
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
    blocks_.push_back(Block{true, container, label});
  }
}

QString MessageBubble::text() const {
  // Labels hold rendered HTML, so expose the original source text.
  return source_;
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
  column->setSizePolicy(
      QSizePolicy::Ignored,
      QSizePolicy::Minimum
  );
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
  action_layout->setContentsMargins(0, 0, 0, 0);
  action_layout->setSpacing(4);

  // A single stretch keeps the id + buttons group on the bubble's side: it leads
  // for user messages (group right-aligned) and trails for the others
  // (left-aligned). The id label is created here so add_action can insert the
  // buttons next to it in either arrangement.
  message_id_label_ = new QLabel(actions_);
  message_id_label_->setObjectName(QStringLiteral("MessageId"));
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
    const QString& icon_name,
    QStyle::StandardPixmap fallback_icon,
    const QString& tip
) {
  auto* button = new QToolButton(actions_);
  button->setObjectName(QStringLiteral("MessageAction"));
  button->setToolButtonStyle(Qt::ToolButtonIconOnly);
  button->setIconSize(QSize(16, 16));
  button->setFixedSize(24, 24);
  button->setAutoRaise(true);
  button->setCursor(Qt::PointingHandCursor);
  button->setFocusPolicy(Qt::NoFocus);
  button->setToolTip(tip);

  QIcon icon = QIcon::fromTheme(icon_name);

  // Fall back to a built-in standard icon when the icon theme is unavailable.
  if (icon.isNull()) {
    icon = style()->standardIcon(fallback_icon);
  }

  button->setIcon(icon);

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

void MessageRow::set_actions_visible(bool visible) {
  if (actions_visible_ == visible) {
    return;
  }

  actions_visible_ = visible;

  for (QToolButton* button:
       actions_->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly)) {
    button->setVisible(visible);
  }

  if (message_id_label_ != nullptr) {
    message_id_label_->setVisible(visible);
  }
}

void MessageRow::set_message_id(uint32_t id) {
  message_id_ = id;

  if (message_id_label_ == nullptr) {
    return;
  }

  message_id_label_->setText(
      id == 0 ? QString{} : QStringLiteral("#%1").arg(id)
  );
}

void MessageRow::enterEvent(QEnterEvent* event) {
  QWidget::enterEvent(event);
  set_actions_visible(true);
}

void MessageRow::leaveEvent(QEvent* event) {
  QWidget::leaveEvent(event);

  // The row also receives a Leave when the pointer moves onto one of its
  // children (the bubble or the action strip), so only hide the buttons once
  // the pointer has really left the entry.
  if (actions_visible_ && !rect().contains(mapFromGlobal(QCursor::pos()))) {
    set_actions_visible(false);
  }
}

// -----------------------------------------------------------------------------
// ConversationView
// -----------------------------------------------------------------------------

ConversationView::ConversationView(lnb::ApplicationState& state, QWidget* parent)
    : state_(state), QWidget(parent) {
  const ThemeMetrics& metrics = theme().metrics;

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
  scroll_->viewport()->setAutoFillBackground(false);

  transcript_ = new QWidget(scroll_);
  transcript_->setObjectName(QStringLiteral("TranscriptInner"));
  transcript_->setMinimumWidth(0);
  transcript_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  transcript_layout_ = new QVBoxLayout(transcript_);
  transcript_layout_->setContentsMargins(
      metrics.page_margin,
      metrics.page_margin,
      metrics.page_margin,
      metrics.page_margin
  );
  transcript_->setMinimumSize(0, 0);
  transcript_layout_->setSpacing(metrics.message_spacing);

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

  connect(
      load_older_,
      &QPushButton::clicked,
      this,
      [this]() { load_older(); }
  );

  // Reaching the top of the transcript pulls in the next page automatically;
  // the button stays as an explicit, keyboard-reachable alternative. The same
  // handler tracks whether the reader is parked at the bottom: auto-scroll only
  // follows new content while they are, and resumes once they scroll back down.
  auto* scrollbar = scroll_->verticalScrollBar();

  connect(
      scrollbar,
      &QScrollBar::valueChanged,
      this,
      [this, scrollbar](int value) {
        stick_to_bottom_ = value >= scrollbar->maximum() - kStickToBottomSlack;

        if (updating_ || value > 0) {
          return;
        }

        if (rendered_count_ < messages_.size()) {
          load_older();
        }
      }
  );

  // Following the range instead of a timer is what keeps the follow smooth: the
  // range only changes once the layout has settled, so the view tracks the true
  // bottom instead of jumping to a stale maximum while Markdown reflows.
  connect(
      scrollbar,
      &QScrollBar::rangeChanged,
      this,
      [this](int, int) {
        if (stick_to_bottom_ && !updating_) {
          pin_to_bottom();
        }
      }
  );

  scroll_->setWidget(transcript_);

  // The transcript column is re-inset whenever the viewport is resized so it
  // stays centred once the pane is wider than the configured message width.
  scroll_->viewport()->installEventFilter(this);
  update_content_margins();

  auto* composer = new QWidget(this);
  composer->setObjectName(QStringLiteral("ComposerBar"));
  composer->setAttribute(Qt::WA_StyledBackground, true);

  auto* composer_layout = new QHBoxLayout(composer);
  composer_layout->setContentsMargins(
      metrics.page_margin,
      metrics.page_margin / 2,
      metrics.page_margin,
      metrics.page_margin
  );
  composer_layout->setSpacing(8);

  input_ = new QPlainTextEdit(composer);
  input_->setObjectName(QStringLiteral("Composer"));
  input_->setPlaceholderText(
      state_.uni_config_->enter_with_modifier_
      ? "Type a message...  (Ctrl+Enter to send, Enter for a new line)"
      : "Type a message...  (Enter to send, Shift+Enter for a new line)"
  );
  input_->setTabChangesFocus(true);
  input_->setFixedHeight(metrics.composer_height);
  input_->setFont(chat_font());
  input_->installEventFilter(this);

  auto* buttons = new QVBoxLayout();
  buttons->setSpacing(6);

  send_button_ = new QPushButton("Send", composer);
  cancel_button_ = new QPushButton("Cancel", composer);
  cancel_button_->setObjectName(QStringLiteral("Secondary"));
  cancel_button_->setVisible(false);

  buttons->addWidget(send_button_);
  buttons->addWidget(cancel_button_);
  buttons->addStretch();

  composer_layout->addWidget(input_, 1);
  composer_layout->addLayout(buttons);

  layout->addWidget(scroll_, 1);
  layout->addWidget(composer);

  // Coalesces streaming updates so Markdown is re-rendered at a steady rate
  // instead of once per token.
  stream_timer_ = new QTimer(this);
  stream_timer_->setSingleShot(true);
  stream_timer_->setInterval(kStreamRenderMs);

  connect(
      stream_timer_,
      &QTimer::timeout,
      this,
      [this]() { flush_streaming_text(); }
  );

  connect(
      send_button_,
      &QPushButton::clicked,
      this,
      [this]() { submit(); }
  );

  connect(
      cancel_button_,
      &QPushButton::clicked,
      this,
      [this]() {
        if (on_cancel_) {
          on_cancel_();
        }
      }
  );
}

void ConversationView::set_on_send(std::function<void(std::string)> handler) {
  on_send_ = std::move(handler);
}

void ConversationView::set_on_cancel(std::function<void()> handler) {
  on_cancel_ = std::move(handler);
}

void ConversationView::set_on_delete_message(std::function<void(uint32_t)> handler) {
  on_delete_message_ = std::move(handler);
}

void ConversationView::set_on_edit_message(
    std::function<void(uint32_t, const std::string&)> handler
) {
  on_edit_message_ = std::move(handler);
}

void ConversationView::set_messages(
    const std::vector<lnb::message>& messages,
    uint32_t self_id
) {
  updating_ = true;
  clear_transcript();

  messages_ = messages;
  self_id_ = self_id;

  // Render only the tail window. The rest stays in memory but out of the widget
  // tree, so the per-reload cost (widget creation plus Markdown rendering) is
  // bounded no matter how long the conversation grows.
  const std::size_t limit = static_cast<std::size_t>(
      std::max(1, state_.uni_config_->rendered_message_limit_)
  );
  rendered_count_ = std::min(limit, messages_.size());

  const std::size_t start = messages_.size() - rendered_count_;

  // Layout index 0 is the leading stretch and index 1 is the load-older button,
  // so the rendered rows start at index 2.
  for (std::size_t k = 0; k < rendered_count_; ++k) {
    render_message_at(start + k, 2 + static_cast<int>(k));
  }

  update_load_older_visibility();
  scroll_to_bottom();
  updating_ = false;
}

void ConversationView::render_message_at(std::size_t index, int layout_index) {
  const lnb::message& message = messages_[index];
  const bool from_self = message.sender_id == self_id_;

  auto* row = make_row(
      from_self ? MessageBubble::Role::User : MessageBubble::Role::Assistant,
      message.id,
      from_self
  );

  row->bubble()->set_text(
      QString::fromUtf8(
          message.content.data(),
          static_cast<qsizetype>(message.content.size())
      )
  );

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
      std::max(1, state_.uni_config_->history_page_size_)
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
    load_older_->setText(
        QStringLiteral("Load earlier messages (%1)").arg(hidden)
    );
  }
}

void ConversationView::set_streaming_text(const QString& text) {
  if (streaming_row_ == nullptr) {
    streaming_row_ = add_row(MessageBubble::Role::Assistant, 0, false);
  }

  // An empty assistant stream means the worker has not produced reply text yet;
  // keep the transient operation title visible in the same row.
  pending_stream_text_ = text.isEmpty() ? streaming_status_ : text;
  has_pending_stream_ = true;

  if (!stream_timer_->isActive()) {
    stream_timer_->start();
  }
}

void ConversationView::set_streaming_status(const QString& status) {
  streaming_status_ = status;

  if (streaming_row_ == nullptr) {
    streaming_row_ = add_row(MessageBubble::Role::Assistant, 0, false);
  }

  if (pending_stream_text_.isEmpty() && streaming_row_->bubble()->text().isEmpty()) {
    pending_stream_text_ = status;
    has_pending_stream_ = true;

    if (!stream_timer_->isActive()) {
      stream_timer_->start();
    }
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

void ConversationView::show_error(const QString& text) {
  if (text.isEmpty()) {
    return;
  }

  auto* row = add_row(MessageBubble::Role::System, 0, false);
  row->bubble()->set_text(text);
  scroll_to_bottom();
}

void ConversationView::set_busy(bool busy) {
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

    if (state_.uni_config_->enter_with_modifier_) {
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
    MessageBubble::Role role,
    uint32_t message_id,
    bool editable
) {
  auto* row = new MessageRow(role, transcript_);
  row->set_message_id(message_id);

  // Only stored messages (id != 0) get actions; the streaming and error rows
  // stay passive.
  if (message_id != 0) {
    // Assistant replies can be copied verbatim: bubble()->text() returns the raw
    // Markdown source, not the rendered rich text.
    if (role == MessageBubble::Role::Assistant) {
      auto* copy = row->add_action(
          QStringLiteral("edit-copy"),
          QStyle::SP_DialogSaveButton,
          QStringLiteral("Copy message")
      );

      connect(
          copy,
          &QToolButton::clicked,
          this,
          [row]() {
            QApplication::clipboard()->setText(row->bubble()->text());
          }
      );
    }

    if (editable) {
      auto* edit = row->add_action(
          QStringLiteral("document-edit"),
          QStyle::SP_FileIcon,
          QStringLiteral("Edit message")
      );

      connect(
          edit,
          &QToolButton::clicked,
          this,
          [this, message_id, row]() {
            prompt_edit(message_id, row->bubble()->text());
          }
      );
    }

    auto* remove = row->add_action(
        QStringLiteral("edit-delete"),
        QStyle::SP_TrashIcon,
        QStringLiteral("Delete message")
    );

    connect(
        remove,
        &QToolButton::clicked,
        this,
        [this, message_id]() {
          if (on_delete_message_) {
            on_delete_message_(message_id);
          }
        }
    );
  }

  return row;
}

MessageRow* ConversationView::add_row(
    MessageBubble::Role role,
    uint32_t message_id,
    bool editable
) {
  auto* row = make_row(role, message_id, editable);
  transcript_layout_->addWidget(row);

  return row;
}

void ConversationView::prompt_edit(uint32_t message_id, const QString& current) {
  bool accepted = false;

  const QString edited = QInputDialog::getMultiLineText(
      this,
      QStringLiteral("Edit message"),
      QStringLiteral("Message:"),
      current,
      &accepted
  );

  if (accepted && edited != current && on_edit_message_) {
    on_edit_message_(message_id, edited.toStdString());
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
  // Force-follow new content (session load, error bubble) even if the reader had
  // scrolled away, then pin once the layout has processed the change.
  stick_to_bottom_ = true;

  QTimer::singleShot(
      0,
      this,
      [this]() {
        if (stick_to_bottom_) {
          pin_to_bottom();
        }
      }
  );
}

void ConversationView::pin_to_bottom() {
  auto* bar = scroll_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void ConversationView::update_content_margins() {
  const ThemeMetrics& metrics = theme().metrics;
  const int max_width = message_max_width();

  // Keep a comfortable minimum inset so the flow never touches the pane edges.
  int side = metrics.page_margin;

  if (kConversationSidePadding > side) {
    side = kConversationSidePadding;
  }

  // When a message width cap is configured and the viewport is wider than it,
  // grow both side insets equally so the content column is centred and the
  // bubbles pull away from the left and right edges.
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
      side, metrics.page_margin, side, metrics.page_margin
  );
}

void ConversationView::submit() {
  if (!send_button_->isEnabled()) {
    return;
  }

  const QString text = input_->toPlainText().trimmed();

  if (text.isEmpty()) {
    return;
  }

  if (on_send_) {
    on_send_(text.toStdString());
  }

  input_->clear();
}

} // namespace ui
