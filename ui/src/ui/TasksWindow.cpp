#include "ui/TasksWindow.hpp"

#include <QEvent>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>

#include "support/i18n.hpp"
#include "ui/dialog_style.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/theme.hpp"

namespace ui {

using namespace dialog_style;

namespace {

// The running (or finished) time of a task, to one decimal of a second.
QString format_elapsed(std::int64_t milliseconds) {
  return trs("%1 s").arg(static_cast<double>(milliseconds) / 1000.0, 0, 'f', 1);
}

// A read-only, scrollable, plain-text dump of one task's log. It deliberately
// never renders Markdown: the log is raw diagnostic text, and it carries detail
// the transcript never shows (the exact request body, the tool calls, and the
// hidden reasoning).
void show_log(QWidget* parent, const QString& title, const QString& log) {
  QDialog dialog(parent);
  dialog.setObjectName(QStringLiteral("TaskLogDialog"));
  dialog.setWindowTitle(title);
  dialog.setModal(true);
  dialog.resize(760, 560);
  dialog.setMinimumSize(420, 300);
  dialog.setFont(ui_font());

  auto* root = new QVBoxLayout(&dialog);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(12);

  auto* heading = new QLabel(title, &dialog);
  heading->setObjectName(QStringLiteral("DialogTitle"));
  apply_derived_font(heading, 1.0, true);
  root->addWidget(heading);

  auto* view = new QPlainTextEdit(&dialog);
  view->setObjectName(QStringLiteral("TaskLog"));
  view->setReadOnly(true);
  view->setLineWrapMode(QPlainTextEdit::NoWrap);
  view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  view->setPlainText(log.isEmpty() ? trs("This task produced no log.") : log);
  root->addWidget(view, 1);

  auto* footer = new QHBoxLayout();
  footer->addStretch(1);

  auto* close = new QPushButton(trs("Close"), &dialog);
  close->setObjectName(QStringLiteral("Secondary"));
  QObject::connect(close, &QPushButton::clicked, &dialog, [&dialog]() { dialog.accept(); });
  footer->addWidget(close);

  root->addLayout(footer);

  dialog.setStyleSheet(dialog_stylesheet(QStringLiteral("TaskLogDialog")));
  reapply_derived_fonts(&dialog);

  dialog.exec();
}

} // namespace

TasksWindow::TasksWindow(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("TasksWindow"));
  setWindowTitle(trs("Tasks"));
  setModal(false);
  resize(460, 360);
  setMinimumSize(340, 220);
  setFont(ui_font());

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto* header = new QWidget(this);
  auto* header_layout = new QVBoxLayout(header);
  header_layout->setContentsMargins(20, 16, 20, 12);
  header_layout->setSpacing(2);

  auto* title = new QLabel(trs("Tasks"), header);
  title->setObjectName(QStringLiteral("PageTitle"));
  apply_derived_font(title, 1.0, true);

  auto* subtitle = new QLabel(
      trs(
          "Running and recently finished generation tasks. "
          "Select one to read its log."
      ),
      header
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
  install_scroll_bars(scroll, ScrollBarTrack::Window);

  list_ = new QWidget(scroll);
  list_layout_ = new QVBoxLayout(list_);
  list_layout_->setContentsMargins(16, 0, 16, 16);
  list_layout_->setSpacing(8);

  empty_ = new QLabel(trs("No tasks yet."), list_);
  empty_->setObjectName(QStringLiteral("Muted"));
  empty_->setAlignment(Qt::AlignCenter);
  list_layout_->addWidget(empty_);
  list_layout_->addStretch(1);

  scroll->setWidget(list_);
  root->addWidget(scroll, 1);

  apply_style();
}

void TasksWindow::set_log_provider(std::function<QString(std::uint64_t)> provider) {
  log_provider_ = std::move(provider);
}

void TasksWindow::set_cancel_handler(std::function<void(std::uint64_t)> handler) {
  cancel_handler_ = std::move(handler);
}

void TasksWindow::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  apply_style();
}

bool TasksWindow::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::MouseButtonRelease) {
    auto* mouse = static_cast<QMouseEvent*>(event);

    if (mouse->button() == Qt::LeftButton) {
      const QVariant index = watched->property("lnbRowIndex");

      if (index.isValid()) {
        open_log(static_cast<std::size_t>(index.toInt()));
        return true;
      }
    }
  }

  return QDialog::eventFilter(watched, event);
}

void TasksWindow::open_log(std::size_t index) {
  if (index >= entries_.size()) {
    return;
  }

  const Entry& entry = entries_[index];
  const QString title = entry.title.isEmpty()
      ? trs("Session #%1").arg(entry.session_id)
      : entry.title;

  const QString log =
      log_provider_ != nullptr ? log_provider_(entry.id) : QString();

  show_log(this, title, log);
}

void TasksWindow::apply_style() {
  QString sheet = dialog_stylesheet(QStringLiteral("TasksWindow"));

  const ThemeColors& c = theme().colors;

  sheet += QStringLiteral(
               "QFrame#Card { background: %1; border: 1px solid %2; border-radius: 8px; }")
               .arg(colour_hex(c.surface), colour_hex(c.border));
  // A clickable card: the border lifts to the accent while the pointer is over it.
  sheet += QStringLiteral("QFrame#Card:hover { border: 1px solid %1; }")
               .arg(colour_hex(c.accent));
  sheet += QStringLiteral("QLabel#TaskError { color: %1; }").arg(colour_hex(c.danger));
  sheet += QStringLiteral("QLabel#TaskState { color: %1; }").arg(colour_hex(c.accent));

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
    card->setCursor(Qt::PointingHandCursor);
    card->setProperty("lnbRowIndex", static_cast<int>(i));
    card->installEventFilter(this);

    auto* outer = new QHBoxLayout(card);
    outer->setContentsMargins(14, 10, 14, 10);
    outer->setSpacing(10);

    // The text column is a layout, not a widget, so a click on any label still
    // reaches the card beneath and opens the log.
    auto* column = new QVBoxLayout();
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);

    auto* row_title = new QLabel(card);
    row_title->setObjectName(QStringLiteral("CurrentName"));

    auto* row_detail = new QLabel(card);
    row_detail->setObjectName(QStringLiteral("Muted"));

    auto* row_error = new QLabel(card);
    row_error->setObjectName(QStringLiteral("TaskError"));
    row_error->setWordWrap(true);

    // The labels must not swallow the click; the card owns the press.
    for (QLabel* label : {row_title, row_detail, row_error}) {
      label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    column->addWidget(row_title);
    column->addWidget(row_detail);
    column->addWidget(row_error);

    // The stop action for a running task. It is the only place a shared assistant
    // fold can be cancelled. The index is captured, so the handler always sees the
    // current entry even though the rows are reused across refreshes.
    auto* cancel = new QPushButton(trs("Cancel"), card);
    cancel->setObjectName(QStringLiteral("Secondary"));
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setVisible(false);

    QObject::connect(cancel, &QPushButton::clicked, this, [this, i]() {
      if (i < entries_.size() && cancel_handler_ != nullptr) {
        cancel_handler_(entries_[i].id);
      }
    });

    outer->addLayout(column, 1);
    outer->addWidget(cancel, 0, Qt::AlignVCenter);

    list_layout_->insertWidget(static_cast<int>(i), card);
    rows_[i] = Row{card, row_title, row_detail, row_error, cancel};
  }
}

void TasksWindow::set_entries(std::vector<Entry> entries) {
  if (entries.size() != rows_.size()) {
    rebuild_rows(entries.size());
  }

  entries_ = std::move(entries);

  for (std::size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];

    rows_[i].title->setText(entry.title);

    // The shared assistant task has no session, so its row reads without the
    // "Session #0" prefix.
    const QString detail = entry.session_id == 0
        ? trs("%1   ·   Round %2   ·   %3")
              .arg(entry.state)
              .arg(entry.rounds)
              .arg(format_elapsed(entry.elapsed_ms))
        : trs("Session #%1   ·   %2   ·   Round %3   ·   %4")
              .arg(entry.session_id)
              .arg(entry.state)
              .arg(entry.rounds)
              .arg(format_elapsed(entry.elapsed_ms));

    rows_[i].detail->setText(detail);

    // The running row reads in the accent colour; a finished one stays muted. The
    // object name is only touched when it actually changes, so a refresh does not
    // force a re-polish on every tick.
    const QString detail_name =
        entry.running ? QStringLiteral("TaskState") : QStringLiteral("Muted");

    if (rows_[i].detail->objectName() != detail_name) {
      rows_[i].detail->setObjectName(detail_name);
      rows_[i].detail->style()->unpolish(rows_[i].detail);
      rows_[i].detail->style()->polish(rows_[i].detail);
    }

    rows_[i].error->setText(entry.error);
    rows_[i].error->setVisible(!entry.error.isEmpty());

    if (rows_[i].cancel != nullptr) {
      rows_[i].cancel->setVisible(entry.cancellable);
    }
  }

  empty_->setVisible(entries_.empty());
}

} // namespace ui
