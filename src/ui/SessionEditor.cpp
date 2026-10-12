#include "SessionEditor.hpp"
#include "../ui.hpp"
#include "../external.hpp"

#include <QEventLoop>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
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
  resize(650, 680);

  create_layout();

  if (session_) {
    load_session();
  }
}

void SessionEditor::create_layout() {
    auto* main_layout = new QVBoxLayout(this);

    main_layout->addWidget(new QLabel("Title:", this));

    title_edit_ = new QLineEdit(this);
    main_layout->addWidget(title_edit_);

    auto* settings_layout = new QGridLayout();

    api_url_edit_ = new QLineEdit(this);
    token_edit_ = new QLineEdit(this);
    model_edit_ = new QLineEdit(this);
    proxy_edit_ = new QLineEdit(this);

    settings_layout->addWidget(new QLabel("API URL:", this), 0, 0);
    settings_layout->addWidget(api_url_edit_, 1, 0);

    settings_layout->addWidget(
        new QLabel("Authentication Token:", this), 0, 1
    );
    settings_layout->addWidget(token_edit_, 1, 1);

    settings_layout->addWidget(new QLabel("Model:", this), 2, 0);
    settings_layout->addWidget(model_edit_, 3, 0);

    settings_layout->addWidget(new QLabel("Proxy:", this), 2, 1);
    settings_layout->addWidget(proxy_edit_, 3, 1);

    main_layout->addLayout(settings_layout);

    main_layout->addWidget(new QLabel("Shared Directory:", this));

    shared_directory_edit_ = new QLineEdit(this);
    main_layout->addWidget(shared_directory_edit_);

    main_layout->addWidget(new QLabel("Custom Instructions:", this));

    instructions_edit_ = new QPlainTextEdit(this);
    instructions_edit_->setTabChangesFocus(false);
    main_layout->addWidget(instructions_edit_, 1);

    // The memory header carries the label and, on the right, a manual trigger.
    // The automatic summariser only folds once the transcript grows past the
    // configured keep window, so this lets a summary be produced on demand.
    auto* memory_header = new QHBoxLayout();
    memory_header->addWidget(new QLabel("Conversation Memory:", this));
    memory_header->addStretch();

    summarize_button_ = new QPushButton("Summarize now", this);
    summarize_button_->setToolTip(
        "Fold the whole conversation into the memory below. Folds everything "
        "except the newest message, ignoring the automatic trigger and the "
        "verbatim keep window."
    );
    // Only a saved session has stored messages to fold.
    summarize_button_->setEnabled(session_.has_value());
    memory_header->addWidget(summarize_button_);

    main_layout->addLayout(memory_header);

    // The generated summary can grow long, so it lives in a self-scrolling,
    // editable plain-text field: it can be inspected and corrected in place.
    memory_edit_ = new QPlainTextEdit(this);
    memory_edit_->setTabChangesFocus(false);
    memory_edit_->setPlaceholderText(
        "Automatically maintained summary of older turns. Edit to correct it, "
        "or clear it to rebuild it from the whole transcript."
    );
    main_layout->addWidget(memory_edit_, 1);

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

  // Keep the watermark that records how much of the transcript the memory
  // already covers, so editing the text does not make older turns reappear.
  // Clearing the memory, however, must re-admit the whole transcript: with an
  // empty summary and a high watermark those turns would otherwise be lost.
  uint32_t memory_watermark = session_ ? session_->memory_watermark_ : 0;

  if (memory.empty()) {
    memory_watermark = 0;
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

  if (memory.isEmpty() || memory == previous) {
    QMessageBox::information(
        this,
        "Nothing to Summarize",
        "There is no new conversation to fold into memory yet."
    );
  }
}

} // namespace ui
