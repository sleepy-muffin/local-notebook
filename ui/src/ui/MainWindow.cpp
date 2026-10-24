#include "ui/MainWindow.hpp"

#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "local-notebook-core/interface.hpp"
#include "settings/settings.hpp"
#include "support/i18n.hpp"
#include "ui/ConversationView.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/SessionItemDelegate.hpp"
#include "ui/SessionListModel.hpp"
#include "ui/SettingsWindow.hpp"
#include "ui/SessionEditor.hpp"
#include "ui/TasksWindow.hpp"
#include "ui/theme.hpp"

namespace ui {

namespace {

constexpr int kSessionListLimit = 500;

// The horizontal inset of every sidebar band. It matches the delegate's text inset,
// so the New Session row, the category label and the current-session name all line
// up with the session-row titles. The title-bar banner reuses it so it aligns too.
constexpr int kSidebarPadding = 12;

// Vertical inset of the New Session row, so its hover fill matches the height of a
// session row without touching the band above or the category below.
constexpr int kNewSessionPadding = 4;

// The narrow band height of the unified title bar, so the chrome reads as one
// continuous strip across the top of the window.
constexpr int kBandHeight = 38;

// The status bar's height. It is a touch shorter than the title bar, so it reads
// as a status strip rather than a second header.
constexpr int kStatusBarHeight = 26;

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

MainWindow::MainWindow(
    lnb::api::interface& api, lnb::Settings& settings, QWidget* parent
)
    : BackgroundWidget(parent, /*use_background_image=*/true),
      api_(api),
      settings_(settings) {
  setWindowTitle(QStringLiteral("Local Notebook"));
  resize(1080, 720);
  setMinimumSize(720, 480);

  if (settings_.geometry.has_value()) {
    restoreGeometry(QByteArray::fromBase64(
        QByteArray::fromStdString(*settings_.geometry)
    ));
  }

  build_ui();

  // Populate the sidebar before opening anything: the model starts empty and
  // open_conversation() only selects a row that the model already holds.
  refresh_sessions();

  // Open the most recent session so the app is useful immediately.
  const std::vector<lnb::Session> sessions =
      api_.recent_sessions(kSessionListLimit);
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

  // Seed the status bar's running-task readout so it does not start blank.
  update_task_indicator();
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
  if (poll_timer_ != nullptr) {
    poll_timer_->stop();
  }

  // Persist the window geometry for the next launch.
  settings_.geometry = saveGeometry().toBase64().toStdString();
  lnb::save_settings(settings_);

  BackgroundWidget::closeEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  // While the left button is held the session lists treat the pointer as a drag
  // and slide their current row to whichever row it passes over, even though no
  // click was made. Swallowing those moves pins the highlight to the pressed
  // row; a genuine click (press and release on the same row) still goes through
  // and opens the session.
  if (event->type() == QEvent::MouseMove) {
    auto* mouse = static_cast<QMouseEvent*>(event);

    if ((mouse->buttons() & Qt::LeftButton) != 0) {
      return true;
    }
  }

  return BackgroundWidget::eventFilter(watched, event);
}

void MainWindow::build_ui() {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // One title bar spanning the whole window, so the chrome reads as a single
  // continuous strip rather than two separate bands.
  root->addWidget(build_title_bar());

  // The body: sidebar, the single hairline between the panes, and the transcript.
  auto* body = new QHBoxLayout();
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);

  sidebar_ = build_sidebar();
  body->addWidget(sidebar_);

  sidebar_separator_ = new QWidget(this);
  sidebar_separator_->setObjectName(QStringLiteral("SidebarSeparator"));
  sidebar_separator_->setAttribute(Qt::WA_StyledBackground, true);
  sidebar_separator_->setFixedWidth(1);
  body->addWidget(sidebar_separator_);

  body->addWidget(build_right_pane(), 1);

  root->addLayout(body, 1);

  // The status strip closes the window: the sidebar toggle on the left, the
  // running-task indicator on the right.
  root->addWidget(build_status_bar());

  // Applied last, so the scoped rules and the derived fonts reach the widgets
  // that now exist.
  apply_window_style();
}

QWidget* MainWindow::build_status_bar() {
  auto* bar = new QFrame(this);
  bar->setObjectName(QStringLiteral("StatusBar"));
  bar->setAttribute(Qt::WA_StyledBackground, true);
  bar->setFixedHeight(kStatusBarHeight);

  auto* layout = new QHBoxLayout(bar);
  layout->setContentsMargins(6, 0, 10, 0);
  layout->setSpacing(8);

  // Left: the sidebar toggle, flat and borderless like the composer actions.
  sidebar_toggle_ = new QToolButton(bar);
  sidebar_toggle_->setObjectName(QStringLiteral("SidebarToggle"));
  sidebar_toggle_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  sidebar_toggle_->setAutoRaise(true);
  sidebar_toggle_->setFocusPolicy(Qt::NoFocus);
  sidebar_toggle_->setCursor(Qt::PointingHandCursor);
  sidebar_toggle_->setFixedSize(20, 20);
  sidebar_toggle_->setIconSize(QSize(15, 15));
  sidebar_toggle_->setToolTip(trs("Toggle sidebar"));

  connect(
      sidebar_toggle_, &QToolButton::clicked, this, [this]() { toggle_sidebar(); }
  );

  // Right: the running-task count, clickable to open the tasks window.
  task_indicator_ = new QToolButton(bar);
  task_indicator_->setObjectName(QStringLiteral("TaskIndicator"));
  task_indicator_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  task_indicator_->setAutoRaise(true);
  task_indicator_->setFocusPolicy(Qt::NoFocus);
  task_indicator_->setCursor(Qt::PointingHandCursor);
  task_indicator_->setIconSize(QSize(15, 15));

  connect(
      task_indicator_, &QToolButton::clicked, this, [this]() { open_tasks_window(); }
  );

  layout->addWidget(sidebar_toggle_);
  layout->addStretch(1);
  layout->addWidget(task_indicator_);

  return bar;
}

QWidget* MainWindow::build_title_bar() {
  auto* bar = new QFrame(this);
  bar->setObjectName(QStringLiteral("TitleBar"));
  bar->setAttribute(Qt::WA_StyledBackground, true);
  bar->setFixedHeight(kBandHeight);

  auto* layout = new QHBoxLayout(bar);
  // The banner is inset like the sidebar content, so it lines up with the
  // session rows; the conversation info is inset a little further from the edge.
  layout->setContentsMargins(kSidebarPadding, 0, 16, 0);
  layout->setSpacing(10);

  brand_ = new QLabel(QStringLiteral("Local Notebook"), bar);
  brand_->setObjectName(QStringLiteral("Brand"));

  header_subtitle_ = new QLabel(bar);
  header_subtitle_->setObjectName(QStringLiteral("HeaderSubtitle"));

  header_summarization_trigger_ = new QLabel(bar);
  header_summarization_trigger_->setObjectName(QStringLiteral("HeaderContext"));

  header_time_ = new QLabel(bar);
  header_time_->setObjectName(QStringLiteral("HeaderTime"));

  layout->addWidget(brand_);
  layout->addStretch(1);
  layout->addWidget(header_subtitle_);
  layout->addWidget(header_summarization_trigger_);
  layout->addSpacing(12);
  layout->addWidget(header_time_);

  return bar;
}

void MainWindow::apply_window_style() {
  const ThemeColors& c = theme().colors;

  // Chrome is painted by hand; only the scoped, per-window rules below are
  // stylesheet driven (design-1.md §12).
  setStyleSheet(
      QStringLiteral(
          "QLabel#Brand { color: %1; }"
          "QLabel#SidebarCategory { color: %2; }"
          "QLabel#HeaderSubtitle, QLabel#HeaderContext, QLabel#HeaderTime { color: %2; }"
          "QLabel#CurrentName { color: %1; }"
          "QLabel#CurrentModel { color: %2; }"
          "QLabel#EmptyHint { color: %2; }"
          "QWidget#Sidebar { background: %4; }"
          "QWidget#SidebarSeparator { background: %3; }"
          "QFrame#TitleBar { background: %4; border: none;"
          "  border-bottom: 1px solid %3; }"
          "QFrame#StatusBar { background: %4; border: none;"
          "  border-top: 1px solid %3; }"
          "QToolButton#SidebarToggle, QToolButton#TaskIndicator { background: transparent;"
          "  border: none; border-radius: 4px; padding: 0 4px; color: %2; }"
          "QToolButton#SidebarToggle:hover, QToolButton#TaskIndicator:hover {"
          "  background: %5; }"
          "QFrame#CurrentUser { background: transparent; border: none;"
          "  border-top: 1px solid %3; }"
          "QListView#Sessions { border: none; outline: none; }"
          "QPushButton#NewSession { background: transparent; border: none; color: %1;"
          "  text-align: left; padding: 6px 12px; border-radius: 0; }"
          "QPushButton#NewSession:hover { background: %5; }"
          "QToolButton#CurrentSettings { background: transparent; border: none; padding: 0; }"
          "QToolButton#CurrentSettings:hover { background: %5; border-radius: 4px; }"
      )
          .arg(c.text.name(), c.text_muted.name(), c.border.name(), rgba(c.surface),
               rgba(row_hover_background()))
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

  if (current_model_ != nullptr) {
    QFont font = ui_font();
    if (font.pointSizeF() > 1.5) {
      font.setPointSizeF(font.pointSizeF() - 1.0);
    }
    current_model_->setFont(font);
  }

  // The plus glyph is embedded in the binary and recoloured to the theme, like
  // the composer icons, so it follows the palette instead of a fixed system
  // theme icon. It is re-applied here (not only at build time) so a theme change
  // in the settings window takes effect without a restart. A missing resource
  // falls back to the system icon theme, then a standard icon.
  if (new_session_button_ != nullptr) {
    QIcon add =
        resource_icon(QStringLiteral("new-session"), c.text, c.text_muted);

    if (add.isNull()) {
      add = QIcon::fromTheme(QStringLiteral("list-add"));
    }
    if (add.isNull()) {
      add = QIcon::fromTheme(QStringLiteral("document-new"));
    }
    if (add.isNull()) {
      add = style()->standardIcon(QStyle::SP_FileDialogNewFolder);
    }

    new_session_button_->setIcon(add);
  }

  // The settings cog is bundled and recoloured to the theme as well, and is
  // re-applied here so a theme change reaches it without a restart.
  if (current_settings_ != nullptr) {
    QIcon gear = resource_icon(QStringLiteral("settings"), c.text_muted, c.text_muted);

    if (gear.isNull()) {
      gear = QIcon::fromTheme(QStringLiteral("settings-configure"));
    }
    if (gear.isNull()) {
      gear = style()->standardIcon(QStyle::SP_FileDialogDetailedView);
    }

    current_settings_->setIcon(gear);
  }

  // The status bar's two icons follow the theme as well.
  refresh_sidebar_toggle();

  if (task_indicator_ != nullptr) {
    QIcon icon = resource_icon(QStringLiteral("tasks"), c.text_muted, c.text_muted);
    if (icon.isNull()) {
      icon = style()->standardIcon(QStyle::SP_FileDialogDetailedView);
    }
    task_indicator_->setIcon(icon);
  }
}

QListView* MainWindow::build_session_list(
    QWidget* parent, const QString& object_name, SessionListModel*& model,
    SessionItemDelegate*& delegate
) {
  auto* view = new QListView(parent);
  view->setObjectName(object_name);
  // The list shows the sidebar behind it; the rule lives on the view (not on the
  // window's stylesheet) so a style that paints from the palette cannot fill the
  // scrollbar column opaque black. See ui::show_parent_background().
  show_parent_background(view);
  view->setSelectionMode(QAbstractItemView::SingleSelection);
  view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view->setFrameShape(QFrame::NoFrame);
  view->setFrameShadow(QFrame::Plain);
  view->setLineWidth(0);
  view->setMidLineWidth(0);
  view->setSpacing(0);
  view->setUniformItemSizes(true);
  view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  install_scroll_bars(view, ScrollBarTrack::Surface);
  view->setSelectionRectVisible(false);
  view->setMouseTracking(true);
  view->viewport()->setMouseTracking(true);
  view->viewport()->setAutoFillBackground(false);
  view->setMinimumWidth(0);

  // Only a genuine click should move the list's own current row; see eventFilter.
  view->viewport()->installEventFilter(this);

  model = new SessionListModel(view);
  view->setModel(model);

  delegate = new SessionItemDelegate(view);
  view->setItemDelegate(delegate);

  // Bind the model and delegate by value: they outlive this call, but the
  // reference parameters do not.
  SessionListModel* const bound_model = model;
  SessionItemDelegate* const bound_delegate = delegate;

  bound_delegate->set_pin_handler([this, bound_model](const QModelIndex& index) {
    pin_session(bound_model->session_id_at(index.row()));
  });
  bound_delegate->set_edit_handler([this, bound_model](const QModelIndex& index) {
    edit_session(bound_model->session_id_at(index.row()));
  });
  bound_delegate->set_delete_handler([this, bound_model](const QModelIndex& index) {
    delete_session(bound_model->session_id_at(index.row()));
  });

  // A press starts a fresh interaction, so drop any stale action-click mark.
  connect(
      view, &QListView::pressed, this,
      [bound_delegate](const QModelIndex&) { bound_delegate->clear_action_click(); }
  );

  // Keep at most one of the two lists highlighted. The highlight is the selection
  // fill, so clearing the other list's selection is enough; clearing the selection
  // leaves its current index alone, which does not emit currentChanged and so
  // cannot recurse back into this handler.
  QListView* const bound_view = view;
  connect(
      view->selectionModel(), &QItemSelectionModel::currentChanged, this,
      [this, bound_view](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) {
          return;
        }

        QListView* const other =
            bound_view == pinned_view_ ? sessions_view_ : pinned_view_;

        if (other != nullptr) {
          other->clearSelection();
        }
      }
  );

  connect(
      view, &QListView::clicked, this,
      [this, bound_model, bound_delegate](const QModelIndex& index) {
        // Clicking a row's action icon must not also switch to that session; the
        // delegate flags such a click before `clicked` is emitted.
        if (bound_delegate->take_action_click()) {
          return;
        }

        if (index.isValid()) {
          open_conversation(bound_model->session_id_at(index.row()));
        }
      }
  );

  return view;
}

QWidget* MainWindow::build_sidebar() {
  const lnb::ThemeMetrics& m = metrics();

  auto* sidebar = new QWidget(this);
  sidebar->setObjectName(QStringLiteral("Sidebar"));
  sidebar->setAttribute(Qt::WA_StyledBackground, true);
  sidebar->setFixedWidth(m.sidebar_width);

  auto* layout = new QVBoxLayout(sidebar);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // ---- New Session action ----
  // Sits above the SESSIONS category, as the first item of the list. It is a flat,
  // borderless row with a subtle hover fill, so it reads like a session row rather
  // than a button.
  auto* new_session_row = new QWidget(sidebar);
  auto* new_session_layout = new QVBoxLayout(new_session_row);
  // No horizontal inset: like a session row, the hover fill runs edge to edge and
  // the padding is applied inside the button, so the text lines up with the rows.
  new_session_layout->setContentsMargins(0, kNewSessionPadding, 0, 0);
  new_session_layout->setSpacing(0);

  new_session_button_ = new QPushButton(trs("New Session"), new_session_row);
  new_session_button_->setObjectName(QStringLiteral("NewSession"));
  new_session_button_->setCursor(Qt::PointingHandCursor);
  new_session_button_->setFocusPolicy(Qt::NoFocus);
  new_session_button_->setFlat(true);
  new_session_button_->setToolTip(trs("New Session"));

  new_session_button_->setIconSize(QSize(16, 16));

  connect(
      new_session_button_, &QPushButton::clicked, this, [this]() { new_session(); }
  );

  new_session_layout->addWidget(new_session_button_);

  // ---- PINNED category + list ----
  // Above SESSIONS, and hidden entirely while no session is pinned. The list
  // sizes to its rows, so the SESSIONS category slides up under the New Session
  // row whenever the band disappears.
  pinned_band_ = new QWidget(sidebar);
  auto* pinned_layout = new QVBoxLayout(pinned_band_);
  pinned_layout->setContentsMargins(0, 0, 0, 0);
  pinned_layout->setSpacing(0);

  {
    auto* pinned_category_row = new QWidget(pinned_band_);
    auto* pinned_category_layout = new QHBoxLayout(pinned_category_row);
    pinned_category_layout->setContentsMargins(kSidebarPadding, 10, kSidebarPadding, 4);
    pinned_category_layout->setSpacing(0);

    auto* pinned_category = new QLabel(trs("PINNED"), pinned_category_row);
    pinned_category->setObjectName(QStringLiteral("SidebarCategory"));

    pinned_category_layout->addWidget(pinned_category);
    pinned_category_layout->addStretch(1);
    pinned_layout->addWidget(pinned_category_row);
  }

  pinned_view_ = build_session_list(
      pinned_band_, QStringLiteral("PinnedSessions"), pinned_model_, pinned_delegate_
  );
  pinned_view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  pinned_layout->addWidget(pinned_view_);
  pinned_band_->setVisible(false);

  // ---- SESSIONS category ----
  // "Sessions" is a category identifier, not a title: small, muted, spaced.
  auto* category_row = new QWidget(sidebar);
  auto* category_layout = new QHBoxLayout(category_row);
  category_layout->setContentsMargins(kSidebarPadding, 10, kSidebarPadding, 4);
  category_layout->setSpacing(0);

  category_ = new QLabel(trs("SESSIONS"), category_row);
  category_->setObjectName(QStringLiteral("SidebarCategory"));

  category_layout->addWidget(category_);
  category_layout->addStretch(1);

  sessions_view_ = build_session_list(
      sidebar, QStringLiteral("Sessions"), sessions_model_, sessions_delegate_
  );

  // ---- Current assistant block (name, model, and the settings action) ----
  auto* current_user = new QFrame(sidebar);
  current_user->setObjectName(QStringLiteral("CurrentUser"));
  current_user->setAttribute(Qt::WA_StyledBackground, true);
  current_user->setFrameShape(QFrame::NoFrame);

  auto* current_layout = new QHBoxLayout(current_user);
  current_layout->setContentsMargins(kSidebarPadding, 8, kSidebarPadding - 4, 8);
  current_layout->setSpacing(8);

  auto* current_text = new QVBoxLayout();
  current_text->setSpacing(0);

  current_name_ = new QLabel(current_user);
  current_name_->setObjectName(QStringLiteral("CurrentName"));
  current_name_->setMinimumWidth(0);

  current_model_ = new QLabel(current_user);
  current_model_->setObjectName(QStringLiteral("CurrentModel"));
  current_model_->setMinimumWidth(0);

  current_text->addWidget(current_name_);
  current_text->addWidget(current_model_);

  current_settings_ = new QToolButton(current_user);
  current_settings_->setObjectName(QStringLiteral("CurrentSettings"));
  current_settings_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  current_settings_->setAutoRaise(true);
  current_settings_->setFocusPolicy(Qt::NoFocus);
  current_settings_->setFixedSize(26, 26);
  current_settings_->setIconSize(QSize(16, 16));
  current_settings_->setCursor(Qt::PointingHandCursor);
  current_settings_->setToolTip(trs("Settings"));

  connect(
      current_settings_, &QToolButton::clicked, this, [this]() { open_settings(); }
  );

  current_layout->addLayout(current_text, 1);
  current_layout->addWidget(current_settings_);

  layout->addWidget(new_session_row);
  layout->addWidget(pinned_band_);
  layout->addWidget(category_row);
  layout->addWidget(sessions_view_, 1);
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

  workspace_ = new QStackedWidget(pane);
  workspace_->setObjectName(QStringLiteral("Workspace"));
  workspace_->setFrameShape(QFrame::NoFrame);
  workspace_->setMinimumWidth(0);
  workspace_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

  empty_hint_ = new QLabel(trs("Select a session, or create one to start."), workspace_);
  empty_hint_->setObjectName(QStringLiteral("EmptyHint"));
  empty_hint_->setAlignment(Qt::AlignCenter);

  workspace_->addWidget(empty_hint_);
  workspace_->setCurrentWidget(empty_hint_);

  layout->addWidget(workspace_, 1);

  return pane;
}

void MainWindow::refresh_sessions() {
  if (sessions_model_ == nullptr || pinned_model_ == nullptr) {
    return;
  }

  std::vector<lnb::Session> sessions = api_.recent_sessions(kSessionListLimit);

  // The newest message time of every session, in one query, aligned with the
  // ordered snapshot for the rows' second line.
  const std::unordered_map<std::uint32_t, std::string> last_times =
      api_.session_last_times();

  // Split into the two categories. most_recent() already returns pinned sessions
  // first and each group keeps its newest-message order, so no extra sort is
  // needed.
  std::vector<lnb::Session> pinned;
  std::vector<lnb::Session> rest;
  std::vector<std::string> pinned_times;
  std::vector<std::string> rest_times;

  for (const lnb::Session& session : sessions) {
    const auto it = last_times.find(session.id_);
    const std::string time = it == last_times.end() ? std::string{} : it->second;

    if (session.pinned_) {
      pinned.push_back(session);
      pinned_times.push_back(time);
    } else {
      rest.push_back(session);
      rest_times.push_back(time);
    }
  }

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

  for (std::uint32_t id : removed) {
    auto it = conversations_.find(id);

    if (it == conversations_.end()) {
      continue;
    }

    if (workspace_->currentWidget() == it->second) {
      workspace_->setCurrentWidget(empty_hint_);
    }

    workspace_->removeWidget(it->second);
    it->second->cancel();
    it->second->deleteLater();
    conversations_.erase(it);

    if (current_session_ == id) {
      current_session_ = 0;
    }
  }

  // Only reset a model when its order, titles, times or pin states actually
  // changed, so a new message does not clear and rebuild the lists on every turn.
  const auto unchanged = [](
                             const SessionListModel* model,
                             const std::vector<lnb::Session>& rows,
                             const std::vector<std::string>& times
                         ) {
    if (static_cast<std::size_t>(model->rowCount()) != rows.size()) {
      return false;
    }

    for (int row = 0; row < model->rowCount(); ++row) {
      const lnb::Session stored = model->session_at(row);
      const lnb::Session& wanted = rows[static_cast<std::size_t>(row)];

      if (stored.id_ != wanted.id_ || stored.title_ != wanted.title_ ||
          stored.pinned_ != wanted.pinned_ ||
          model->last_time_at(row) != times[static_cast<std::size_t>(row)]) {
        return false;
      }
    }

    return true;
  };

  if (!unchanged(pinned_model_, pinned, pinned_times)) {
    pinned_model_->set_sessions(std::move(pinned), std::move(pinned_times));
  }

  if (!unchanged(sessions_model_, rest, rest_times)) {
    sessions_model_->set_sessions(std::move(rest), std::move(rest_times));
  }

  // The pinned band sizes to its rows and disappears entirely when empty.
  int pinned_height = 0;

  for (int row = 0; row < pinned_model_->rowCount(); ++row) {
    pinned_height += std::max(0, pinned_view_->sizeHintForRow(row));
  }

  pinned_view_->setFixedHeight(pinned_height);
  pinned_band_->setVisible(pinned_model_->rowCount() > 0);

  // Re-select the current session (a model reset clears the selection). It lives
  // in exactly one of the two lists.
  const int pinned_row = pinned_model_->row_for(current_session_);

  if (pinned_row >= 0) {
    pinned_view_->setCurrentIndex(pinned_model_->index(pinned_row, 0));
    sessions_view_->clearSelection();
  } else {
    pinned_view_->clearSelection();

    const int row = sessions_model_->row_for(current_session_);
    if (row >= 0) {
      sessions_view_->setCurrentIndex(sessions_model_->index(row, 0));
    } else {
      sessions_view_->clearSelection();
    }
  }

  update_current_block();
}

ConversationView* MainWindow::ensure_conversation_view(std::uint32_t session_id) {
  if (session_id == 0) {
    return nullptr;
  }

  const auto existing = conversations_.find(session_id);

  if (existing != conversations_.end()) {
    return existing->second;
  }

  const auto session = api_.session(session_id);

  if (!session.has_value()) {
    return nullptr;
  }

  auto* view = new ConversationView(api_, settings_, session_id, workspace_);
  view->set_on_changed([this]() {
    refresh_sessions();
    update_header();
  });

  // The failure notice points at the status bar's tasks icon; clicking it opens
  // the same window the icon does.
  view->set_failure_notice_handler([this]() { open_tasks_window(); });

  // The workspace always holds the empty hint, so adding a widget here never
  // takes the current index: the visible pane is chosen explicitly by the caller.
  workspace_->addWidget(view);
  conversations_.emplace(session_id, view);

  return view;
}

void MainWindow::open_conversation(std::uint32_t session_id) {
  if (session_id == 0) {
    return;
  }

  ConversationView* view = ensure_conversation_view(session_id);

  if (view == nullptr) {
    refresh_sessions();
    return;
  }

  current_session_ = session_id;
  workspace_->setCurrentWidget(view);

  // The row lives in one of the two lists; select it there and clear the other,
  // so only ever one list shows a highlight.
  const int pinned_row = pinned_model_->row_for(session_id);

  if (pinned_row >= 0) {
    pinned_view_->setCurrentIndex(pinned_model_->index(pinned_row, 0));
    sessions_view_->clearSelection();
  } else {
    const int row = sessions_model_->row_for(session_id);

    if (row >= 0) {
      sessions_view_->setCurrentIndex(sessions_model_->index(row, 0));
    } else {
      sessions_view_->clearSelection();
    }

    pinned_view_->clearSelection();
  }

  update_header();
  update_current_block();
}

void MainWindow::new_session() {
  SessionEditor editor(api_, 0, this);

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
    settings_window_ = new SettingsWindow(api_, settings_, this);

    // A theme change must restyle this window's hand-painted chrome and the
    // transcript on screen; an assistant change can move the context budget, so
    // the header is refreshed too.
    settings_window_->set_on_saved([this]() {
      apply_window_style();

      // Only the visible transcript is restyled now. Every other cached
      // conversation is marked stale and restyles when it is next shown, so a
      // save never re-renders Markdown for transcripts that are off screen.
      for (auto& [id, view] : conversations_) {
        if (view == nullptr) {
          continue;
        }

        if (id == current_session_) {
          view->refresh_style();
        } else {
          view->mark_style_stale();
        }
      }

      refresh_sessions();
      update_header();
      update_current_block();

      // The assistant and session summary budgets may have moved, so the cached
      // assistant trigger and every composer's primary action are re-evaluated.
      api_.mark_assistant_summary_stale();
      last_assistant_due_ = api_.assistant_summary_due();

      last_assistant_busy_ = api_.assistant_busy();

      for (auto& [id, view] : conversations_) {
        if (view != nullptr) {
          view->refresh_send_mode();
        }
      }
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

  // The assistant memory is shared, so a turn in one session can turn every
  // composer's primary action into a summarize pass, and a fold in flight bans
  // sending in every session until it finishes. The application caches the
  // trigger, so this is a cheap compare; the views are asked to re-evaluate only
  // when one of the two flags actually flips.
  const bool assistant_due = api_.assistant_summary_due();

  // "Busy" here means the shared assistant fold, not a session turn: a session
  // turn only disables its own composer now, and querying the session trigger
  // while that turn writes the database is unnecessary.
  const bool assistant_busy = api_.assistant_busy();

  if (assistant_due != last_assistant_due_ || assistant_busy != last_assistant_busy_) {
    last_assistant_due_ = assistant_due;
    last_assistant_busy_ = assistant_busy;

    for (ConversationView* view : views) {
      view->refresh_send_mode();
    }
  }

  update_task_indicator();
}

void MainWindow::toggle_sidebar() {
  sidebar_visible_ = !sidebar_visible_;

  if (sidebar_ != nullptr) {
    sidebar_->setVisible(sidebar_visible_);
  }

  if (sidebar_separator_ != nullptr) {
    sidebar_separator_->setVisible(sidebar_visible_);
  }

  refresh_sidebar_toggle();
}

void MainWindow::refresh_sidebar_toggle() {
  if (sidebar_toggle_ == nullptr) {
    return;
  }

  // Tinted with the accent while the sidebar is shown, so its state reads at a
  // glance (the same accent the send button uses); muted while it is hidden.
  const ThemeColors& c = theme().colors;
  const QColor colour = sidebar_visible_ ? c.accent : c.text_muted;

  QIcon icon = resource_icon(QStringLiteral("sidebar"), colour, c.text_muted);
  if (icon.isNull()) {
    icon = style()->standardIcon(QStyle::SP_FileDialogListView);
  }
  sidebar_toggle_->setIcon(icon);
}

void MainWindow::update_task_indicator() {
  if (task_indicator_ == nullptr) {
    return;
  }

  // Count the running tasks without touching the database: this runs on every
  // poll tick, while the full entry list is only built for a visible window.
  const std::shared_ptr<lnb::GenerationTask> assistant = api_.assistant_task();
  int running = 0;

  for (const auto& [id, view] : conversations_) {
    if (view == nullptr) {
      continue;
    }

    if (view->busy()) {
      ++running;
    }
  }

  // The shared assistant fold has its own slot, shared by no session view.
  if (api_.assistant_busy()) {
    ++running;
  }

  if (running != shown_task_count_) {
    shown_task_count_ = running;

    // An idle status bar stays calm: the glyph alone when nothing is running, the
    // glyph beside the count otherwise.
    task_indicator_->setText(running == 0 ? QString() : QString::number(running));
    task_indicator_->setToolTip(
        running == 0 ? trs("No tasks running")
        : running == 1 ? trs("1 task running")
                      : trs("%1 tasks running").arg(running)
    );
  }

  // Only build (and refresh) the full list while the window is on screen, so a
  // hidden one costs nothing — in particular, no per-session database reads.
  if (tasks_window_ == nullptr || !tasks_window_->isVisible()) {
    return;
  }

  const auto describe = [](lnb::GenerationTask::Status status) -> QString {
    switch (status) {
      case lnb::GenerationTask::Status::PENDING:
      case lnb::GenerationTask::Status::RUNNING:
        return trs("Running");
      case lnb::GenerationTask::Status::FAILED:
        return trs("Failed");
      case lnb::GenerationTask::Status::CANCELLED:
        return trs("Cancelled");
      default:
        return trs("Completed");
    }
  };

  // One entry per session view that has a task (running or the last finished
  // one), plus the shared assistant fold when it has ever run. Session slots and
  // the assistant slot are separate, so no task belongs to two entries.
  std::vector<TasksWindow::Entry> entries;

  for (const auto& [id, view] : conversations_) {
    if (view == nullptr) {
      continue;
    }

    if (view->task() == nullptr) {
      continue;
    }

    const bool busy = view->busy();

    TasksWindow::Entry entry;
    entry.id = id;
    entry.session_id = id;
    entry.running = busy;
    entry.cancellable = busy;

    const auto session = api_.session(id);
    entry.title = session.has_value() && !session->title_.empty()
        ? from_utf8(session->title_)
        : trs("Session #%1").arg(id);

    const lnb::GenerationTask::Snapshot snapshot = view->last_snapshot();
    entry.rounds = snapshot.rounds;
    entry.elapsed_ms = snapshot.elapsed_ms;
    entry.error = from_utf8(snapshot.error);
    entry.state = describe(snapshot.status);

    entries.push_back(std::move(entry));
  }

  if (assistant != nullptr) {
    const bool busy = !assistant->is_terminal();

    TasksWindow::Entry entry;
    entry.id = 0;
    entry.session_id = 0;
    entry.running = busy;
    entry.cancellable = busy;
    entry.title = trs("Assistant memory");

    const lnb::GenerationTask::Snapshot snapshot = assistant->snapshot();
    entry.rounds = snapshot.rounds;
    entry.elapsed_ms = snapshot.elapsed_ms;
    entry.error = from_utf8(snapshot.error);
    entry.state = describe(snapshot.status);

    entries.push_back(std::move(entry));
  }

  tasks_window_->set_entries(std::move(entries));
}

void MainWindow::open_tasks_window() {
  if (tasks_window_ == nullptr) {
    tasks_window_ = new TasksWindow(this);

    // The log is only composed when the reader opens a task, so a hidden window
    // never pays for the (potentially large) request bodies it recorded.
    tasks_window_->set_log_provider([this](std::uint64_t id) -> QString {
      if (id == 0) {
        const auto task = api_.assistant_task();
        return task != nullptr ? from_utf8(task->log_text()) : QString();
      }

      const auto it = conversations_.find(static_cast<std::uint32_t>(id));
      return it != conversations_.end() && it->second != nullptr
          ? from_utf8(it->second->task_log())
          : QString();
    });

    // A running task can be stopped from here. This is the only place a shared
    // assistant fold can be cancelled: the session composer never offers it.
    tasks_window_->set_cancel_handler([this](std::uint64_t id) {
      if (id == 0) {
        api_.cancel_assistant();
      } else {
        const auto it = conversations_.find(static_cast<std::uint32_t>(id));

        if (it != conversations_.end() && it->second != nullptr) {
          it->second->cancel();
        }
      }

      update_task_indicator();
    });
  }

  tasks_window_->show();
  tasks_window_->raise();
  tasks_window_->activateWindow();

  // The window is visible now, so this populates it immediately rather than
  // waiting for the next poll tick.
  update_task_indicator();
}

void MainWindow::pin_session(std::uint32_t session_id) {
  if (session_id == 0) {
    return;
  }

  const auto stored = api_.session(session_id);

  if (!stored.has_value()) {
    return;
  }

  lnb::Session updated = *stored;
  updated.pinned_ = !updated.pinned_;
  api_.save_session(updated);

  refresh_sessions();
}

void MainWindow::edit_session(std::uint32_t session_id) {
  if (session_id == 0) {
    return;
  }

  SessionEditor editor(api_, session_id, this);

  // A manual "Update memory now" runs as the session's own background task, so it
  // shows up in the tasks window like any other turn and can never race a
  // generation on the same session. The editor drives the view that owns that
  // task; it is created on first use here, without switching the visible pane.
  if (ConversationView* view = ensure_conversation_view(session_id)) {
    editor.set_conversation_view(view);
  }

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
      this, trs("Delete Session"),
      trs("Are you sure you want to delete this session? This cannot be undone."),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No
  );

  if (answer != QMessageBox::Yes) {
    return;
  }

  api_.delete_session(session_id);
  // A removed session's messages no longer contribute to the assistant trigger,
  // and its task has no row to appear on any more.
  api_.mark_assistant_summary_stale();
  refresh_sessions();
  update_header();
}

void MainWindow::update_header() {
  const auto session = api_.session(current_session_);

  if (!session.has_value()) {
    // No session open: the conversation info is simply absent; the banner on the
    // left of the bar stays.
    header_subtitle_->clear();
    header_summarization_trigger_->clear();
    header_summarization_trigger_->setToolTip(QString());
    header_time_->clear();
    return;
  }

  header_subtitle_->setText(trs("Session #%1").arg(session->id_));

  const lnb::api::ContextUsage usage = api_.context_usage(current_session_);

  header_summarization_trigger_->setText(
      trs("memory trigger: ") + format_tokens(usage.used_tokens, usage.limit_tokens)
  );
  header_summarization_trigger_->setToolTip(
      trs("%1 / %2 conversation tokens (system prompt excluded; the memory "
          "update fires at the limit)")
          .arg(usage.used_tokens)
          .arg(usage.limit_tokens)
  );

  header_time_->setText(format_time(api_.last_message_time(current_session_)));
}

void MainWindow::update_current_block() {
  const lnb::Assistant assistant = api_.assistant();

  const QString name = from_utf8(assistant.name_);
  current_name_->setText(name.isEmpty() ? trs("Assistant") : name);

  const QString model = from_utf8(assistant.model_);
  current_model_->setText(model);
  current_model_->setVisible(!model.isEmpty());

  current_settings_->setEnabled(true);
}

} // namespace ui
