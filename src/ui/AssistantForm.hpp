#pragma once

#include <QWidget>

#include "model.hpp"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

namespace ui {

// A reusable editor for an assistant's configuration: identity, connection
// details, custom instructions and the assistant-level summarisation budgets.
//
// It is embedded both by the startup picker's create/edit dialog and by the
// Assistant page of the settings window, so the two never drift apart. The form
// owns only the fields it displays; write_into() therefore leaves every other
// field — most notably Assistant::memory_, which is edited elsewhere — exactly
// as it was, so the form can be layered over an existing assistant without
// clobbering it.
class AssistantForm : public QWidget {
public:
  explicit AssistantForm(QWidget* parent = nullptr);

  // Fills every field from `assistant`.
  void set_assistant(const lnb::Assistant& assistant);

  // Writes the edited fields back into `assistant`, leaving the fields the form
  // does not own untouched.
  void write_into(lnb::Assistant& assistant) const;

private:
  QLineEdit* name_ = nullptr;
  QLineEdit* model_ = nullptr;
  QLineEdit* api_url_ = nullptr;
  QLineEdit* token_ = nullptr;
  QLineEdit* proxy_ = nullptr;
  QPlainTextEdit* instructions_ = nullptr;
  QComboBox* preset_ = nullptr;
  QSpinBox* summary_token_budget_ = nullptr;
  QSpinBox* summary_reasoning_headroom_ = nullptr;
  QSpinBox* summary_trigger_ = nullptr;
  QSpinBox* summary_session_budget_ = nullptr;
};

} // namespace ui
