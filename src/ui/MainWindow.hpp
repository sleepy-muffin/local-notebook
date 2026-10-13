#pragma once

#include "ui.hpp"

#include <QMainWindow>

#include <cstdint>
#include <memory>
#include <unordered_map>

class QButtonGroup;
class QCloseEvent;
class QLabel;
class QListView;
class QModelIndex;
class QPoint;
class QPushButton;
class QStackedWidget;
class QTimer;
class QWidget;

namespace ui {

class ConversationController;
class ConversationView;
class SessionListModel;
class SettingsWindow;

class MainWindow : public QMainWindow {
public:
  explicit MainWindow(
      lnb::ApplicationState& state,
      QWidget* parent = nullptr
  );

  ~MainWindow() override;

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  // One lazily created chat pane plus its controller, keyed by session id.
  struct ConversationEntry {
    lnb::session session_info;
    ConversationView* view = nullptr;
    QWidget* container = nullptr;
    QLabel* context_label = nullptr;
    std::unique_ptr<ConversationController> controller;
  };

  lnb::ApplicationState& state_;

  QWidget* navigation_ = nullptr;
  QButtonGroup* navigation_group_ = nullptr;
  QStackedWidget* pages_ = nullptr;

  QListView* sessions_view_ = nullptr;
  SessionListModel* sessions_model_ = nullptr;
  QPushButton* new_session_button_ = nullptr;

  QStackedWidget* workspace_ = nullptr;
  QLabel* empty_hint_ = nullptr;
  QTimer* poll_timer_ = nullptr;
  SettingsWindow* settings_window_ = nullptr;

  std::unordered_map<uint32_t, ConversationEntry> conversations_;

  void create_layout();
  void create_navigation();
  void create_pages();
  QWidget* create_sessions_page();
  QWidget* create_conversation_container(
      ConversationView* view,
      const lnb::session& session,
      QLabel** context_label_out
  );


  void switch_page(int index);
  void refresh_sessions();
  void open_conversation(uint32_t session_id);
  void add_session();
  void poll_conversations();
  void open_settings();

  void show_session_menu(
      const QModelIndex& index,
      const QPoint& position
  );
};

} // namespace ui
