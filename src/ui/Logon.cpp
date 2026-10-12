#include "../ui.hpp"
#include "Logon.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QModelIndex>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

#include "BackgroundWidget.hpp"
#include "SessionEditor.hpp"
#include "SessionItemDelegate.hpp"
#include "SessionListModel.hpp"
#include "theme.hpp"

namespace ui {
QString to_qstring(const std::string& value) {
  return QString::fromUtf8(
    value.data(),
    static_cast<qsizetype>(value.size())
  );
}

Logon::Logon(
  lnb::ApplicationState& state,
  LoginCallback on_login,
  QWidget* parent
)
  : QMainWindow(parent),
    state_(state),
    on_login_(std::move(on_login)) {

  setWindowTitle("local-notebook - Login");
  resize(940, 580);
  setMinimumSize(760, 460);

  auto* main_widget = new BackgroundWidget(this);
  auto* main_layout = new QHBoxLayout(main_widget);

  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  // ---- Left: the identity list, mirroring the MainWindow sidebar ----
  const ThemeMetrics& metrics = theme().metrics;

  auto* sidebar = new QWidget(main_widget);
  sidebar->setObjectName(QStringLiteral("Sidebar"));
  sidebar->setAttribute(Qt::WA_StyledBackground, true);
  sidebar->setFixedWidth(metrics.sidebar_width);
  // Scoped to the sidebar itself, like the MainWindow sidebar, so the rule does
  // not leak into its children.
  sidebar->setStyleSheet(QStringLiteral("QWidget#Sidebar { border-right: 1px solid %1; }").arg(
      theme().colors.border.name(QColor::HexArgb)
  ));

  auto* sidebar_layout = new QVBoxLayout(sidebar);
  sidebar_layout->setContentsMargins(metrics.page_margin, metrics.page_margin,
                                     metrics.page_margin, metrics.page_margin);
  sidebar_layout->setSpacing(8);

  auto* brand = new QLabel("Local Notebook", sidebar);
  brand->setObjectName(QStringLiteral("SidebarBrand"));

  auto* list_title = new QLabel("Identities", sidebar);
  list_title->setObjectName(QStringLiteral("SidebarTitle"));

  sessions_view_ = new QListView(sidebar);
  sessions_view_->setObjectName(QStringLiteral("Sessions"));
  sessions_view_->setAttribute(Qt::WA_StyledBackground, true);
  sessions_view_->setSelectionMode(QAbstractItemView::SingleSelection);
  sessions_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  sessions_view_->setFrameShape(QFrame::NoFrame);
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

  // The identity picker only selects: no per-row menu, so the delegate omits its
  // overflow button and the rows use the same subtle hover/selection colours as
  // the MainWindow session list.
  sessions_view_->setItemDelegate(new SessionItemDelegate(sessions_view_));

  add_session_button_ = new QPushButton("Add Session", sidebar);
  add_session_button_->setObjectName(QStringLiteral("Secondary"));

  sidebar_layout->addWidget(brand);
  sidebar_layout->addWidget(list_title);
  sidebar_layout->addWidget(sessions_view_, 1);
  sidebar_layout->addWidget(add_session_button_);

  // ---- Right: the selected identity, styled like a settings page ----
  auto* page = new QWidget(main_widget);
  page->setObjectName(QStringLiteral("Page"));

  auto* page_layout = new QVBoxLayout(page);
  page_layout->setContentsMargins(0, 0, 0, 0);
  page_layout->setSpacing(0);

  // The settings-page style hooks are reused here so the panel matches the
  // settings window (transparent scroll area, page title/subtitle, footer).
  auto* scroll = new QScrollArea(page);
  scroll->setObjectName(QStringLiteral("SettingsPage"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);

  auto* inner = new QWidget(scroll);
  inner->setObjectName(QStringLiteral("SettingsPageInner"));

  auto* inner_layout = new QVBoxLayout(inner);
  inner_layout->setContentsMargins(28, 24, 28, 24);
  inner_layout->setSpacing(6);

  title_label_ = new QLabel("Select an identity", inner);
  title_label_->setObjectName(QStringLiteral("SettingsPageTitle"));

  model_label_ = new QLabel(inner);
  model_label_->setObjectName(QStringLiteral("SettingsPageSubtitle"));

  instructions_title_ = new QLabel("Custom instructions", inner);
  instructions_title_->setObjectName(QStringLiteral("SidebarTitle"));

  instructions_label_ = new QLabel(inner);
  instructions_label_->setWordWrap(true);
  instructions_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  inner_layout->addWidget(title_label_);
  inner_layout->addWidget(model_label_);
  inner_layout->addSpacing(10);
  inner_layout->addWidget(instructions_title_);
  inner_layout->addWidget(instructions_label_);
  inner_layout->addStretch(1);

  scroll->setWidget(inner);

  auto* divider = new QFrame(page);
  divider->setObjectName(QStringLiteral("SettingsDivider"));
  divider->setFixedHeight(1);
  divider->setAttribute(Qt::WA_StyledBackground, true);

  auto* footer = new QHBoxLayout();
  footer->setContentsMargins(16, 10, 16, 14);
  footer->setSpacing(8);
  footer->addStretch(1);

  login_button_ = new QPushButton("Login", page);
  login_button_->setEnabled(false);
  footer->addWidget(login_button_);

  page_layout->addWidget(scroll, 1);
  page_layout->addWidget(divider);
  page_layout->addLayout(footer);

  main_layout->addWidget(sidebar);
  main_layout->addWidget(page, 1);

  setCentralWidget(main_widget);

  connect(
    sessions_view_->selectionModel(),
    &QItemSelectionModel::currentChanged,
    this,
    [this](const QModelIndex&, const QModelIndex&) {
      update_preview();
    }
  );

  connect(
    add_session_button_,
    &QPushButton::clicked,
    this,
    [this]() {
      add_session();
    }
  );

  connect(
    login_button_,
    &QPushButton::clicked,
    this,
    [this]() {
      login();
    }
  );

  refresh_sessions();
}


// ----------------------------------------
// Refresh Session List
// ----------------------------------------

void Logon::refresh_sessions() {
  const uint32_t previous = selected_session_id_;
  const auto sessions = state_.uni_db_->get_all_sessions();

  sessions_model_->set_sessions(sessions);

  // Keep the previously selected identity selected when it still exists.
  QModelIndex target;

  if (sessions) {
    for (std::size_t i = 0; i < sessions->size(); ++i) {
      if ((*sessions)[i].id_ == previous) {
        target = sessions_model_->index(static_cast<int>(i), 0);
        break;
      }
    }
  }

  sessions_view_->setCurrentIndex(target);
  update_preview();
}


// ----------------------------------------
// Update Preview
// ----------------------------------------

void Logon::update_preview() {
  const QModelIndex index = sessions_view_->currentIndex();

  const auto show_empty = [this]() {
    selected_session_id_ = 0;
    title_label_->setText("Select an identity");
    model_label_->clear();
    model_label_->setVisible(false);
    set_instructions_visible(false);
    login_button_->setEnabled(false);
  };

  if (!index.isValid()) {
    show_empty();
    return;
  }

  const uint32_t id = sessions_model_->session_id_at(index.row());
  const auto session = state_.uni_db_->get_session_by_id(id);

  if (!session.has_value()) {
    show_empty();
    return;
  }

  selected_session_id_ = id;
  title_label_->setText(to_qstring(session->title_));

  if (session->model_.empty()) {
    model_label_->clear();
    model_label_->setVisible(false);
  } else {
    model_label_->setText(to_qstring(session->model_));
    model_label_->setVisible(true);
  }

  if (session->custom_instructions_.empty()) {
    set_instructions_visible(false);
  } else {
    instructions_label_->setText(to_qstring(session->custom_instructions_));
    set_instructions_visible(true);
  }

  login_button_->setEnabled(true);
}

void Logon::set_instructions_visible(bool visible) {
  instructions_title_->setVisible(visible);
  instructions_label_->setVisible(visible);
}

// ----------------------------------------
// Add Session
// ----------------------------------------

void Logon::add_session() {
  SessionEditor dialog(state_, std::nullopt, this);

  if (dialog.exec() == QDialog::Accepted) {
    refresh_sessions();
  }
}

// ----------------------------------------
// Login
// ----------------------------------------

void Logon::login() {
  if (selected_session_id_ == 0) {
    return;
  }

  const auto session =
      state_.uni_db_->get_session_by_id(selected_session_id_);

  if (!session.has_value()) {
    refresh_sessions();
    return;
  }

  state_.self_id_ = selected_session_id_;

  if (on_login_) {
    on_login_();
  }
}


}
