#include "ui/SummaryReviewDialog.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <string>
#include <utility>

#include "ui/BackgroundWidget.hpp"
#include "ui/dialog_style.hpp"
#include "ui/theme.hpp"

namespace ui {

using namespace dialog_style;

namespace {

// Renders the exact request body as plain text: the model, the completion cap
// and every message with its role. This is the same JSON that send_message_once
// posts, only formatted for reading.
QString render_request(const QJsonObject& request) {
  QString text;

  text += QStringLiteral("Model: %1\n").arg(request.value("model").toString());
  text += QStringLiteral("Completion cap: %1 tokens\n\n")
              .arg(request.value("max_tokens").toInt());

  const QJsonArray messages = request.value("messages").toArray();

  for (const QJsonValue& value: messages) {
    const QJsonObject message = value.toObject();
    text += QStringLiteral("----- %1 -----\n").arg(message.value("role").toString());
    text += message.value("content").toString();
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
    const lnb::net::SummaryPlan& plan, QWidget* parent
)
    : QDialog(parent) {
  setObjectName(QStringLiteral("SummaryReview"));
  setWindowTitle(QStringLiteral("Review summarization"));
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

  auto* heading = new QLabel(QStringLiteral("Review summarization"), background);
  heading->setObjectName(QStringLiteral("FormTitle"));
  heading->setFont(scaled_font(4.0, true));
  outer->addWidget(heading);
  outer->addSpacing(6);

  auto* subtitle = new QLabel(
      QStringLiteral(
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
        QStringLiteral(
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

  auto* cancel = new QPushButton(QStringLiteral("Cancel"), background);
  cancel->setObjectName(QStringLiteral("Secondary"));

  auto* run = new QPushButton(QStringLiteral("Run"), background);
  run->setObjectName(QStringLiteral("Primary"));
  run->setDefault(true);

  footer->addWidget(cancel);
  footer->addWidget(run);
  outer->addLayout(footer);

  setStyleSheet(dialog_stylesheet(QStringLiteral("SummaryReview")));

  connect(cancel, &QPushButton::clicked, this, [this]() { reject(); });
  connect(run, &QPushButton::clicked, this, [this]() { accept(); });
}

bool review_summary(const lnb::net::SummaryPlan& plan) {
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
          QApplication::activeWindow(), QStringLiteral("Summarization Skipped"),
          QStringLiteral(
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
