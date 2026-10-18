#pragma once

#include <QWidget>

#include "local-notebook-core/model.hpp"

class QComboBox;
class QEvent;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QWidget;

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

  // True when the API URL is usable (an http/https base ending in /v1). On false,
  // `error` carries a message naming the expected shape. Callers must check this
  // before writing the assistant, so an invalid endpoint is never saved.
  [[nodiscard]] bool validate_base_url(QString& error) const;

private:
  // Rebuilds the reasoning list for the current model, appending `preferred`
  // when the model no longer offers it (so a stale level stays visible and
  // correctable instead of being silently dropped).
  void rebuild_reasoning(const QString& preferred);

  // Completes the URL scheme when the field loses focus.
  bool eventFilter(QObject* watched, QEvent* event) override;

  QLineEdit* name_ = nullptr;
  QLineEdit* model_ = nullptr;
  QLineEdit* api_url_ = nullptr;
  QLineEdit* token_ = nullptr;
  QLineEdit* proxy_ = nullptr;
  QSpinBox* turn_timeout_ = nullptr;
  QSpinBox* summary_timeout_ = nullptr;
  QPlainTextEdit* instructions_ = nullptr;
  QComboBox* protocol_ = nullptr;
  QComboBox* reasoning_ = nullptr;
  QComboBox* preset_ = nullptr;
  QSpinBox* summary_token_budget_ = nullptr;
  QSpinBox* summary_reasoning_headroom_ = nullptr;
  QSpinBox* summary_trigger_ = nullptr;
  QSpinBox* summary_session_budget_ = nullptr;
};

} // namespace ui
