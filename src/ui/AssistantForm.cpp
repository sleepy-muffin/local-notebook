#include "ui/AssistantForm.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cstddef>
#include <optional>
#include <string>

#include "ui/dialog_style.hpp"

namespace ui {

using namespace dialog_style;

namespace {

// Presets for the assistant-level summarisation budgets. Selecting one only fills
// the fields; it never saves, so the values can still be tweaked before Save.
struct SummaryPreset {
  const char* name;
  int memory_budget;
  int headroom;
  int trigger;
  int session_pool;
};

constexpr SummaryPreset kSummaryPresets[] = {
    // memory, reasoning headroom, trigger, session pool
    {"Very Light",          1000, 2000, 12000,  2000},
    {"Light",               2000, 4000, 24000,  4000},
    {"Balanced (default)",  4000, 8000, 48000,  8000},
    {"Deep memory",         8000,16000, 96000, 16000},
};

// constexpr SummaryPreset kSummaryPresets[] = {
//     {"Light", 2000, 4000, 40000, 8000},
//     {"Balanced (default)", 4000, 12000, 80000, 16000},
//     {"Deep memory", 8000, 24000, 160000, 32000},
// };

} // namespace

AssistantForm::AssistantForm(QWidget* parent) : QWidget(parent) {
  // Named so callers can locate the form without qobject_cast (the app has no
  // Q_OBJECT anywhere, so qobject_cast cannot be used on its own classes).
  setObjectName(QStringLiteral("AssistantFormWidget"));

  auto* content = new QVBoxLayout(this);
  // The right margin leaves room for the vertical scrollbar when the caller
  // drops the form into a scroll area (both callers do).
  content->setContentsMargins(0, 0, 12, 0);
  content->setSpacing(12);

  // ---- Assistant ----
  content->addWidget(section_label(this, QStringLiteral("Assistant")));
  {
    auto* form = form_layout(this);

    name_ = new QLineEdit(this);
    name_->setPlaceholderText(QStringLiteral("Assistant name"));

    model_ = new QLineEdit(this);
    model_->setPlaceholderText(QStringLiteral("Model id, e.g. gpt-4o"));

    form->addRow(field_label(this, QStringLiteral("Name")), name_);
    form->addRow(field_label(this, QStringLiteral("Model")), model_);
    content->addLayout(form);
  }

  content->addWidget(h_separator(this));

  // ---- Connection ----
  content->addWidget(section_label(this, QStringLiteral("Connection")));
  {
    auto* form = form_layout(this);

    api_url_ = new QLineEdit(this);
    api_url_->setPlaceholderText(QStringLiteral("https://api.example.com/v1/chat/completions"));

    token_ = new QLineEdit(this);
    token_->setPlaceholderText(QStringLiteral("Authentication token"));
    token_->setEchoMode(QLineEdit::Password);

    proxy_ = new QLineEdit(this);
    proxy_->setPlaceholderText(QStringLiteral("Optional"));

    form->addRow(field_label(this, QStringLiteral("API URL")), api_url_);
    form->addRow(field_label(this, QStringLiteral("Token")), token_);
    form->addRow(field_label(this, QStringLiteral("Proxy")), proxy_);
    content->addLayout(form);
  }

  content->addWidget(h_separator(this));

  // ---- Instructions ----
  content->addWidget(section_label(this, QStringLiteral("Custom instructions")));
  instructions_ = new QPlainTextEdit(this);
  instructions_->setPlaceholderText(QStringLiteral("System prompt for this assistant"));
  instructions_->setMinimumHeight(96);
  content->addWidget(instructions_);

  content->addWidget(h_separator(this));

  // ---- Summarisation ----
  content->addWidget(section_label(this, QStringLiteral("Summarisation")));

  preset_ = new ComboBox(this);
  preset_->addItem(QStringLiteral("Apply a preset..."));
  for (const SummaryPreset& summary_preset : kSummaryPresets) {
    preset_->addItem(QString::fromUtf8(summary_preset.name));
  }
  preset_->setToolTip(
      QStringLiteral("Fill the budgets below with a preset. Nothing is saved until you "
                     "press Save.")
  );
  content->addWidget(preset_);

  {
    auto* form = form_layout(this);

    summary_token_budget_ = make_spin(this, 0);
    summary_reasoning_headroom_ = make_spin(this, 0);
    summary_trigger_ = make_spin(this, 0);
    summary_session_budget_ = make_spin(this, 0);

    summary_token_budget_->setToolTip(
        QStringLiteral("Target size of the generated assistant memory.")
    );
    summary_reasoning_headroom_->setToolTip(
        QStringLiteral(
            "Extra completion budget for a reasoning model's hidden thinking, so the "
            "visible memory is not starved."
        )
    );
    summary_trigger_->setToolTip(
        QStringLiteral(
            "New tokens across all sessions that trigger the summariser; also the cap "
            "on the recent messages gathered."
        )
    );
    summary_session_budget_->setToolTip(
        QStringLiteral(
            "Token cap on the per-session summaries included in the assistant fold."
        )
    );

    form->addRow(field_label(this, QStringLiteral("Memory budget")), summary_token_budget_);
    form->addRow(
        field_label(this, QStringLiteral("Reasoning headroom")), summary_reasoning_headroom_
    );
    form->addRow(field_label(this, QStringLiteral("Trigger tokens")), summary_trigger_);
    form->addRow(field_label(this, QStringLiteral("Session pool")), summary_session_budget_);
    content->addLayout(form);

    connect(preset_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
      if (index <= 0) {
        return;
      }

      const SummaryPreset& summary_preset = kSummaryPresets[index - 1];
      summary_token_budget_->setValue(summary_preset.memory_budget);
      summary_reasoning_headroom_->setValue(summary_preset.headroom);
      summary_trigger_->setValue(summary_preset.trigger);
      summary_session_budget_->setValue(summary_preset.session_pool);
    });
  }

  content->addStretch(1);
}

void AssistantForm::set_assistant(const lnb::Assistant& assistant) {
  name_->setText(to_qstring(assistant.name_));
  model_->setText(to_qstring(assistant.model_));
  api_url_->setText(to_qstring(assistant.api_url_));
  token_->setText(to_qstring(assistant.authentication_token_));
  proxy_->setText(
      assistant.proxy_.has_value() ? to_qstring(*assistant.proxy_) : QString()
  );
  instructions_->setPlainText(to_qstring(assistant.custom_instructions_));

  summary_token_budget_->setValue(static_cast<int>(assistant.summary_token_budget_));
  summary_reasoning_headroom_->setValue(
      static_cast<int>(assistant.summary_reasoning_headroom_tokens_)
  );
  summary_trigger_->setValue(static_cast<int>(assistant.summary_trigger_tokens_));
  summary_session_budget_->setValue(
      static_cast<int>(assistant.summary_token_budget_for_session_summarization_)
  );
}

void AssistantForm::write_into(lnb::Assistant& assistant) const {
  assistant.name_ = name_->text().trimmed().toStdString();
  if (assistant.name_.empty()) {
    assistant.name_ = "Untitled assistant";
  }

  assistant.api_url_ = api_url_->text().trimmed().toStdString();
  assistant.authentication_token_ = token_->text().trimmed().toStdString();
  assistant.model_ = model_->text().trimmed().toStdString();

  const QString proxy = proxy_->text().trimmed();
  assistant.proxy_ = proxy.isEmpty()
      ? std::nullopt
      : std::optional<std::string>(proxy.toStdString());

  assistant.custom_instructions_ = instructions_->toPlainText().toStdString();

  assistant.summary_token_budget_ =
      static_cast<std::size_t>(summary_token_budget_->value());
  assistant.summary_reasoning_headroom_tokens_ =
      static_cast<std::size_t>(summary_reasoning_headroom_->value());
  assistant.summary_trigger_tokens_ =
      static_cast<std::size_t>(summary_trigger_->value());
  assistant.summary_token_budget_for_session_summarization_ =
      static_cast<std::size_t>(summary_session_budget_->value());
}

} // namespace ui
