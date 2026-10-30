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
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>

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

// A compact token count: plain below a thousand, otherwise one decimal of a
// thousand. The same shorthand the title bar's summarization readout uses, so the
// two numbers read alike.
QString compact_tokens(std::size_t value) {
  if (value < 1000) {
    return QString::number(static_cast<qulonglong>(value));
  }

  const double thousands = static_cast<double>(value) / 1000.0;

  if (value % 1000 == 0 || value >= 100000) {
    return QStringLiteral("%1k").arg(qRound(thousands));
  }

  return QStringLiteral("%1k").arg(thousands, 0, 'f', 1);
}

// The configured limit a task's produced total is charged against, named after
// the setting it comes from. Empty when the task publishes no bound.
QString budget_name(lnb::GenerationTask::BudgetKind kind) {
  switch (kind) {
    case lnb::GenerationTask::BudgetKind::RESPONSE:
      return trs("response budget");
    case lnb::GenerationTask::BudgetKind::SUMMARIZATION:
      return trs("summarization budget");
    case lnb::GenerationTask::BudgetKind::MEMORY:
      return trs("memory budget");
    default:
      break;
  }

  return {};
}

} // namespace

TasksWindow::TasksWindow(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("TasksWindow"));
  setWindowTitle(trs("Tasks"));
  setModal(false);
  resize(920, 560);
  setMinimumSize(560, 320);
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

  // Two columns, like the main window: the task list on the left and the selected
  // task's log on the right. The log is a pane, never a separate dialog, so the
  // list stays visible while it is being read.
  auto* body = new QHBoxLayout();
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);

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
  body->addWidget(scroll, 2);

  body->addWidget(v_separator(this));

  auto* log_pane = new QWidget(this);
  log_pane->setObjectName(QStringLiteral("TaskLogPane"));
  auto* log_layout = new QVBoxLayout(log_pane);
  log_layout->setContentsMargins(16, 0, 16, 16);
  log_layout->setSpacing(8);

  log_title_ = new QLabel(log_pane);
  log_title_->setObjectName(QStringLiteral("TaskLogTitle"));
  apply_derived_font(log_title_, 0.0, true);

  // Raw diagnostic text: never rendered as Markdown. NoWrap keeps the request
  // bodies readable exactly as they were sent.
  log_view_ = new QPlainTextEdit(log_pane);
  log_view_->setObjectName(QStringLiteral("TaskLog"));
  log_view_->setReadOnly(true);
  log_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
  log_view_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  log_view_->setPlaceholderText(trs("Select a task to read its log."));

  log_layout->addWidget(log_title_);
  log_layout->addWidget(log_view_, 1);

  body->addWidget(log_pane, 3);

  root->addLayout(body, 1);

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
        select_row(static_cast<std::size_t>(index.toInt()));
        return true;
      }
    }
  }

  return QDialog::eventFilter(watched, event);
}

void TasksWindow::select_row(std::size_t index) {
  if (index >= entries_.size()) {
    return;
  }

  selected_ = index;
  update_selection_style();
  refresh_log();
}

void TasksWindow::refresh_log() {
  if (log_view_ == nullptr) {
    return;
  }

  if (selected_ >= entries_.size()) {
    log_title_->clear();
    log_view_->clear();
    return;
  }

  const Entry& entry = entries_[selected_];

  log_title_->setText(
      entry.title.isEmpty() ? trs("Session #%1").arg(entry.session_id) : entry.title
  );

  const QString log =
      log_provider_ != nullptr ? log_provider_(entry.id) : QString();
  const QString text = log.isEmpty() ? trs("This task produced no log.") : log;

  // The window refreshes on every poll tick, but rewriting identical text would
  // reset the reader's scroll position for nothing.
  if (text == log_view_->toPlainText()) {
    return;
  }

  // Keep the reader's place: follow the tail only when it was already at the
  // bottom, otherwise restore the previous offset after the rewrite.
  QScrollBar* bar = log_view_->verticalScrollBar();
  const bool at_bottom = bar->value() >= bar->maximum() - 2;
  const int offset = bar->value();

  log_view_->setPlainText(text);

  if (at_bottom) {
    bar->setValue(bar->maximum());
  } else {
    bar->setValue(std::min(offset, bar->maximum()));
  }
}

void TasksWindow::update_selection_style() {
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    QWidget* card = rows_[i].widget;

    if (card == nullptr) {
      continue;
    }

    const bool selected = i == selected_;

    if (card->property("lnbSelected").toBool() == selected) {
      continue;
    }

    card->setProperty("lnbSelected", selected);
    card->style()->unpolish(card);
    card->style()->polish(card);
  }
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
  // The card whose log is shown in the right-hand pane keeps the accent border.
  sheet += QStringLiteral("QFrame#Card[lnbSelected=\"true\"] { border: 1px solid %1; }")
               .arg(colour_hex(c.accent));
  sheet += QStringLiteral("QLabel#TaskLogTitle { color: %1; }")
               .arg(colour_hex(c.text));
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

    // Row 1: the session name, with its id pushed to the trailing edge. The id
    // label is right-aligned in the layout's direction, so it follows the
    // interface direction instead of assuming one.
    auto* heading = new QHBoxLayout();
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(8);

    auto* row_title = new QLabel(card);
    row_title->setObjectName(QStringLiteral("CurrentName"));

    auto* row_id = new QLabel(card);
    row_id->setObjectName(QStringLiteral("Muted"));
    row_id->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    heading->addWidget(row_title);
    heading->addStretch(1);
    heading->addWidget(row_id);

    // Row 2: the two token indicators. When the task publishes a bound, the
    // output figure is shown against it — the chat turn's response budget, a
    // session summary's summarization budget, or the memory pass's memory budget.
    // The tooltip names the bound so the three never read alike.
    auto* row_tokens = new QLabel(card);
    row_tokens->setObjectName(QStringLiteral("Muted"));

    // Row 3: the state, round and elapsed time, exactly what the single detail
    // line carried before the token row was added.
    auto* row_detail = new QLabel(card);
    row_detail->setObjectName(QStringLiteral("Muted"));

    auto* row_error = new QLabel(card);
    row_error->setObjectName(QStringLiteral("TaskError"));
    row_error->setWordWrap(true);

    // The labels must not swallow the click; the card owns the press.
    for (QLabel* label : {row_title, row_id, row_tokens, row_detail, row_error}) {
      label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    column->addLayout(heading);
    column->addWidget(row_tokens);
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
    rows_[i] = Row{card, row_title, row_id, row_tokens, row_detail, row_error, cancel};
  }

  update_selection_style();
}

void TasksWindow::set_entries(std::vector<Entry> entries) {
  // Follow the selected task across a refresh: the entries are replaced on every
  // poll tick, so the reader's selection must survive while the task is listed.
  const bool had_selection = selected_ < entries_.size();
  const std::uint64_t keep_id = had_selection ? entries_[selected_].id : 0;
  const std::uint32_t keep_session =
      had_selection ? entries_[selected_].session_id : 0;

  if (entries.size() != rows_.size()) {
    rebuild_rows(entries.size());
  }

  entries_ = std::move(entries);

  selected_ = npos;

  if (had_selection) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].id == keep_id && entries_[i].session_id == keep_session) {
        selected_ = i;
        break;
      }
    }
  }

  for (std::size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];

    rows_[i].title->setText(entry.title);
    rows_[i].id->setText(
        entry.session_id == 0 ? QString() : trs("#%1").arg(entry.session_id)
    );

    // The produced figure merges what the model generated with the tool results
    // it was fed, charged against whichever budget the task publishes. A leading
    // tilde marks an estimate: the service did not report usage for at least one
    // request, so the number is locally approximated.
    const std::size_t produced = entry.output_tokens + entry.tool_tokens;
    const QString mark = entry.usage_measured ? QString() : QStringLiteral("~");
    const QString produced_text = entry.output_budget == 0
        ? mark + compact_tokens(produced)
        : mark + compact_tokens(produced) + QStringLiteral("/") +
            compact_tokens(entry.output_budget);

    rows_[i].tokens->setText(
        trs("Input %1   ·   Output %2")
            .arg(mark + compact_tokens(entry.input_tokens), produced_text)
    );

    const QString budget = budget_name(entry.output_budget_kind);
    const QString bound = budget.isEmpty()
        ? QString()
        : trs(", charged against the %1").arg(budget);

    const QString token_tooltip = trs(
        "Input: prompt tokens billed by every request of this task, summed "
        "over its tool rounds. Output: tokens generated plus the tool "
        "results fed back%1. Tool output is always counted at an estimated "
        "cost; a leading ~ marks a task whose request usage was estimated."
    ).arg(bound);

    rows_[i].tokens->setToolTip(token_tooltip);

    // The labels ignore mouse events so a click reaches the card, which also
    // keeps Qt from delivering their tooltips; the card carries the same text so
    // the budget a row is charged against can actually be read.
    if (rows_[i].widget != nullptr) {
      rows_[i].widget->setToolTip(token_tooltip);
    }

    // The session id has its own row, so this line carries only the state, the
    // round reached and the elapsed time.
    const QString detail = trs("%1   ·   Round %2   ·   %3")
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

  update_selection_style();
  refresh_log();
}

} // namespace ui
