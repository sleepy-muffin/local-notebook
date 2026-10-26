#include "ui/SessionEditor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QEventLoop>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
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

// A CSS colour with the colour's own alpha, so a themed row highlight can be
// layered over the dialog's background.
QString rgba(const QColor& color) {
  return QStringLiteral("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alphaF());
}

// Presets for the session budgets (design-1.md §8). Selecting one only fills the
// fields below; it never saves, so the values can still be tweaked before Save.
// The values are starting points, not limits.
struct MemoryPreset {
  const char* name;
  int trigger;
  int keep;
  int memory_budget;
};
struct BudgetPreset {
  const char* name;
  int response;
};
struct MidwayPreset {
  const char* name;
  // Cumulative budget for the tool output fed back to the model in one turn.
  int midway_input;
};

/**
 * summary_trigger_ summary_keep_ summary_budget_
 *
 * response_budget_ midturn_input_budget_
 */
constexpr MemoryPreset kMemoryPresets[] = {
    // memory:
    // trigger (the input bound), keep (the level it merges down to), memory;
    // response; TODO, response is extremely tight for a reasoning model with thinking mode on
    //           A usable value for them is usually above 16000, much larger than a non-reasoning model.
    // tool: cumulative input produced midway (tool results fed back)
    {"Instant",      1000,   250,   250},
    {"Very Light",   2000,   500,  1000},
    {"Light",        4000,  1000,  1200},
    {"Balanced",    12000,  2000,  4000},
    {"Heavy",       24000,  4000,  6000},
    {"Very Heavy",  48000,  8000,  8000},
    {"Ultra",       64000, 16000, 16000},
};
constexpr BudgetPreset kBudgetPresets[] = {
    // memory:
    // trigger (the input bound), keep (the level it merges down to), memory;
    // response; TODO, response is extremely tight for a reasoning model with thinking mode on
    //           A usable value for them is usually above 16000, much larger than a non-reasoning model.
    // tool: cumulative input produced midway (tool results fed back)
    {"Instant",                    500},
    {"Very Light",                1000},
    {"Light",                     2000},
    {"Balanced",                  4000},
    {"Heavy",                     8000},
    {"Light (Reasoning)",        16000},
    {"Balanced (Reasoning)",     24000},
    {"Heavy (Reasoning)",        32000},
    {"Very Heavy (Reasoning)",   64000},
};
constexpr MidwayPreset kMidwayPresets[] = {
    // memory:
    // trigger (the input bound), keep (the level it merges down to), memory;
    // response; TODO, response is extremely tight for a reasoning model with thinking mode on
    //           A usable value for them is usually above 16000, much larger than a non-reasoning model.
    // tool: cumulative input produced midway (tool results fed back)
    {"Very Light",  20000},
    {"Light",       50000},
    {"Balanced",    80000},
    {"Heavy",      120000},
    {"Very Heavy", 170000},
};

// Two fields per row, so a section reads as a small group of related numbers
// instead of one long budget wall.
QGridLayout* make_budget_grid() {
  auto* grid = new QGridLayout();
  grid->setHorizontalSpacing(16);
  grid->setVerticalSpacing(10);
  return grid;
}

QSpinBox* add_budget_spin(
    QWidget* parent, QGridLayout* grid, int row, int column,
    const QString& label, int value, int minimum, int maximum, int step
) {
  auto* spin = new SpinBox(parent);
  spin->setRange(minimum, maximum);
  spin->setSingleStep(step);
  spin->setGroupSeparatorShown(true);
  spin->setValue(value);
  // Keep the field able to shrink, so the two-column grid never forces the page
  // wider than its viewport (which would clip the right-hand column).
  spin->setMinimumWidth(76);
  spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  grid->addWidget(field_label(parent, label), row, column * 2);
  grid->addWidget(spin, row, column * 2 + 1);
  return spin;
}

// A "Preset" row above a budget grid. The combo only fills fields; it is wired
// to them by the caller, which knows which page they belong to.
QComboBox* add_preset_row(
    QWidget* parent, QVBoxLayout* layout, const QString& object_name,
    const QString& tooltip, const auto& presets
) {
  auto* row = new QHBoxLayout();
  row->setSpacing(8);

  auto* combo = new ComboBox(parent);
  combo->setObjectName(object_name);
  combo->addItem(trs("Apply a preset..."));
  for (const auto& preset : presets) {
    combo->addItem(trs(preset.name));
  }
  combo->setToolTip(tooltip);

  row->addWidget(field_label(parent, trs("Preset")));
  row->addWidget(combo, 1);
  layout->addLayout(row);

  return combo;
}

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
  // The left rail costs about 190 px, so the window is wider than the old
  // tabbed one to keep the two-column budget grids from clipping.
  resize(800, 700);
  setMinimumSize(680, 480);
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

  outer->addWidget(build_body(), 1);
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

  setStyleSheet(session_stylesheet());

  connect(cancel_button_, &QPushButton::clicked, this, [this]() { reject(); });
  connect(save_button_, &QPushButton::clicked, this, [this]() { save_session(); });
  connect(summarize_button_, &QPushButton::clicked, this, [this]() { summarize_now(); });

  load_session();
}

QWidget* SessionEditor::build_body() {
  auto* body = new QWidget();
  auto* layout = new QHBoxLayout(body);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  nav_ = new QListWidget(body);
  nav_->setObjectName(QStringLiteral("SessionNav"));
  // The rail shows the dialog behind it; the rule lives on the list so a style
  // that paints from the palette cannot fill the scrollbar column black.
  show_parent_background(nav_);
  install_scroll_bars(nav_, ScrollBarTrack::Window);
  nav_->setFrameShape(QFrame::NoFrame);
  nav_->setSelectionMode(QAbstractItemView::SingleSelection);
  nav_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  nav_->setFocusPolicy(Qt::NoFocus);
  nav_->addItem(trs("General"));
  nav_->addItem(trs("Memory"));
  nav_->addItem(trs("Tools"));

  auto* nav_wrap = new QWidget(body);
  auto* nav_layout = new QVBoxLayout(nav_wrap);
  nav_layout->setContentsMargins(0, 0, 8, 0);
  nav_layout->setSpacing(0);
  nav_layout->addWidget(nav_);
  nav_wrap->setFixedWidth(188);

  stack_ = new QStackedWidget(body);
  stack_->setObjectName(QStringLiteral("SessionStack"));
  stack_->setFrameShape(QFrame::NoFrame);

  stack_->addWidget(scrolled_tab(build_general_tab()));
  stack_->addWidget(scrolled_tab(build_memory_tab()));
  stack_->addWidget(scrolled_tab(build_tools_tab()));

  layout->addWidget(nav_wrap);
  layout->addWidget(v_separator(body));
  layout->addWidget(stack_, 1);

  connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row >= 0) {
      stack_->setCurrentIndex(row);
    }
  });
  nav_->setCurrentRow(0);

  return body;
}

QString SessionEditor::session_stylesheet() const {
  QString sheet = dialog_stylesheet(QStringLiteral("SessionEditor"));

  // The rail carries the same row states as the settings window's, so both
  // windows read as one navigation idiom.
  sheet += QStringLiteral(
               "QListWidget#SessionNav::item { color: %1; padding: 7px 10px;"
               " border-radius: 6px; margin: 1px 0; }")
               .arg(theme().colors.text_muted.name());
  sheet += QStringLiteral("QListWidget#SessionNav::item:hover { background: %1; }")
               .arg(rgba(row_hover_background()));
  sheet += QStringLiteral(
               "QListWidget#SessionNav::item:selected { background: %1; color: %2; }")
               .arg(rgba(row_selected_background()), theme().colors.text.name());
  sheet += QStringLiteral("QStackedWidget#SessionStack { background: transparent; }");

  return sheet;
}

QWidget* SessionEditor::scrolled_tab(QWidget* content) {
  auto* page = new QWidget();
  auto* layout = new QVBoxLayout(page);
  // The left margin matches the settings window's page padding (22 px), so the
  // pages sit away from the divider instead of touching it.
  layout->setContentsMargins(22, 0, 0, 0);
  layout->setSpacing(0);

  // Each page owns its scroll area: a single scroll area around the stack would
  // carry the navigation rail away with the content.
  auto* scroll = new QScrollArea(page);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  install_scroll_bars(scroll, ScrollBarTrack::Window);
  scroll->setWidget(content);

  layout->addWidget(scroll);
  return page;
}

QWidget* SessionEditor::build_general_tab() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  // The right margin leaves room for the page's own scrollbar.
  layout->setContentsMargins(0, 10, 12, 10);
  layout->setSpacing(12);

  // ---- Session ----
  layout->addWidget(section_label(content, trs("Session")));
  {
    auto* form = form_layout(content);

    title_ = new QLineEdit(content);
    title_->setPlaceholderText(trs("Session title"));

    // The reasoning level this session asks for. It is stored per session, so
    // switching models (which changes the available levels) can be caught here.
    reasoning_ = new ComboBox(content);
    reasoning_->setObjectName(QStringLiteral("SessionReasoning"));
    reasoning_->setToolTip(
        trs("The reasoning level this session requests for chat turns.")
    );

    form->addRow(field_label(content, trs("Title")), title_);
    form->addRow(field_label(content, trs("Reasoning")), reasoning_);
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

  // ---- Budgets ----
  layout->addWidget(section_label(content, trs("Budgets")));
  {
    const lnb::Session defaults;
    auto* grid = make_budget_grid();

    response_budget_ = add_budget_spin(
        content, grid, 0, 0, trs("Response budget"),
        static_cast<int>(defaults.response_token_budget_), 128, 2'000'000'000, 256
    );

    budget_preset_ = add_preset_row(
        content, layout, QStringLiteral("SessionToolsPreset"),
        trs("Fill the tool limits below with a preset. Nothing is saved until you "
            "press Save."), kBudgetPresets
    );

    connect(
        budget_preset_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int index) {
          if (index <= 0) {
            return;
          }

          response_budget_->setValue(kBudgetPresets[index - 1].response);
        }
    );

    // TODO
    // Add a row of description under `response_budget_`
    // It should tell the user that it involves the output caused by thinking

    layout->addLayout(grid);
  }

  layout->addStretch(1);

  return content;
}

QWidget* SessionEditor::build_memory_tab() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 10, 12, 10);
  layout->setSpacing(12);

  // ---- Conversation memory ----
  auto* memory_header = new QHBoxLayout();
  memory_header->setSpacing(8);
  memory_header->addWidget(section_label(content, trs("Conversation memory")));
  memory_header->addStretch(1);

  summarize_button_ = new QPushButton(trs("Update memory now"), content);
  summarize_button_->setObjectName(QStringLiteral("Secondary"));
  summarize_button_->setToolTip(
      trs(
          "Merge the conversation into the memory below, ignoring the automatic "
          "memory trigger. Idempotent: pressing it again with no new messages "
          "changes nothing."
      )
  );
  memory_header->addWidget(summarize_button_);
  layout->addLayout(memory_header);

  // The assistant memory is shared by every session; this one can opt out
  // without losing its own conversation memory.
  include_in_assistant_memory_ = new QCheckBox(
      trs("Include in assistant memory"), content
  );
  include_in_assistant_memory_->setToolTip(
      trs(
          "When off, this session's messages and memory are left out of the "
          "shared assistant memory. This session's own conversation memory is "
          "unaffected."
      )
  );
  layout->addWidget(include_in_assistant_memory_);

  memory_ = new PlainTextEdit(content);
  memory_->setTabChangesFocus(true);
  memory_->setMinimumHeight(140);
  memory_->setPlaceholderText(
      trs(
          "Automatically maintained record of older turns. Edit to correct it, "
          "or clear it to rebuild it from the whole transcript."
      )
  );
  layout->addWidget(memory_);

  // The watermark sits under the memory: the highest message id already merged.
  {
    auto* form = form_layout(content);

    watermark_ = new SpinBox(content);
    watermark_->setRange(0, 2'000'000'000);
    watermark_->setToolTip(
        trs(
            "Conversation memory position: the highest message id already merged "
            "into the memory above. Used only to decide when the automatic memory "
            "update fires."
        )
    );

    assistant_watermark_ = new SpinBox(content);
    assistant_watermark_->setRange(0, 2'000'000'000);
    assistant_watermark_->setToolTip(
        trs(
            "Assistant memory position for this session: the highest message id "
            "already merged into the shared assistant memory."
        )
    );

    form->addRow(field_label(content, trs("Memory watermark")), watermark_);
    form->addRow(
        field_label(content, trs("Assistant watermark")), assistant_watermark_
    );
    layout->addLayout(form);
  }

  layout->addWidget(h_separator(content));

  // ---- Memory triggers ----
  layout->addWidget(section_label(content, trs("Memory triggers")));

  memory_preset_ = add_preset_row(
      content, layout, QStringLiteral("SessionMemoryPreset"),
      trs("Fill the memory fields below with a preset. It also sets the response "
          "budget on the General page, which must not exceed the memory trigger. "
          "Nothing is saved until you press Save."), kMemoryPresets
  );

  connect(
      memory_preset_, qOverload<int>(&QComboBox::currentIndexChanged), this,
      [this](int index) {
        if (index <= 0) {
          return;
        }

        const MemoryPreset& memory_preset = kMemoryPresets[index - 1];
        summary_trigger_->setValue(memory_preset.trigger);
        summary_keep_->setValue(memory_preset.keep);
        summary_budget_->setValue(memory_preset.memory_budget);
        // response_budget_->setValue(budget_preset.response);
      }
  );

  {
    auto* form = form_layout(content);

    // The summariser's own level, independent of the chat level on the General
    // page. A memory update can therefore run without reasoning while turns
    // reason (or the other way round).
    summary_reasoning_ = new ComboBox(content);
    summary_reasoning_->setObjectName(QStringLiteral("SessionMemoryReasoning"));
    summary_reasoning_->setToolTip(
        trs("The reasoning level used when this session's memory is updated. "
            "Chat turns are unaffected.")
    );

    form->addRow(
        field_label(content, trs("Memory reasoning")), summary_reasoning_
    );
    layout->addLayout(form);
  }

  {
    const lnb::Session defaults;
    auto* grid = make_budget_grid();

    // A memory setting decides *when* the transcript is merged and how much
    // stays verbatim.
    summary_trigger_ = add_budget_spin(
        content, grid, 0, 0, trs("Memory trigger"),
        static_cast<int>(defaults.memory_trigger_tokens_), 256, 2'000'000'000, 256
    );
    summary_keep_ = add_budget_spin(
        content, grid, 0, 1, trs("Retained tokens"),
        static_cast<int>(defaults.summary_keep_tokens_), 128, 2'000'000'000, 128
    );
    summary_budget_ = add_budget_spin(
        content, grid, 1, 0, trs("Memory budget"),
        static_cast<int>(defaults.summary_token_budget_), 128, 2'000'000'000, 256
    );

    layout->addLayout(grid);
  }

  layout->addWidget(h_separator(content));

  // A permanent reference, kept at the bottom of the page so the fields above can
  // be used with confidence.
  auto* docs = new QLabel(content);
  docs->setObjectName(QStringLiteral("Muted"));
  docs->setWordWrap(true);
  docs->setTextFormat(Qt::RichText);
  QSizePolicy docs_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
  docs_policy.setHeightForWidth(true);
  docs->setSizePolicy(docs_policy);
  docs->setText(trs(
      "<b>How conversation memory works</b><br>"
      "Conversation memory is a rolling record of older turns. A request carries the "
      "system prompt plus the transcript; when that reaches <i>Memory trigger</i>, the "
      "older messages are merged into the memory, trimming the transcript to the "
      "newest <i>Retained tokens</i>. Keep the retained level well below the trigger, "
      "or the merge would run again on the next turn. The message that crosses the "
      "boundary is kept whole.<br><br>"
      "The watermarks above are <b>bookkeeping only</b>: they count how much is new so "
      "the trigger can fire. They never decide what is sent or what is read — a memory "
      "update always reads by recency, so pressing <b>Update memory now</b> twice with "
      "no new messages reads the same set and produces the same memory "
      "(idempotent). Custom instructions are kept separate and are never touched.<br><br>"
      "The <b>assistant memory</b> is shared across every session: it is merged from "
      "the recent messages of all sessions (bounded by the assistant's own trigger) "
      "plus each session's memory (bounded by the session-pool budget). Older "
      "sessions may therefore be forgotten. This session opts out of that sharing "
      "when <i>Include in assistant memory</i> is off.<br><br>"
      "Clearing the memory and saving resets its watermark to 0, re-admitting the "
      "whole transcript."
  ));
  layout->addWidget(docs);

  layout->addStretch(1);

  return content;
}

QWidget* SessionEditor::build_tools_tab() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 10, 12, 10);
  layout->setSpacing(12);

  // ---- Shared directories ----
//   {
//     auto* form = form_layout(content);
//
//     layout->addLayout(form);
//   }

  // layout->addWidget(h_separator(content));

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


    shared_directories_ = new PlainTextEdit(content);
    shared_directories_->setTabChangesFocus(true);
    shared_directories_->setMinimumHeight(64);
    shared_directories_->setMaximumHeight(96);
    shared_directories_->setPlaceholderText(
        trs("Directories the assistant may access, one per line (optional)")
    );

    auto* description = field_label(
        content,
        trs("One directory per line. The model only sees relative paths inside "
            "these directories, and its writes stay inside them. If several "
            "directories share a name, the later ones gain a numeric suffix.")
    );

    form->addRow(field_label(content, trs("Tools")), tools_enabled_);
    form->addRow(field_label(content, trs("Read")), read_tools_);
    form->addRow(field_label(content, trs("Write")), write_tools_);
    form->addRow(field_label(content, trs("Online")), online_tools_);

    form->addRow(field_label(content, trs("Shared directories")), shared_directories_);
    form->addRow(QString(), description);

    connect(tools_enabled_, &QCheckBox::toggled, this, [this](bool enabled) {
      read_tools_->setEnabled(enabled);
      write_tools_->setEnabled(enabled);
      online_tools_->setEnabled(enabled);
    });

    layout->addLayout(form);
  }

  layout->addWidget(h_separator(content));

  // ---- Tool limits ----
  layout->addWidget(section_label(content, trs("Tool limits")));

  tools_preset_ = add_preset_row(
      content, layout, QStringLiteral("SessionToolsPreset"),
      trs("Fill the tool limits below with a preset. Nothing is saved until you "
          "press Save."), kMidwayPresets
  );

  connect(
      tools_preset_, qOverload<int>(&QComboBox::currentIndexChanged), this,
      [this](int index) {
        if (index <= 0) {
          return;
        }

        midturn_input_budget_->setValue(kMidwayPresets[index - 1].midway_input);
      }
  );

  {
    const lnb::Session defaults;
    auto* grid = make_budget_grid();

    midturn_input_budget_ = add_budget_spin(
        content, grid, 0, 0,
        trs("Input token produced midway"),
        static_cast<int>(defaults.input_token_produced_midway_), 0,
        2'000'000'000, 1000
    );
    layout->addLayout(grid);

    auto* description = field_label(
        content,
        trs(
            "Total tokens of tool output this session may feed back to the "
            "model in one turn, counted across every tool call in that turn. "
            "Once the budget is spent, the remaining answer is produced without "
            "tools. 0 disables the limit."
        )
    );
    layout->addWidget(description);
  }

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
  populate_reasoning(reasoning_, preferred);

  // The summariser has its own level (default none), independent of the chat
  // level above. A new session uses the Session default.
  populate_reasoning(summary_reasoning_, session_.summary_reasoning_effort_);

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

  include_in_assistant_memory_->setChecked(session_.include_in_assistant_memory_);

  watermark_->setValue(static_cast<int>(session_.memory_watermark_));
  assistant_watermark_->setValue(static_cast<int>(session_.assistant_memory_watermark_));

  summary_keep_->setValue(static_cast<int>(session_.summary_keep_tokens_));
  summary_trigger_->setValue(static_cast<int>(session_.memory_trigger_tokens_));
  summary_budget_->setValue(static_cast<int>(session_.summary_token_budget_));
  response_budget_->setValue(static_cast<int>(session_.response_token_budget_));
  midturn_input_budget_->setValue(
      static_cast<int>(session_.input_token_produced_midway_)
  );

  // Only a saved session has stored messages to merge.
  summarize_button_->setEnabled(session_id_ != 0);
}

void SessionEditor::populate_reasoning(
    QComboBox* combo, const std::string& preferred
) {
  if (combo == nullptr) {
    return;
  }

  const std::string model = api_.assistant().model_;
  const std::string wanted = preferred;
  const std::string target =
      wanted.empty() ? lnb::api::reasoning_default_for(model) : wanted;

  const std::vector<std::string> efforts =
      lnb::api::reasoning_levels_for(model, target);

  const QSignalBlocker blocker(combo);
  combo->clear();

  int selected = 0;
  for (std::size_t i = 0; i < efforts.size(); ++i) {
    combo->addItem(to_qstring(efforts[i]));

    if (QString::compare(to_qstring(efforts[i]), to_qstring(target), Qt::CaseInsensitive) == 0) {
      selected = static_cast<int>(i);
    }
  }

  combo->setCurrentIndex(selected);
}

void SessionEditor::save_session() {
  // Validate before anything is written. The two budgets are related: the reply
  // must fit inside the input bound, and the retained level must sit below the
  // trigger, or the memory update would fire again the moment it finished. A
  // violated rule keeps the window open so the values can be corrected.
  const std::size_t trigger = static_cast<std::size_t>(summary_trigger_->value());
  const std::size_t keep = static_cast<std::size_t>(summary_keep_->value());
  const std::size_t response = static_cast<std::size_t>(response_budget_->value());

  // if (response > trigger) {
  //   QMessageBox::warning(
  //       this, trs("Invalid Budget"),
  //       trs(
  //           "The response budget must not be greater than the memory "
  //           "trigger. A reply could otherwise never complete before the "
  //           "conversation is merged again."
  //       )
  //   );
  //   return;
  // }

  if (keep >= trigger) {
    QMessageBox::warning(
        this, trs("Invalid Budget"),
        trs(
            "The retained-token level must be smaller than the memory "
            "trigger. Otherwise the memory update would be triggered again "
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
  session.response_token_budget_ = static_cast<std::size_t>(response_budget_->value());
  session.input_token_produced_midway_ =
      static_cast<std::size_t>(midturn_input_budget_->value());
  session.reasoning_effort_ = reasoning_->currentText().toStdString();
  session.summary_reasoning_effort_ =
      summary_reasoning_->currentText().toStdString();

  session.tools_enabled_ = tools_enabled_->isChecked();
  session.read_tools_enabled_ = read_tools_->isChecked();
  session.write_tools_enabled_ = write_tools_->isChecked();
  session.online_tools_enabled_ = online_tools_->isChecked();
  session.include_in_assistant_memory_ = include_in_assistant_memory_->isChecked();

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

  // A manual memory update runs as the session's own task, so it must never be
  // started while a turn is already in flight: the one-task-per-session model is
  // what guarantees a generation and a memory update cannot touch the same
  // session's memory, watermark and messages at once.
  if (conversation_view_->busy() || api_.assistant_busy()) {
    QMessageBox::information(
        this, trs("Session Busy"),
        trs(
            "A task is already running. Wait for it to finish, then update "
            "the memory."
        )
    );
    return;
  }

  summarizing_ = true;
  summarize_button_->setEnabled(false);
  summarize_button_->setText(trs("Updating memory..."));
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

  // The update is driven through the view, so it runs as the session's single
  // task and appears in the tasks window like any other turn.
  conversation_view_->summarize_session_now(approver);

  // Keep the modal dialog responsive while the worker merges the transcript: the
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
  summarize_button_->setText(trs("Update memory now"));
  save_button_->setEnabled(true);
  cancel_button_->setEnabled(true);

  // A declined review is a deliberate no-op: say nothing about it.
  if (declined) {
    return;
  }

  const lnb::GenerationTask::Snapshot snapshot =
      conversation_view_->last_snapshot();

  // A cancelled pass merged nothing and must not be reported as "nothing to
  // update": cancelling is a deliberate no-op, exactly like a declined
  // review, and the memory is simply left untouched.
  if (snapshot.status == lnb::GenerationTask::Status::CANCELLED) {
    return;
  }

  if (snapshot.status == lnb::GenerationTask::Status::FAILED) {
    QMessageBox::warning(
        this, trs("Memory Update Failed"), to_qstring(snapshot.error)
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
        this, trs("Nothing to Update"),
        trs("There is no new conversation to merge into memory yet.")
    );
  }
}

} // namespace ui
