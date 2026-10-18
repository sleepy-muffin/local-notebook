#include "ui/SummaryReviewDialog.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <string>
#include <utility>

#include "support/i18n.hpp"
#include "ui/BackgroundWidget.hpp"
#include "ui/dialog_style.hpp"
#include "ui/theme.hpp"

namespace ui {

using namespace dialog_style;

namespace {

// Renders the exact request body as plain text: the model, the completion cap
// and every message with its role. This is the same JSON that send_message_once
// posts, only formatted for reading.
QString render_request(const boost::json::object& request) {
  QString text;

  const boost::json::value* model = request.if_contains("model");
  text += trs("Model: %1\n").arg(
      model != nullptr && model->is_string()
          ? QString::fromStdString(std::string(model->as_string()))
          : QString{}
  );

  int max_tokens = 0;
  if (const boost::json::value* cap = request.if_contains("max_tokens");
      cap != nullptr && cap->is_number()) {
    max_tokens = static_cast<int>(cap->to_number<double>());
  }

  text += trs("Completion cap: %1 tokens\n\n").arg(max_tokens);

  const boost::json::value* messages = request.if_contains("messages");

  if (messages == nullptr || !messages->is_array()) {
    return text;
  }

  for (const boost::json::value& value: messages->as_array()) {
    if (!value.is_object()) {
      continue;
    }

    const boost::json::object& message = value.as_object();
    const boost::json::value* role = message.if_contains("role");
    const boost::json::value* content = message.if_contains("content");

    text += QStringLiteral("----- %1 -----\n").arg(
        role != nullptr && role->is_string()
            ? QString::fromStdString(std::string(role->as_string()))
            : QString{}
    );

    if (content != nullptr && content->is_string()) {
      text += QString::fromStdString(std::string(content->as_string()));
    }

    text += QStringLiteral("\n\n");
  }

  return text;
}

// Runs `show` on the interface thread, blocking the caller until it returns.
// When the caller already is the interface thread the function runs inline.
template <typename Function>
void on_ui_thread(Function&& show) {
  QCoreApplication* application = QApplication::instance();

  if (application == nullptr || QThread::currentThread() == application->thread()) {
    show();
    return;
  }

  QMetaObject::invokeMethod(
      application, std::forward<Function>(show), Qt::BlockingQueuedConnection
  );
}

} // namespace

SummaryReviewDialog::SummaryReviewDialog(
    const lnb::SummaryPlan& plan, QWidget* parent
)
    : QDialog(parent) {
  setObjectName(QStringLiteral("SummaryReview"));
  setWindowTitle(trs("Review summarization"));
  setModal(true);
  resize(680, 640);
  setMinimumSize(480, 400);
  setFont(ui_font());

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto* background = new BackgroundWidget(this);
  root->addWidget(background);

  auto* outer = new QVBoxLayout(background);
  outer->setContentsMargins(24, 20, 24, 16);
  outer->setSpacing(0);

  auto* heading = new QLabel(trs("Review summarization"), background);
  heading->setObjectName(QStringLiteral("FormTitle"));
  heading->setFont(scaled_font(4.0, true));
  outer->addWidget(heading);
  outer->addSpacing(6);

  auto* subtitle = new QLabel(
      trs(
          "The following request is about to be sent to the model to update the %1. "
          "Review it and choose whether to run the summarisation."
      )
          .arg(to_qstring(plan.kind)),
      background
  );
  subtitle->setObjectName(QStringLiteral("FormSubtitle"));
  subtitle->setWordWrap(true);
  outer->addWidget(subtitle);
  outer->addSpacing(14);

  auto* preview = new QPlainTextEdit(background);
  preview->setObjectName(QStringLiteral("SummaryPreview"));
  preview->setReadOnly(true);
  preview->setPlainText(render_request(plan.request));
  preview->setLineWrapMode(QPlainTextEdit::NoWrap);
  outer->addWidget(preview, 1);

  if (!plan.watermark_text.empty()) {
    outer->addSpacing(10);

    auto* watermark = new QLabel(to_qstring(plan.watermark_text), background);
    watermark->setObjectName(QStringLiteral("Muted"));
    watermark->setWordWrap(true);
    outer->addWidget(watermark);
  }

  if (plan.more_requests) {
    outer->addSpacing(6);

    auto* note = new QLabel(
        trs(
            "This is the first of several requests: the remaining messages are folded "
            "in further batches after this one."
        ),
        background
    );
    note->setObjectName(QStringLiteral("Muted"));
    note->setWordWrap(true);
    outer->addWidget(note);
  }

  outer->addSpacing(12);
  outer->addWidget(h_separator(background));
  outer->addSpacing(12);

  auto* footer = new QHBoxLayout();
  footer->setSpacing(8);
  footer->addStretch(1);

  auto* cancel = new QPushButton(trs("Cancel"), background);
  cancel->setObjectName(QStringLiteral("Secondary"));

  auto* run = new QPushButton(trs("Run"), background);
  run->setObjectName(QStringLiteral("Primary"));
  run->setDefault(true);

  footer->addWidget(cancel);
  footer->addWidget(run);
  outer->addLayout(footer);

  setStyleSheet(dialog_stylesheet(QStringLiteral("SummaryReview")));

  connect(cancel, &QPushButton::clicked, this, [this]() { reject(); });
  connect(run, &QPushButton::clicked, this, [this]() { accept(); });
}

bool review_summary(const lnb::SummaryPlan& plan) {
  bool approved = false;

  on_ui_thread([&plan, &approved]() {
    SummaryReviewDialog dialog(plan);
    approved = dialog.exec() == QDialog::Accepted;
  });

  if (!approved && plan.automatic) {
    // Skipping an automatic pass leaves the messages unfolded, which is not
    // harmless: say so, and point at the manual action.
    on_ui_thread([]() {
      QMessageBox::warning(
          QApplication::activeWindow(), trs("Summarization Skipped"),
          trs(
              "The automatic summarisation was skipped. Some messages may not be "
              "folded into memory; use Summarize now before sending new messages if "
              "you want them converged."
          )
      );
    });
  }

  return approved;
}

} // namespace ui
