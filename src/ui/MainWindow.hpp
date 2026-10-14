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

// The application shell: a sidebar of sessions on the left, a narrow title bar
// and the transcript on the right. It owns the lazily created ConversationViews
// (one per open session) and drives their poll loop, so a generation keeps
// running while its session is off screen.
class MainWindow : public BackgroundWidget {
public:
  explicit MainWindow(lnb::Application& app, QWidget* parent = nullptr);
  ~MainWindow() override;

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void build_ui();
  QWidget* build_sidebar();
  QWidget* build_right_pane();

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

  // The session row's two hover actions: edit opens the session editor, delete
  // asks for confirmation and then removes the session.
  void edit_session(std::uint32_t session_id);
  void delete_session(std::uint32_t session_id);

  void poll_conversations();

  // Refreshes the title bar: session name, id, context usage and last-message
  // time of the current session.
  void update_header();

  // Refreshes the current-session block at the foot of the sidebar.
  void update_current_block();

  lnb::Application& app_;

  QListView* sessions_view_ = nullptr;
  SessionListModel* sessions_model_ = nullptr;
  SessionItemDelegate* sessions_delegate_ = nullptr;
  QPushButton* new_session_button_ = nullptr;

  QLabel* current_name_ = nullptr;
  QLabel* current_id_ = nullptr;
  QToolButton* current_settings_ = nullptr;

  QLabel* brand_ = nullptr;
  QLabel* category_ = nullptr;

  QLabel* header_title_ = nullptr;
  QLabel* header_subtitle_ = nullptr;
  QLabel* header_context_ = nullptr;
  QLabel* header_time_ = nullptr;

  QStackedWidget* workspace_ = nullptr;
  QLabel* empty_hint_ = nullptr;

  QTimer* poll_timer_ = nullptr;
  SettingsWindow* settings_window_ = nullptr;

  std::uint32_t current_session_ = 0;
  std::unordered_map<std::uint32_t, ConversationView*> conversations_;
};

} // namespace ui
