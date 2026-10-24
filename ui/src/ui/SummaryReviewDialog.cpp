#include "ui/SummaryReviewDialog.hpp"

#include <QApplication>
#include <QCheckBox>
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

#include <boost/json/serialize.hpp>

#include <string>
#include <string_view>
#include <utility>

#include "support/i18n.hpp"
#include "ui/BackgroundWidget.hpp"
#include "ui/dialog_style.hpp"
#include "ui/theme.hpp"

namespace ui {

using namespace dialog_style;

namespace {

QString as_qstring(const boost::json::value& value) {
  return QString::fromStdString(std::string(value.as_string()));
}

QString string_field(const boost::json::object& object, std::string_view key) {
  const boost::json::value* value = object.if_contains(key);
  return value != nullptr && value->is_string() ? as_qstring(*value) : QString{};
}

QString compact_json(const boost::json::value& value) {
  return QString::fromStdString(boost::json::serialize(value));
}

// Indents a request so the exact body is still readable in the preview. Scalars
// are delegated to Boost's serializer, which guarantees the same escaping as the
// bytes actually sent.
void append_pretty(
    QString& out, const boost::json::value& value, int depth
) {
  const QString pad(depth * 2, QLatin1Char(' '));
  const QString inner((depth + 1) * 2, QLatin1Char(' '));

  if (value.is_object()) {
    const boost::json::object& object = value.as_object();

    if (object.empty()) {
      out += QStringLiteral("{}");
      return;
    }

    out += QStringLiteral("{\n");
    bool first = true;

    for (const boost::json::key_value_pair& entry: object) {
      if (!first) {
        out += QStringLiteral(",\n");
      }
      first = false;
      out += inner;
      out += compact_json(boost::json::value(entry.key()));
      out += QStringLiteral(": ");
      append_pretty(out, entry.value(), depth + 1);
    }

    out += QLatin1Char('\n');
    out += pad;
    out += QLatin1Char('}');
    return;
  }

  if (value.is_array()) {
    const boost::json::array& array = value.as_array();

    if (array.empty()) {
      out += QStringLiteral("[]");
      return;
    }

    out += QStringLiteral("[\n");
    bool first = true;

    for (const boost::json::value& element: array) {
      if (!first) {
        out += QStringLiteral(",\n");
      }
      first = false;
      out += inner;
      append_pretty(out, element, depth + 1);
    }

    out += QLatin1Char('\n');
    out += pad;
    out += QLatin1Char(']');
    return;
  }

  out += compact_json(value);
}

// Content is a plain string in both protocols, but Chat content parts and
// Responses output can also be an array of typed parts. Text parts are shown
// verbatim; non-text parts are named so they are not silently dropped.
QString content_text(const boost::json::value* content) {
  if (content == nullptr) {
    return {};
  }

  if (content->is_string()) {
    return as_qstring(*content);
  }

  if (!content->is_array()) {
    return {};
  }

  QString text;

  for (const boost::json::value& part: content->as_array()) {
    if (part.is_string()) {
      if (!text.isEmpty()) {
        text += QLatin1Char('\n');
      }
      text += as_qstring(part);
      continue;
    }

    if (!part.is_object()) {
      continue;
    }

    const boost::json::object& object = part.as_object();
    const QString part_text = string_field(object, "text");

    if (!text.isEmpty()) {
      text += QLatin1Char('\n');
    }

    if (!part_text.isEmpty()) {
      text += part_text;
      continue;
    }

    const QString type = string_field(object, "type");

    if (!type.isEmpty()) {
      text += QStringLiteral("[%1]").arg(type);
    } else if (object.if_contains("image_url") != nullptr) {
      text += trs("[image]");
    } else {
      text += compact_json(part);
    }
  }

  return text;
}

QString reasoning_text(const boost::json::object& item) {
  if (const boost::json::value* summary = item.if_contains("summary");
      summary != nullptr) {
    QString text = content_text(summary);

    if (!text.isEmpty()) {
      return text;
    }
  }

  return content_text(item.if_contains("content"));
}

// Chat nests tool calls under `function`; Responses puts the name and arguments
// on the item itself. Accept either shape.
QString render_tool_calls(const boost::json::value* calls) {
  if (calls == nullptr || !calls->is_array()) {
    return {};
  }

  QString text;

  for (const boost::json::value& value: calls->as_array()) {
    if (!value.is_object()) {
      continue;
    }

    const boost::json::object& call = value.as_object();
    const boost::json::object* function = &call;

    if (const boost::json::value* nested = call.if_contains("function");
        nested != nullptr && nested->is_object()) {
      function = &nested->as_object();
    }

    if (!text.isEmpty()) {
      text += QLatin1Char('\n');
    }

    text += trs("Tool call: %1").arg(string_field(*function, "name"));

    const QString id = string_field(call, "id");

    if (!id.isEmpty()) {
      text += QStringLiteral(" (%1)").arg(id);
    }

    const QString arguments = string_field(*function, "arguments");

    if (!arguments.isEmpty()) {
      text += QLatin1Char('\n');
      text += arguments;
    }
  }

  return text;
}

QString render_item(const boost::json::object& item) {
  const QString type = string_field(item, "type");
  const QString role = string_field(item, "role");
  QString label;
  QString body;

  if (type == QStringLiteral("function_call")) {
    label = trs("function call: %1").arg(string_field(item, "name"));

    const QString call_id = string_field(item, "call_id");

    if (!call_id.isEmpty()) {
      label += QStringLiteral(" (%1)").arg(call_id);
    }

    const boost::json::value* arguments = item.if_contains("arguments");
    body = content_text(arguments);

    if (body.isEmpty() && arguments != nullptr) {
      body = compact_json(*arguments);
    }
  } else if (type == QStringLiteral("function_call_output")) {
    const QString call_id = string_field(item, "call_id");
    label = trs("function result (%1)").arg(call_id);

    const boost::json::value* output = item.if_contains("output");
    body = content_text(output);

    if (body.isEmpty() && output != nullptr) {
      body = compact_json(*output);
    }
  } else if (type == QStringLiteral("reasoning")) {
    label = trs("reasoning");
    body = reasoning_text(item);
  } else {
    label = !role.isEmpty() ? role : (!type.isEmpty() ? type : trs("item"));

    if (role == QStringLiteral("tool")) {
      const QString call_id = string_field(item, "tool_call_id");

      if (!call_id.isEmpty()) {
        label += QStringLiteral(" (%1)").arg(call_id);
      }
    }

    body = content_text(item.if_contains("content"));

    const QString calls = render_tool_calls(item.if_contains("tool_calls"));

    if (!calls.isEmpty()) {
      if (!body.isEmpty()) {
        body += QLatin1Char('\n');
      }
      body += calls;
    }
  }

  if (body.isEmpty()) {
    body = trs("(empty)");
  }

  return QStringLiteral("----- %1 -----\n%2\n\n").arg(label, body);
}

// Renders the request body as plain text: the model, the completion cap and
// every message with its role. Responses sends the system prompt as top-level
// `instructions` and the conversation as `input`; Chat Completions puts the
// system prompt at the head of `messages`. Both shapes are handled, so the
// preview is faithful whichever protocol the assistant uses.
QString render_request(const boost::json::object& request) {
  QString text;

  const QString model = string_field(request, "model");
  text += trs("Model: %1\n").arg(model.isEmpty() ? trs("(not set)") : model);

  int max_tokens = 0;

  for (const std::string_view key: {"max_output_tokens", "max_tokens"}) {
    if (const boost::json::value* cap = request.if_contains(key);
        cap != nullptr && cap->is_number()) {
      max_tokens = static_cast<int>(cap->to_number<double>());
      break;
    }
  }

  text += trs("Completion cap: %1 tokens\n").arg(max_tokens);

  QString effort;

  if (const boost::json::value* value = request.if_contains("reasoning_effort");
      value != nullptr && value->is_string()) {
    effort = as_qstring(*value);
  } else if (const boost::json::value* reasoning = request.if_contains("reasoning");
             reasoning != nullptr && reasoning->is_object()) {
    effort = string_field(reasoning->as_object(), "effort");
  }

  if (!effort.isEmpty()) {
    text += trs("Reasoning effort: %1\n").arg(effort);
  }

  const boost::json::value* input = request.if_contains("input");
  const bool responses = input != nullptr && input->is_array();
  const boost::json::value* items = responses ? input : request.if_contains("messages");

  if (items == nullptr || !items->is_array()) {
    text += trs("Messages: 0\n\n");
    text += trs("(no messages in this request)\n");
    return text;
  }

  text += (responses ? trs("Input items: %1\n\n") : trs("Messages: %1\n\n"))
              .arg(static_cast<qlonglong>(items->as_array().size()));

  const QString instructions = string_field(request, "instructions");

  if (!instructions.isEmpty()) {
    text += trs("----- system (instructions) -----\n%1\n\n").arg(instructions);
  }

  for (const boost::json::value& value: items->as_array()) {
    if (!value.is_object()) {
      text += QStringLiteral("----- item -----\n%1\n\n").arg(compact_json(value));
      continue;
    }

    text += render_item(value.as_object());
  }

  return text;
}

QString render_exact(const boost::json::object& request) {
  QString text;
  append_pretty(text, boost::json::value(request), 0);
  text += QLatin1Char('\n');
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
  setWindowTitle(trs("Review memory update"));
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

  auto* heading = new QLabel(trs("Review memory update"), background);
  heading->setObjectName(QStringLiteral("FormTitle"));
  heading->setFont(scaled_font(4.0, true));
  outer->addWidget(heading);
  outer->addSpacing(6);

  auto* subtitle = new QLabel(
      trs(
          "The following request is about to be sent to the model to update the %1. "
          "Review it and choose whether to run the memory update."
      )
          .arg(to_qstring(plan.kind)),
      background
  );
  subtitle->setObjectName(QStringLiteral("FormSubtitle"));
  subtitle->setWordWrap(true);
  outer->addWidget(subtitle);
  outer->addSpacing(14);

  const QString readable = render_request(plan.request);
  const QString exact = render_exact(plan.request);

  auto* exact_toggle = new QCheckBox(trs("Show exact JSON request body"), background);
  exact_toggle->setToolTip(
      trs("Shows the pretty-printed JSON that is posted to the API, including "
          "every field the readable view does not spell out.")
  );
  outer->addWidget(exact_toggle);
  outer->addSpacing(8);

  auto* preview = new QPlainTextEdit(background);
  preview->setObjectName(QStringLiteral("SummaryPreview"));
  preview->setReadOnly(true);
  preview->setPlainText(readable);
  preview->setLineWrapMode(QPlainTextEdit::NoWrap);
  outer->addWidget(preview, 1);

  connect(
      exact_toggle, &QCheckBox::toggled, preview,
      [preview, readable, exact](bool checked) {
        preview->setPlainText(checked ? exact : readable);
      }
  );

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
            "This is the first of several requests: the remaining messages are merged "
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
    // Skipping an automatic pass leaves the messages unmerged, which is not
    // harmless: say so, and point at the manual action.
    on_ui_thread([]() {
      QMessageBox::warning(
          QApplication::activeWindow(), trs("Memory Update Skipped"),
          trs(
              "The automatic memory update was skipped. Some messages may not be "
              "merged into memory; use Update memory now before sending new messages "
              "if you want them included."
          )
      );
    });
  }

  return approved;
}

} // namespace ui
