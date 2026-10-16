#include "ui/TasksWindow.hpp"

#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>

#include "ui/dialog_style.hpp"
#include "ui/theme.hpp"

namespace ui {

using namespace dialog_style;

namespace {

// The running time of a task, to one decimal of a second. A running turn is
// short-lived, so seconds are enough; a long tool loop simply reads as more
// seconds.
QString format_elapsed(std::int64_t milliseconds) {
  return QStringLiteral("%1 s").arg(static_cast<double>(milliseconds) / 1000.0, 0, 'f', 1);
}

} // namespace

TasksWindow::TasksWindow(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("TasksWindow"));
  setWindowTitle(QStringLiteral("Running Tasks"));
  setModal(false);
  resize(420, 320);
  setMinimumSize(320, 200);
  setFont(ui_font());

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto* header = new QWidget(this);
  auto* header_layout = new QVBoxLayout(header);
  header_layout->setContentsMargins(20, 16, 20, 12);
  header_layout->setSpacing(2);

  auto* title = new QLabel(QStringLiteral("Running Tasks"), header);
  title->setObjectName(QStringLiteral("PageTitle"));
  apply_derived_font(title, 1.0, true);

  auto* subtitle = new QLabel(
      QStringLiteral("Generation tasks running in the background."), header
  );
  subtitle->setObjectName(QStringLiteral("PageSubtitle"));
  subtitle->setWordWrap(true);

  header_layout->addWidget(title);
  header_layout->addWidget(subtitle);
  root->addWidget(header);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  list_ = new QWidget(scroll);
  list_layout_ = new QVBoxLayout(list_);
  list_layout_->setContentsMargins(16, 0, 16, 16);
  list_layout_->setSpacing(8);

  empty_ = new QLabel(QStringLiteral("No tasks are running."), list_);
  empty_->setObjectName(QStringLiteral("Muted"));
  empty_->setAlignment(Qt::AlignCenter);
  list_layout_->addWidget(empty_);
  list_layout_->addStretch(1);

  scroll->setWidget(list_);
  root->addWidget(scroll, 1);

  apply_style();
}

void TasksWindow::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  apply_style();
}

void TasksWindow::apply_style() {
  QString sheet = dialog_stylesheet(QStringLiteral("TasksWindow"));
  sheet += QStringLiteral(
               "QFrame#Card { background: %1; border: 1px solid %2; border-radius: 8px; }")
               .arg(colour_hex(theme().colors.surface), colour_hex(theme().colors.border));
  setStyleSheet(sheet);

  // The window is cached by the main window, so re-derive its fonts from the
  // current base font on every show, exactly as the settings window does.
  reapply_derived_fonts(this);
}

void TasksWindow::rebuild_rows(std::size_t count) {
  for (const Row& row : rows_) {
    if (row.widget != nullptr) {
      list_layout_->removeWidget(row.widget);
      row.widget->deleteLater();
    }
  }

  rows_.assign(count, Row{});

  // Rows are inserted before the trailing stretch (the last layout item), so the
  // empty-state label keeps its place at the top of the list.
  for (std::size_t i = 0; i < count; ++i) {
    auto* card = new QFrame(list_);
    card->setObjectName(QStringLiteral("Card"));
    card->setAttribute(Qt::WA_StyledBackground, true);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(2);

    auto* row_title = new QLabel(card);
    row_title->setObjectName(QStringLiteral("CurrentName"));

    auto* row_detail = new QLabel(card);
    row_detail->setObjectName(QStringLiteral("Muted"));

    layout->addWidget(row_title);
    layout->addWidget(row_detail);

    list_layout_->insertWidget(static_cast<int>(i), card);
    rows_[i] = Row{card, row_title, row_detail};
  }
}

void TasksWindow::set_entries(std::vector<Entry> entries) {
  if (entries.size() != rows_.size()) {
    rebuild_rows(entries.size());
  }

  for (std::size_t i = 0; i < entries.size(); ++i) {
    const Entry& entry = entries[i];

    rows_[i].title->setText(entry.title);
    rows_[i].detail->setText(
        QStringLiteral("Session #%1   ·   Round %2   ·   %3")
            .arg(entry.session_id)
            .arg(entry.rounds)
            .arg(format_elapsed(entry.elapsed_ms))
    );
  }

  empty_->setVisible(entries.empty());
}

} // namespace ui
