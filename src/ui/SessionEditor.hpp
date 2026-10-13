#pragma once

#include "../ui.hpp"

#include <QDialog>

#include <cstdint>
#include <optional>

class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

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

  // The memory watermark: the highest message id already folded into the
  // summary. Messages above it are still sent verbatim. Editable so it can be
  // corrected alongside a hand-edited memory.
  QSpinBox* watermark_spin_ = nullptr;

  // Session-bound generation budgets (request/summarisation/tool loop).
  QSpinBox* model_context_spin_ = nullptr;
  QSpinBox* response_budget_spin_ = nullptr;
  QSpinBox* safety_margin_spin_ = nullptr;
  QSpinBox* summary_keep_spin_ = nullptr;
  QSpinBox* summary_budget_spin_ = nullptr;
  QSpinBox* summary_headroom_spin_ = nullptr;
  QSpinBox* summary_trigger_spin_ = nullptr;
  QSpinBox* max_tool_rounds_spin_ = nullptr;
  QSpinBox* max_tool_calls_spin_ = nullptr;
  QSpinBox* tool_output_cap_spin_ = nullptr;

  QPushButton* save_button_ = nullptr;
  QPushButton* cancel_button_ = nullptr;
  QPushButton* summarize_button_ = nullptr;

  // True while a manual summarisation is running, so the action cannot be
  // started twice.
  bool summarizing_ = false;

  void create_layout();
  void load_session();
  void save_session();

  // Folds the whole transcript into the memory box on demand, regardless of the
  // automatic summarisation thresholds. Runs off the UI thread and reloads the
  // persisted memory when it finishes.
  void summarize_now();
};

}