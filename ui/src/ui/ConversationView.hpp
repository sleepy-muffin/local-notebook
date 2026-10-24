#pragma once

#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "render/document.hpp"
#include "ui/ConversationController.hpp"
#include "ui/TranscriptCanvas.hpp"

class QEvent;
class QPlainTextEdit;
class QShowEvent;
class QTimer;
class QToolButton;

namespace lnb {
struct Settings;
}

namespace ui {

class ReasoningSelector;
class StatusNotice;
class TranscriptCanvas;

// The chat pane for one session: transcript on top, composer at the bottom. The
// view is passive — it renders the stored transcript and forwards intent to its
// ConversationController, which owns the background worker for the session's
// turn. Switching sessions therefore never interrupts a generation.
//
// The transcript itself is a single custom-painted canvas (TranscriptCanvas):
// messages are model data, not widgets, so a long conversation has no widget
// forest to build and selection can span every message and code block.
class ConversationView : public QWidget {
public:
  ConversationView(
      lnb::api::interface& api, const lnb::Settings& settings, std::uint32_t session_id,
      QWidget* parent = nullptr);

  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }
  [[nodiscard]] bool busy() const { return controller_.busy(); }

  // Progress of the session's in-flight turn, for the running-task readout.
  [[nodiscard]] ConversationController::Progress progress() const { return controller_.progress(); }

  // The task the session's controller is polling, so the window can map it back
  // to this view for the running-task readout.
  [[nodiscard]] std::shared_ptr<lnb::GenerationTask> task() const { return controller_.task(); }

  // Requests the session's in-flight task to stop.
  void cancel() { controller_.cancel(); }

  // Rebuilds the transcript from the database.
  void reload();

  // Re-applies theme colours to the composer and the transcript.
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
  // The session editor's "Update memory now" drives this, so the pass shows up
  // in the tasks window like any other turn and never races a generation on the
  // same session. `approver` reviews the request; an absent one reviews
  // interactively.
  void summarize_session_now(lnb::SummaryApprover approver = {});

  // The diagnostic log for the current (or last finished) turn, and the last
  // finished turn's snapshot; both feed the tasks window.
  [[nodiscard]] std::string task_log() const { return controller_.task_log(); }
  [[nodiscard]] bool has_finished_task() const { return controller_.has_finished_task(); }
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
  void set_on_changed(std::function<void()> handler) { on_changed_ = std::move(handler); }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  // Rebuilds the canvas document from messages_[window_start_..] and hands it to
  // the transcript, which keeps the reader anchored on `anchor` or follows the
  // bottom when `follow_bottom` is set.
  void rebuild_transcript(const TranscriptCanvas::ScrollAnchor& anchor, bool follow_bottom);

  // Builds the renderer document for the messages in the current window.
  [[nodiscard]] render::Document build_document() const;

  // Appends one stored message (its metadata and styled blocks) to `document`.
  void append_rendered_message(render::Document& document, const lnb::Message& message) const;

  // Reveals one more page of older messages above the current window. The
  // canvas restores the reader's place, so paging never jumps.
  void load_older();

  void prompt_edit(std::uint32_t message_id, const QString& current);
  void flush_streaming_text();
  void submit();
  void set_busy(bool busy);

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

  TranscriptCanvas* transcript_ = nullptr;

  // Full history for the session, plus the first message index of the window
  // currently handed to the canvas. Older pages are revealed on demand; the
  // number of rendered messages is therefore bounded no matter how long the
  // conversation grows.
  std::vector<lnb::Message> messages_;
  std::size_t window_start_ = 0;

  bool busy_ = false;

  // True when a theme change happened while this view was hidden, so the restyle
  // is deferred until it is shown.
  bool style_stale_ = false;

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

  // True once the in-flight reply has been added to the canvas, so only the
  // first token creates the message and later ones replace its blocks.
  bool streaming_started_ = false;
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
