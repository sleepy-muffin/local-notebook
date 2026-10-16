#pragma once

#include <QDialog>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class QEvent;
class QLabel;
class QShowEvent;
class QVBoxLayout;
class QWidget;

namespace ui {

// A unique, non-modal window listing generation tasks: the ones running now and
// the last finished one per session. A failed task is never silently dropped, so
// its reason stays visible; clicking any row dumps that task's own diagnostic log
// (the exact request body, the tool calls, and any hidden reasoning the model
// produced — none of which the transcript shows).
//
// The main window owns one and refreshes it from its poll timer, so it never
// blocks the window it belongs to.
class TasksWindow : public QDialog {
public:
  // One task: its session, its state, and the progress the worker reported.
  struct Entry {
    std::uint32_t session_id = 0;
    QString title;
    bool running = false;
    // "Running", "Completed", "Failed" or "Cancelled".
    QString state;
    std::size_t rounds = 0;
    std::int64_t elapsed_ms = 0;
    // The failure reason; empty unless the task failed.
    QString error;
  };

  explicit TasksWindow(QWidget* parent = nullptr);

  // Supplies the diagnostic log for a session on demand, so the (potentially
  // large) log is copied only when the reader actually opens it.
  void set_log_provider(std::function<QString(std::uint32_t)> provider);

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
    QLabel* detail = nullptr;
    QLabel* error = nullptr;
  };

  void apply_style();
  void rebuild_rows(std::size_t count);
  void open_log(std::size_t index);

  QWidget* list_ = nullptr;
  QVBoxLayout* list_layout_ = nullptr;
  QLabel* empty_ = nullptr;
  std::vector<Row> rows_;

  // The entries currently shown, so a click can map a card back to its session.
  std::vector<Entry> entries_;

  std::function<QString(std::uint32_t)> log_provider_;
};

} // namespace ui
