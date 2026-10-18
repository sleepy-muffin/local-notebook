#pragma once

#include <QFrame>
#include <QStyle>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/ConversationController.hpp"

class QEnterEvent;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace lnb {
struct Settings;
}

namespace ui {

class ReasoningSelector;
class StatusNotice;

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
  // it. The bundled icon named `icon_name` is preferred; fallback_icon is used
  // when the artwork is missing.
  QToolButton* add_action(
      const QString& icon_name, QStyle::StandardPixmap fallback_icon, const QString& tip
  );

  void set_actions_visible(bool visible);

  // Re-applies the action icons for the active theme, so a theme change takes
  // effect without rebuilding the transcript.
  void refresh_action_icons();

  // Records the stored message id and shows it (as "#<id>") alongside the hover
  // actions. Ids of 0 (streaming/notice rows) leave the label empty.
  void set_message_id(std::uint32_t id);

  // Records the message's arrival time and shows it next to the id. An empty
  // string leaves the label blank.
  void set_message_time(const QString& time);

  // Records the estimated token cost of the message (its text plus the small
  // per-message overhead — the same estimate the title bar sums) and shows it
  // next to the time, so the strip reads "#3  2026-10-16 14:32  1.2k tok".
  void set_message_tokens(std::size_t tokens);

protected:
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;

private:
  // One hover action's icon, remembered so the strip can be recoloured when the
  // theme changes.
  struct ActionIcon {
    QToolButton* button = nullptr;
    QString name;
    QStyle::StandardPixmap fallback = QStyle::SP_CustomBase;
  };

  void apply_action_icon(ActionIcon& entry);

  MessageBubble::Role role_;
  MessageBubble* bubble_ = nullptr;
  QWidget* actions_ = nullptr;
  QLabel* message_id_label_ = nullptr;
  QLabel* message_time_label_ = nullptr;
  QLabel* message_tokens_label_ = nullptr;
  std::uint32_t message_id_ = 0;
  bool actions_visible_ = false;
  std::vector<ActionIcon> action_icons_;
};

// The chat pane for one session: transcript on top, composer at the bottom. The
// view is passive — it renders the stored transcript and forwards intent to its
// ConversationController, which owns the background worker for the session's
// turn. Switching sessions therefore never interrupts a generation.
class ConversationView : public QWidget {
public:
  ConversationView(
      lnb::api::interface& api, const lnb::Settings& settings,
      std::uint32_t session_id, QWidget* parent = nullptr
  );

  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }
  [[nodiscard]] bool busy() const { return controller_.busy(); }

  // Progress of the session's in-flight turn, for the running-task readout.
  [[nodiscard]] ConversationController::Progress progress() const {
    return controller_.progress();
  }

  // Rebuilds the transcript from the database.
  void reload();

  // Unloads the oldest rendered rows so the number of rendered message rows stays
  // within the configured initial rendering amount, leaving room for `reserved`
  // extra rows (an in-flight streaming row). Called whenever new content arrives,
  // so a long conversation never keeps more than the configured amount rendered;
  // history revealed by paging is temporary until the next arrival.
  void enforce_render_limit(std::size_t reserved = 0);

  // Re-applies theme colours to the composer and every rendered bubble.
  void refresh_style();

  // True when the unsummarised transcript has reached the session trigger, so the
  // composer's primary action is "summarize" rather than "send".
  [[nodiscard]] bool summarize_due() const;

  // Re-evaluates the primary action and swaps the button when the answer changed.
  // Called when a shared trigger (the assistant memory) may have flipped, so a
  // turn in another session can update this composer too.
  void refresh_send_mode();

  // Folds the session's transcript into memory on demand as the session's single
  // task, forcing the pass even when the automatic trigger has not been reached.
  // The session editor's "Summarize now" drives this, so the fold shows up in the
  // tasks window like any other turn and never races a generation on the same
  // session. `approver` reviews the request; an absent one reviews interactively.
  void summarize_session_now(lnb::SummaryApprover approver = {});

  // The diagnostic log for the current (or last finished) turn, and the last
  // finished turn's snapshot; both feed the tasks window.
  [[nodiscard]] std::string task_log() const { return controller_.task_log(); }
  [[nodiscard]] bool has_finished_task() const {
    return controller_.has_finished_task();
  }
  [[nodiscard]] lnb::GenerationTask::Snapshot last_snapshot() const {
    return controller_.last_snapshot();
  }

  // Marks the view as needing a restyle without doing the work now. A hidden
  // view (another session's transcript) is restyled when it is next shown, so a
  // settings save never re-renders every cached conversation at once.
  void mark_style_stale() { style_stale_ = true; }

  // Called by the window's poll timer; advances the in-flight generation.
  void poll();

  // Wires the failure notice's click action to the window that owns the status
  // bar's tasks icon, so the notice is a shortcut back to the reasons.
  void set_failure_notice_handler(std::function<void()> handler);

  // Called after every reload, so the window can refresh the session list, the
  // title bar and the context readout.
  void set_on_changed(std::function<void()> handler) {
    on_changed_ = std::move(handler);
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void showEvent(QShowEvent* event) override;

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

  // Re-evaluates whether the primary composer action is "summarize" and, when it
  // changed, swaps the send button's icon and tooltip.
  void update_send_mode();

  // Reloads the composer's reasoning dropdown from the stored session and the
  // assistant's model. Called on every reload and after a theme change.
  void refresh_reasoning_selector();

  // (Re)applies the two composer action icons for the active theme.
  void apply_composer_icons();

  lnb::api::interface& api_;
  const lnb::Settings& settings_;
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

  // True when a theme change happened while this view was hidden, so the restyle
  // is deferred until it is shown.
  bool style_stale_ = false;

  // Cached left/right inset of the transcript layout; -1 until first computed.
  int content_side_margin_ = -1;

  QPlainTextEdit* input_ = nullptr;
  QToolButton* send_button_ = nullptr;
  QToolButton* expand_button_ = nullptr;
  QToolButton* cancel_button_ = nullptr;

  // The reasoning-effort control, sitting to the left of the send button. It
  // writes the chosen level straight to the session's row.
  ReasoningSelector* reasoning_selector_ = nullptr;

  QWidget* composer_bar_ = nullptr;

  // The small notice above the composer that points at the status bar's tasks
  // icon after a failed turn. Hidden while no turn has failed.
  StatusNotice* notice_ = nullptr;

  // True while the composer field is raised to composer_height_expanded.
  bool composer_expanded_ = false;

  // True while the primary composer action is "summarize" (the transcript has
  // reached the trigger), rather than "send".
  bool summarize_mode_ = false;

  MessageRow* streaming_row_ = nullptr;
  QString streaming_status_;

  // Coalesces rapid streaming updates so Markdown is not re-parsed on every poll
  // tick; the pending text is rendered when the timer fires.
  QTimer* stream_timer_ = nullptr;
  QString pending_stream_text_;
  bool has_pending_stream_ = false;

  // A non-fatal warning from the finished turn, rendered after the transcript on
  // the next reload (e.g. a truncated reply).
  QString pending_warning_;

  std::function<void()> on_changed_;
};

} // namespace ui
