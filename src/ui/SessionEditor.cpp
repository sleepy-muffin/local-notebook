#include "SessionEditor.hpp"
#include "../ui.hpp"
#include "../external.hpp"

#include <QEventLoop>
#include <QFrame>
#include <QGroupBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMessageBox>

#include <exception>
#include <memory>
#include <optional>
#include <string>

namespace ui {

SessionEditor::SessionEditor(
    lnb::ApplicationState& state,
    std::optional<lnb::session> session,
    QWidget* parent
)
    : QDialog(parent),
      state_(state),
      session_(std::move(session))
{
  setWindowTitle(session_ ? "Edit Session" : "Add Session");
  resize(720, 780);

  create_layout();

  if (session_) {
    load_session();
  }
}

void SessionEditor::create_layout() {
    auto* main_layout = new QVBoxLayout(this);

    // All fields live in a scroll area so the dialog stays usable on a small
    // screen: the content keeps its comfortable size and scrolls instead of
    // squeezing the editors. The action buttons stay pinned below the scroll
    // area.
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(new QLabel("Title:", content));

    title_edit_ = new QLineEdit(content);
    layout->addWidget(title_edit_);

    auto* settings_layout = new QGridLayout();

    api_url_edit_ = new QLineEdit(content);
    token_edit_ = new QLineEdit(content);
    model_edit_ = new QLineEdit(content);
    proxy_edit_ = new QLineEdit(content);

    settings_layout->addWidget(new QLabel("API URL:", content), 0, 0);
    settings_layout->addWidget(api_url_edit_, 1, 0);

    settings_layout->addWidget(
        new QLabel("Authentication Token:", content), 0, 1
    );
    settings_layout->addWidget(token_edit_, 1, 1);

    settings_layout->addWidget(new QLabel("Model:", content), 2, 0);
    settings_layout->addWidget(model_edit_, 3, 0);

    settings_layout->addWidget(new QLabel("Proxy:", content), 2, 1);
    settings_layout->addWidget(proxy_edit_, 3, 1);

    layout->addLayout(settings_layout);

    layout->addWidget(new QLabel("Shared Directory:", content));

    shared_directory_edit_ = new QLineEdit(content);
    layout->addWidget(shared_directory_edit_);

    // ---- Generation & context ----
    // Session-bound request/summarisation/tool budgets. Each session can target
    // a different model, so these live with the session rather than in the
    // global config. Laid out two per row to stay compact.
    auto* generation = new QGroupBox("Generation & Context:", content);
    auto* generation_layout = new QGridLayout(generation);
    generation_layout->setHorizontalSpacing(16);
    generation_layout->setVerticalSpacing(6);

    const lnb::generation_settings defaults;

    const auto add_generation = [&](
        int row, int column, const QString& label, int value,
        int minimum, int maximum, int step
    ) -> QSpinBox* {
      auto* box = new QSpinBox(generation);
      box->setRange(minimum, maximum);
      box->setSingleStep(step);
      box->setValue(value);
      generation_layout->addWidget(new QLabel(label, generation), row, column * 2);
      generation_layout->addWidget(box, row, column * 2 + 1);
      return box;
    };

    model_context_spin_ = add_generation(
        0, 0, "Model context (tokens)",
        static_cast<int>(defaults.model_context_tokens_), 1024, 2000000, 1024
    );
    response_budget_spin_ = add_generation(
        0, 1, "Response budget (tokens)",
        static_cast<int>(defaults.response_token_budget_), 128, 2000000, 256
    );
    safety_margin_spin_ = add_generation(
        1, 0, "Safety margin (tokens)",
        static_cast<int>(defaults.context_safety_margin_tokens_), 0, 2000000, 256
    );
    summary_trigger_spin_ = add_generation(
        1, 1, "Summary trigger (tokens)",
        static_cast<int>(defaults.summary_trigger_tokens_), 256, 2000000, 256
    );
    summary_keep_spin_ = add_generation(
        2, 0, "Summary keep (tokens)",
        static_cast<int>(defaults.summary_keep_tokens_), 256, 2000000, 256
    );
    summary_budget_spin_ = add_generation(
        2, 1, "Summary budget (tokens)",
        static_cast<int>(defaults.summary_token_budget_), 128, 2000000, 256
    );
    summary_headroom_spin_ = add_generation(
        3, 0, "Summary reasoning headroom (tokens)",
        static_cast<int>(defaults.summary_reasoning_headroom_tokens_), 0, 2000000, 256
    );
    tool_output_cap_spin_ = add_generation(
        3, 1, "Tool output cap (tokens)",
        static_cast<int>(defaults.tool_output_max_tokens_), 128, 2000000, 256
    );
    max_tool_rounds_spin_ = add_generation(
        4, 0, "Max tool rounds", defaults.max_tool_rounds_, 1, 1000, 1
    );
    max_tool_calls_spin_ = add_generation(
        4, 1, "Max tool calls", defaults.max_tool_calls_, 1, 10000, 1
    );

    layout->addWidget(generation);

    layout->addWidget(new QLabel("Custom Instructions:", content));

    instructions_edit_ = new QPlainTextEdit(content);
    instructions_edit_->setTabChangesFocus(false);
    instructions_edit_->setMinimumHeight(120);
    layout->addWidget(instructions_edit_, 1);

    // The memory header carries the label and, on the right, a manual trigger.
    // The automatic summariser only folds once the transcript grows past the
    // configured keep window, so this lets a summary be produced on demand.
    auto* memory_header = new QHBoxLayout();
    memory_header->addWidget(new QLabel("Conversation Memory:", content));
    memory_header->addStretch();

    summarize_button_ = new QPushButton("Summarize now", content);
    summarize_button_->setToolTip(
        "Fold the whole conversation into the memory below. Folds everything "
        "except the newest message, ignoring the automatic trigger and the "
        "verbatim keep window."
    );
    // Only a saved session has stored messages to fold.
    summarize_button_->setEnabled(session_.has_value());
    memory_header->addWidget(summarize_button_);

    layout->addLayout(memory_header);

    // The generated summary can grow long, so it lives in a self-scrolling,
    // editable plain-text field: it can be inspected and corrected in place.
    memory_edit_ = new QPlainTextEdit(content);
    memory_edit_->setTabChangesFocus(false);
    memory_edit_->setMinimumHeight(120);
    memory_edit_->setPlaceholderText(
        "Automatically maintained summary of older turns. Edit to correct it, "
        "or clear it to rebuild it from the whole transcript."
    );
    layout->addWidget(memory_edit_, 1);

    // ---- Memory watermark ----
    // Records how much of the transcript the summary above already covers: only
    // messages with a higher id are still sent verbatim. Exposed so it can be
    // corrected alongside a hand-edited memory.
    auto* watermark_layout = new QHBoxLayout();
    watermark_layout->addWidget(
        new QLabel("Memory watermark (folded up to message id):", content)
    );

    watermark_spin_ = new QSpinBox(content);
    watermark_spin_->setRange(0, 2000000000);
    watermark_spin_->setSingleStep(1);
    watermark_spin_->setToolTip(
        "Highest message id already folded into the summary. Messages above it "
        "are still sent verbatim. Clearing the memory resets this to 0."
    );
    watermark_layout->addWidget(watermark_spin_);
    watermark_layout->addStretch();
    layout->addLayout(watermark_layout);

    // A short, permanent reference for how the automatic memory behaves, so the
    // fields above can be used with confidence.
    auto* docs = new QLabel(content);
    docs->setObjectName(QStringLiteral("SessionDocs"));
    docs->setWordWrap(true);
    docs->setTextFormat(Qt::RichText);
    // Word-wrapped labels can collapse to a single line inside a resizable
    // scroll area unless the layout is told the height depends on the width.
    QSizePolicy docs_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    docs_policy.setHeightForWidth(true);
    docs->setSizePolicy(docs_policy);
    docs->setText(QStringLiteral(
        "<b>How summarization works</b><br>"
        "The conversation memory is a rolling summary of older turns. Messages "
        "with an id above the watermark are still sent verbatim; older turns are "
        "represented only by the summary.<br><br>"
        "When the transcript approaches the model's context limit (or you press "
        "&ldquo;Summarize now&rdquo;), an internal LLM call folds the older "
        "messages into the summary, keeps the newest messages verbatim, and "
        "advances the watermark to the last message it folded. Custom "
        "instructions are kept separate and are never touched by this.<br><br>"
        "The box above holds the persisted summary. Press <b>Save</b> to keep a "
        "hand-edited version; <b>Summarize now</b> always reads the stored "
        "memory from the database, so unsaved edits in the box are ignored and "
        "overwritten on screen.<br><br>"
        "<b>Pressing &ldquo;Summarize now&rdquo; twice</b><br>"
        "While the first run is in flight the button is disabled, so a second "
        "press does nothing. Once it finishes, the first press has already "
        "folded everything except the newest message and saved it, so a second "
        "press finds nothing new, makes no API call, and reports &ldquo;Nothing "
        "to Summarize&rdquo; &mdash; the operation is idempotent, with no "
        "double-folding and no extra cost. It is only <i>not</i> a no-op when "
        "the first pass could not fold everything (for example it hit the "
        "maximum number of summary rounds, or the summariser returned an empty "
        "result); then the second press folds the remainder.<br><br>"
        "Clearing the memory and saving resets the watermark to 0, which "
        "re-admits the whole transcript."
    ));
    layout->addWidget(docs);

    scroll->setWidget(content);
    main_layout->addWidget(scroll, 1);

    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    save_button_ = new QPushButton(
        session_ ? "Save" : "Create",
        this
    );

    cancel_button_ = new QPushButton("Cancel", this);

    button_layout->addWidget(save_button_);
    button_layout->addWidget(cancel_button_);

    main_layout->addLayout(button_layout);

    connect(
        summarize_button_,
        &QPushButton::clicked,
        this,
        [this]() {
            summarize_now();
        }
    );

    connect(
        save_button_,
        &QPushButton::clicked,
        this,
        [this]() {
            save_session();
        }
    );

    connect(
        cancel_button_,
        &QPushButton::clicked,
        this,
        &QDialog::reject
    );
}

void SessionEditor::load_session() {
  if (!session_) {
    return;
  }

  title_edit_->setText(
      QString::fromUtf8(session_->title_.data(),
                        static_cast<qsizetype>(session_->title_.size()))
  );

  api_url_edit_->setText(
      QString::fromUtf8(session_->api_url_.data(),
                        static_cast<qsizetype>(session_->api_url_.size()))
  );

  token_edit_->setText(
      QString::fromUtf8(session_->authentication_token_.data(),
                        static_cast<qsizetype>(session_->authentication_token_.size()))
  );

  model_edit_->setText(
      QString::fromUtf8(session_->model_.data(),
                        static_cast<qsizetype>(session_->model_.size()))
  );

  proxy_edit_->setText(
      session_->proxy_
          ? QString::fromUtf8(
                session_->proxy_->data(),
                static_cast<qsizetype>(session_->proxy_->size())
            )
          : QString{}
  );

  shared_directory_edit_->setText(
      session_->shared_directory_
          ? QString::fromUtf8(
                session_->shared_directory_->data(),
                static_cast<qsizetype>(session_->shared_directory_->size())
            )
          : QString{}
  );

  instructions_edit_->setPlainText(
      QString::fromUtf8(
          session_->custom_instructions_.data(),
          static_cast<qsizetype>(session_->custom_instructions_.size())
      )
  );

  memory_edit_->setPlainText(
      QString::fromUtf8(
          session_->memory_summary_.data(),
          static_cast<qsizetype>(session_->memory_summary_.size())
      )
  );

  watermark_spin_->setValue(static_cast<int>(session_->memory_watermark_));

  const lnb::generation_settings& gen = session_->generation_;
  model_context_spin_->setValue(static_cast<int>(gen.model_context_tokens_));
  response_budget_spin_->setValue(static_cast<int>(gen.response_token_budget_));
  safety_margin_spin_->setValue(static_cast<int>(gen.context_safety_margin_tokens_));
  summary_trigger_spin_->setValue(static_cast<int>(gen.summary_trigger_tokens_));
  summary_keep_spin_->setValue(static_cast<int>(gen.summary_keep_tokens_));
  summary_budget_spin_->setValue(static_cast<int>(gen.summary_token_budget_));
  summary_headroom_spin_->setValue(static_cast<int>(gen.summary_reasoning_headroom_tokens_));
  max_tool_rounds_spin_->setValue(gen.max_tool_rounds_);
  max_tool_calls_spin_->setValue(gen.max_tool_calls_);
  tool_output_cap_spin_->setValue(static_cast<int>(gen.tool_output_max_tokens_));
}

void SessionEditor::save_session() {
  const QString title = title_edit_->text().trimmed();

  if (title.isEmpty()) {
    QMessageBox::warning(
        this,
        "Invalid Session",
        "Title cannot be empty."
    );
    return;
  }

  const std::string title_value = title.toStdString();
  const std::string api_url = api_url_edit_->text().toStdString();
  const std::string authentication_token =
      token_edit_->text().toStdString();
  const std::string model = model_edit_->text().toStdString();
  const std::string proxy = proxy_edit_->text().toStdString();
  const std::string shared_directory =
      shared_directory_edit_->text().toStdString();
  const std::string instructions =
      instructions_edit_->toPlainText().toStdString();
  const std::string memory =
      memory_edit_->toPlainText().toStdString();

  const auto optional_string = [](const std::string& value)
      -> std::optional<std::string> {
    if (value.empty()) {
      return std::nullopt;
    }

    return value;
  };

  // The watermark records how much of the transcript the memory already covers,
  // so editing the text does not make older turns reappear. It is editable, but
  // clearing the memory must re-admit the whole transcript: with an empty
  // summary and a high watermark those turns would otherwise be lost.
  uint32_t memory_watermark = static_cast<uint32_t>(watermark_spin_->value());

  if (memory.empty()) {
    memory_watermark = 0;
    watermark_spin_->setValue(0);
  }

  lnb::session updated_session{
    session_ ? session_->id_ : 0,
    title_value,
    instructions,
    api_url,
    authentication_token,
    model,
    optional_string(proxy),
    optional_string(shared_directory),
    memory,
    session_ ? session_->last_message_at_ : std::nullopt,
    memory_watermark
  };

  // The generation budgets are not constructor parameters; attach them after
  // construction from the editors above.
  updated_session.generation_.model_context_tokens_ =
      static_cast<std::size_t>(model_context_spin_->value());
  updated_session.generation_.response_token_budget_ =
      static_cast<std::size_t>(response_budget_spin_->value());
  updated_session.generation_.context_safety_margin_tokens_ =
      static_cast<std::size_t>(safety_margin_spin_->value());
  updated_session.generation_.summary_keep_tokens_ =
      static_cast<std::size_t>(summary_keep_spin_->value());
  updated_session.generation_.summary_token_budget_ =
      static_cast<std::size_t>(summary_budget_spin_->value());
  updated_session.generation_.summary_reasoning_headroom_tokens_ =
      static_cast<std::size_t>(summary_headroom_spin_->value());
  updated_session.generation_.summary_trigger_tokens_ =
      static_cast<std::size_t>(summary_trigger_spin_->value());
  updated_session.generation_.max_tool_rounds_ = max_tool_rounds_spin_->value();
  updated_session.generation_.max_tool_calls_ = max_tool_calls_spin_->value();
  updated_session.generation_.tool_output_max_tokens_ =
      static_cast<std::size_t>(tool_output_cap_spin_->value());

  try {
    if (session_) {
      state_.uni_db_->update_session(updated_session);
    } else {
      state_.uni_db_->insert_session(updated_session);
    }
  } catch (const std::exception& error) {
    QMessageBox::critical(
        this,
        "Unable to Save Session",
        QString::fromUtf8(error.what())
    );
    return;
  }

  accept();
}

void SessionEditor::summarize_now() {
  // A new (unsaved) session has no stored messages to fold.
  if (!session_ || summarizing_) {
    return;
  }

  const std::string model = model_edit_->text().toStdString();
  const std::string api_url = api_url_edit_->text().toStdString();
  const std::string api_key = token_edit_->text().toStdString();
  const std::string proxy_text = proxy_edit_->text().toStdString();
  const std::optional<std::string> proxy = proxy_text.empty()
      ? std::nullopt
      : std::optional<std::string>(proxy_text);

  const uint32_t session_id = session_->id_;

  auto task = std::make_shared<lnb::GenerationTask>();
  task->start();
  const auto stop_token = task->stop_token();

  summarizing_ = true;
  summarize_button_->setEnabled(false);
  summarize_button_->setText("Summarizing...");
  // Block the other actions so the dialog cannot be dismissed or saved while a
  // summary is in flight.
  save_button_->setEnabled(false);
  cancel_button_->setEnabled(false);

  state_.multi_executor_->push(
      [task,
       stop_token,
       state = &state_,
       session_id,
       self_id = state_.self_id_,
       model,
       api_url,
       api_key,
       proxy]() {
        try {
          lnb::summarize_session_now(
              *state, session_id, self_id, *task, stop_token, model, api_url, api_key, proxy
          );
          task->complete();
        } catch (const std::exception& error) {
          if (stop_token.stop_requested()) {
            return;
          }

          task->fail(error.what());
        } catch (...) {
          task->fail("Unknown summarization error");
        }
      }
  );

  // Keep the modal dialog responsive while the worker folds the transcript: the
  // nested loop only pumps events until the task reaches a terminal state.
  QEventLoop loop;
  QTimer timer;

  QObject::connect(
      &timer,
      &QTimer::timeout,
      &loop,
      [&task, &loop]() {
        if (task->is_terminal()) {
          loop.quit();
        }
      }
  );

  timer.start(100);
  loop.exec();
  timer.stop();

  summarizing_ = false;
  summarize_button_->setEnabled(true);
  summarize_button_->setText("Summarize now");
  save_button_->setEnabled(true);
  cancel_button_->setEnabled(true);

  const auto snapshot = task->snapshot();

  if (snapshot.status == lnb::GenerationTask::Status::FAILED) {
    QMessageBox::warning(
        this,
        "Summarization Failed",
        QString::fromUtf8(
            snapshot.error.data(), static_cast<qsizetype>(snapshot.error.size())
        )
    );
    return;
  }

  // The summariser persists the new memory; reload it so the box shows the
  // result and a later Save keeps the advanced watermark.
  const auto updated = state_.uni_db_->get_session_by_id(session_id);

  if (!updated.has_value()) {
    return;
  }

  const QString memory = QString::fromUtf8(
      updated->memory_summary_.data(),
      static_cast<qsizetype>(updated->memory_summary_.size())
  );
  const QString previous = memory_edit_->toPlainText();

  memory_edit_->setPlainText(memory);
  session_->memory_summary_ = updated->memory_summary_;
  session_->memory_watermark_ = updated->memory_watermark_;
  watermark_spin_->setValue(static_cast<int>(updated->memory_watermark_));

  if (memory.isEmpty() || memory == previous) {
    QMessageBox::information(
        this,
        "Nothing to Summarize",
        "There is no new conversation to fold into memory yet."
    );
  }
}

} // namespace ui
