#pragma once

#include <QDialog>

#include <cstdint>

#include "model.hpp"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QWidget;

namespace lnb {
class Application;
}

namespace ui {

// Edits one session: its title, custom instructions, shared directory, memory and
// every session-bound budget (design-1.md §8). The window is scrollable and the
// memory box is directly editable, with a "Summarize now" action that folds the
// transcript into memory on demand.
//
// Pass a session_id of 0 to create a new session; the accepted id is exposed by
// session_id().
class SessionEditor : public QDialog {
public:
  SessionEditor(lnb::Application& app, std::uint32_t session_id, QWidget* parent = nullptr);

  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }

private:
  QWidget* build_content();
  void load_session();
  void save_session();

  // Folds the whole transcript into the memory box on demand, regardless of the
  // automatic trigger. Runs off the UI thread and reloads the persisted memory
  // when it finishes.
  void summarize_now();

  lnb::Application& app_;

  // 0 while creating; the stored id once the session exists.
  std::uint32_t session_id_ = 0;
  lnb::Session session_;

  QLineEdit* title_ = nullptr;
  QPlainTextEdit* instructions_ = nullptr;
  QLineEdit* shared_directory_ = nullptr;

  QPlainTextEdit* memory_ = nullptr;
  QSpinBox* watermark_ = nullptr;
  QSpinBox* assistant_watermark_ = nullptr;

  QPushButton* summarize_button_ = nullptr;
  QPushButton* save_button_ = nullptr;
  QPushButton* cancel_button_ = nullptr;

  // Fills the budget fields below with a preset; it never saves by itself.
  QComboBox* preset_ = nullptr;

  // Session-bound budgets.
  QSpinBox* summary_keep_ = nullptr;
  QSpinBox* summary_trigger_ = nullptr;
  QSpinBox* summary_budget_ = nullptr;
  QSpinBox* summary_headroom_ = nullptr;
  QSpinBox* max_tool_rounds_ = nullptr;
  QSpinBox* max_tool_calls_ = nullptr;
  QSpinBox* tool_output_cap_ = nullptr;
  QSpinBox* response_budget_ = nullptr;

  // True while a manual summarisation is running, so the action cannot be
  // started twice.
  bool summarizing_ = false;
};

} // namespace ui
