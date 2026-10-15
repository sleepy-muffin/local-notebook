#pragma once

#include <QFrame>
#include <QStyle>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "model.hpp"
#include "ui/ConversationController.hpp"

class QEnterEvent;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace lnb {
class Application;
}

namespace ui {

// A single rendered chat message body. The user's own messages get a rounded
// block (the same surface as the composer); assistant replies are bare,
// Markdown-formatted text; notices and errors get a muted, bordered block. Every
// body occupies the full width of the conversation flow, so a message's size
// never depends on how much was written.
class MessageBubble : public QFrame {
public:
  enum class Role {
    User,
    Assistant,
    Notice, // a non-fatal remark (e.g. a truncated reply)
    Error,  // a failed turn
  };

  MessageBubble(Role role, QWidget* parent = nullptr);

  void set_text(const QString& text);

  // The raw source text (never the rendered rich text), so assistant replies can
  // be copied verbatim.
  [[nodiscard]] QString text() const { return source_; }

  // Re-applies the bubble's colours and re-renders its text, so a theme change
  // takes effect without rebuilding the transcript.
  void refresh_style();

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
    // The rounded frame of a code block, kept so a theme change can restyle it
    // without rebuilding the block.
    QFrame* frame = nullptr;
  };

  void apply_style();
  [[nodiscard]] std::vector<Segment> parse_segments(const QString& markdown) const;
  void set_markdown(const QString& markdown);
  void rebuild_blocks(const std::vector<Segment>& segments);

  // Re-applies the code background to every code block, so a theme change reaches
  // frames that are otherwise left untouched by an in-place text update.
  void refresh_code_blocks();

  Role role_;
  bool markdown_ = false;
  // Raw source text (assistant Markdown included), so redundant updates can be
  // skipped and the body can be re-rendered on a theme change.
  QString source_;
  QLabel* text_label_ = nullptr;
  QVBoxLayout* body_layout_ = nullptr;
  std::vector<Block> blocks_;
};

// One transcript entry: a message body plus the hover-only action strip
// ("#id" alongside copy / edit / delete). The strip always occupies the same
// space, so revealing its buttons never reflows the transcript, and the buttons
// only appear while the pointer is over the entry.
class MessageRow : public QWidget {
public:
  MessageRow(MessageBubble::Role role, QWidget* parent = nullptr);

  [[nodiscard]] MessageBubble* bubble() const { return bubble_; }

  // Adds an icon-only action button and returns it so the caller can connect to
  // it. The named icon theme icon is preferred; fallback_icon is used when the
  // theme does not provide it.
  QToolButton* add_action(
      const QString& icon_name, QStyle::StandardPixmap fallback_icon, const QString& tip
  );

  void set_actions_visible(bool visible);

  // Records the stored message id and shows it (as "#<id>") alongside the hover
  // actions. Ids of 0 (streaming/notice rows) leave the label empty.
  void set_message_id(std::uint32_t id);

protected:
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;

private:
  MessageBubble::Role role_;
  MessageBubble* bubble_ = nullptr;
  QWidget* actions_ = nullptr;
  QLabel* message_id_label_ = nullptr;
  std::uint32_t message_id_ = 0;
  bool actions_visible_ = false;
};

// The chat pane for one session: transcript on top, composer at the bottom. The
// view is passive — it renders the stored transcript and forwards intent to its
// ConversationController, which owns the background worker for the session's
// turn. Switching sessions therefore never interrupts a generation.
class ConversationView : public QWidget {
public:
  ConversationView(
      lnb::Application& app, std::uint32_t session_id, QWidget* parent = nullptr
  );

  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }
  [[nodiscard]] bool busy() const { return controller_.busy(); }

  // Rebuilds the transcript from the database.
  void reload();

  // Re-applies theme colours to the composer and every rendered bubble.
  void refresh_style();

  // Called by the window's poll timer; advances the in-flight generation.
  void poll();

  // Called after every reload, so the window can refresh the session list, the
  // title bar and the context readout.
  void set_on_changed(std::function<void()> handler) {
    on_changed_ = std::move(handler);
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  MessageRow* add_row(MessageBubble::Role role, std::uint32_t message_id, bool editable);

  // Builds a row and registers it, without inserting it into the layout, so
  // callers can place it at an arbitrary index (used when prepending history).
  MessageRow* make_row(MessageBubble::Role role, std::uint32_t message_id, bool editable);

  // Creates the row for messages_[index] and inserts it at layout_index.
  void render_message_at(std::size_t index, int layout_index);

  // Reveals one more page of older messages above the current window, keeping
  // the reader anchored on the message they were looking at.
  void load_older();
  void update_load_older_visibility();

  void prompt_edit(std::uint32_t message_id, const QString& current);
  void clear_transcript();
  void flush_streaming_text();
  void scroll_to_bottom();
  void pin_to_bottom();
  void submit();
  void set_busy(bool busy);

  // Keeps the transcript column centred when the pane is wider than the
  // configured flow width, so the content pulls away from both edges.
  void update_content_margins();

  // Gives the composer field the same surface as the user's own message block.
  void apply_composer_style();

  // Sets the composer field's height for the current expand state and refreshes
  // the expand toggle's icon and tooltip.
  void apply_composer_height();
  void toggle_composer_height();

  // (Re)applies the two composer action icons for the active theme.
  void apply_composer_icons();

  lnb::Application& app_;
  std::uint32_t session_id_;
  ConversationController controller_;

  QScrollArea* scroll_ = nullptr;
  QWidget* transcript_ = nullptr;
  QVBoxLayout* transcript_layout_ = nullptr;

  // Full history for the session plus the trailing window currently rendered.
  // messages_ is authoritative (loaded once per reload); rendered_count_ counts
  // how many of its most recent entries are actually in the widget tree.
  std::vector<lnb::Message> messages_;
  std::size_t rendered_count_ = 0;

  // Every row currently in the layout, so a theme change can restyle them.
  std::vector<MessageRow*> rows_;

  // Reveals the next page of older messages; hidden while the whole history is
  // already rendered.
  QPushButton* load_older_ = nullptr;

  // True while the transcript is being rebuilt or paged, so the scrollbar's
  // valueChanged handler does not mistake a programmatic jump for a request to
  // load more history.
  bool updating_ = false;

  // True while the reader is parked at the bottom of the transcript. New content
  // is followed only while this holds; scrolling up suspends it until the reader
  // returns to the bottom.
  bool stick_to_bottom_ = true;

  bool busy_ = false;

  // Cached left/right inset of the transcript layout; -1 until first computed.
  int content_side_margin_ = -1;

  QPlainTextEdit* input_ = nullptr;
  QToolButton* send_button_ = nullptr;
  QToolButton* expand_button_ = nullptr;
  QToolButton* cancel_button_ = nullptr;
  QWidget* composer_bar_ = nullptr;

  // True while the composer field is raised to composer_height_expanded.
  bool composer_expanded_ = false;

  MessageRow* streaming_row_ = nullptr;
  QString streaming_status_;

  // Coalesces rapid streaming updates so Markdown is not re-parsed on every poll
  // tick; the pending text is rendered when the timer fires.
  QTimer* stream_timer_ = nullptr;
  QString pending_stream_text_;
  bool has_pending_stream_ = false;

  // Notices from the finished turn, rendered after the transcript on the next
  // reload: a failure, or a non-fatal warning such as a truncated reply.
  QString pending_error_;
  QString pending_warning_;

  std::function<void()> on_changed_;
};

} // namespace ui
