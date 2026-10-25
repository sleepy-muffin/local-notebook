#pragma once

#include <QDialog>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class QEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QVBoxLayout;
class QWidget;

namespace ui {

// A unique, non-modal window listing generation tasks: the ones running now and
// the last finished one per slot (one per session, plus the shared assistant
// memory). A failed task is never silently dropped, so its reason stays visible;
// clicking any row dumps that task's own diagnostic log (the exact request body,
// the tool calls, and any hidden reasoning the model produced — none of which the
// transcript shows). A running task carries a Cancel button, the only place a
// shared assistant fold can be stopped.
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
    std::size_t output_budget = 0;
    // False when a request reported no usage and the figures are local estimates.
    bool usage_measured = true;
    std::int64_t elapsed_ms = 0;
    // The failure reason; empty unless the task failed.
    QString error;
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
    QLabel* error = nullptr;
    QPushButton* cancel = nullptr;
  };

  void apply_style();
  void rebuild_rows(std::size_t count);
  void open_log(std::size_t index);

  QWidget* list_ = nullptr;
  QVBoxLayout* list_layout_ = nullptr;
  QLabel* empty_ = nullptr;
  std::vector<Row> rows_;

  // The entries currently shown, so a click can map a card back to its task.
  std::vector<Entry> entries_;

  std::function<QString(std::uint64_t)> log_provider_;
  std::function<void(std::uint64_t)> cancel_handler_;
};

} // namespace ui
