#pragma once

#include <QDialog>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <vector>

class QLabel;
class QShowEvent;
class QVBoxLayout;
class QWidget;

namespace ui {

// A unique, non-modal window listing the generation tasks currently running. The
// main window owns one and refreshes it from its poll timer, so it never blocks
// the window it belongs to.
class TasksWindow : public QDialog {
public:
  // One running task: its session and the progress the worker has reported.
  struct Entry {
    std::uint32_t session_id = 0;
    QString title;
    std::size_t rounds = 0;
    std::int64_t elapsed_ms = 0;
  };

  explicit TasksWindow(QWidget* parent = nullptr);

  // Replaces the list. Reuses the existing rows when the count is unchanged, so a
  // timer-driven refresh does not rebuild the widgets on every tick.
  void set_entries(std::vector<Entry> entries);

protected:
  void showEvent(QShowEvent* event) override;

private:
  struct Row {
    QWidget* widget = nullptr;
    QLabel* title = nullptr;
    QLabel* detail = nullptr;
  };

  void apply_style();
  void rebuild_rows(std::size_t count);

  QWidget* list_ = nullptr;
  QVBoxLayout* list_layout_ = nullptr;
  QLabel* empty_ = nullptr;
  std::vector<Row> rows_;
};

} // namespace ui
