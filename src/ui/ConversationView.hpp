#pragma once

#include "../ui.hpp"

#include <QFrame>
#include <QStyle>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QEnterEvent;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace ui {

// A single rendered chat message body. User messages get a rounded bubble;
// assistant replies are rendered as bare, lightly Markdown-formatted text (like
// ChatGPT), and error messages get a danger-coloured bubble.
class MessageBubble : public QFrame {
public:
  enum class Role {
    User,
    Assistant,
    System,
  };

  MessageBubble(Role role, QWidget* parent = nullptr);

  void set_text(const QString& text);
  [[nodiscard]] QString text() const;

private:
  // A reply is rendered as a stack of blocks: Markdown text blocks (rich-text
  // labels) and fenced code blocks (rounded frames holding a monospace label).
  struct Segment {
    bool code = false;
    QString text;
  };

  struct Block {
    bool code = false;
    QWidget* container = nullptr;
    QLabel* label = nullptr;
  };

  void apply_style();
  [[nodiscard]] std::vector<Segment> parse_segments(const QString& markdown) const;
  void set_markdown(const QString& markdown);
  void rebuild_blocks(const std::vector<Segment>& segments);

  Role role_;
  bool markdown_ = false;
  // Raw Markdown source (assistant only) so redundant updates can be skipped
  // even though the blocks hold the rendered rich text.
  QString source_;
  QLabel* text_label_ = nullptr;
  QVBoxLayout* body_layout_ = nullptr;
  std::vector<Block> blocks_;
};

// One transcript entry: a message body plus the hover-only action buttons
// ("Edit message" / "Delete message"). The action strip always occupies the
// same space, so revealing its buttons never reflows the transcript, and the
// buttons only appear while the pointer is over the entry.
class MessageRow : public QWidget {
public:
  MessageRow(MessageBubble::Role role, QWidget* parent = nullptr);

  [[nodiscard]] MessageBubble* bubble() const { return bubble_; }

  // Adds an icon-only action button and returns it so the caller can connect to
  // it. The named icon theme icon is preferred; fallback_icon is used when the
  // theme does not provide it.
  QToolButton* add_action(
      const QString& icon_name,
      QStyle::StandardPixmap fallback_icon,
      const QString& tip
  );

  void set_actions_visible(bool visible);

protected:
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;

private:
  MessageBubble::Role role_;
  MessageBubble* bubble_ = nullptr;
  QWidget* actions_ = nullptr;
  bool actions_visible_ = false;
};

// The chat pane for one session: transcript on top, composer at the bottom.
// The view is intentionally passive - it only renders state and forwards user
// intent through callbacks. All conversation logic lives in
// ConversationController so that switching sessions never interrupts a
// generation and never loses the in-progress draft.
class ConversationView : public QWidget {
public:
  explicit ConversationView(lnb::ApplicationState& state, QWidget* parent = nullptr);

  // Rebuilds the whole transcript from scratch.
  void set_messages(const std::vector<lnb::message>& messages, uint32_t self_id);

  // Creates or updates the ephemeral bubble used to show a reply as it streams
  // in. The bubble is dropped on the next set_messages() call.
  void set_streaming_text(const QString& text);

  // Updates the temporary title shown while a tool or other background
  // operation is in progress. It is removed when assistant text arrives.
  void set_streaming_status(const QString& status);

  // Shows a transient error bubble (also dropped on the next set_messages()).
  void show_error(const QString& text);

  void set_busy(bool busy);

  void set_on_send(std::function<void(std::string)> handler);
  void set_on_cancel(std::function<void()> handler);
  void set_on_delete_message(std::function<void(uint32_t)> handler);
  void set_on_edit_message(std::function<void(uint32_t, const std::string&)> handler);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  MessageRow* add_row(MessageBubble::Role role, uint32_t message_id, bool editable);

  // Builds a row without inserting it into the layout, so callers can place it
  // at an arbitrary index (used when prepending older messages).
  MessageRow* make_row(MessageBubble::Role role, uint32_t message_id, bool editable);

  // Creates the row for messages_[index] and inserts it at layout_index.
  void render_message_at(std::size_t index, int layout_index);

  // Reveals one more page of older messages above the current window, keeping
  // the reader anchored on the message they were looking at.
  void load_older();

  // Shows or hides the "Load earlier messages" affordance and keeps its count
  // in sync with the number of messages still hidden above the window.
  void update_load_older_visibility();

  void prompt_edit(uint32_t message_id, const QString& current);
  void clear_transcript();
  void flush_streaming_text();
  void scroll_to_bottom();
  void submit();

  // Keeps the transcript column centred when the pane is wider than the
  // configured message width, so the content pulls away from both edges instead
  // of hugging the left margin.
  void update_content_margins();

  lnb::ApplicationState& state_;

  QScrollArea* scroll_ = nullptr;
  QWidget* transcript_ = nullptr;
  QVBoxLayout* transcript_layout_ = nullptr;

  // Full history for the session plus the trailing window currently rendered.
  // messages_ is authoritative (loaded once per reload); rendered_count_ counts
  // how many of its most recent entries are actually in the widget tree.
  std::vector<lnb::message> messages_;
  uint32_t self_id_ = 0;
  std::size_t rendered_count_ = 0;

  // Reveals the next page of older messages; hidden while the whole history is
  // already rendered.
  QPushButton* load_older_ = nullptr;

  // True while the transcript is being rebuilt or paged, so the scrollbar's
  // valueChanged handler does not mistake a programmatic jump for a request to
  // load more history.
  bool updating_ = false;

  // Cached left/right inset of the transcript layout; -1 until first computed.
  int content_side_margin_ = -1;

  QPlainTextEdit* input_ = nullptr;
  QPushButton* send_button_ = nullptr;
  QPushButton* cancel_button_ = nullptr;

  MessageRow* streaming_row_ = nullptr;
  QString streaming_status_;

  // Coalesces rapid streaming updates so Markdown is not re-parsed on every
  // poll tick; the pending text is rendered when the timer fires.
  QTimer* stream_timer_ = nullptr;
  QString pending_stream_text_;
  bool has_pending_stream_ = false;

  std::function<void(std::string)> on_send_;
  std::function<void()> on_cancel_;
  std::function<void(uint32_t)> on_delete_message_;
  std::function<void(uint32_t, const std::string&)> on_edit_message_;
};

} // namespace ui
