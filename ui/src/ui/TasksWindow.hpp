#pragma once

#include <QDialog>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "local_notebook/core/generation_task.hpp"

class QEvent;
class QLabel;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace ui {

// One readable item of a task's diagnostic log. The log itself is line oriented
// (`[request …]` lines followed by their body, tool calls, ...); it is parsed
// into these so the window can show a list instead of a wall of unwrapped text.
// `raw` keeps the exact bytes, which is what the row's Copy button hands to an
// editor.
struct TaskLogEntry {
  enum class Kind {
    Request,
    Response,
    ToolCall,
    ToolResult,
    Reasoning,
    Error,
    Note,
  };

  Kind kind = Kind::Note;
  // A short category label ("Request", "Tool call", ...).
  QString badge;
  // The primary line: the request kind, the tool name, or the note itself.
  QString title;
  // A right-aligned secondary figure (the body size, in characters).
  QString detail;
  // A wrapped, readable excerpt. Empty when the entry has nothing to preview.
  QString preview;
  // The entry verbatim, including any body that is not shown inline.
  QString raw;
};

// A unique, non-modal window listing generation tasks: the ones running now and
// the last finished one per slot (one per session, plus the shared assistant
// memory). A failed task is never silently dropped: its reason is part of the
// task's diagnostic log, so clicking the row shows it in the right-hand pane
// along with the requests that led to it (the exact request body, the tool calls,
// and any hidden reasoning the model produced — none of which the transcript
// shows). A running task carries a Cancel button, the only place a shared
// assistant fold can be stopped.
//
// The log is shown as a list of entries rather than a text dump: request bodies
// are never rendered inline (they are long and unreadable), but every entry
// carries a Copy button so the exact text can be pasted into an editor.
//
// The main window owns one and refreshes it from its poll timer, so it never
// blocks the window it belongs to.
class TasksWindow : public QDialog {
public:
  // One task: its slot, its state, and the progress the worker reported.
  struct Entry {
    // Identifies the task in the manager (used for the log and the cancel).
    std::uint64_t id = 0;
    // The owning session, or 0 for the shared assistant task.
    std::uint32_t session_id = 0;
    QString title;
    bool running = false;
    // True while the task may still be stopped.
    bool cancellable = false;
    // "Running", "Completed", "Failed" or "Cancelled".
    QString state;
    std::size_t rounds = 0;
    // What the turn billed so far. `output_tokens` is only what the model
    // generated; `tool_tokens` is the cost of the tool results it read back and
    // is charged against the same `output_budget`.
    std::size_t input_tokens = 0;
    std::size_t output_tokens = 0;
    std::size_t tool_tokens = 0;
    // The limit the produced tokens are charged against, and which configured
    // budget it is: a chat turn's response budget, a session summary's
    // summarization budget, or the assistant memory pass's memory budget.
    std::size_t output_budget = 0;
    lnb::core::GenerationTask::BudgetKind output_budget_kind =
        lnb::core::GenerationTask::BudgetKind::NONE;
    // False when a request reported no usage and the figures are local estimates.
    bool usage_measured = true;
    std::int64_t elapsed_ms = 0;
  };

  explicit TasksWindow(QWidget* parent = nullptr);

  // Supplies the diagnostic log for a task on demand, so the (potentially large)
  // log is copied only when the reader actually opens it.
  void set_log_provider(std::function<QString(std::uint64_t)> provider);

  // Handles a Cancel request for a task. The window stays passive: the main
  // window forwards this to the task manager.
  void set_cancel_handler(std::function<void(std::uint64_t)> handler);

  // Replaces the list. Reuses the existing rows when the count is unchanged, so a
  // timer-driven refresh does not rebuild the widgets on every tick.
  void set_entries(std::vector<Entry> entries);

protected:
  void showEvent(QShowEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  struct Row {
    QWidget* widget = nullptr;
    QLabel* title = nullptr;
    QLabel* id = nullptr;
    QLabel* tokens = nullptr;
    QLabel* detail = nullptr;
    QPushButton* cancel = nullptr;
  };

  struct LogRow {
    QWidget* widget = nullptr;
    QLabel* title = nullptr;
    QLabel* detail = nullptr;
    QLabel* preview = nullptr;
  };

  void apply_style();
  void rebuild_rows(std::size_t count);

  // Shows `index`'s diagnostic log in the right-hand pane. The log is composed
  // here, on demand, so a task nobody selects costs nothing.
  void select_row(std::size_t index);
  void refresh_log();

  // Turns the log text into list rows. The rows are rebuilt only when the text
  // actually changed, so a poll tick never disturbs the reader.
  void rebuild_log_rows(const QString& text);
  void clear_log_rows();

  // Marks the selected card with the accent border, so the list and the pane
  // agree on which task is being read.
  void update_selection_style();

  QWidget* list_pane_ = nullptr;
  QWidget* list_ = nullptr;
  QVBoxLayout* list_layout_ = nullptr;
  QLabel* empty_ = nullptr;
  std::vector<Row> rows_;

  // The right-hand pane: the selected task's name and its parsed log list.
  QLabel* log_title_ = nullptr;
  QToolButton* log_copy_ = nullptr;
  QScrollArea* log_scroll_ = nullptr;
  QWidget* log_list_ = nullptr;
  QVBoxLayout* log_list_layout_ = nullptr;
  QLabel* log_placeholder_ = nullptr;

  std::vector<TaskLogEntry> log_entries_;
  std::vector<LogRow> log_rows_;
  // The log text currently rendered, so an unchanged poll tick is a no-op.
  QString log_text_;
  bool log_shown_ = false;

  // Index into `entries_`, or `npos` while nothing is selected.
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);
  std::size_t selected_ = npos;

  // The entries currently shown, so a click can map a card back to its task.
  std::vector<Entry> entries_;

  std::function<QString(std::uint64_t)> log_provider_;
  std::function<void(std::uint64_t)> cancel_handler_;
};

} // namespace ui
