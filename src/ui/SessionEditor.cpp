#include "SessionEditor.hpp"
#include "../ui.hpp"

#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMessageBox>

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
  resize(650, 550);

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

    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    auto* save_button = new QPushButton(
        session_ ? "Save" : "Create",
        this
    );

    auto* cancel_button = new QPushButton("Cancel", this);

    button_layout->addWidget(save_button);
    button_layout->addWidget(cancel_button);

    main_layout->addLayout(button_layout);

    connect(
        save_button,
        &QPushButton::clicked,
        this,
        [this]() {
            save_session();
        }
    );

    connect(
        cancel_button,
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

  const auto optional_string = [](const std::string& value)
      -> std::optional<std::string> {
    if (value.empty()) {
      return std::nullopt;
    }

    return value;
  };

  lnb::session updated_session{
    session_ ? session_->id_ : 0,
    title_value,
    instructions,
    api_url,
    authentication_token,
    model,
    optional_string(proxy),
    optional_string(shared_directory)
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


}