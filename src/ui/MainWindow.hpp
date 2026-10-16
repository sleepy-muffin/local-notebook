#pragma once

#include <QWidget>

#include <cstdint>
#include <unordered_map>

#include "ui/BackgroundWidget.hpp"

class QLabel;
class QListView;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace lnb {
class Application;
}

namespace ui {

class ConversationView;
class SessionItemDelegate;
class SessionListModel;
class SettingsWindow;
class TasksWindow;

// The application shell: one full-width title bar across the top, a sidebar of
// sessions on the left and the transcript on the right. It owns the lazily
// created ConversationViews (one per open session) and drives their poll loop,
// so a generation keeps running while its session is off screen.
class MainWindow : public BackgroundWidget {
public:
  explicit MainWindow(lnb::Application& app, QWidget* parent = nullptr);
  ~MainWindow() override;

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void build_ui();
  QWidget* build_title_bar();
  QWidget* build_sidebar();
  QWidget* build_right_pane();
  QWidget* build_status_bar();

  // Builds one session list (view + model + delegate) with the shared row
  // behaviour: hover actions (pin / edit / delete) and click-to-open. Returns the
  // view and hands back the model and delegate it created.
  QListView* build_session_list(
      QWidget* parent, const QString& object_name, SessionListModel*& model,
      SessionItemDelegate*& delegate
  );

  // (Re)builds the window's scoped stylesheet from the current theme, so a
  // theme change in the settings window is reflected in the hand-painted chrome.
  void apply_window_style();

  // Rebuilds the sidebar list from the database and prunes views for sessions
  // that no longer exist.
  void refresh_sessions();

  // Opens (creating on first use) the chat pane for `session_id`.
  void open_conversation(std::uint32_t session_id);

  void new_session();
  void open_settings();

  // The session row's hover actions: pin toggles the pinned flag, edit opens the
  // session editor, delete asks for confirmation and then removes the session.
  void pin_session(std::uint32_t session_id);
  void edit_session(std::uint32_t session_id);
  void delete_session(std::uint32_t session_id);

  void poll_conversations();

  // The status bar: toggles the sidebar's visibility, and opens the window that
  // lists the running generation tasks.
  void toggle_sidebar();
  // Repaints the sidebar toggle's icon: accent while the sidebar is shown, muted
  // while it is hidden.
  void refresh_sidebar_toggle();
  void update_task_indicator();
  void open_tasks_window();

  // Refreshes the title bar: session name, id, context usage and last-message
  // time of the current session.
  void update_header();

  // Refreshes the current-session block at the foot of the sidebar.
  void update_current_block();

  lnb::Application& app_;

  QWidget* sidebar_ = nullptr;
  QWidget* sidebar_separator_ = nullptr;
  bool sidebar_visible_ = true;

  QListView* sessions_view_ = nullptr;
  SessionListModel* sessions_model_ = nullptr;
  SessionItemDelegate* sessions_delegate_ = nullptr;

  // The pinned category, above SESSIONS. The band (its category label plus its
  // list) is hidden entirely while no session is pinned.
  QWidget* pinned_band_ = nullptr;
  QListView* pinned_view_ = nullptr;
  SessionListModel* pinned_model_ = nullptr;
  SessionItemDelegate* pinned_delegate_ = nullptr;

  QPushButton* new_session_button_ = nullptr;

  QLabel* current_name_ = nullptr;
  QLabel* current_model_ = nullptr;
  QToolButton* current_settings_ = nullptr;

  QLabel* brand_ = nullptr;
  QLabel* category_ = nullptr;

  QLabel* header_subtitle_ = nullptr;
  QLabel* header_summarization_trigger_ = nullptr;
  QLabel* header_time_ = nullptr;

  QStackedWidget* workspace_ = nullptr;
  QLabel* empty_hint_ = nullptr;

  QTimer* poll_timer_ = nullptr;
  SettingsWindow* settings_window_ = nullptr;

  // The status bar's right-hand running-task indicator, and the unique,
  // non-modal window it opens.
  QToolButton* task_indicator_ = nullptr;
  QToolButton* sidebar_toggle_ = nullptr;
  TasksWindow* tasks_window_ = nullptr;
  int shown_task_count_ = -1;

  std::uint32_t current_session_ = 0;
  std::unordered_map<std::uint32_t, ConversationView*> conversations_;
};

} // namespace ui
