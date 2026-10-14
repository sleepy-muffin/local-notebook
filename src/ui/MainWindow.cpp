#include "ui/MainWindow.hpp"

#include <QCloseEvent>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "app/application.hpp"
#include "net/client.hpp"
#include "ui/ConversationView.hpp"
#include "ui/SessionItemDelegate.hpp"
#include "ui/SessionListModel.hpp"
#include "ui/SettingsWindow.hpp"
#include "ui/SessionEditor.hpp"
#include "ui/theme.hpp"

namespace ui {

namespace {

constexpr int kSessionListLimit = 500;

// How many recent messages are enough to estimate the next request's context
// usage; the verbatim window is a suffix, so this bounds the work.
constexpr std::size_t kContextSampleMessages = 400;

QString from_utf8(const std::string& value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString rgba(const QColor& color) {
  return QStringLiteral("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alphaF());
}

// "1.2k" / "12k" / "900" — compact token counts for the title bar.
QString format_tokens(std::size_t used, std::size_t limit) {
  auto compact = [](std::size_t value) -> QString {
    if (value < 1000) {
      return QString::number(value);
    }

    const double thousands = static_cast<double>(value) / 1000.0;

    if (value % 1000 == 0 || value >= 100000) {
      return QStringLiteral("%1k").arg(qRound(thousands));
    }

    return QStringLiteral("%1k").arg(thousands, 0, 'f', 1);
  };

  return compact(used) + QStringLiteral(" / ") + compact(limit);
}

// The stored ISO-8601 UTC timestamp, shown in the reader's local time.
QString format_time(const std::optional<std::string>& iso) {
  if (!iso.has_value() || iso->empty()) {
    return {};
  }

  const QDateTime parsed = QDateTime::fromString(from_utf8(*iso), Qt::ISODate);

  if (!parsed.isValid()) {
    return from_utf8(*iso);
  }

  return parsed.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

MainWindow::MainWindow(lnb::Application& app, QWidget* parent)
    : BackgroundWidget(parent),
      app_(app) {
  setWindowTitle(QStringLiteral("Local Notebook"));
  resize(1080, 720);
  setMinimumSize(720, 480);

  if (app_.settings().geometry.has_value()) {
    restoreGeometry(QByteArray::fromBase64(
        QByteArray::fromStdString(*app_.settings().geometry)
    ));
  }

  build_ui();

  // Populate the sidebar before opening anything: the model starts empty and
  // open_conversation() only selects a row that the model already holds.
  refresh_sessions();

  // Open the most recent session so the app is useful immediately.
  const std::vector<lnb::Session> sessions = app_.sessions().most_recent(kSessionListLimit);
  if (!sessions.empty()) {
    open_conversation(sessions.front().id_);
  } else {
    update_header();
  }

  // A single timer drives the live streaming of every conversation, visible or
  // not, so a generation keeps advancing (and completes) while its session is
  // off screen.
  poll_timer_ = new QTimer(this);

  connect(poll_timer_, &QTimer::timeout, this, [this]() { poll_conversations(); });

  poll_timer_->start(33);
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
  if (poll_timer_ != nullptr) {
    poll_timer_->stop();
  }

  // Persist the window geometry for the next launch.
  app_.settings().geometry = saveGeometry().toBase64().toStdString();
  app_.save_settings();

  BackgroundWidget::closeEvent(event);
}

void MainWindow::build_ui() {
  auto* root = new QHBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  root->addWidget(build_sidebar());

  // The single hairline between the panes.
  auto* separator = new QWidget(this);
  separator->setObjectName(QStringLiteral("SidebarSeparator"));
  separator->setAttribute(Qt::WA_StyledBackground, true);
  separator->setFixedWidth(1);
  root->addWidget(separator);

  root->addWidget(build_right_pane(), 1);

  // Applied last, so the scoped rules and the derived fonts reach the widgets
  // that now exist.
  apply_window_style();
}

void MainWindow::apply_window_style() {
  const ThemeColors& c = theme().colors;
  const lnb::ThemeMetrics& m = metrics();

  // Chrome is painted by hand; only the scoped, per-window rules below are
  // stylesheet driven (design-1.md §12).
  QColor current_background = c.base;
  current_background.setAlpha(150);

  setStyleSheet(
      QStringLiteral(
          "QLabel#Brand { color: %1; }"
          "QLabel#SidebarCategory { color: %2; }"
          "QLabel#HeaderTitle { color: %1; }"
          "QLabel#HeaderSubtitle, QLabel#HeaderContext, QLabel#HeaderTime { color: %2; }"
          "QLabel#CurrentName { color: %1; }"
          "QLabel#CurrentId { color: %2; }"
          "QLabel#EmptyHint { color: %2; }"
          "QWidget#SidebarSeparator { background: %3; }"
          "QFrame#CurrentUser { background: %4; border-radius: 8px; }"
          "QListView#Sessions { background: transparent; border: none; outline: none; }"
          "QToolButton#CurrentSettings { background: transparent; border: none; padding: 0; }"
          "QToolButton#CurrentSettings:hover { background: %5; border-radius: 4px; }"
      )
          .arg(c.text.name(), c.text_muted.name(), c.border.name(), rgba(current_background),
               rgba(c.surface))
  );

  // Labels that carry a font derived from the base one must be re-derived here,
  // so a font or size change in the settings window takes effect without a
  // restart.
  if (brand_ != nullptr) {
    QFont font = ui_font();
    font.setWeight(QFont::DemiBold);
    if (font.pointSizeF() > 1.0) {
      font.setPointSizeF(font.pointSizeF() + 1.0);
    }
    brand_->setFont(font);
  }

  if (category_ != nullptr) {
    QFont font = ui_font();
    if (font.pointSizeF() > 1.5) {
      font.setPointSizeF(font.pointSizeF() - 1.0);
    }
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    category_->setFont(font);
  }

  if (current_id_ != nullptr) {
    QFont font = ui_font();
    if (font.pointSizeF() > 1.5) {
      font.setPointSizeF(font.pointSizeF() - 1.0);
    }
    current_id_->setFont(font);
  }

  if (header_title_ != nullptr) {
    QFont font = ui_font();
    font.setWeight(QFont::DemiBold);
    if (m.title_font_size > 0) {
      font.setPointSize(m.title_font_size);
    }
    header_title_->setFont(font);
  }
}

QWidget* MainWindow::build_sidebar() {
  const lnb::ThemeMetrics& m = metrics();

  auto* sidebar = new QWidget(this);
  sidebar->setObjectName(QStringLiteral("Sidebar"));
  sidebar->setFixedWidth(m.sidebar_width);

  auto* layout = new QVBoxLayout(sidebar);
  layout->setContentsMargins(m.page_margin, m.page_margin, m.page_margin, m.page_margin);
  layout->setSpacing(8);

  // The banner sits at the window's top-left. It is deliberately calm: a small
  // step up from the body text, no accent colour. Its font is derived in
  // apply_window_style().
  brand_ = new QLabel(QStringLiteral("Local Notebook"), sidebar);
  brand_->setObjectName(QStringLiteral("Brand"));

  // "Sessions" is a category identifier, not a title: small, muted, spaced.
  category_ = new QLabel(QStringLiteral("SESSIONS"), sidebar);
  category_->setObjectName(QStringLiteral("SidebarCategory"));

  sessions_view_ = new QListView(sidebar);
  sessions_view_->setObjectName(QStringLiteral("Sessions"));
  sessions_view_->setSelectionMode(QAbstractItemView::SingleSelection);
  sessions_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  sessions_view_->setFrameShape(QFrame::NoFrame);
  sessions_view_->setFrameShadow(QFrame::Plain);
  sessions_view_->setLineWidth(0);
  sessions_view_->setMidLineWidth(0);
  sessions_view_->setSpacing(0);
  sessions_view_->setUniformItemSizes(true);
  sessions_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  sessions_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  sessions_view_->setSelectionRectVisible(false);
  sessions_view_->setMouseTracking(true);
  sessions_view_->viewport()->setMouseTracking(true);
  sessions_view_->viewport()->setAutoFillBackground(false);
  sessions_view_->setMinimumWidth(0);

  sessions_model_ = new SessionListModel(sessions_view_);
  sessions_view_->setModel(sessions_model_);

  sessions_delegate_ = new SessionItemDelegate(sessions_view_);
  sessions_view_->setItemDelegate(sessions_delegate_);
  sessions_delegate_->set_edit_handler(
      [this](const QModelIndex& index) {
        edit_session(sessions_model_->session_id_at(index.row()));
      }
  );
  sessions_delegate_->set_delete_handler(
      [this](const QModelIndex& index) {
        delete_session(sessions_model_->session_id_at(index.row()));
      }
  );

  // A press starts a fresh interaction, so drop any stale action-click mark.
  connect(
      sessions_view_, &QListView::pressed, this,
      [this](const QModelIndex&) {
        if (sessions_delegate_ != nullptr) {
          sessions_delegate_->clear_action_click();
        }
      }
  );

  connect(
      sessions_view_, &QListView::clicked, this,
      [this](const QModelIndex& index) {
        // Clicking a row's edit/delete icon must not also switch to that
        // session; the delegate flags such a click before `clicked` is emitted.
        if (sessions_delegate_ != nullptr && sessions_delegate_->take_action_click()) {
          return;
        }

        if (index.isValid()) {
          open_conversation(sessions_model_->session_id_at(index.row()));
        }
      }
  );

  new_session_button_ = new QPushButton(QStringLiteral("New Session"), sidebar);
  new_session_button_->setObjectName(QStringLiteral("Secondary"));

  connect(
      new_session_button_, &QPushButton::clicked, this, [this]() { new_session(); }
  );

  // ---- Current session block (the ChatGPT-style account row) ----
  auto* current_user = new QFrame(sidebar);
  current_user->setObjectName(QStringLiteral("CurrentUser"));
  current_user->setAttribute(Qt::WA_StyledBackground, true);
  current_user->setFrameShape(QFrame::NoFrame);

  auto* current_layout = new QHBoxLayout(current_user);
  current_layout->setContentsMargins(10, 8, 8, 8);
  current_layout->setSpacing(8);

  auto* current_text = new QVBoxLayout();
  current_text->setSpacing(0);

  current_name_ = new QLabel(current_user);
  current_name_->setObjectName(QStringLiteral("CurrentName"));
  current_name_->setMinimumWidth(0);

  current_id_ = new QLabel(current_user);
  current_id_->setObjectName(QStringLiteral("CurrentId"));

  current_text->addWidget(current_name_);
  current_text->addWidget(current_id_);

  current_settings_ = new QToolButton(current_user);
  current_settings_->setObjectName(QStringLiteral("CurrentSettings"));
  current_settings_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  current_settings_->setAutoRaise(true);
  current_settings_->setFocusPolicy(Qt::NoFocus);
  current_settings_->setFixedSize(26, 26);
  current_settings_->setIconSize(QSize(16, 16));
  current_settings_->setCursor(Qt::PointingHandCursor);

  QIcon gear = QIcon::fromTheme(QStringLiteral("settings-configure"));
  if (gear.isNull()) {
    gear = QIcon::fromTheme(QStringLiteral("preferences-system"));
  }
  if (gear.isNull()) {
    gear = style()->standardIcon(QStyle::SP_FileDialogDetailedView);
  }
  current_settings_->setIcon(gear);
  current_settings_->setToolTip(QStringLiteral("Settings"));

  connect(
      current_settings_, &QToolButton::clicked, this, [this]() { open_settings(); }
  );

  current_layout->addLayout(current_text, 1);
  current_layout->addWidget(current_settings_);

  layout->addWidget(brand_);
  layout->addWidget(category_);
  layout->addWidget(sessions_view_, 1);
  layout->addWidget(new_session_button_);
  layout->addWidget(current_user);

  return sidebar;
}

QWidget* MainWindow::build_right_pane() {
  auto* pane = new QWidget(this);
  pane->setObjectName(QStringLiteral("RightPane"));
  pane->setMinimumWidth(0);

  auto* layout = new QVBoxLayout(pane);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // ---- Narrow, Zed-like title bar ----
  auto* header = new QFrame(pane);
  header->setObjectName(QStringLiteral("Header"));
  header->setFixedHeight(38);

  auto* header_layout = new QHBoxLayout(header);
  header_layout->setContentsMargins(16, 0, 16, 0);
  header_layout->setSpacing(10);

  header_title_ = new QLabel(header);
  header_title_->setObjectName(QStringLiteral("HeaderTitle"));

  header_subtitle_ = new QLabel(header);
  header_subtitle_->setObjectName(QStringLiteral("HeaderSubtitle"));

  header_context_ = new QLabel(header);
  header_context_->setObjectName(QStringLiteral("HeaderContext"));

  header_time_ = new QLabel(header);
  header_time_->setObjectName(QStringLiteral("HeaderTime"));

  header_layout->addWidget(header_title_);
  header_layout->addWidget(header_subtitle_);
  header_layout->addStretch(1);
  header_layout->addWidget(header_context_);
  header_layout->addSpacing(12);
  header_layout->addWidget(header_time_);

  workspace_ = new QStackedWidget(pane);
  workspace_->setObjectName(QStringLiteral("Workspace"));
  workspace_->setFrameShape(QFrame::NoFrame);
  workspace_->setMinimumWidth(0);
  workspace_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

  empty_hint_ = new QLabel(QStringLiteral("Select a session, or create one to start."), workspace_);
  empty_hint_->setObjectName(QStringLiteral("EmptyHint"));
  empty_hint_->setAlignment(Qt::AlignCenter);

  workspace_->addWidget(empty_hint_);
  workspace_->setCurrentWidget(empty_hint_);

  layout->addWidget(header);
  layout->addWidget(workspace_, 1);

  return pane;
}

void MainWindow::refresh_sessions() {
  if (sessions_model_ == nullptr) {
    return;
  }

  std::vector<lnb::Session> sessions = app_.sessions().most_recent(kSessionListLimit);

  // Prune views for sessions that no longer exist.
  std::vector<std::uint32_t> removed;

  for (const auto& [id, view] : conversations_) {
    const bool found = std::any_of(
        sessions.begin(), sessions.end(), [id](const lnb::Session& s) { return s.id_ == id; }
    );

    if (!found) {
      removed.push_back(id);
    }
  }

  for (const std::uint32_t id : removed) {
    auto it = conversations_.find(id);

    if (it == conversations_.end()) {
      continue;
    }

    if (workspace_->currentWidget() == it->second) {
      workspace_->setCurrentWidget(empty_hint_);
    }

    workspace_->removeWidget(it->second);
    it->second->deleteLater();
    conversations_.erase(it);

    if (current_session_ == id) {
      current_session_ = 0;
    }
  }

  // Only reset the model when the order or the titles actually changed, so a new
  // message does not clear and rebuild the list on every turn.
  bool same = static_cast<std::size_t>(sessions_model_->rowCount()) == sessions.size();

  if (same) {
    for (int row = 0; row < sessions_model_->rowCount(); ++row) {
      const lnb::Session stored = sessions_model_->session_at(row);
      if (stored.id_ != sessions[static_cast<std::size_t>(row)].id_ ||
          stored.title_ != sessions[static_cast<std::size_t>(row)].title_) {
        same = false;
        break;
      }
    }
  }

  if (!same) {
    sessions_model_->set_sessions(std::move(sessions));
  }

  // Re-select the current session (a model reset clears the selection).
  const int row = sessions_model_->row_for(current_session_);
  if (row >= 0) {
    sessions_view_->setCurrentIndex(sessions_model_->index(row, 0));
  } else {
    sessions_view_->clearSelection();
  }

  update_current_block();
}

void MainWindow::open_conversation(std::uint32_t session_id) {
  if (session_id == 0) {
    return;
  }

  auto it = conversations_.find(session_id);

  if (it == conversations_.end()) {
    const auto session = app_.db().session(session_id);

    if (!session.has_value()) {
      refresh_sessions();
      return;
    }

    auto* view = new ConversationView(app_, session_id, workspace_);
    view->set_on_changed([this]() {
      refresh_sessions();
      update_header();
    });

    workspace_->addWidget(view);
    it = conversations_.emplace(session_id, view).first;
  }

  current_session_ = session_id;
  workspace_->setCurrentWidget(it->second);

  const int row = sessions_model_->row_for(session_id);
  if (row >= 0 && sessions_view_->currentIndex().row() != row) {
    sessions_view_->setCurrentIndex(sessions_model_->index(row, 0));
  }

  update_header();
  update_current_block();
}

void MainWindow::new_session() {
  SessionEditor editor(app_, 0, this);

  if (editor.exec() != QDialog::Accepted) {
    return;
  }

  refresh_sessions();

  if (editor.session_id() != 0) {
    open_conversation(editor.session_id());
  }
}

void MainWindow::open_settings() {
  if (settings_window_ == nullptr) {
    settings_window_ = new SettingsWindow(app_, this);

    // A theme change must restyle this window's hand-painted chrome and every
    // rendered transcript; an assistant change can move the context budget, so
    // the header is refreshed too.
    settings_window_->set_on_saved([this]() {
      apply_window_style();

      for (auto& [id, view] : conversations_) {
        if (view != nullptr) {
          view->refresh_style();
        }
      }

      refresh_sessions();
      update_header();
    });
  }

  settings_window_->set_current_session(current_session_);
  settings_window_->show();
  settings_window_->raise();
  settings_window_->activateWindow();
}

void MainWindow::poll_conversations() {
  // Snapshot the views first: a poll can trigger a reload, which refreshes the
  // session list and may drop a view for a session deleted elsewhere. Iterating
  // the map directly would then use an invalidated iterator.
  std::vector<ConversationView*> views;
  views.reserve(conversations_.size());

  for (const auto& [id, view] : conversations_) {
    if (view != nullptr) {
      views.push_back(view);
    }
  }

  for (ConversationView* view : views) {
    view->poll();
  }
}

void MainWindow::edit_session(std::uint32_t session_id) {
  if (session_id == 0) {
    return;
  }

  SessionEditor editor(app_, session_id, this);

  if (editor.exec() != QDialog::Accepted) {
    return;
  }

  refresh_sessions();
  update_header();

  // The view reads the session from the database on every reload, so a changed
  // title is picked up on the next turn; refresh it now anyway.
  auto it = conversations_.find(session_id);
  if (it != conversations_.end() && it->second != nullptr) {
    it->second->reload();
  }
}

void MainWindow::delete_session(std::uint32_t session_id) {
  if (session_id == 0) {
    return;
  }

  const QMessageBox::StandardButton answer = QMessageBox::warning(
      this, QStringLiteral("Delete Session"),
      QStringLiteral("Are you sure you want to delete this session? This cannot be undone."),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No
  );

  if (answer != QMessageBox::Yes) {
    return;
  }

  app_.db().remove_session(session_id);
  refresh_sessions();
  update_header();
}

void MainWindow::update_header() {
  const auto session = app_.db().session(current_session_);

  if (!session.has_value()) {
    header_title_->setText(QStringLiteral("Local Notebook"));
    header_subtitle_->clear();
    header_context_->clear();
    header_context_->setToolTip(QString());
    header_time_->clear();
    return;
  }

  header_title_->setText(
      session->title_.empty() ? QStringLiteral("New Session") : from_utf8(session->title_)
  );
  header_subtitle_->setText(QStringLiteral("Session #%1").arg(session->id_));

  const std::vector<lnb::Message> sample =
      app_.db().recent_messages(current_session_, kContextSampleMessages);
  const lnb::net::ContextUsage usage =
      lnb::net::context_usage(app_.assistant(), *session, sample);

  header_context_->setText(format_tokens(usage.used_tokens, usage.limit_tokens));
  header_context_->setToolTip(
      QStringLiteral("%1 / %2 tokens used by the next request")
          .arg(usage.used_tokens)
          .arg(usage.limit_tokens)
  );

  header_time_->setText(format_time(app_.db().last_message_time(current_session_)));
}

void MainWindow::update_current_block() {
  const auto session = app_.db().session(current_session_);

  if (!session.has_value()) {
    current_name_->setText(QStringLiteral("No session"));
    current_id_->clear();
    current_settings_->setEnabled(false);
    return;
  }

  current_name_->setText(
      session->title_.empty() ? QStringLiteral("New Session") : from_utf8(session->title_)
  );
  current_id_->setText(QStringLiteral("Session #%1").arg(session->id_));
  current_settings_->setEnabled(true);
}

} // namespace ui
