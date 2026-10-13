//
// Created by menv on 10/11/26.
//

#include "MainWindow.hpp"

#include "SessionListModel.hpp"
#include "SessionItemDelegate.hpp"
#include "SessionEditor.hpp"
#include "ConversationView.hpp"
#include "ConversationController.hpp"
#include "BackgroundWidget.hpp"
#include "SettingsWindow.hpp"
#include "theme.hpp"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSize>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstddef>
#include <utility>
#include <vector>

namespace ui {
namespace {

// Formats a token pair the way editor agent panels do: compact numbers so the
// label stays short ("12.4k / 128k").
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

} // namespace

MainWindow::MainWindow(
    lnb::ApplicationState& state,
    QWidget* parent
)
    : QMainWindow(parent),
      state_(state) {
  setWindowTitle("Local Notebook");
  resize(1000, 680);

  create_layout();
  refresh_sessions();

  // A single timer drives the live streaming of every conversation, visible or
  // not, so a generation keeps advancing (and completes) while its session is
  // not on screen.
  poll_timer_ = new QTimer(this);

  connect(
      poll_timer_,
      &QTimer::timeout,
      this,
      [this]() { poll_conversations(); }
  );

  poll_timer_->start(33);
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
  // Ask every in-flight generation to stop, so the worker pool can be joined
  // promptly during shutdown.
  for (auto& [id, entry]: conversations_) {
    if (entry.controller) {
      entry.controller->cancel();
    }
  }

  if (poll_timer_) {
    poll_timer_->stop();
  }

  QMainWindow::closeEvent(event);
}

void MainWindow::create_navigation() {
  navigation_ = new QWidget(this);
  navigation_->setObjectName(QStringLiteral("Nav"));
  navigation_->setAttribute(Qt::WA_StyledBackground, true);
  navigation_->setFixedWidth(theme().metrics.nav_width);

  auto* layout = new QVBoxLayout(navigation_);
  layout->setContentsMargins(12, 14, 12, 14);
  layout->setSpacing(6);

  // One exclusive button group drives the page stack. Buttons are icon-only;
  // their label is exposed as a tooltip instead of taking up room.
  navigation_group_ = new QButtonGroup(this);
  navigation_group_->setExclusive(true);

  struct NavEntry {
    const char* icon;
    const char* label;
  };

  const NavEntry entries[] = {
      {"mail-message-new", "Chat"},
      {"settings-configure", "Settings"},
      {"user-identity", "Profile"},
  };

  int index = 0;

  for (const NavEntry& entry: entries) {
    auto* button = new QToolButton(navigation_);
    button->setObjectName(QStringLiteral("NavButton"));
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(22, 22));
    button->setFixedSize(44, 44);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(QString::fromUtf8(entry.label));

    const QIcon icon = QIcon::fromTheme(QString::fromUtf8(entry.icon));

    // Fall back to the text label when the icon theme does not provide it.
    if (icon.isNull()) {
      button->setToolButtonStyle(Qt::ToolButtonTextOnly);
      button->setText(QString::fromUtf8(entry.label));
    } else {
      button->setIcon(icon);
    }

    navigation_group_->addButton(button, index++);
    layout->addWidget(button, 0, Qt::AlignHCenter);
  }

  layout->addStretch(1);

  connect(
      navigation_group_,
      &QButtonGroup::idClicked,
      this,
      [this](int page_index) { switch_page(page_index); }
  );
}

void MainWindow::create_layout() {
  // The backdrop paints the optional background image (or the window colour),
  // so nothing is ever left unpainted - this also removes the thin line some
  // window managers drew above the content area.
  auto* central_widget = new BackgroundWidget(this);
  auto* main_layout = new QHBoxLayout(central_widget);

  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);
  central_widget->setMinimumWidth(0);

  create_pages();

  main_layout->addWidget(pages_, 1);
  setCentralWidget(central_widget);
}

QWidget* MainWindow::create_conversation_container(
    ConversationView* view,
    const lnb::session& session,
    QLabel** context_label_out
) {
  auto* container = new QWidget(workspace_);
  container->setObjectName(QStringLiteral("ConversationContainer"));
  container->setMinimumWidth(0);
  container->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  auto* layout = new QVBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto* header = new QFrame(container);
  header->setObjectName(QStringLiteral("ConversationHeader"));
  header->setAttribute(Qt::WA_StyledBackground, true);
  header->setFixedHeight(52);

  auto* header_layout = new QVBoxLayout(header);
  header_layout->setContentsMargins(18, 7, 18, 7);
  header_layout->setSpacing(0);

  auto* title = new QLabel(QString::fromStdString(session.title_), header);
  title->setObjectName(QStringLiteral("ConversationTitle"));
  auto* subtitle = new QLabel(
      QStringLiteral("Session #%1").arg(session.id_), header
  );
  subtitle->setObjectName(QStringLiteral("ConversationSubtitle"));
  auto* timestamp = new QLabel(
      session.last_message_at_.has_value()
          ? QString::fromStdString(*session.last_message_at_)
          : QStringLiteral("No messages"),
      header
  );
  timestamp->setObjectName(QStringLiteral("ConversationTimestamp"));
  auto* context = new QLabel(QString(), header);
  context->setObjectName(QStringLiteral("ConversationContext"));
  context->setToolTip(QStringLiteral("Context tokens used by the next request"));
  auto* header_row = new QHBoxLayout();
  header_row->setContentsMargins(0, 0, 0, 0);
  header_row->setSpacing(0);
  auto* subtitle_column = new QVBoxLayout();
  subtitle_column->setContentsMargins(0, 0, 0, 0);
  subtitle_column->setSpacing(0);
  subtitle_column->addWidget(subtitle);
  header_row->addLayout(subtitle_column);
  header_row->addStretch(1);
  header_row->addWidget(context);
  header_row->addSpacing(12);
  header_row->addWidget(timestamp);
  header_layout->addWidget(title);
  header_layout->addLayout(header_row);

  if (context_label_out != nullptr) {
    *context_label_out = context;
  }

  layout->addWidget(header);
  layout->addWidget(view, 1);
  return container;
}

void MainWindow::switch_page(int index) {
  if (index < 0 || index >= pages_->count()) {
    return;
  }

  pages_->setCurrentIndex(index);
}

void MainWindow::create_pages() {
  pages_ = new QStackedWidget(this);
  pages_->setObjectName(QStringLiteral("Pages"));
  pages_->setFrameShape(QFrame::NoFrame);

  auto* sessions_page = create_sessions_page();
  auto* settings_page = new QWidget(pages_);
  auto* profile_page = new QWidget(pages_);

  settings_page->setObjectName(QStringLiteral("Page"));
  profile_page->setObjectName(QStringLiteral("Page"));

  pages_->addWidget(sessions_page);
  pages_->addWidget(settings_page);
  pages_->addWidget(profile_page);

  pages_->setCurrentIndex(0);
}

QWidget* MainWindow::create_sessions_page() {
  const ThemeMetrics& metrics = theme().metrics;

  auto* page = new QWidget(this);
  page->setObjectName(QStringLiteral("Page"));
  auto* layout = new QHBoxLayout(page);

  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto* sidebar = new QWidget(page);
  sidebar->setObjectName(QStringLiteral("Sidebar"));
  sidebar->setAttribute(Qt::WA_StyledBackground, true);

  auto* sidebar_layout = new QVBoxLayout(sidebar);

  sidebar->setFixedWidth(metrics.sidebar_width);
  // Scoped to the sidebar itself: an unqualified rule would be inherited by
  // every child widget (including the current-user card and its buttons), which
  // painted a stray vertical border beside the settings button.
  sidebar->setStyleSheet(QStringLiteral("QWidget#Sidebar { border-right: 1px solid %1; }").arg(
      theme().colors.border.name(QColor::HexArgb)
  ));

  sidebar_layout->setContentsMargins(metrics.page_margin, metrics.page_margin,
                                     metrics.page_margin, metrics.page_margin);
  sidebar_layout->setSpacing(8);

  auto* brand = new QLabel("Local Notebook", sidebar);
  brand->setObjectName(QStringLiteral("SidebarBrand"));

  auto* title = new QLabel("Sessions", sidebar);
  title->setObjectName(QStringLiteral("SidebarTitle"));

  sessions_view_ = new QListView(sidebar);
  sessions_view_->setObjectName(QStringLiteral("Sessions"));
  sessions_view_->setAttribute(Qt::WA_StyledBackground, true);

  sessions_view_->setSelectionMode(QAbstractItemView::SingleSelection);
  sessions_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);

  sessions_view_->setFrameShape(QFrame::NoFrame);
  sessions_view_->setFrameShadow(QFrame::Plain);
  sessions_view_->setLineWidth(0);
  sessions_view_->setMidLineWidth(0);
  sessions_view_->setStyleSheet(QStringLiteral("QListView#Sessions { border: 0; outline: 0; }"));
  sessions_view_->viewport()->setStyleSheet(QStringLiteral("border: 0; outline: 0;"));
  sessions_view_->setSpacing(0);
  sessions_view_->setUniformItemSizes(true);
  sessions_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  sessions_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  sessions_view_->setSelectionRectVisible(false);

  sessions_view_->setMouseTracking(true);
  sessions_view_->viewport()->setMouseTracking(true);

  sessions_model_ = new SessionListModel(
      state_.uni_db_->get_all_sessions(),
      sessions_view_
  );

  sessions_view_->setModel(sessions_model_);

  auto* delegate = new SessionItemDelegate(sessions_view_);
  sessions_view_->setItemDelegate(delegate);
  delegate->set_menu_handler(
      [this](
          const QModelIndex& index,
          const QPoint& position
      ) {
        show_session_menu(index, position);
      }
  );

  connect(
      sessions_view_,
      &QListView::clicked,
      this,
      [this](const QModelIndex& index) {
        if (index.isValid()) {
          open_conversation(sessions_model_->session_id_at(index.row()));
        }
      }
  );

  new_session_button_ = new QPushButton("New Session", sidebar);
  new_session_button_->setObjectName(QStringLiteral("Secondary"));

  connect(
      new_session_button_,
      &QPushButton::clicked,
      this,
      [this]() { add_session(); }
  );

  auto* current_user = new QFrame(sidebar);
  current_user->setObjectName(QStringLiteral("CurrentUser"));
  current_user->setAttribute(Qt::WA_StyledBackground, true);
  current_user->setFrameShape(QFrame::NoFrame);
  current_user->setLineWidth(0);
  current_user->setMidLineWidth(0);
  auto* current_layout = new QHBoxLayout(current_user);
  current_layout->setContentsMargins(10, 8, 8, 8);
  current_layout->setSpacing(8);

  auto* current_text = new QVBoxLayout();
  current_text->setSpacing(0);
  auto current_session = state_.uni_db_->get_session_by_id(state_.self_id_);
  auto* current_name = new QLabel(
      current_session.has_value() ? QString::fromStdString(current_session->title_)
                                  : QStringLiteral("Unknown user"),
      current_user
  );
  current_name->setObjectName(QStringLiteral("CurrentUserName"));
  auto* current_id = new QLabel(
      QStringLiteral("Session #%1").arg(state_.self_id_), current_user
  );
  current_id->setObjectName(QStringLiteral("CurrentUserId"));
  current_text->addWidget(current_name);
  current_text->addWidget(current_id);

  auto* current_settings = new QToolButton(current_user);
  current_settings->setObjectName(QStringLiteral("CurrentUserSettings"));
  current_settings->setToolButtonStyle(Qt::ToolButtonIconOnly);
  current_settings->setAutoRaise(true);
  current_settings->setFocusPolicy(Qt::NoFocus);
  current_settings->setFixedSize(28, 28);
  current_settings->setIconSize(QSize(18, 18));
  current_settings->setIcon(QIcon::fromTheme(QStringLiteral("settings-configure")));
  current_settings->setToolTip(QStringLiteral("User settings"));

  connect(
      current_settings,
      &QToolButton::clicked,
      this,
      [this]() { open_settings(); }
  );

  current_layout->addLayout(current_text, 1);
  current_layout->addWidget(current_settings);

  sidebar_layout->addWidget(brand);
  sidebar_layout->addWidget(title);
  sidebar_layout->addWidget(sessions_view_, 1);
  sidebar_layout->addWidget(new_session_button_);
  sidebar_layout->addWidget(current_user);

  workspace_ = new QStackedWidget(page);
  workspace_->setObjectName(QStringLiteral("Workspace"));
  workspace_->setFrameShape(QFrame::NoFrame);
  workspace_->setMinimumWidth(0);
  workspace_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

  empty_hint_ = new QLabel(
      "Select a session to start a conversation",
      workspace_
  );

  empty_hint_->setObjectName(QStringLiteral("EmptyHint"));
  empty_hint_->setAlignment(Qt::AlignCenter);

  workspace_->addWidget(empty_hint_);
  workspace_->setCurrentWidget(empty_hint_);

  layout->addWidget(sidebar);
  layout->addWidget(workspace_, 1);

  return page;
}

void MainWindow::refresh_sessions() {
  if (!sessions_model_) {
    return;
  }

  try {
    const auto all = state_.uni_db_->get_all_sessions();

    // A session cannot talk to itself, so it is never listed as a chat target.
    auto visible = std::make_shared<lnb::session_list>();
    visible->reserve(all->size());

    for (const auto& session: *all) {
      if (session.id_ != state_.self_id_) {
        visible->push_back(session);
      }
    }

    sessions_model_->set_sessions(std::move(visible));

    // Drop conversations whose session no longer exists, and refresh the
    // settings of the rest (they may have been edited).
    std::vector<uint32_t> removed;

    for (auto& [id, entry]: conversations_) {
      const lnb::session* current = nullptr;

      for (const auto& session: *all) {
        if (session.id_ == id) {
          current = &session;
          break;
        }
      }

      if (current == nullptr) {
        removed.push_back(id);
      } else {
        entry.session_info = *current;

        if (entry.controller) {
          entry.controller->set_session_info(*current);
        }
      }
    }

    for (const uint32_t id: removed) {
      auto it = conversations_.find(id);

      if (it == conversations_.end()) {
        continue;
      }

      if (workspace_->currentWidget() == it->second.container) {
        workspace_->setCurrentWidget(empty_hint_);
      }

      workspace_->removeWidget(it->second.container);
      it->second.container->deleteLater();
      conversations_.erase(it);
    }
  } catch (const std::exception& error) {
    QMessageBox::critical(
        this,
        "Unable to Load Sessions",
        QString::fromUtf8(error.what())
    );
  }
}

void MainWindow::open_conversation(uint32_t session_id) {
  if (session_id == 0 || session_id == state_.self_id_) {
    return;
  }

  auto it = conversations_.find(session_id);

  if (it == conversations_.end()) {
    const auto session = state_.uni_db_->get_session_by_id(session_id);

    if (!session.has_value()) {
      refresh_sessions();
      return;
    }

    auto* view = new ConversationView(state_, workspace_);
    QLabel* context_label = nullptr;
    auto* container = create_conversation_container(view, *session, &context_label);
    auto controller = std::make_unique<ConversationController>(
        state_,
        *session,
        view
    );

    ConversationController* raw_controller = controller.get();

    controller->set_on_context_usage(
        [context_label](std::size_t used, std::size_t limit) {
          context_label->setText(format_tokens(used, limit));
          context_label->setToolTip(
              QStringLiteral("%1 / %2 tokens").arg(used).arg(limit)
          );
        }
    );

    view->set_on_send(
        [raw_controller](std::string text) {
          raw_controller->send(text);
        }
    );

    view->set_on_cancel(
        [raw_controller]() {
          raw_controller->cancel();
        }
    );

    view->set_on_delete_message(
        [raw_controller](uint32_t message_id) {
          raw_controller->delete_message(message_id);
        }
    );

    view->set_on_edit_message(
        [raw_controller](uint32_t message_id, const std::string& text) {
          raw_controller->edit_message(message_id, text);
        }
    );

    workspace_->addWidget(container);

    it = conversations_.emplace(
        session_id,
        ConversationEntry{*session, view, container, context_label, std::move(controller)}
    ).first;
  }

  workspace_->setCurrentWidget(it->second.container);
}

void MainWindow::add_session() {
  SessionEditor dialog(state_, std::nullopt, this);

  if (dialog.exec() == QDialog::Accepted) {
    refresh_sessions();
  }
}

void MainWindow::open_settings() {
  // A single, reused window: closing it only hides it.
  if (settings_window_ == nullptr) {
    settings_window_ = new SettingsWindow(state_, this);
  }

  settings_window_->show();
  settings_window_->raise();
  settings_window_->activateWindow();
}

void MainWindow::poll_conversations() {
  for (auto& [id, entry]: conversations_) {
    if (entry.controller) {
      entry.controller->poll();
    }
  }
}

void MainWindow::show_session_menu(
    const QModelIndex& index,
    const QPoint& position
) {
  if (!index.isValid()) {
    return;
  }

  const uint32_t session_id = sessions_model_->session_id_at(index.row());

  if (session_id == 0) {
    return;
  }

  try {
    const auto session = state_.uni_db_->get_session_by_id(session_id);

    if (!session.has_value()) {
      refresh_sessions();
      return;
    }

    QMenu menu(this);
    QAction* edit_action = menu.addAction("Edit");
    QAction* delete_action = menu.addAction("Delete");

    QAction* selected_action = menu.exec(position);

    if (selected_action == edit_action) {
      SessionEditor dialog(state_, *session, this);

      if (dialog.exec() == QDialog::Accepted) {
        refresh_sessions();
      }
    } else if (selected_action == delete_action) {
      const auto result = QMessageBox::question(
          this,
          "Delete Session",
          "Are you sure you want to delete this session?"
      );

      if (result == QMessageBox::Yes) {
        state_.uni_db_->remove_session(session_id);
        refresh_sessions();
      }
    }
  } catch (const std::exception& error) {
    QMessageBox::critical(
        this,
        "Session Operation Failed",
        QString::fromUtf8(error.what())
    );
  }
}

} // namespace ui
