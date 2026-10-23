#include "ui/SessionEditor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QEventLoop>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstddef>
#include <optional>
#include <string>

#include "local-notebook-core/interface.hpp"
#include "local-notebook-core/generation_task.hpp"
#include "local-notebook-core/model.hpp"
#include "support/i18n.hpp"
#include "ui/BackgroundWidget.hpp"
#include "ui/ConversationView.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/SpinBox.hpp"
#include "ui/SummaryReviewDialog.hpp"
#include "ui/dialog_style.hpp"
#include "ui/theme.hpp"

namespace ui {

using namespace dialog_style;

namespace {

// Presets for the session budgets (design-1.md §8). Selecting one only fills the
// fields below; it never saves, so the values can still be tweaked before Save.
// The values are starting points, not limits.
struct BudgetPreset {
  const char* name;
  int trigger;
  int keep;
  int memory_budget;
  int headroom;
  int response;
  int tool_output;
  int rounds;
  int calls;
};

/**
 * summary_trigger_ summary_keep_ summary_budget_ summary_headroom_
 *
 * response_budget_ tool_output_cap_
 *
 * max_tool_rounds_ max_tool_calls_
 */
constexpr BudgetPreset kBudgetPresets[] = {
    // summary: trigger (the input bound), keep (the level it collapses to),
    // memory, reasoning;
    // response;
    // tool: output, rounds, calls
    {"Instant",      1000,   250,   250,   500,   500,  1000,  0,   0},
    {"Very Light",   2000,   500,   500,  1000,  1000,  2000,  2,   6},
    {"Light",        4000,  1000,  1000,  2000,  2000,  4000, 16,  64},
    {"Balanced",     8000,  2000,  2000,  4000,  4000,  6000, 24,  96},
    {"Heavy",       16000,  4000,  4000,  8000,  8000,  8000, 32, 128},
    {"Very Heavy",  32000,  8000,  6000, 16000, 12000, 12000, 48, 192},
    {"Ultra",       64000, 16000,  8000, 24000, 16000, 16000, 64, 256},
};

// constexpr BudgetPreset kBudgetPresets[] = {
//     {"Light", 8000, 2000, 4000, 2000, 4000, 16, 64},
//     {"Balanced", 16000, 4000, 12000, 8000, 8000, 24, 96},
//     {"Heavy", 48000, 6000, 24000, 16000, 12000, 24, 96},
//     {"Very Heavy", 96000, 8000, 32000, 16000, 16000, 32, 128},
//     {"Ultra", 96000, 8000, 32000, 16000, 16000, 64, 256},
// };

} // namespace

SessionEditor::SessionEditor(
    lnb::api::interface& api, std::uint32_t session_id, QWidget* parent
)
    : QDialog(parent),
      api_(api),
      session_id_(session_id) {
  setObjectName(QStringLiteral("SessionEditor"));
  setWindowTitle(session_id == 0 ? trs("New Session") : trs("Edit Session"));
  setModal(true);
  resize(620, 700);
  setMinimumSize(520, 480);
  setFont(ui_font());

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto* background = new BackgroundWidget(this);
  root->addWidget(background);

  auto* outer = new QVBoxLayout(background);
  outer->setContentsMargins(24, 20, 24, 16);
  outer->setSpacing(0);

  auto* heading = new QLabel(
      session_id == 0 ? trs("New Session") : trs("Edit Session"), background
  );
  heading->setObjectName(QStringLiteral("FormTitle"));
  heading->setFont(scaled_font(4.0, true));
  outer->addWidget(heading);
  outer->addSpacing(16);

  auto* scroll = new QScrollArea(background);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  install_scroll_bars(scroll, ScrollBarTrack::Window);

  auto* inner = build_content();
  scroll->setWidget(inner);

  outer->addWidget(scroll, 1);
  outer->addSpacing(12);
  outer->addWidget(h_separator(background));
  outer->addSpacing(12);

  auto* footer = new QHBoxLayout();
  footer->setSpacing(8);
  footer->addStretch(1);

  cancel_button_ = new QPushButton(trs("Cancel"), background);
  cancel_button_->setObjectName(QStringLiteral("Secondary"));

  save_button_ = new QPushButton(
      session_id == 0 ? trs("Create") : trs("Save"), background
  );
  save_button_->setObjectName(QStringLiteral("Primary"));
  save_button_->setDefault(true);

  footer->addWidget(cancel_button_);
  footer->addWidget(save_button_);
  outer->addLayout(footer);

  setStyleSheet(dialog_stylesheet(QStringLiteral("SessionEditor")));

  connect(cancel_button_, &QPushButton::clicked, this, [this]() { reject(); });
  connect(save_button_, &QPushButton::clicked, this, [this]() { save_session(); });
  connect(summarize_button_, &QPushButton::clicked, this, [this]() { summarize_now(); });

  load_session();
}

QWidget* SessionEditor::build_content() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 12, 0);
  layout->setSpacing(12);

  // ---- Session ----
  layout->addWidget(section_label(content, trs("Session")));
  {
    auto* form = form_layout(content);

    title_ = new QLineEdit(content);
    title_->setPlaceholderText(trs("Session title"));

    shared_directories_ = new PlainTextEdit(content);
    shared_directories_->setTabChangesFocus(true);
    shared_directories_->setMinimumHeight(64);
    shared_directories_->setMaximumHeight(96);
    shared_directories_->setPlaceholderText(
        trs("Directories the assistant may access, one per line (optional)")
    );

    // The reasoning level this session asks for. It is stored per session, so
    // switching models (which changes the available levels) can be caught here.
    reasoning_ = new ComboBox(content);
    reasoning_->setObjectName(QStringLiteral("SessionReasoning"));
    reasoning_->setToolTip(
        trs("The reasoning level this session requests. The summariser uses the "
            "same level.")
    );

    form->addRow(field_label(content, trs("Title")), title_);
    form->addRow(
        field_label(content, trs("Shared directories")), shared_directories_
    );
    form->addRow(field_label(content, trs("Reasoning")), reasoning_);
    layout->addLayout(form);
  }

  layout->addWidget(h_separator(content));

  // ---- Tool permissions ----
  layout->addWidget(section_label(content, trs("Tool permissions")));
  {
    auto* form = form_layout(content);

    tools_enabled_ = new QCheckBox(trs("Enable for this session"), content);
    tools_enabled_->setToolTip(
        trs(
            "When off, no tools are offered to the model for this session. The "
            "switches below keep their values."
        )
    );

    read_tools_ = new QCheckBox(trs("Read files"), content);
    read_tools_->setToolTip(
        trs("Allow listing, searching and reading files in the shared directories.")
    );

    write_tools_ = new QCheckBox(trs("Write files"), content);
    write_tools_->setToolTip(
        trs(
            "Allow creating, overwriting, editing and deleting files in the "
            "shared directories."
        )
    );

    online_tools_ = new QCheckBox(trs("Search online"), content);
    online_tools_->setToolTip(
        trs("Allow web searches and fetching web pages.")
    );

    form->addRow(field_label(content, trs("Tools")), tools_enabled_);
    form->addRow(field_label(content, trs("Read")), read_tools_);
    form->addRow(field_label(content, trs("Write")), write_tools_);
    form->addRow(field_label(content, trs("Online")), online_tools_);

    connect(tools_enabled_, &QCheckBox::toggled, this, [this](bool enabled) {
      read_tools_->setEnabled(enabled);
      write_tools_->setEnabled(enabled);
      online_tools_->setEnabled(enabled);
    });

    layout->addLayout(form);
  }

  layout->addWidget(h_separator(content));

  // ---- Custom instructions ----
  layout->addWidget(section_label(content, trs("Custom instructions")));
  instructions_ = new QPlainTextEdit(content);
  instructions_->setTabChangesFocus(true);
  instructions_->setMinimumHeight(96);
  instructions_->setPlaceholderText(
      trs("Kept separate from the generated memory; never overwritten by it.")
  );
  layout->addWidget(instructions_);

  layout->addWidget(h_separator(content));

  // ---- Memory ----
  auto* memory_header = new QHBoxLayout();
  memory_header->setSpacing(8);
  memory_header->addWidget(section_label(content, trs("Conversation memory")));
  memory_header->addStretch(1);

  summarize_button_ = new QPushButton(trs("Summarize now"), content);
  summarize_button_->setObjectName(QStringLiteral("Secondary"));
  summarize_button_->setToolTip(
      trs(
          "Fold the conversation into the memory below, ignoring the automatic "
          "trigger. Idempotent: pressing it again with no new messages changes "
          "nothing."
      )
  );
  memory_header->addWidget(summarize_button_);
  layout->addLayout(memory_header);

  memory_ = new PlainTextEdit(content);
  memory_->setTabChangesFocus(true);
  memory_->setMinimumHeight(140);
  memory_->setPlaceholderText(
      trs(
          "Automatically maintained summary of older turns. Edit to correct it, "
          "or clear it to rebuild it from the whole transcript."
      )
  );
  layout->addWidget(memory_);

  // The watermark sits under the summary: the highest message id already folded.
  {
    auto* form = form_layout(content);

    watermark_ = new SpinBox(content);
    watermark_->setRange(0, 2'000'000'000);
    watermark_->setToolTip(
        trs(
            "Session fold frontier: the highest message id already folded into the "
            "memory above. Used only to decide when the automatic trigger fires."
        )
    );

    assistant_watermark_ = new SpinBox(content);
    assistant_watermark_->setRange(0, 2'000'000'000);
    assistant_watermark_->setToolTip(
        trs(
            "Assistant fold frontier for this session: the highest message id already "
            "folded into the shared assistant memory."
        )
    );

    form->addRow(field_label(content, trs("Memory watermark")), watermark_);
    form->addRow(
        field_label(content, trs("Assistant watermark")), assistant_watermark_
    );
    layout->addLayout(form);
  }

  // A permanent reference so the fields above can be used with confidence.
  auto* docs = new QLabel(content);
  docs->setObjectName(QStringLiteral("Muted"));
  docs->setWordWrap(true);
  docs->setTextFormat(Qt::RichText);
  QSizePolicy docs_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
  docs_policy.setHeightForWidth(true);
  docs->setSizePolicy(docs_policy);
  docs->setText(trs(
      "<b>How summarisation works</b><br>"
      "Conversation memory is a rolling summary of older turns. A request carries the "
      "system prompt plus the transcript; when that reaches <i>Summary trigger</i>, the "
      "older messages are folded into the memory, collapsing the transcript to the "
      "newest <i>Retained tokens</i>. Keep the retained level well below the trigger, or "
      "the collapse would run again on the next turn. The message that crosses the "
      "boundary is kept whole.<br><br>"
      "The watermarks above are <b>bookkeeping only</b>: they count how much is new so "
      "the trigger can fire. They never decide what is sent or what is read — the "
      "summariser always reads by recency, so pressing <b>Summarize now</b> twice with "
      "no new messages reads the same set and produces the same memory "
      "(idempotent). Custom instructions are kept separate and are never touched.<br><br>"
      "The <b>assistant memory</b> is shared across every session: it is folded from "
      "the recent messages of all sessions (bounded by the assistant's own trigger) "
      "plus each session's summary (bounded by the session-pool budget). Older "
      "sessions may therefore be forgotten.<br><br>"
      "Clearing the memory and saving resets its watermark to 0, re-admitting the "
      "whole transcript."
  ));
  layout->addWidget(docs);

  layout->addWidget(h_separator(content));

  // ---- Budgets ----
  layout->addWidget(section_label(content, trs("Budgets")));

  {
    auto* preset_row = new QHBoxLayout();
    preset_row->setSpacing(8);

    preset_ = new ComboBox(content);
    preset_->setObjectName(QStringLiteral("SessionPreset"));
    preset_->addItem(trs("Apply a preset..."));
    for (const BudgetPreset& budget_preset : kBudgetPresets) {
      preset_->addItem(trs(budget_preset.name));
    }
    preset_->setToolTip(
        trs("Fill the budgets below with a preset. Nothing is saved until you "
             "press Save.")
    );

    preset_row->addWidget(field_label(content, trs("Preset")));
    preset_row->addWidget(preset_, 1);
    layout->addLayout(preset_row);

    connect(preset_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
      if (index <= 0) {
        return;
      }

      const BudgetPreset& budget_preset = kBudgetPresets[index - 1];
      summary_trigger_->setValue(budget_preset.trigger);
      summary_keep_->setValue(budget_preset.keep);
      summary_budget_->setValue(budget_preset.memory_budget);
      summary_headroom_->setValue(budget_preset.headroom);
      response_budget_->setValue(budget_preset.response);
      tool_output_cap_->setValue(budget_preset.tool_output);
      max_tool_rounds_->setValue(budget_preset.rounds);
      max_tool_calls_->setValue(budget_preset.calls);
    });
  }

  auto* grid = new QGridLayout();
  grid->setHorizontalSpacing(16);
  grid->setVerticalSpacing(10);

  int row = 0;
  int column = 0;

  const auto add_spin = [&](const QString& label, int value, int minimum, int maximum, int step,
                            QSpinBox** out) {
    auto* spin = new SpinBox(content);
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setGroupSeparatorShown(true);
    spin->setValue(value);
    // Keep the field able to shrink, so the two-column grid never forces the
    // dialog wider than its viewport (which would clip the right-hand column).
    spin->setMinimumWidth(76);
    spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    grid->addWidget(field_label(content, label), row, column * 2);
    grid->addWidget(spin, row, column * 2 + 1);
    *out = spin;
    ++column;
    if (column == 2) {
      column = 0;
      ++row;
    }
  };

  const lnb::Session defaults;

  add_spin(
      trs("Summary trigger"), static_cast<int>(defaults.memory_trigger_tokens_), 256,
      2'000'000'000, 256, &summary_trigger_
  );
  add_spin(
      trs("Retained tokens"), static_cast<int>(defaults.summary_keep_tokens_), 128,
      2'000'000'000, 128, &summary_keep_
  );
  add_spin(
      trs("Memory budget"), static_cast<int>(defaults.summary_token_budget_), 128,
      2'000'000'000, 256, &summary_budget_
  );
  add_spin(
      trs("Reasoning headroom"),
      static_cast<int>(defaults.summary_reasoning_headroom_tokens_), 0, 2'000'000'000, 256,
      &summary_headroom_
  );
  add_spin(
      trs("Response budget"), static_cast<int>(defaults.response_token_budget_), 128,
      2'000'000'000, 256, &response_budget_
  );
  add_spin(
      trs("Tool output cap"), static_cast<int>(defaults.tool_output_max_tokens_), 128,
      2'000'000'000, 256, &tool_output_cap_
  );
  add_spin(
      trs("Max tool rounds"), static_cast<int>(defaults.max_tool_rounds_), 1, 1000, 1,
      &max_tool_rounds_
  );
  add_spin(
      trs("Max tool calls"), static_cast<int>(defaults.max_tool_calls_), 1, 10000, 1,
      &max_tool_calls_
  );

  layout->addLayout(grid);
  layout->addStretch(1);

  return content;
}

void SessionEditor::load_session() {
  if (session_id_ != 0) {
    const auto stored = api_.session(session_id_);

    if (stored.has_value()) {
      session_ = *stored;
    }
  }

  title_->setText(to_qstring(session_.title_));
  instructions_->setPlainText(to_qstring(session_.custom_instructions_));
  memory_->setPlainText(to_qstring(session_.memory_));

  // A brand-new session starts on the assistant's level (which itself defaults to
  // the model's first available one); an existing row keeps the level it stores.
  // Either way an empty value resolves to the model's first available level.
  const std::string preferred = session_id_ == 0
      ? api_.assistant().reasoning_effort_
      : session_.reasoning_effort_;
  populate_reasoning(preferred);

  QStringList directories;

  for (const std::string& directory: session_.shared_directories_) {
    directories << to_qstring(directory);
  }

  shared_directories_->setPlainText(directories.join(u'\n'));

  tools_enabled_->setChecked(session_.tools_enabled_);
  read_tools_->setChecked(session_.read_tools_enabled_);
  write_tools_->setChecked(session_.write_tools_enabled_);
  online_tools_->setChecked(session_.online_tools_enabled_);
  read_tools_->setEnabled(session_.tools_enabled_);
  write_tools_->setEnabled(session_.tools_enabled_);
  online_tools_->setEnabled(session_.tools_enabled_);

  watermark_->setValue(static_cast<int>(session_.memory_watermark_));
  assistant_watermark_->setValue(static_cast<int>(session_.assistant_memory_watermark_));

  summary_keep_->setValue(static_cast<int>(session_.summary_keep_tokens_));
  summary_trigger_->setValue(static_cast<int>(session_.memory_trigger_tokens_));
  summary_budget_->setValue(static_cast<int>(session_.summary_token_budget_));
  summary_headroom_->setValue(static_cast<int>(session_.summary_reasoning_headroom_tokens_));
  response_budget_->setValue(static_cast<int>(session_.response_token_budget_));
  tool_output_cap_->setValue(static_cast<int>(session_.tool_output_max_tokens_));
  max_tool_rounds_->setValue(static_cast<int>(session_.max_tool_rounds_));
  max_tool_calls_->setValue(static_cast<int>(session_.max_tool_calls_));

  // Only a saved session has stored messages to fold.
  summarize_button_->setEnabled(session_id_ != 0);
}

void SessionEditor::populate_reasoning(const std::string& preferred) {
  if (reasoning_ == nullptr) {
    return;
  }

  const std::string model = api_.assistant().model_;
  const std::string wanted = preferred;
  const std::string target =
      wanted.empty() ? lnb::api::reasoning_default_for(model) : wanted;

  const std::vector<std::string> efforts =
      lnb::api::reasoning_levels_for(model, target);

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

void SessionEditor::save_session() {
  // Validate before anything is written. The two budgets are related: the reply
  // must fit inside the input bound, and the retained level must sit below the
  // trigger, or the collapse would fire again the moment it finished. A violated
  // rule keeps the window open so the values can be corrected.
  const std::size_t trigger = static_cast<std::size_t>(summary_trigger_->value());
  const std::size_t keep = static_cast<std::size_t>(summary_keep_->value());
  const std::size_t response = static_cast<std::size_t>(response_budget_->value());

  if (response > trigger) {
    QMessageBox::warning(
        this, trs("Invalid Budget"),
        trs(
            "The response budget must not be greater than the summarization "
            "trigger. A reply could otherwise never complete before the "
            "conversation is collapsed again."
        )
    );
    return;
  }

  if (keep >= trigger) {
    QMessageBox::warning(
        this, trs("Invalid Budget"),
        trs(
            "The retained-token level must be smaller than the summarization "
            "trigger. Otherwise the collapse would be triggered again "
            "immediately after it runs."
        )
    );
    return;
  }

  lnb::Session session = session_;

  session.title_ = title_->text().trimmed().toStdString();
  session.custom_instructions_ = instructions_->toPlainText().toStdString();
  session.memory_ = memory_->toPlainText().toStdString();

  session.shared_directories_.clear();

  const QStringList directories = shared_directories_->toPlainText().split(
      u'\n', Qt::SkipEmptyParts
  );

  for (const QString& entry: directories) {
    const QString directory = entry.trimmed();

    if (!directory.isEmpty()) {
      session.shared_directories_.push_back(directory.toStdString());
    }
  }

  // Clearing the memory re-admits the whole transcript.
  session.memory_watermark_ = session.memory_.empty()
      ? 0
      : static_cast<std::uint32_t>(watermark_->value());
  session.assistant_memory_watermark_ =
      static_cast<std::uint32_t>(assistant_watermark_->value());

  session.summary_keep_tokens_ = static_cast<std::size_t>(summary_keep_->value());
  session.memory_trigger_tokens_ = static_cast<std::size_t>(summary_trigger_->value());
  session.summary_token_budget_ = static_cast<std::size_t>(summary_budget_->value());
  session.summary_reasoning_headroom_tokens_ =
      static_cast<std::size_t>(summary_headroom_->value());
  session.response_token_budget_ = static_cast<std::size_t>(response_budget_->value());
  session.tool_output_max_tokens_ = static_cast<std::size_t>(tool_output_cap_->value());
  session.max_tool_rounds_ = static_cast<std::size_t>(max_tool_rounds_->value());
  session.max_tool_calls_ = static_cast<std::size_t>(max_tool_calls_->value());
  session.reasoning_effort_ = reasoning_->currentText().toStdString();

  session.tools_enabled_ = tools_enabled_->isChecked();
  session.read_tools_enabled_ = read_tools_->isChecked();
  session.write_tools_enabled_ = write_tools_->isChecked();
  session.online_tools_enabled_ = online_tools_->isChecked();

  if (session_id_ == 0) {
    session_id_ = api_.create_session(session);
  } else {
    session.id_ = session_id_;
    api_.save_session(session);
  }

  session_ = session;
  accept();
}

void SessionEditor::summarize_now() {
  if (session_id_ == 0 || summarizing_ || conversation_view_ == nullptr) {
    return;
  }

  // A manual fold runs as the session's own task, so it must never be started
  // while a turn is already in flight: the one-task-per-session model is what
  // guarantees a generation and a summarisation cannot touch the same session's
  // memory, watermark and messages at once.
  if (conversation_view_->busy() || api_.busy()) {
    QMessageBox::information(
        this, trs("Session Busy"),
        trs(
            "A task is already running. Wait for it to finish, then summarize."
        )
    );
    return;
  }

  summarizing_ = true;
  summarize_button_->setEnabled(false);
  summarize_button_->setText(trs("Summarizing..."));
  save_button_->setEnabled(false);
  cancel_button_->setEnabled(false);

  // The review runs on the worker thread and blocks on the interface thread, so a
  // decline is recorded here and read after the task finishes.
  bool declined = false;
  const lnb::SummaryApprover approver =
      [&declined](const lnb::SummaryPlan& plan) {
        const bool approved = review_summary(plan);
        if (!approved) {
          declined = true;
        }
        return approved;
      };

  // The fold is driven through the view, so it runs as the session's single task
  // and appears in the tasks window like any other turn.
  conversation_view_->summarize_session_now(approver);

  // Keep the modal dialog responsive while the worker folds the transcript: the
  // nested loop only pumps events until the task reaches a terminal state. The
  // window's poll timer keeps firing inside it, so the tasks window updates live.
  QEventLoop loop;
  QTimer timer;

  QObject::connect(&timer, &QTimer::timeout, &loop, [this, &loop]() {
    if (!conversation_view_->busy()) {
      loop.quit();
    }
  });

  timer.start(100);
  loop.exec();
  timer.stop();

  summarizing_ = false;
  summarize_button_->setEnabled(true);
  summarize_button_->setText(trs("Summarize now"));
  save_button_->setEnabled(true);
  cancel_button_->setEnabled(true);

  // A declined review is a deliberate no-op: say nothing about it.
  if (declined) {
    return;
  }

  const lnb::GenerationTask::Snapshot snapshot =
      conversation_view_->last_snapshot();

  // A cancelled pass folded nothing and must not be reported as "nothing to
  // summarise": cancelling is a deliberate no-op, exactly like a declined
  // review, and the memory is simply left untouched.
  if (snapshot.status == lnb::GenerationTask::Status::CANCELLED) {
    return;
  }

  if (snapshot.status == lnb::GenerationTask::Status::FAILED) {
    QMessageBox::warning(
        this, trs("Summarization Failed"), to_qstring(snapshot.error)
    );
    return;
  }

  // The summariser persists the new memory; reload it so the box shows the result
  // and a later Save keeps the advanced watermark.
  const auto updated = api_.session(session_id_);

  if (!updated.has_value()) {
    return;
  }

  const QString previous = memory_->toPlainText();
  const QString memory = to_qstring(updated->memory_);

  memory_->setPlainText(memory);
  session_.memory_ = updated->memory_;
  session_.memory_watermark_ = updated->memory_watermark_;
  watermark_->setValue(static_cast<int>(updated->memory_watermark_));

  if (memory.isEmpty() || memory == previous) {
    QMessageBox::information(
        this, trs("Nothing to Summarize"),
        trs("There is no new conversation to fold into memory yet.")
    );
  }
}

} // namespace ui
