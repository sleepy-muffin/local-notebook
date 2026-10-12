#pragma once

#include "../ui.hpp"

#include <QDialog>

#include <cstdint>
#include <optional>

class QLineEdit;
class QPlainTextEdit;

namespace ui {

class SessionEditor : public QDialog {
public:
  explicit SessionEditor(
      lnb::ApplicationState& state,
      std::optional<lnb::session> session = std::nullopt,
      QWidget* parent = nullptr
  );

private:
  lnb::ApplicationState& state_;
  std::optional<lnb::session> session_;

  QLineEdit* title_edit_ = nullptr;
  QLineEdit* api_url_edit_ = nullptr;
  QLineEdit* token_edit_ = nullptr;
  QLineEdit* model_edit_ = nullptr;
  QLineEdit* proxy_edit_ = nullptr;
  QLineEdit* shared_directory_edit_ = nullptr;

  QPlainTextEdit* instructions_edit_ = nullptr;
  QPlainTextEdit* memory_edit_ = nullptr;

  void create_layout();
  void load_session();
  void save_session();
};

}