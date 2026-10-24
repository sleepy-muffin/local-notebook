#include "ui/AssistantForm.hpp"

#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cstddef>
#include <optional>
#include <string>

#include "local-notebook-core/interface.hpp"
#include "local-notebook-core/model.hpp"
#include "support/i18n.hpp"
#include "ui/dialog_style.hpp"

namespace ui {

using namespace dialog_style;

namespace {

// Presets for the assistant-level memory budgets. Selecting one only fills the
// fields; it never saves, so the values can still be tweaked before Save.
struct SummaryPreset {
  const char* name;
  int memory_budget;
  int headroom;
  int trigger;
  int session_pool;
};

constexpr SummaryPreset kSummaryPresets[] = {
    // memory, reasoning headroom, trigger, session pool
    {"Instant",             250,  1000,  3000,   500},
    {"Very Light",          500,  2000,  6000,  1000},
    {"Light",              1000,  2000, 12000,  2000},
    {"Balanced",           2000,  4000, 24000,  4000},
    {"Heavy",              4000,  8000, 48000,  8000},
    {"Very Heavy",         8000, 16000, 96000, 16000},
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
  content->addWidget(section_label(this, trs("Assistant")));
  {
    auto* form = form_layout(this);

    name_ = new QLineEdit(this);
    name_->setPlaceholderText(trs("Assistant name"));

    model_ = new QLineEdit(this);
    model_->setPlaceholderText(trs("Model id, e.g. gpt-4o"));

    // The reasoning level a new session of this assistant starts on. The list is
    // model-specific and leads with the cheapest level; it follows the model id
    // as it is typed.
    reasoning_ = new ComboBox(this);
    reasoning_->setObjectName(QStringLiteral("AssistantReasoning"));
    reasoning_->setToolTip(
        trs("The reasoning level a new session starts on. Each session can override "
            "it from the composer.")
    );

    // The wire protocol. Responses is the default (and the only endpoint that
    // supports tool calling for the GPT-6 family); Chat Completions stays
    // available for OpenAI-compatible servers that expose only that shape.
    protocol_ = new ComboBox(this);
    protocol_->setObjectName(QStringLiteral("AssistantProtocol"));
    protocol_->addItem(
        trs("Responses API"), static_cast<int>(lnb::net::ApiProtocol::Responses)
    );
    protocol_->addItem(
        trs("Chat Completions"), static_cast<int>(lnb::net::ApiProtocol::ChatCompletions)
    );
    protocol_->setToolTip(
        trs("The endpoint used for every request. Responses is required for tool "
            "calling on the GPT-6 family.")
    );

    form->addRow(field_label(this, trs("Name")), name_);
    form->addRow(field_label(this, trs("Model")), model_);
    form->addRow(field_label(this, trs("Reasoning")), reasoning_);

    // A description indented under the control, so it reads as part of the
    // Reasoning row: the level applies to new sessions and to assistant-wide
    // memory updates, while a turn and a session memory update use the session's
    // level.
    form->addRow(
        QString(), field_label(this, trs(
                       "Applies to a new session's starting level and to assistant-wide "
                       "memory updates, the shared memory across sessions. Chat turns "
                       "and session memory updates use the session's own level instead."
                   ))
    );

    form->addRow(field_label(this, trs("API protocol")), protocol_);
    content->addLayout(form);

    // A model change re-derives the reasoning list; a stored level the new model
    // does not offer stays visible so it can be corrected, never silently reset.
    connect(model_, &QLineEdit::textChanged, this, [this](const QString&) {
      rebuild_reasoning(reasoning_->currentText());
    });
  }

  content->addWidget(h_separator(this));

  // ---- Connection ----
  content->addWidget(section_label(this, trs("Connection")));
  {
    auto* form = form_layout(this);

    api_url_ = new QLineEdit(this);
    api_url_->setObjectName(QStringLiteral("AssistantApiUrl"));
    api_url_->setPlaceholderText(trs("https://api.example.com/v1"));
    api_url_->setToolTip(
        trs("The API base, ending in /v1. The endpoint (/responses or "
            "/chat/completions) is appended from the protocol above.")
    );
    // A base typed without a scheme gains "https://" when the field loses focus.
    api_url_->installEventFilter(this);

    token_ = new QLineEdit(this);
    token_->setPlaceholderText(trs("Authentication token"));
    token_->setEchoMode(QLineEdit::Password);

    proxy_ = new QLineEdit(this);
    proxy_->setPlaceholderText(trs("Optional"));

    // Request windows live with the connection because they describe how long a
    // silent socket is tolerated, not how much context is sent. A turn and a
    // memory update legitimately need different windows.
    turn_timeout_ = make_spin(this, 180);
    turn_timeout_->setObjectName(QStringLiteral("AssistantTurnTimeout"));
    turn_timeout_->setRange(30, 3600);
    turn_timeout_->setSingleStep(30);
    turn_timeout_->setSuffix(QStringLiteral(" s"));
    turn_timeout_->setToolTip(
        trs("Maximum silence, in seconds, while a chat turn streams its reply.")
    );

    summary_timeout_ = make_spin(this, 420);
    summary_timeout_->setObjectName(QStringLiteral("AssistantSummaryTimeout"));
    summary_timeout_->setRange(30, 7200);
    summary_timeout_->setSingleStep(60);
    summary_timeout_->setSuffix(QStringLiteral(" s"));
    summary_timeout_->setToolTip(
        trs("Maximum silence, in seconds, while a session or assistant memory "
            "update runs. Reasoning models often need longer.")
    );

    form->addRow(field_label(this, trs("API URL")), api_url_);
    form->addRow(field_label(this, trs("Token")), token_);
    form->addRow(field_label(this, trs("Proxy")), proxy_);
    form->addRow(field_label(this, trs("Turn timeout")), turn_timeout_);
    form->addRow(
        field_label(this, trs("Memory update timeout")), summary_timeout_
    );
    form->addRow(
        QString(), field_label(this, trs(
                       "Seconds a request may wait for the next response byte "
                       "before it is abandoned. Chat turns use the turn timeout; "
                       "session and assistant memory updates use the memory update "
                       "timeout. Connecting and uploading keep a fixed 60-second "
                       "window."
                   ))
    );
    content->addLayout(form);
  }

  content->addWidget(h_separator(this));

  // ---- Instructions ----
  content->addWidget(section_label(this, trs("Custom instructions")));
  instructions_ = new PlainTextEdit(this);
  instructions_->setPlaceholderText(trs("System prompt for this assistant"));
  instructions_->setMinimumHeight(96);
  content->addWidget(instructions_);

  content->addWidget(h_separator(this));

  // ---- Memory triggers ----
  content->addWidget(section_label(this, trs("Memory triggers")));

  preset_ = new ComboBox(this);
  preset_->setObjectName(QStringLiteral("SummaryPreset"));
  preset_->addItem(trs("Apply a preset..."));
  for (const SummaryPreset& summary_preset : kSummaryPresets) {
    preset_->addItem(trs(summary_preset.name));
  }
  preset_->setToolTip(
      trs("Fill the memory trigger and budget fields below with a preset. "
          "Nothing is saved until you press Save.")
  );
  content->addWidget(preset_);

  {
    auto* form = form_layout(this);

    summary_trigger_ = make_spin(this, 0);
    summary_session_budget_ = make_spin(this, 0);

    summary_trigger_->setToolTip(
        trs(
            "New tokens across all sessions that trigger a memory update; also the cap "
            "on the recent messages gathered."
        )
    );
    summary_session_budget_->setToolTip(
        trs(
            "Token cap on the per-session memories included in the assistant memory."
        )
    );

    form->addRow(field_label(this, trs("Trigger tokens")), summary_trigger_);
    form->addRow(field_label(this, trs("Session pool")), summary_session_budget_);
    content->addLayout(form);
  }

  content->addWidget(h_separator(this));

  // ---- Budgets ----
  content->addWidget(section_label(this, trs("Budgets")));

  {
    auto* form = form_layout(this);

    summary_token_budget_ = make_spin(this, 0);
    summary_reasoning_headroom_ = make_spin(this, 0);

    summary_token_budget_->setToolTip(
        trs("Target size of the generated assistant memory.")
    );
    summary_reasoning_headroom_->setToolTip(
        trs(
            "Extra completion budget for a reasoning model's hidden thinking, so the "
            "visible memory is not starved."
        )
    );

    form->addRow(field_label(this, trs("Memory budget")), summary_token_budget_);
    form->addRow(
        field_label(this, trs("Reasoning headroom")), summary_reasoning_headroom_
    );
    content->addLayout(form);
  }

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

  content->addStretch(1);
}

void AssistantForm::set_assistant(const lnb::Assistant& assistant) {
  name_->setText(to_qstring(assistant.name_));
  model_->setText(to_qstring(assistant.model_));

  // Settings the model above first fired textChanged, which rebuilt the reasoning
  // list; re-derive it once more with the stored level so the value survives.
  rebuild_reasoning(to_qstring(assistant.reasoning_effort_));

  const int protocol_index = protocol_->findData(
      static_cast<int>(assistant.protocol_)
  );
  protocol_->setCurrentIndex(protocol_index >= 0 ? protocol_index : 0);

  api_url_->setText(to_qstring(assistant.api_url_));
  token_->setText(to_qstring(assistant.authentication_token_));
  proxy_->setText(
      assistant.proxy_.has_value() ? to_qstring(*assistant.proxy_) : QString()
  );
  turn_timeout_->setValue(static_cast<int>(assistant.turn_timeout_seconds_));
  summary_timeout_->setValue(
      static_cast<int>(assistant.summary_timeout_seconds_)
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

  assistant.authentication_token_ = token_->text().trimmed().toStdString();
  assistant.model_ = model_->text().trimmed().toStdString();

  // The base URL is normalised (a legacy full endpoint migrates to its base) and
  // the scheme is completed here too, so a save that never blurred the field
  // still stores a usable base. An empty field stays empty rather than gaining a
  // bare scheme.
  const std::string url_text = api_url_->text().trimmed().toStdString();
  assistant.api_url_ = url_text.empty()
      ? std::string{}
      : lnb::api::normalize_base_url(lnb::api::ensure_scheme(url_text));

  assistant.protocol_ = static_cast<lnb::net::ApiProtocol>(
      protocol_->currentData().toInt()
  );
  assistant.reasoning_effort_ = reasoning_->currentText().toStdString();

  const QString proxy = proxy_->text().trimmed();
  assistant.proxy_ = proxy.isEmpty()
      ? std::nullopt
      : std::optional<std::string>(proxy.toStdString());

  assistant.turn_timeout_seconds_ =
      static_cast<std::size_t>(turn_timeout_->value());
  assistant.summary_timeout_seconds_ =
      static_cast<std::size_t>(summary_timeout_->value());

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

bool AssistantForm::validate_base_url(QString& error) const {
  // The stored base is passed through the same completion a focus-out performs,
  // so typing "example.com/v1" validates as "https://example.com/v1".
  const std::string url = lnb::api::ensure_scheme(
      api_url_->text().trimmed().toStdString()
  );

  if (!lnb::api::is_valid_base_url(url)) {
    error = trs("The API URL must be an http:// or https:// base ending in /v1, "
                "for example https://api.example.com/v1.");
    return false;
  }

  return true;
}

void AssistantForm::rebuild_reasoning(const QString& preferred) {
  const std::string model = model_->text().trimmed().toStdString();
  const std::string wanted = preferred.trimmed().toStdString();

  // An empty selection (a fresh form) starts on the model's first available level.
  const std::string target =
      wanted.empty() ? lnb::api::reasoning_default_for(model) : wanted;

  const std::vector<std::string> efforts =
      lnb::api::reasoning_levels_for(model, target);

  // Rebuilding the list would otherwise emit spurious change signals.
  const QSignalBlocker blocker(reasoning_);
  reasoning_->clear();

  int selected = 0;
  for (std::size_t i = 0; i < efforts.size(); ++i) {
    reasoning_->addItem(to_qstring(efforts[i]));

    if (QString::compare(to_qstring(efforts[i]), to_qstring(target), Qt::CaseInsensitive) == 0) {
      selected = static_cast<int>(i);
    }
  }

  reasoning_->setCurrentIndex(selected);
}

bool AssistantForm::eventFilter(QObject* watched, QEvent* event) {
  if (watched == api_url_ && event->type() == QEvent::FocusOut) {
    const QString text = api_url_->text().trimmed();

    if (!text.isEmpty()) {
      // Completing the scheme is a visible, explicit edit: the field now shows
      // exactly the base that will be stored.
      api_url_->setText(
          to_qstring(lnb::api::ensure_scheme(text.toStdString()))
      );
    }
  }

  return QWidget::eventFilter(watched, event);
}

} // namespace ui
