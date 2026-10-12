#pragma once

#include <QMainWindow>
#include <cstdint>
#include <functional>

#include "../ui.hpp"

class QWidget;
class QListView;
class QLabel;
class QPushButton;

namespace ui {

class SessionListModel;

using LoginCallback = std::function<void()>;

class Logon : public QMainWindow {
public:
  explicit Logon(
      lnb::ApplicationState& state,
      LoginCallback on_login,
      QWidget* parent = nullptr
  );

  void refresh_sessions();

private:
  lnb::ApplicationState& state_;
  LoginCallback on_login_;

  QListView* sessions_view_ = nullptr;
  SessionListModel* sessions_model_ = nullptr;
  QLabel* title_label_ = nullptr;
  QLabel* model_label_ = nullptr;
  QLabel* instructions_label_ = nullptr;
  QLabel* instructions_title_ = nullptr;
  QPushButton* add_session_button_ = nullptr;
  QPushButton* login_button_ = nullptr;

  uint32_t selected_session_id_ = 0;

  void update_preview();
  void set_instructions_visible(bool visible);
  void add_session();
  void login();
};

}
