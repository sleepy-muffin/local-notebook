#include "ui/ConversationView.hpp"

#include "ui/SummaryReviewDialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "local-notebook-core/interface.hpp"
#include "settings/settings.hpp"
#include "support/i18n.hpp"
#include "ui/MessageDocument.hpp"
#include "ui/StatusNotice.hpp"
#include "ui/TranscriptCanvas.hpp"
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

std::u16string to_u16(const QString& value) {
  return std::u16string(
      reinterpret_cast<const char16_t*>(value.utf16()), static_cast<std::size_t>(value.size()));
}

// A stored message's arrival time (ISO-8601 UTC) in the reader's local time. An
// empty or unparsable value yields an empty string, so notice rows stay bare.
QString format_message_time(const std::string& iso) {
  if (iso.empty()) {
    return {};
  }

  const QDateTime parsed = QDateTime::fromString(from_utf8(iso), Qt::ISODate);

  if (!parsed.isValid()) {
    return {};
  }

  return parsed.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

// The estimated token cost of one message, on the same compact scale as the
// title bar's readout, with a unit suffix so the value in the hover strip cannot
// be mistaken for the id or the time.
QString format_token_estimate(std::size_t tokens) {
  if (tokens < 1000) {
    return trs("%1 tok").arg(static_cast<qulonglong>(tokens));
  }

  const double thousands = static_cast<double>(tokens) / 1000.0;

  if (tokens % 1000 == 0 || tokens >= 100000) {
    return trs("%1k tok").arg(qRound(thousands));
  }

  return trs("%1k tok").arg(thousands, 0, 'f', 1);
}

// Streaming Markdown is re-rendered at most this often (ms), so a long reply is
// not re-parsed on every poll tick.
constexpr int kStreamRenderMs = 50;

} // namespace

// -----------------------------------------------------------------------------
// ReasoningSelector
// -----------------------------------------------------------------------------

// The composer's reasoning-effort control. It is deliberately bare: no frame, no
// background, no border — just the level's text with a chevron on its right, so
// it reads as part of the composer surface. Clicking anywhere on it (text or
// chevron) opens the dropdown. The app has no Q_OBJECT, so the widget paints
// itself and dispatches through a plain std::function.
class ReasoningSelector : public QWidget {
public:
  explicit ReasoningSelector(QWidget* parent = nullptr) : QWidget(parent) {
    setObjectName(QStringLiteral("ReasoningSelector"));
    setCursor(Qt::PointingHandCursor);
    // Never take focus: the composer field keeps it, so typing is uninterrupted.
    setFocusPolicy(Qt::NoFocus);
    setFont(ui_font());
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // Fully transparent: no background is ever painted. Hover only brightens the
    // text and chevron, never fills a surface.
    setAttribute(Qt::WA_Hover, true);
    setAutoFillBackground(false);
    setFixedHeight(28);
  }

  // Shows `current` and offers `levels`. A level not present in `levels` is not
  // shown (the caller builds the list with reasoning_efforts_with_stored so the
  // stored value is always included).
  void set_levels(const std::vector<std::string>& levels, const std::string& current) {
    levels_ = levels;
    current_ = current;
    update_geometry();
    update();
  }

  void set_on_selected(std::function<void(const std::string&)> handler) {
    on_selected_ = std::move(handler);
  }

  // Re-reads the font (a metric change) and repaints after a theme change.
  void refresh_style() {
    setFont(ui_font());
    update_geometry();
    update();
  }

protected:
  void paintEvent(QPaintEvent*) override {
    if (current_.empty()) {
      return;
    }

    const ThemeColors& colors = theme().colors;
    const QColor ink = (hovered_ || menu_open_) ? colors.text : colors.text_muted;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int text_pad = 6;
    const int arrow = 9;
    const QRect text_rect = rect().adjusted(text_pad, 0, -(arrow + text_pad), 0);

    painter.setPen(ink);
    painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter, from_utf8(current_));

    // A small chevron on the right, the only chrome the control carries.
    const qreal cx = static_cast<qreal>(width()) - text_pad - arrow / 2.0;
    const qreal cy = height() / 2.0;
    QPen pen(ink);
    pen.setWidthF(1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawLine(QPointF(cx - 3.0, cy - 1.5), QPointF(cx, cy + 1.5));
    painter.drawLine(QPointF(cx, cy + 1.5), QPointF(cx + 3.0, cy - 1.5));
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && !levels_.empty()) {
      open_menu();
      return;
    }

    QWidget::mouseReleaseEvent(event);
  }

  void enterEvent(QEnterEvent* event) override {
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
  }

  void leaveEvent(QEvent* event) override {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
  }

private:
  void update_geometry() {
    const QFontMetrics metrics(font());
    int text_width = 0;

    for (const std::string& level: levels_) {
      text_width = std::max(text_width, metrics.horizontalAdvance(from_utf8(level)));
    }

    // Text, a gap, the chevron, and the same padding on both sides.
    setFixedWidth(text_width + 9 + 12);
  }

  void open_menu() {
    QMenu menu(this);

    for (const std::string& level: levels_) {
      QAction* action = menu.addAction(from_utf8(level));
      action->setCheckable(true);
      action->setChecked(level == current_);
      action->setData(from_utf8(level));
    }

    menu_open_ = true;
    update();

    // Drop above the control: it sits at the very bottom of the window.
    QAction* chosen = menu.exec(mapToGlobal(QPoint(0, -menu.sizeHint().height() - 2)));

    menu_open_ = false;
    update();

    if (chosen != nullptr && on_selected_ && chosen->data().toString() != from_utf8(current_)) {
      on_selected_(chosen->data().toString().toStdString());
    }
  }

  std::vector<std::string> levels_;
  std::string current_;
  std::function<void(const std::string&)> on_selected_;
  bool hovered_ = false;
  bool menu_open_ = false;
};

// -----------------------------------------------------------------------------
// ConversationView
// -----------------------------------------------------------------------------

ConversationView::ConversationView(
    lnb::api::interface& api, const lnb::Settings& settings, std::uint32_t session_id,
    QWidget* parent)
    : QWidget(parent), api_(api), settings_(settings), session_id_(session_id),
      controller_(api, session_id) {
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

  // One custom-painted canvas holds every message. Reaching the top pages in
  // older history automatically; there is no "Load earlier" button and no
  // widget tree whose insertion could race the scroll position.
  transcript_ = new TranscriptCanvas(this);
  transcript_->set_on_reach_top([this]() { load_older(); });

  transcript_->set_on_copy([this](std::uint32_t message_id) {
    for (const lnb::Message& message: messages_) {
      if (message.id_ == message_id) {
        QApplication::clipboard()->setText(from_utf8(message.content_));
        return;
      }
    }
  });

  transcript_->set_on_edit([this](std::uint32_t message_id) {
    for (const lnb::Message& message: messages_) {
      if (message.id_ == message_id) {
        prompt_edit(message_id, from_utf8(message.content_));
        return;
      }
    }
  });

  transcript_->set_on_delete(
      [this](std::uint32_t message_id) { controller_.delete_message(message_id); });

  layout->addWidget(transcript_, 1);

  auto* composer = new QWidget(this);
  composer->setObjectName(QStringLiteral("ComposerBar"));
  composer->setAttribute(Qt::WA_StyledBackground, true);
  composer->setAutoFillBackground(false);
  composer_bar_ = composer;

  auto* composer_layout = new QHBoxLayout(composer);
  composer_layout->setContentsMargins(
      m.page_margin, m.page_margin / 2, m.page_margin, m.page_margin);
  composer_layout->setSpacing(8);

  input_ = new QPlainTextEdit(composer);
  input_->setObjectName(QStringLiteral("Composer"));
  input_->setPlaceholderText(
      settings_.enter_with_modifier
          ? trs("Type a message...  (Ctrl+Enter to send, Enter for a new line)")
          : trs("Type a message...  (Enter to send, Shift+Enter for a new line)"));
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

  // The reasoning control sits immediately to the left of the send button, at the
  // block's bottom-right. Reading it as part of the composer, it is borderless and
  // fully transparent; the group it belongs to takes width from the input field.
  reasoning_selector_ = new ReasoningSelector(composer);
  reasoning_selector_->set_on_selected([this](const std::string& level) {
    api_.set_session_reasoning_effort(session_id_, level);
    refresh_reasoning_selector();
  });

  auto* bottom_actions = new QHBoxLayout();
  bottom_actions->setContentsMargins(0, 0, 0, 0);
  bottom_actions->setSpacing(2);
  bottom_actions->addStretch(1);
  bottom_actions->addWidget(reasoning_selector_, 0, Qt::AlignVCenter);
  bottom_actions->addWidget(send_button_, 0, Qt::AlignVCenter);
  bottom_actions->addWidget(cancel_button_, 0, Qt::AlignVCenter);

  buttons->addLayout(bottom_actions);

  composer_layout->addWidget(input_, 1);
  composer_layout->addLayout(buttons);

  // The small failure notice lives just above the composer, so it reads as a
  // remark on the turn rather than part of the transcript. It is hidden until a
  // turn fails.
  notice_ = new StatusNotice(this);
  layout->addWidget(notice_);

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
    // The first token creates the reply on the canvas; later ones only replace
    // its blocks, leaving the rest of the transcript shaped as-is.
    if (!streaming_started_) {
      streaming_started_ = true;
      transcript_->begin_streaming();
    }

    pending_stream_text_ = text.empty() ? streaming_status_ : from_utf8(text);
    has_pending_stream_ = true;

    if (!stream_timer_->isActive()) {
      stream_timer_->start();
    }
  });

  controller_.set_on_status([this](const std::string& status) {
    streaming_status_ = from_utf8(status);

    if (!streaming_started_) {
      streaming_started_ = true;
      transcript_->begin_streaming();
    }

    if (pending_stream_text_.isEmpty()) {
      pending_stream_text_ = streaming_status_;
      has_pending_stream_ = true;

      if (!stream_timer_->isActive()) {
        stream_timer_->start();
      }
    }
  });

  controller_.set_on_error([this](const std::string&) {
    // The reason itself now lives in the tasks window, where it can be read in
    // full and does not vanish on the next reload; the transcript only raises the
    // small nudge above the composer and points at the status bar's tasks icon.
    if (notice_ != nullptr) {
      notice_->set_content(
          QStringLiteral("tasks"),
          trs("A generation task failed. Open the tasks window from the status bar "
              "to see why."));
    }
  });

  controller_.set_on_warning(
      [this](const std::string& text) { pending_warning_ = from_utf8(text); });

  reload();
}

void ConversationView::reload() {
  // Remember the reader's place before the document is replaced: following
  // content is only ever a consequence of having been parked at the bottom, and a
  // rebuild must not yank someone who had scrolled away back to the end.
  const TranscriptCanvas::ScrollAnchor anchor = transcript_->anchor();
  const bool follow = transcript_->at_bottom();

  // A finished turn has been stored as a message, so the streamed preview is
  // superseded; the next turn starts a fresh one.
  if (stream_timer_ != nullptr) {
    stream_timer_->stop();
  }

  has_pending_stream_ = false;
  pending_stream_text_.clear();
  streaming_status_.clear();
  streaming_started_ = false;

  messages_ = api_.messages(session_id_);

  // Render only the tail window. The rest stays in memory but out of the
  // renderer, so the per-reload cost is bounded no matter how long the
  // conversation grows. Older pages are paged in on demand.
  const std::size_t limit = static_cast<std::size_t>(std::max(1, settings_.rendered_message_limit));
  window_start_ = messages_.size() > limit ? messages_.size() - limit : 0;

  rebuild_transcript(anchor, follow);

  // The warning has been folded into the document; a later reload must not show
  // it again.
  pending_warning_.clear();

  set_busy(controller_.busy());
  update_send_mode();
  refresh_reasoning_selector();
}

render::Document ConversationView::build_document() const {
  render::Document document;

  for (std::size_t i = window_start_; i < messages_.size(); ++i) {
    append_rendered_message(document, messages_[i]);
  }

  // A warning from the last finished turn is rendered after the transcript, then
  // cleared by rebuild_transcript so a later reload does not show it again. A
  // failure is not shown here: its reason lives in the tasks window, and the
  // composer notice points there.
  if (!pending_warning_.isEmpty()) {
    render::MessageMeta meta;
    meta.role = render::Role::Notice;
    append_message(document, std::move(meta), make_plain_blocks(pending_warning_.toStdString()));
  }

  return document;
}

void ConversationView::append_rendered_message(
    render::Document& document, const lnb::Message& message) const {
  const bool from_self = message.role_ == lnb::Role::User;
  const render::Role role = from_self ? render::Role::User : render::Role::Assistant;

  const QString raw = from_utf8(message.content_);

  render::MessageMeta meta = make_message_meta(
      role, message.id_, to_u16(format_message_time(message.made_at_)),
      to_u16(format_token_estimate(api_.message_tokens(message))), from_self, to_u16(raw));

  auto blocks =
      from_self ? make_plain_blocks(message.content_) : make_markdown_blocks(message.content_);

  append_message(document, std::move(meta), std::move(blocks));
}

void ConversationView::rebuild_transcript(
    const TranscriptCanvas::ScrollAnchor& anchor, bool follow_bottom) {
  transcript_->set_document(build_document(), anchor, follow_bottom);
}

void ConversationView::load_older() {
  if (window_start_ == 0) {
    return;
  }

  // The canvas keeps the reader anchored on the message they were looking at, so
  // prepending a page never jumps the transcript.
  const TranscriptCanvas::ScrollAnchor anchor = transcript_->anchor();

  const std::size_t page = static_cast<std::size_t>(std::max(1, settings_.history_page_size));

  window_start_ = window_start_ > page ? window_start_ - page : 0;

  rebuild_transcript(anchor, false);
}

void ConversationView::refresh_style() {
  apply_composer_style();
  apply_composer_height();

  if (transcript_ != nullptr) {
    transcript_->refresh_style();
  }

  refresh_reasoning_selector();
}

void ConversationView::refresh_reasoning_selector() {
  // The stored value may name a level the current model no longer offers; the
  // manager resolves it to the supported set and reports what is actually shown.
  const auto stored = api_.session(session_id_);
  const std::string current = stored.has_value() ? stored->reasoning_effort_ : std::string{};
  // An empty value means "use the model's default", which the selector shows as
  // the default level rather than as a blank control.
  const std::string shown = current.empty() ? api_.default_reasoning_effort() : current;

  reasoning_selector_->set_levels(api_.reasoning_efforts_with_stored(shown), shown);
}

void ConversationView::flush_streaming_text() {
  if (!has_pending_stream_ || !streaming_started_) {
    return;
  }

  has_pending_stream_ = false;
  transcript_->set_streaming_blocks(make_markdown_blocks(pending_stream_text_.toStdString()));
}

void ConversationView::poll() {
  controller_.poll();
  set_busy(controller_.busy());
}

void ConversationView::set_failure_notice_handler(std::function<void()> handler) {
  if (notice_ != nullptr) {
    notice_->set_on_click(std::move(handler));
  }
}

void ConversationView::set_busy(bool busy) {
  // The interface runs one task at a time. A task in another session therefore
  // blocks this composer's send button too, even though this view has no task
  // of its own to show a cancel button for.
  send_button_->setEnabled(!(busy || api_.busy()));

  if (busy_ == busy) {
    return;
  }

  busy_ = busy;
  cancel_button_->setVisible(busy);

  // A new turn supersedes the last failure: drop the nudge so it cannot linger
  // over a fresh generation.
  if (busy && notice_ != nullptr) {
    notice_->setVisible(false);
  }
}

void ConversationView::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);

  // A theme change that happened while this transcript was hidden was deferred
  // (mark_style_stale); do the restyle now that it is on screen.
  if (style_stale_) {
    style_stale_ = false;
    refresh_style();
  }
}

bool ConversationView::eventFilter(QObject* watched, QEvent* event) {
  if (watched == input_ && event->type() == QEvent::KeyPress) {
    auto* key_event = static_cast<QKeyEvent*>(event);

    const bool is_enter = key_event->key() == Qt::Key_Return || key_event->key() == Qt::Key_Enter;

    if (settings_.enter_with_modifier) {
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

void ConversationView::prompt_edit(std::uint32_t message_id, const QString& current) {
  bool accepted = false;

  const QString edited = QInputDialog::getMultiLineText(
      this, trs("Edit message"), trs("Message:"), current, &accepted);

  if (accepted && edited != current) {
    controller_.edit_message(message_id, edited.toStdString());
  }
}

void ConversationView::apply_composer_style() {
  const ThemeColors& colors = theme().colors;

  // The conversation surface. A theme with a background image keeps it fully
  // transparent, so the tinted image shows through; a plain theme uses its
  // window colour.
  setStyleSheet(QStringLiteral("QWidget#ConversationView { background: %1; }")
                    .arg(css_color(colors.conversation_background)));

  // Zed-like composer: the field is bare text on the conversation surface, and
  // the whole block is set off from the transcript by a single hairline. No field
  // chrome and no rounded corners. The field's own background is themeable.
  input_->setStyleSheet(QStringLiteral("QPlainTextEdit#Composer { background: %1; border: none;"
                                       " padding: 4px 2px; color: %2; }")
                            .arg(css_color(colors.input_background), css_color(colors.text)));

  if (composer_bar_ != nullptr) {
    // The action buttons are flat and borderless; a hover fill is the only chrome
    // they carry, the way Zed's composer actions behave.
    composer_bar_->setStyleSheet(
        QStringLiteral("QWidget#ComposerBar { background: transparent;"
                       " border-top: 1px solid %1; }"
                       "QToolButton#SendButton, QToolButton#ComposerExpand,"
                       "QToolButton#CancelButton {"
                       "  background: transparent; border: none; border-radius: 4px; padding: 0; }"
                       "QToolButton#SendButton:hover, QToolButton#ComposerExpand:hover,"
                       "QToolButton#CancelButton:hover {"
                       "  background: %2; }")
            .arg(css_color(colors.border), css_color(row_hover_background())));
  }

  apply_composer_icons();
}

void ConversationView::apply_composer_icons() {
  const ThemeColors& colors = theme().colors;

  if (send_button_ != nullptr) {
    if (summarize_mode_) {
      // The action becomes a collapse-into-memory: fold the overflow now rather
      // than send a message into an over-long request.
      QIcon icon = resource_icon(QStringLiteral("summarize"), colors.accent, colors.text_muted);

      if (icon.isNull()) {
        icon = style()->standardIcon(QStyle::SP_DialogSaveButton);
      }

      send_button_->setIcon(icon);
      send_button_->setToolTip(trs("Summarize the conversation into memory"));
    } else {
      // The send arrow takes the accent colour; a busy composer mutes it through
      // the icon's disabled pixmap.
      QIcon icon = resource_icon(QStringLiteral("send"), colors.accent, colors.text_muted);

      if (icon.isNull()) {
        icon = style()->standardIcon(QStyle::SP_ArrowUp);
      }

      send_button_->setIcon(icon);
      send_button_->setToolTip(trs("Send"));
    }
  }

  if (expand_button_ != nullptr) {
    const bool expanded = composer_expanded_;
    QIcon icon = resource_icon(
        expanded ? QStringLiteral("collapse") : QStringLiteral("expand"), colors.text_muted,
        colors.text_muted);

    if (icon.isNull()) {
      icon = style()->standardIcon(expanded ? QStyle::SP_ArrowDown : QStyle::SP_ArrowUp);
    }

    expand_button_->setIcon(icon);
    expand_button_->setToolTip(expanded ? trs("Shrink the composer") : trs("Expand the composer"));
  }

  if (cancel_button_ != nullptr) {
    // The stop square shares the muted colour of the other chrome actions.
    QIcon icon = resource_icon(QStringLiteral("stop"), colors.text_muted, colors.text_muted);

    if (icon.isNull()) {
      icon = style()->standardIcon(QStyle::SP_MediaStop);
    }

    cancel_button_->setIcon(icon);
    cancel_button_->setToolTip(trs("Cancel"));
  }
}

void ConversationView::apply_composer_height() {
  if (input_ != nullptr) {
    const lnb::ThemeMetrics& m = metrics();
    input_->setFixedHeight(composer_expanded_ ? m.composer_height_expanded : m.composer_height);
  }

  apply_composer_icons();
}

void ConversationView::toggle_composer_height() {
  composer_expanded_ = !composer_expanded_;
  apply_composer_height();
}

bool ConversationView::summarize_due() const {
  const lnb::Session session = controller_.session();

  if (session.id_ == 0) {
    return false;
  }

  // The decision belongs to the manager, not the widget: the session's own
  // overflow first, then the shared assistant trigger (or an assistant fold
  // already in flight). Every composer therefore reports the same action, and the
  // send button is banned the moment either trigger is met.
  return api_.has_session_memory_trigger_met(session_id_) != lnb::api::error_code::SUCCESS;
}

void ConversationView::refresh_send_mode() { update_send_mode(); }

void ConversationView::update_send_mode() {
  const bool due = summarize_due();

  if (due == summarize_mode_) {
    return;
  }

  summarize_mode_ = due;
  apply_composer_icons();
}

void ConversationView::summarize_session_now(lnb::SummaryApprover approver) {
  if (controller_.busy() || api_.busy()) {
    return;
  }

  // A manual fold is a fresh pass, so land at the bottom like the composer's
  // summarize action. The manager bans the composer's send/summarize button (and
  // the stop action replaces it) for the whole pass, so the fold cannot be
  // re-triggered while it runs.
  transcript_->scroll_to_bottom();
  controller_.summarize_session_now(std::move(approver));
  set_busy(controller_.busy());
}

void ConversationView::submit() {
  if (controller_.busy() || api_.busy()) {
    return;
  }

  // Re-derive the primary action first, so a trigger that flipped since the last
  // refresh cannot make the composer send into a fold the manager has already
  // called for.
  update_send_mode();

  if (summarize_mode_) {
    // The send button has become a summarize button: fold the overflow into
    // memory, sending nothing. A fresh pass lands at the bottom.
    transcript_->scroll_to_bottom();
    const lnb::SummaryApprover approver =
        [](const lnb::SummaryPlan& plan) { return review_summary(plan); };
    controller_.summarize(approver);
    set_busy(controller_.busy());
    return;
  }

  const QString text = input_->toPlainText().trimmed();

  if (text.isEmpty()) {
    return;
  }

  // A freshly submitted message is content the writer wants to see, so jump back
  // to the bottom even if they had scrolled away; the reload triggered by send()
  // then follows the reply. The field is only cleared once the manager accepted
  // the turn, so a refused send never loses the text. set_busy reflects whether a
  // task actually started, so a refused one never wedges the composer.
  transcript_->scroll_to_bottom();

  if (controller_.send(text.toStdString())) {
    input_->clear();
  }

  set_busy(controller_.busy());
}

} // namespace ui
