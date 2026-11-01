#include "ui/TasksWindow.hpp"

#include <QClipboard>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QToolButton>
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
QString budget_name(lnb::core::GenerationTask::BudgetKind kind) {
  switch (kind) {
    case lnb::core::GenerationTask::BudgetKind::RESPONSE:
      return trs("response budget");
    case lnb::core::GenerationTask::BudgetKind::SUMMARIZATION:
      return trs("summarization budget");
    case lnb::core::GenerationTask::BudgetKind::MEMORY:
      return trs("memory budget");
    default:
      break;
  }

  return {};
}

// -----------------------------------------------------------------------------
// Log parsing
// -----------------------------------------------------------------------------

// How much of a long line a row previews. Enough to tell entries apart at a
// glance; the full text is one Copy away.
constexpr int kPreviewChars = 320;

// The longest log the list renders, newest last. The task log itself is bounded,
// but a turn that used many tools can still produce thousands of entries, and one
// widget per entry would be slow to build for no benefit when only the tail of a
// finished turn is usually inspected.
constexpr std::size_t kMaxLogRows = 400;

// True for a line that starts a new log entry, so a multi-line body (a tool
// result, the hidden reasoning) knows where it ends.
bool starts_entry(const QString& line) {
  return line.startsWith(QStringLiteral("[request ")) ||
         line.startsWith(QStringLiteral("[response ")) ||
         line.startsWith(QStringLiteral("tool call: ")) ||
         line.startsWith(QStringLiteral("tool result: ")) ||
         line.startsWith(QStringLiteral("generate_reply round=")) ||
         line.startsWith(QStringLiteral("[assistant-summary]")) ||
         line.startsWith(QStringLiteral("--- hidden reasoning ---"));
}

// A single-line, length-capped excerpt of `text`, for the preview line.
QString snippet(QString text) {
  text = text.trimmed();

  if (text.size() <= kPreviewChars) {
    return text;
  }

  return text.left(kPreviewChars).trimmed() + QStringLiteral("…");
}

QString character_count(const QString& text) {
  return trs("%1 characters").arg(static_cast<qulonglong>(text.size()));
}

// Reads a top-level string field out of a request body. Only used to label a
// request row with the model it was sent to; the body itself is never shown.
QString json_string_field(const QString& json, const QString& key) {
  const QString needle = QStringLiteral("\"%1\"").arg(key);
  int at = json.indexOf(needle);

  if (at < 0) {
    return {};
  }

  at = json.indexOf(QLatin1Char(':'), at + needle.size());

  if (at < 0) {
    return {};
  }

  at = json.indexOf(QLatin1Char('"'), at + 1);
  const int end = at < 0 ? -1 : json.indexOf(QLatin1Char('"'), at + 1);

  if (at < 0 || end < 0) {
    return {};
  }

  return json.mid(at + 1, end - at - 1);
}

// Splits the diagnostic log into readable entries. The format, written by the
// network layer, is one header line per item, with an optional body following:
//
//   [request chat] POST https://…
//   {"model": …}
//   [response chat] finish_reason=stop tool_calls=0
//   tool call: read_file({"path": "…"})
//   tool result: read_file -> <file contents>
//   --- hidden reasoning ---
//   <reasoning>
std::vector<TaskLogEntry> parse_log(const QString& text) {
  const QStringList lines = text.split(QLatin1Char('\n'));
  const int count = static_cast<int>(lines.size());

  std::vector<TaskLogEntry> entries;

  for (int i = 0; i < count; ++i) {
    const QString line = lines[i];

    if (line.trimmed().isEmpty()) {
      continue;
    }

    TaskLogEntry entry;

    if (line.startsWith(QStringLiteral("[request "))) {
      const int sep = line.indexOf(QStringLiteral("] POST "));

      entry.kind = TaskLogEntry::Kind::Request;
      entry.badge = trs("Request");
      entry.title = sep > 0 ? line.mid(9, sep - 9) : QString();

      QString body;

      while (i + 1 < count && !lines[i + 1].trimmed().isEmpty() &&
             !starts_entry(lines[i + 1].trimmed()) && !lines[i + 1].startsWith(QLatin1Char('['))) {
        if (!body.isEmpty()) {
          body += QLatin1Char('\n');
        }

        body += lines[++i];
      }

      entry.raw = line + (body.isEmpty() ? QString() : QLatin1Char('\n') + body);
      entry.detail = character_count(body);

      const QString endpoint = sep > 0 ? line.mid(sep + 2) : QString();
      const QString model = json_string_field(body, QStringLiteral("model"));

      entry.preview = endpoint;

      if (!model.isEmpty()) {
        entry.preview += (entry.preview.isEmpty() ? QString() : QStringLiteral("\n")) +
                         trs("Model: %1").arg(model);
      }
    } else if (line.startsWith(QStringLiteral("[response "))) {
      const int sep = line.indexOf(QLatin1Char(']'));

      entry.kind = TaskLogEntry::Kind::Response;
      entry.badge = trs("Response");
      entry.title = sep > 0 ? line.mid(10, sep - 10) : QString();
      entry.preview = sep > 0 ? line.mid(sep + 1).trimmed() : line;
      entry.raw = line;
    } else if (line.startsWith(QStringLiteral("tool call: "))) {
      const QString rest = line.mid(11);
      const int open = rest.indexOf(QLatin1Char('('));

      entry.kind = TaskLogEntry::Kind::ToolCall;
      entry.badge = trs("Tool call");
      entry.title = open > 0 ? rest.left(open) : rest;
      entry.preview = snippet(open >= 0 ? rest.mid(open + 1) : QString());
      entry.raw = line;
    } else if (line.startsWith(QStringLiteral("tool result: "))) {
      const QString rest = line.mid(13);
      const int arrow = rest.indexOf(QStringLiteral(" -> "));

      entry.kind = TaskLogEntry::Kind::ToolResult;
      entry.badge = trs("Tool result");
      entry.title = arrow > 0 ? rest.left(arrow) : rest;

      QString output = arrow > 0 ? rest.mid(arrow + 4) : QString();
      QString raw = line;

      while (i + 1 < count && !starts_entry(lines[i + 1].trimmed())) {
        raw += QLatin1Char('\n') + lines[++i];
        output += QLatin1Char('\n') + lines[i];
      }

      entry.raw = raw;
      entry.detail = character_count(output.trimmed());
      entry.preview = snippet(output);
    } else if (line.startsWith(QStringLiteral("--- hidden reasoning ---"))) {
      entry.kind = TaskLogEntry::Kind::Reasoning;
      entry.badge = trs("Reasoning");

      QString reasoning;

      while (i + 1 < count) {
        reasoning += QLatin1Char('\n') + lines[++i];
      }

      reasoning = reasoning.trimmed();
      entry.raw = line + QLatin1Char('\n') + reasoning;
      entry.detail = character_count(reasoning);
      entry.preview = snippet(reasoning);
    } else if (line.startsWith(QStringLiteral("generate_reply round="))) {
      entry.kind = TaskLogEntry::Kind::Note;
      entry.badge = trs("Round");
      entry.title = line.mid(QStringLiteral("generate_reply round=").size()).trimmed();
      entry.raw = line;
    } else {
      entry.kind = TaskLogEntry::Kind::Note;
      entry.badge = trs("Log");
      entry.title = line;
      entry.raw = line;
    }

    entries.push_back(std::move(entry));
  }

  if (entries.size() > kMaxLogRows) {
    entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(kMaxLogRows));
  }

  return entries;
}

} // namespace

TasksWindow::TasksWindow(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("TasksWindow"));
  setWindowTitle(trs("Tasks"));
  setModal(false);
  resize(1040, 620);
  setMinimumSize(640, 360);
  setFont(ui_font());

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // The header mirrors the main window's title bar: the page title, a quiet
  // subtitle, and a hairline underneath.
  auto* header = new QFrame(this);
  header->setObjectName(QStringLiteral("TaskHeader"));
  header->setAttribute(Qt::WA_StyledBackground, true);

  auto* header_layout = new QVBoxLayout(header);
  header_layout->setContentsMargins(20, 14, 20, 12);
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

  // Two columns, like the main window's sidebar and transcript: a fixed-width
  // task list on the left and the selected task's log on the right. The log is a
  // pane, never a separate dialog, so the list stays visible while it is read.
  auto* body = new QHBoxLayout();
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);

  // The list side is a fixed-width column with its own surface, exactly like the
  // main window's sidebar, so the two windows read as the same chrome.
  list_pane_ = new QWidget(this);
  list_pane_->setObjectName(QStringLiteral("TaskListPane"));
  list_pane_->setAttribute(Qt::WA_StyledBackground, true);

  auto* pane_layout = new QVBoxLayout(list_pane_);
  pane_layout->setContentsMargins(0, 0, 0, 0);
  pane_layout->setSpacing(0);

  auto* scroll = new QScrollArea(list_pane_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  install_scroll_bars(scroll, ScrollBarTrack::Window);

  list_ = new QWidget(scroll);
  list_layout_ = new QVBoxLayout(list_);
  list_layout_->setContentsMargins(10, 12, 10, 12);
  list_layout_->setSpacing(2);

  empty_ = new QLabel(trs("No tasks yet."), list_);
  empty_->setObjectName(QStringLiteral("Muted"));
  empty_->setAlignment(Qt::AlignCenter);
  empty_->setWordWrap(true);
  empty_->setContentsMargins(8, 16, 8, 16);
  list_layout_->addWidget(empty_);
  list_layout_->addStretch(1);

  scroll->setWidget(list_);
  pane_layout->addWidget(scroll, 1);
  body->addWidget(list_pane_, 0);

  // The log side scrolls on the window canvas.
  auto* log_pane = new QWidget(this);
  log_pane->setObjectName(QStringLiteral("TaskLogPane"));
  auto* log_layout = new QVBoxLayout(log_pane);
  log_layout->setContentsMargins(20, 14, 20, 14);
  log_layout->setSpacing(10);

  auto* log_header = new QHBoxLayout();
  log_header->setContentsMargins(0, 0, 0, 0);
  log_header->setSpacing(8);

  log_title_ = new QLabel(log_pane);
  log_title_->setObjectName(QStringLiteral("TaskLogTitle"));
  apply_derived_font(log_title_, 0.0, true);

  // Copies the whole log, so a finished task can be pasted into an editor in one
  // go. Hidden while nothing is selected or the selected task logged nothing.
  log_copy_ = new QToolButton(log_pane);
  log_copy_->setObjectName(QStringLiteral("LogCopy"));
  log_copy_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  log_copy_->setIconSize(QSize(15, 15));
  log_copy_->setAutoRaise(true);
  log_copy_->setCursor(Qt::PointingHandCursor);
  log_copy_->setFocusPolicy(Qt::NoFocus);
  log_copy_->setToolTip(trs("Copy log"));
  log_copy_->setVisible(false);

  QObject::connect(log_copy_, &QToolButton::clicked, this, [this]() {
    if (!log_text_.isEmpty()) {
      QGuiApplication::clipboard()->setText(log_text_);
    }
  });

  log_header->addWidget(log_title_);
  log_header->addStretch(1);
  log_header->addWidget(log_copy_);

  log_scroll_ = new QScrollArea(log_pane);
  log_scroll_->setWidgetResizable(true);
  log_scroll_->setFrameShape(QFrame::NoFrame);
  log_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  install_scroll_bars(log_scroll_, ScrollBarTrack::Window);

  log_list_ = new QWidget(log_scroll_);
  log_list_layout_ = new QVBoxLayout(log_list_);
  log_list_layout_->setContentsMargins(0, 0, 0, 0);
  log_list_layout_->setSpacing(0);

  log_placeholder_ = new QLabel(trs("Select a task to read its log."), log_list_);
  log_placeholder_->setObjectName(QStringLiteral("Muted"));
  log_placeholder_->setWordWrap(true);
  log_placeholder_->setContentsMargins(0, 8, 0, 8);
  log_list_layout_->addWidget(log_placeholder_);
  log_list_layout_->addStretch(1);

  log_scroll_->setWidget(log_list_);

  log_layout->addLayout(log_header);
  log_layout->addWidget(log_scroll_, 1);

  body->addWidget(log_pane, 1);

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

  if (selected_ != index) {
    log_shown_ = false;
  }

  selected_ = index;
  update_selection_style();
  refresh_log();
}

void TasksWindow::clear_log_rows() {
  for (const LogRow& row : log_rows_) {
    if (row.widget != nullptr) {
      log_list_layout_->removeWidget(row.widget);
      row.widget->deleteLater();
    }
  }

  log_rows_.clear();
  log_entries_.clear();
}

void TasksWindow::rebuild_log_rows(const QString& text) {
  clear_log_rows();
  log_entries_ = parse_log(text);

  for (const TaskLogEntry& entry : log_entries_) {
    auto* row = new QFrame(log_list_);
    row->setObjectName(QStringLiteral("LogEntry"));
    row->setAttribute(Qt::WA_StyledBackground, true);

    auto* outer = new QHBoxLayout(row);
    outer->setContentsMargins(2, 9, 2, 9);
    outer->setSpacing(10);

    auto* column = new QVBoxLayout();
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);

    auto* heading = new QHBoxLayout();
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(8);

    auto* badge = new QLabel(entry.badge, row);
    badge->setObjectName(QStringLiteral("LogBadge"));
    apply_derived_font(badge, -0.5, true);

    auto* row_title = new QLabel(entry.title, row);
    row_title->setObjectName(QStringLiteral("CurrentName"));
    row_title->setWordWrap(true);
    row_title->setVisible(!entry.title.isEmpty());
    row_title->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* row_detail = new QLabel(entry.detail, row);
    row_detail->setObjectName(QStringLiteral("Muted"));
    row_detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row_detail->setVisible(!entry.detail.isEmpty());

    heading->addWidget(badge);
    heading->addWidget(row_title, 1);
    heading->addWidget(row_detail);

    auto* row_preview = new QLabel(entry.preview, row);
    row_preview->setObjectName(QStringLiteral("LogPreview"));
    row_preview->setWordWrap(true);
    row_preview->setVisible(!entry.preview.isEmpty());
    row_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);

    column->addLayout(heading);
    column->addWidget(row_preview);

    // Every entry copies its own exact text, so a request body that is never
    // shown inline is still one click away from an editor.
    auto* copy = new QToolButton(row);
    copy->setObjectName(QStringLiteral("LogCopy"));
    copy->setToolButtonStyle(Qt::ToolButtonIconOnly);
    copy->setIconSize(QSize(15, 15));
    copy->setAutoRaise(true);
    copy->setCursor(Qt::PointingHandCursor);
    copy->setFocusPolicy(Qt::NoFocus);
    copy->setToolTip(trs("Copy"));

    QIcon artwork = resource_icon(
        QStringLiteral("copy"), theme().colors.text_muted, theme().colors.text_muted
    );

    if (artwork.isNull()) {
      artwork = style()->standardIcon(QStyle::SP_DialogSaveButton);
    }

    copy->setIcon(artwork);

    const QString raw = entry.raw;
    QObject::connect(copy, &QToolButton::clicked, this, [raw]() {
      QGuiApplication::clipboard()->setText(raw);
    });

    outer->addLayout(column, 1);
    outer->addWidget(copy, 0, Qt::AlignTop);

    // Insert before the placeholder and the trailing stretch.
    log_list_layout_->insertWidget(static_cast<int>(log_rows_.size() + 1), row);
    log_rows_.push_back(LogRow{row, row_title, row_detail, row_preview});
  }

  const bool empty = log_entries_.empty();
  log_placeholder_->setVisible(empty);
  log_placeholder_->setText(trs("This task produced no log."));
  log_copy_->setVisible(!text.isEmpty());
}

void TasksWindow::refresh_log() {
  if (selected_ >= entries_.size()) {
    log_title_->clear();
    clear_log_rows();
    log_placeholder_->setText(trs("Select a task to read its log."));
    log_placeholder_->setVisible(true);
    log_copy_->setVisible(false);
    log_text_.clear();
    log_shown_ = false;
    return;
  }

  const Entry& entry = entries_[selected_];

  log_title_->setText(
      entry.title.isEmpty() ? trs("Session #%1").arg(entry.session_id) : entry.title
  );

  const QString log =
      log_provider_ != nullptr ? log_provider_(entry.id) : QString();

  // The window refreshes on every poll tick, but rebuilding identical rows would
  // reset the reader's scroll position (and cost a full widget rebuild) for
  // nothing.
  if (log_shown_ && log == log_text_) {
    return;
  }

  QScrollBar* bar = log_scroll_->verticalScrollBar();
  const bool at_bottom = bar->value() >= bar->maximum() - 2;
  const int offset = bar->value();

  log_text_ = log;
  log_shown_ = true;
  rebuild_log_rows(log);

  // Keep the reader's place: follow the tail only when it was already at the
  // bottom, otherwise restore the previous offset after the rebuild.
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

  // The header matches the main window's title bar.
  sheet += QStringLiteral(
               "QFrame#TaskHeader { background: %1; border: none;"
               "  border-bottom: 1px solid %2; }")
               .arg(colour_hex(c.window), colour_hex(c.border));

  // The task list is the sidebar of this window: its own surface, a hairline
  // separating it from the log, and no stretch of its own.
  sheet += QStringLiteral(
               "QWidget#TaskListPane { background: %1; border: none;"
               "  border-right: 1px solid %2; }")
               .arg(colour_hex(c.surface), colour_hex(c.border));

  // Cards read like sidebar rows: flat until hovered, brighter when selected.
  sheet += QStringLiteral(
               "QFrame#Card { background: transparent; border: 1px solid transparent;"
               "  border-radius: 6px; }");
  sheet += QStringLiteral("QFrame#Card:hover { background: %1; }")
               .arg(colour_hex(row_hover_background()));
  sheet += QStringLiteral(
               "QFrame#Card[lnbSelected=\"true\"] { background: %1;"
               "  border: 1px solid %2; }")
               .arg(colour_hex(row_selected_background()), colour_hex(c.accent));

  sheet += QStringLiteral("QLabel#TaskLogTitle { color: %1; }").arg(colour_hex(c.text));
  sheet += QStringLiteral("QLabel#TaskError { color: %1; }").arg(colour_hex(c.danger));
  sheet += QStringLiteral("QLabel#TaskState { color: %1; }").arg(colour_hex(c.accent));

  // Log entries: a plain list, separated by hairlines, with the category in the
  // accent colour. No fixed-size font and no unwrapped body: the text is the
  // interface font, and only excerpts are shown.
  sheet += QStringLiteral(
               "QFrame#LogEntry { background: transparent; border: none;"
               "  border-bottom: 1px solid %1; }")
               .arg(colour_hex(c.border));
  sheet += QStringLiteral("QLabel#LogBadge { color: %1; }").arg(colour_hex(c.accent));
  sheet += QStringLiteral("QLabel#LogPreview { color: %1; }").arg(colour_hex(c.text_muted));
  sheet += QStringLiteral(
               "QToolButton#LogCopy { background: transparent; border: none;"
               "  border-radius: 4px; padding: 0; }");
  sheet += QStringLiteral("QToolButton#LogCopy:hover { background: %1; }")
               .arg(colour_hex(row_hover_background()));

  setStyleSheet(sheet);

  // The list column is a fixed width from the metrics, exactly like the main
  // window's sidebar, so resizing the window only grows the log pane.
  if (list_pane_ != nullptr) {
    list_pane_->setFixedWidth(metrics().tasks_list_width);
  }

  // The window is cached by the main window, so re-derive its fonts from the
  // current base font on every show, exactly as the settings window does.
  reapply_derived_fonts(this);

  // The per-entry copy icons are recoloured from the theme, so they are refreshed
  // here rather than only when the rows were built.
  const QIcon copy_icon = [this]() {
    QIcon artwork = resource_icon(
        QStringLiteral("copy"), theme().colors.text_muted, theme().colors.text_muted
    );

    if (artwork.isNull()) {
      artwork = style()->standardIcon(QStyle::SP_DialogSaveButton);
    }

    return artwork;
  }();

  if (log_copy_ != nullptr) {
    log_copy_->setIcon(copy_icon);
  }

  for (QToolButton* button : log_list_->findChildren<QToolButton*>()) {
    button->setIcon(copy_icon);
  }
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
    outer->setContentsMargins(10, 8, 10, 8);
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
    row_title->setWordWrap(true);

    auto* row_id = new QLabel(card);
    row_id->setObjectName(QStringLiteral("Muted"));
    row_id->setAlignment(Qt::AlignRight | Qt::AlignTop);

    heading->addWidget(row_title, 1);
    heading->addWidget(row_id);

    // Row 2: the two token indicators. When the task publishes a bound, the
    // output figure is shown against it — the chat turn's response budget, a
    // session summary's summarization budget, or the memory pass's memory budget.
    // The tooltip names the bound so the three never read alike.
    auto* row_tokens = new QLabel(card);
    row_tokens->setObjectName(QStringLiteral("Muted"));
    row_tokens->setWordWrap(true);

    // Row 3: the state, round and elapsed time, exactly what the single detail
    // line carried before the token row was added.
    auto* row_detail = new QLabel(card);
    row_detail->setObjectName(QStringLiteral("Muted"));
    row_detail->setWordWrap(true);

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
