#include "ui/SettingsWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "app/application.hpp"
#include "app/task_manager.hpp"
#include "net/client.hpp"
#include "support/generation_task.hpp"
#include "support/i18n.hpp"
#include "ui/AssistantForm.hpp"
#include "ui/BackgroundWidget.hpp"
#include "ui/SpinBox.hpp"
#include "ui/SummaryReviewDialog.hpp"
#include "ui/dialog_style.hpp"
#include "ui/theme.hpp"

namespace ui {

using namespace dialog_style;

namespace {

QString rgba(const QColor& color) {
  return QStringLiteral("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alphaF());
}

// A titled card used by the Account and About pages. Its frame is styled by the
// window's own scoped sheet (see apply_style), so a theme change reaches it.
QWidget* make_card(QWidget* parent, const QString& heading) {
  auto* card = new QFrame(parent);
  card->setObjectName(QStringLiteral("Card"));
  card->setAttribute(Qt::WA_StyledBackground, true);

  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(16, 14, 16, 14);
  layout->setSpacing(4);

  if (!heading.isEmpty()) {
    auto* label = new QLabel(heading, card);
    label->setObjectName(QStringLiteral("SectionLabel"));
    apply_derived_font(label, 0.0, true);
    layout->addWidget(label);
    layout->addSpacing(4);
  }

  return card;
}

// The vertical body of a card, added to it and returned for filling.
QVBoxLayout* card_body(QWidget* card) {
  auto* body = new QWidget(card);
  auto* layout = new QVBoxLayout(body);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(2);
  static_cast<QVBoxLayout*>(card->layout())->addWidget(body);
  return layout;
}

QLabel* value_label(QWidget* parent, const QString& text, bool prominent) {
  auto* label = new QLabel(text, parent);
  label->setObjectName(prominent ? QStringLiteral("PageTitle") : QStringLiteral("Muted"));
  apply_derived_font(label, prominent ? 1.0 : 0.0, prominent);
  label->setWordWrap(true);
  return label;
}

// Reads a bundled license text. It returns an empty string when the resource is
// missing, so the About page degrades to its short caption rather than failing to
// build.
QString read_license(const QString& path) {
  QFile file(path);

  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }

  return QString::fromUtf8(file.readAll());
}

// The verbatim license text. It is a plain, read-only text view rather than a
// label: the text is shown exactly as shipped, so it is not run through the
// markdown-ish label styling, and a long license scrolls within a fixed height.
QPlainTextEdit* license_view(QWidget* parent, const QString& text) {
  auto* view = new QPlainTextEdit(text, parent);
  view->setReadOnly(true);
  view->setTabChangesFocus(true);
  view->setMinimumHeight(220);
  return view;
}

// Fills a font selector from a stored family. An empty family means "the system
// default", so the selectors were left untouched at build time; on a reload we
// resolve it to the application's font, which is what an empty family already
// behaves as.
void set_font_box(FontComboBox* box, const std::string& family) {
  if (!family.empty()) {
    box->setCurrentFont(QFont(QString::fromStdString(family)));
  } else {
    box->setCurrentFont(QApplication::font());
  }
}

// Whether the two settings agree on everything the rendered theme depends on.
// Saving an unrelated field (a budget, a directory) must not trigger a full
// application restyle and background decode, which is what makes Save feel slow.
bool theme_inputs_equal(const lnb::Settings& a, const lnb::Settings& b) {
  return a.theme == b.theme && a.ui_font == b.ui_font &&
         a.ui_font_size == b.ui_font_size && a.font == b.font &&
         a.font_size == b.font_size && a.mono_font == b.mono_font &&
         a.mono_font_size == b.mono_font_size &&
         a.background_url == b.background_url &&
         a.background_override == b.background_override && a.metrics == b.metrics;
}

} // namespace

SettingsWindow::SettingsWindow(lnb::Application& app, QWidget* parent)
    : QDialog(parent),
      app_(app),
      pending_(app.settings()),
      pending_metrics_(metrics()) {
  setObjectName(QStringLiteral("SettingsWindow"));
  setWindowTitle(trs("Settings"));
  setModal(false);
  resize(780, 580);
  setMinimumSize(660, 480);
  setFont(ui_font());

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto* background = new BackgroundWidget(this);
  root->addWidget(background);

  auto* outer = new QVBoxLayout(background);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* body = new QHBoxLayout();
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);

  nav_ = new QListWidget(background);
  nav_->setObjectName(QStringLiteral("SettingsNav"));
  // The rail shows the dialog behind it; the rule lives on the list so a style
  // that paints from the palette cannot fill the scrollbar column black.
  show_parent_background(nav_);
  nav_->setFrameShape(QFrame::NoFrame);
  nav_->setSelectionMode(QAbstractItemView::SingleSelection);
  nav_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  nav_->setFocusPolicy(Qt::NoFocus);
  nav_->addItem(trs("Configuration"));
  nav_->addItem(trs("Metrics"));
  nav_->addItem(trs("Assistant"));
  nav_->addItem(trs("Account"));
  nav_->addItem(trs("About"));

  auto* nav_wrap = new QWidget(background);
  auto* nav_layout = new QVBoxLayout(nav_wrap);
  nav_layout->setContentsMargins(12, 14, 8, 14);
  nav_layout->setSpacing(0);
  nav_layout->addWidget(nav_);
  nav_wrap->setFixedWidth(188);

  stack_ = new QStackedWidget(background);
  stack_->setObjectName(QStringLiteral("SettingsStack"));
  stack_->setFrameShape(QFrame::NoFrame);

  stack_->addWidget(build_config_page());
  stack_->addWidget(build_metrics_page());
  stack_->addWidget(build_assistant_page());
  stack_->addWidget(build_account_page());
  stack_->addWidget(build_about_page());

  body->addWidget(nav_wrap);
  body->addWidget(v_separator(background));
  body->addWidget(stack_, 1);

  outer->addLayout(body, 1);

  // A divider above the action row, as requested.
  outer->addWidget(h_separator(background));

  auto* footer = new QWidget(background);
  auto* footer_layout = new QHBoxLayout(footer);
  footer_layout->setContentsMargins(16, 12, 16, 12);
  footer_layout->setSpacing(8);
  footer_layout->addStretch(1);

  auto* close_button = new QPushButton(trs("Close"), footer);
  close_button->setObjectName(QStringLiteral("Secondary"));

  auto* save_button = new QPushButton(trs("Save"), footer);
  save_button->setObjectName(QStringLiteral("Primary"));
  save_button->setDefault(true);

  footer_layout->addWidget(close_button);
  footer_layout->addWidget(save_button);

  outer->addWidget(footer);

  apply_style();

  connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row >= 0) {
      stack_->setCurrentIndex(row);
    }
  });
  nav_->setCurrentRow(0);

  connect(close_button, &QPushButton::clicked, this, [this]() { reject(); });
  connect(save_button, &QPushButton::clicked, this, [this]() { save(); });
}

void SettingsWindow::apply_style() {
  // Scoped chrome plus the navigation's own row states, so the rail reads like
  // the sidebar session list. The card frame is styled here too (rather than on
  // the widget) so a theme change reaches every card on the next show.
  QString sheet = dialog_stylesheet(QStringLiteral("SettingsWindow"));
  sheet += QStringLiteral(
               "QFrame#Card { background: %1; border: 1px solid %2; border-radius: 8px; }")
               .arg(rgba(theme().colors.base), rgba(theme().colors.border));
  sheet += QStringLiteral(
               "QListWidget#SettingsNav::item { color: %1; padding: 7px 10px;"
               " border-radius: 6px; margin: 1px 0; }")
               .arg(theme().colors.text_muted.name());
  sheet += QStringLiteral("QListWidget#SettingsNav::item:hover { background: %1; }")
               .arg(rgba(row_hover_background()));
  sheet += QStringLiteral("QListWidget#SettingsNav::item:selected { background: %1; color: %2; }")
               .arg(rgba(row_selected_background()), theme().colors.text.name());
  sheet += QStringLiteral("QStackedWidget#SettingsStack { background: transparent; }");
  setStyleSheet(sheet);

  // The derived fonts are set programmatically, so the stylesheet cannot refresh
  // them: re-derive them from the current base font, which also covers a font
  // change (not only a theme change).
  reapply_derived_fonts(this);
}

void SettingsWindow::set_current_session(std::uint32_t session_id) {
  current_session_ = session_id;
  update_account_page();
}

QWidget* SettingsWindow::make_page(
    const QString& title, const QString& subtitle, QWidget* content
) {
  auto* page = new QWidget(stack_);
  page->setObjectName(QStringLiteral("Page"));
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(22, 18, 22, 18);
  layout->setSpacing(0);

  auto* heading = new QLabel(title, page);
  heading->setObjectName(QStringLiteral("PageTitle"));
  apply_derived_font(heading, 4.0, true);

  auto* sub = new QLabel(subtitle, page);
  sub->setObjectName(QStringLiteral("PageSubtitle"));

  layout->addWidget(heading);
  layout->addWidget(sub);
  layout->addSpacing(14);

  auto* scroll = new QScrollArea(page);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(content);

  layout->addWidget(scroll, 1);
  return page;
}

QWidget* SettingsWindow::build_config_page() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 12, 0);
  layout->setSpacing(12);

  // ---- Appearance ----
  layout->addWidget(section_label(content, trs("Appearance")));
  {
    auto* form = form_layout(content);

    auto* language_box = new ComboBox(content);
    for (const lnb::i18n::Language language : lnb::i18n::kLanguages) {
      language_box->addItem(lnb::i18n::language_name(language), static_cast<int>(language));
    }
    {
      const int index = language_box->findData(
          static_cast<int>(lnb::i18n::language_from_code(pending_.language))
      );
      if (index >= 0) {
        language_box->setCurrentIndex(index);
      }
    }
    form->addRow(field_label(content, trs("Language")), language_box);
    config_writers_.push_back([this, language_box]() {
      pending_.language = std::string(lnb::i18n::code_from_language(
          static_cast<lnb::i18n::Language>(language_box->currentData().toInt())
      ));
    });
    config_reloaders_.push_back([this, language_box]() {
      const int index = language_box->findData(
          static_cast<int>(lnb::i18n::language_from_code(pending_.language))
      );
      if (index >= 0) {
        language_box->setCurrentIndex(index);
      }
    });

    auto* theme_box = new ComboBox(content);
    theme_box->addItems(theme_names());
    const int theme_index = theme_box->findText(QString::fromStdString(pending_.theme));
    if (theme_index >= 0) {
      theme_box->setCurrentIndex(theme_index);
    }
    form->addRow(field_label(content, trs("Theme")), theme_box);
    config_writers_.push_back([this, theme_box]() {
      pending_.theme = theme_box->currentText().toStdString();
    });
    config_reloaders_.push_back([this, theme_box]() {
      const int index = theme_box->findText(QString::fromStdString(pending_.theme));
      if (index >= 0) {
        theme_box->setCurrentIndex(index);
      }
    });

    auto* ui_font_box = new FontComboBox(content);
    if (!pending_.ui_font.empty()) {
      ui_font_box->setCurrentFont(QFont(QString::fromStdString(pending_.ui_font)));
    }
    auto* ui_font_size = new SpinBox(content);
    ui_font_size->setRange(6, 48);
    ui_font_size->setValue(pending_.ui_font_size);
    form->addRow(field_label(content, trs("Interface font")), ui_font_box);
    form->addRow(field_label(content, trs("Interface size")), ui_font_size);
    config_writers_.push_back([this, ui_font_box]() {
      pending_.ui_font = ui_font_box->currentFont().family().toStdString();
    });
    config_writers_.push_back([this, ui_font_size]() {
      pending_.ui_font_size = ui_font_size->value();
    });
    config_reloaders_.push_back([this, ui_font_box]() { set_font_box(ui_font_box, pending_.ui_font); });
    config_reloaders_.push_back([this, ui_font_size]() {
      ui_font_size->setValue(pending_.ui_font_size);
    });

    auto* font_box = new FontComboBox(content);
    if (!pending_.font.empty()) {
      font_box->setCurrentFont(QFont(QString::fromStdString(pending_.font)));
    }
    auto* font_size = new SpinBox(content);
    font_size->setRange(6, 48);
    font_size->setValue(pending_.font_size);
    form->addRow(field_label(content, trs("Chat font")), font_box);
    form->addRow(field_label(content, trs("Chat size")), font_size);
    config_writers_.push_back([this, font_box]() {
      pending_.font = font_box->currentFont().family().toStdString();
    });
    config_writers_.push_back([this, font_size]() {
      pending_.font_size = font_size->value();
    });
    config_reloaders_.push_back([this, font_box]() { set_font_box(font_box, pending_.font); });
    config_reloaders_.push_back([this, font_size]() { font_size->setValue(pending_.font_size); });

    auto* mono_box = new FontComboBox(content);
    if (!pending_.mono_font.empty()) {
      mono_box->setCurrentFont(QFont(QString::fromStdString(pending_.mono_font)));
    }
    auto* mono_size = new SpinBox(content);
    mono_size->setRange(6, 48);
    mono_size->setValue(pending_.mono_font_size);
    form->addRow(field_label(content, trs("Monospace font")), mono_box);
    form->addRow(field_label(content, trs("Monospace size")), mono_size);
    config_writers_.push_back([this, mono_box]() {
      pending_.mono_font = mono_box->currentFont().family().toStdString();
    });
    config_writers_.push_back([this, mono_size]() {
      pending_.mono_font_size = mono_size->value();
    });
    config_reloaders_.push_back([this, mono_box]() { set_font_box(mono_box, pending_.mono_font); });
    config_reloaders_.push_back([this, mono_size]() {
      mono_size->setValue(pending_.mono_font_size);
    });

    layout->addLayout(form);
  }

  layout->addWidget(h_separator(content));

  // ---- Composer ----
  layout->addWidget(section_label(content, trs("Composer")));
  {
    auto* send_check = new QCheckBox(trs("Send with Ctrl+Enter"), content);
    send_check->setToolTip(
        trs("When on, Ctrl+Enter sends and Enter inserts a new line.")
    );
    send_check->setChecked(pending_.enter_with_modifier);
    layout->addWidget(send_check);
    config_writers_.push_back([this, send_check]() {
      pending_.enter_with_modifier = send_check->isChecked();
    });
    config_reloaders_.push_back([this, send_check]() {
      send_check->setChecked(pending_.enter_with_modifier);
    });
  }

  layout->addWidget(h_separator(content));

  // ---- Transcript ----
  layout->addWidget(section_label(content, trs("Transcript")));
  {
    auto* form = form_layout(content);

    auto* rendered = new SpinBox(content);
    rendered->setObjectName(QStringLiteral("RenderedMessages"));
    rendered->setRange(1, 100000);
    rendered->setValue(pending_.rendered_message_limit);
    form->addRow(field_label(content, trs("Rendered messages")), rendered);
    config_writers_.push_back([this, rendered]() {
      pending_.rendered_message_limit = rendered->value();
    });
    config_reloaders_.push_back([this, rendered]() {
      rendered->setValue(pending_.rendered_message_limit);
    });

    auto* page_size = new SpinBox(content);
    page_size->setRange(1, 100000);
    page_size->setValue(pending_.history_page_size);
    form->addRow(field_label(content, trs("History page size")), page_size);
    config_writers_.push_back([this, page_size]() {
      pending_.history_page_size = page_size->value();
    });
    config_reloaders_.push_back([this, page_size]() {
      page_size->setValue(pending_.history_page_size);
    });

    layout->addLayout(form);
  }

  layout->addWidget(h_separator(content));

  // ---- Background ----
  layout->addWidget(section_label(content, trs("Background")));
  {
    auto* form = form_layout(content);

    auto* url = new QLineEdit(content);
    if (pending_.background_url.has_value()) {
      url->setText(QString::fromStdString(*pending_.background_url));
    }
    url->setPlaceholderText(trs("Path to a background image (optional)"));
    form->addRow(field_label(content, trs("Image")), url);
    config_writers_.push_back([this, url]() {
      const QString text = url->text().trimmed();
      pending_.background_url =
          text.isEmpty() ? std::nullopt : std::optional<std::string>(text.toStdString());
    });
    config_reloaders_.push_back([this, url]() {
      url->setText(
          pending_.background_url.has_value()
              ? QString::fromStdString(*pending_.background_url)
              : QString()
      );
    });

    layout->addLayout(form);

    auto* tint_check = new QCheckBox(trs("Tint the image"), content);
    tint_check->setChecked(pending_.background_override.has_value());
    layout->addWidget(tint_check);

    const std::array<float, 4> initial = pending_.background_override.value_or(
        std::array<float, 4>{0.094f, 0.098f, 0.149f, 0.85f}
    );

    auto* channels = new QGridLayout();
    channels->setHorizontalSpacing(12);
    channels->setVerticalSpacing(8);
    const char* names[] = {"R", "G", "B", "A"};
    std::array<QDoubleSpinBox*, 4> spins{};

    // Two channels per row: a single row of four fields is the widest content on
    // any settings page, and would push the page past its viewport on platforms
    // with wide font metrics.
    for (int i = 0; i < 4; ++i) {
      auto* spin = new DoubleSpinBox(content);
      spin->setRange(0.0, 1.0);
      spin->setSingleStep(0.05);
      spin->setDecimals(2);
      spin->setMinimumWidth(64);
      spin->setValue(static_cast<double>(initial[static_cast<std::size_t>(i)]));
      spins[static_cast<std::size_t>(i)] = spin;

      const int row = i / 2;
      const int column = i % 2;
      channels->addWidget(new QLabel(QString::fromUtf8(names[i]), content), row, column * 2);
      channels->addWidget(spin, row, column * 2 + 1);
    }
    channels->setColumnStretch(4, 1);
    layout->addLayout(channels);

    config_writers_.push_back([this, tint_check, spins]() {
      if (!tint_check->isChecked()) {
        pending_.background_override = std::nullopt;
        return;
      }

      pending_.background_override = std::array<float, 4>{
          static_cast<float>(spins[0]->value()),
          static_cast<float>(spins[1]->value()),
          static_cast<float>(spins[2]->value()),
          static_cast<float>(spins[3]->value()),
      };
    });
    config_reloaders_.push_back([this, tint_check, spins]() {
      const std::array<float, 4> values = pending_.background_override.value_or(
          std::array<float, 4>{0.094f, 0.098f, 0.149f, 0.85f}
      );
      tint_check->setChecked(pending_.background_override.has_value());

      for (int i = 0; i < 4; ++i) {
        spins[static_cast<std::size_t>(i)]->setValue(
            static_cast<double>(values[static_cast<std::size_t>(i)])
        );
      }
    });
  }

  // ---- Restore ----
  {
    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    actions->addStretch(1);

    auto* restore = new QPushButton(trs("Restore to defaults"), content);
    restore->setObjectName(QStringLiteral("RestoreConfig"));
    restore->setToolTip(
        trs("Reset this page's fields to their shipped defaults. Nothing is "
            "saved until you press Save.")
    );
    actions->addWidget(restore);
    layout->addLayout(actions);

    connect(restore, &QPushButton::clicked, this, [this]() { restore_config_defaults(); });
  }

  layout->addStretch(1);

  return make_page(
      trs("Configuration"),
      trs("Application-wide options stored in data/settings.json."), content
  );
}

QWidget* SettingsWindow::build_metrics_page() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 12, 0);
  layout->setSpacing(12);

  layout->addWidget(section_label(content, trs("Layout")));

  // One field per row, as on every other settings page: a two-column grid was the
  // widest content in the window and could push the page past its viewport on
  // platforms whose font metrics are wider than the developer's.
  auto* form = form_layout(content);

  const auto add_metric = [&](
                              const QString& label, int value, int minimum, int maximum,
                              int step, int lnb::ThemeMetrics::*field
                          ) {
    auto* spin = new SpinBox(content);
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setValue(value);
    spin->setMinimumWidth(76);
    spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addRow(field_label(content, label), spin);
    metric_writers_.push_back([this, spin, field]() { pending_metrics_.*field = spin->value(); });
    metric_reloaders_.push_back([this, spin, field]() { spin->setValue(pending_metrics_.*field); });
  };

  add_metric(
      trs("Sidebar width"), pending_metrics_.sidebar_width, 120, 800, 4,
      &lnb::ThemeMetrics::sidebar_width
  );
  add_metric(
      trs("Page margin"), pending_metrics_.page_margin, 0, 200, 1,
      &lnb::ThemeMetrics::page_margin
  );
  add_metric(
      trs("Message spacing"), pending_metrics_.message_spacing, 0, 200, 1,
      &lnb::ThemeMetrics::message_spacing
  );
  add_metric(
      trs("Composer height"), pending_metrics_.composer_height, 32, 400, 2,
      &lnb::ThemeMetrics::composer_height
  );
  add_metric(
      trs("Expanded height"), pending_metrics_.composer_height_expanded, 32, 800, 2,
      &lnb::ThemeMetrics::composer_height_expanded
  );
  add_metric(
      trs("Session item height"), pending_metrics_.session_item_height, 20, 120, 2,
      &lnb::ThemeMetrics::session_item_height
  );
  add_metric(
      trs("Title font size"), pending_metrics_.title_font_size, 6, 48, 1,
      &lnb::ThemeMetrics::title_font_size
  );
  add_metric(
      trs("Flow max width"), pending_metrics_.message_max_width, 0, 4000, 10,
      &lnb::ThemeMetrics::message_max_width
  );
  add_metric(
      trs("Bubble radius"), pending_metrics_.bubble_radius, 0, 40, 1,
      &lnb::ThemeMetrics::bubble_radius
  );

  layout->addLayout(form);
  layout->addWidget(h_separator(content));

  layout->addWidget(section_label(content, trs("Reading rhythm")));
  layout->addWidget(value_label(
      content, trs("Line heights are percentages (100 is single spacing)."), false
  ));

  auto* rhythm = form_layout(content);

  const auto add_rhythm = [&](
                              const QString& label, int value, int minimum, int maximum,
                              int step, int lnb::ThemeMetrics::*field
                          ) {
    auto* spin = new SpinBox(content);
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setValue(value);
    spin->setMinimumWidth(76);
    spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    rhythm->addRow(field_label(content, label), spin);
    metric_writers_.push_back([this, spin, field]() { pending_metrics_.*field = spin->value(); });
    metric_reloaders_.push_back([this, spin, field]() { spin->setValue(pending_metrics_.*field); });
  };

  add_rhythm(
      trs("Assistant line height"), pending_metrics_.line_height_other, 80, 300, 5,
      &lnb::ThemeMetrics::line_height_other
  );
  add_rhythm(
      trs("User line height"), pending_metrics_.line_height_self, 80, 300, 5,
      &lnb::ThemeMetrics::line_height_self
  );
  add_rhythm(
      trs("Paragraph spacing"), pending_metrics_.paragraph_spacing, 0, 80, 1,
      &lnb::ThemeMetrics::paragraph_spacing
  );
  add_rhythm(
      trs("List spacing"), pending_metrics_.list_paragraph_spacing, 0, 80, 1,
      &lnb::ThemeMetrics::list_paragraph_spacing
  );

  layout->addLayout(rhythm);

  // ---- Restore ----
  {
    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    actions->addStretch(1);

    auto* restore = new QPushButton(trs("Restore to defaults"), content);
    restore->setObjectName(QStringLiteral("RestoreMetrics"));
    restore->setToolTip(
        trs("Reset these metrics to the current theme's own defaults. Nothing "
            "is saved until you press Save.")
    );
    actions->addWidget(restore);
    layout->addLayout(actions);

    connect(restore, &QPushButton::clicked, this, [this]() { restore_metrics_defaults(); });
  }

  layout->addStretch(1);

  return make_page(
      trs("Metrics"), trs("Sizing and spacing of the interface."), content
  );
}

QWidget* SettingsWindow::build_assistant_page() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  // The form brings its own right margin, so this stays flush.
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);

  assistant_form_ = new AssistantForm(content);
  assistant_form_->set_assistant(app_.assistant());
  layout->addWidget(assistant_form_);

  // ---- Assistant memory ----
  // A sibling of the form, matching its right margin so the two sections align.
  auto* memory_block = new QWidget(content);
  auto* memory_layout = new QVBoxLayout(memory_block);
  memory_layout->setContentsMargins(0, 0, 12, 0);
  memory_layout->setSpacing(12);

  memory_layout->addWidget(h_separator(memory_block));

  auto* memory_header = new QHBoxLayout();
  memory_header->setSpacing(8);
  memory_header->addWidget(section_label(memory_block, trs("Assistant memory")));
  memory_header->addStretch(1);

  assistant_summarize_ = new QPushButton(trs("Summarize now"), memory_block);
  assistant_summarize_->setObjectName(QStringLiteral("Secondary"));
  assistant_summarize_->setToolTip(
      trs(
          "Fold the recent messages of every session into the memory below, ignoring "
          "the automatic trigger. Idempotent: pressing it again with no new messages "
          "changes nothing."
      )
  );
  memory_header->addWidget(assistant_summarize_);
  memory_layout->addLayout(memory_header);

  assistant_memory_ = new QPlainTextEdit(memory_block);
  assistant_memory_->setTabChangesFocus(true);
  assistant_memory_->setMinimumHeight(160);
  assistant_memory_->setPlaceholderText(
      trs(
          "Automatically maintained summary shared across every session. Edit to "
          "correct it, or clear it to rebuild it from the transcript."
      )
  );
  assistant_memory_->setPlainText(to_qstring(app_.assistant().memory_));
  memory_layout->addWidget(assistant_memory_);

  auto* docs = new QLabel(memory_block);
  docs->setObjectName(QStringLiteral("Muted"));
  docs->setWordWrap(true);
  docs->setTextFormat(Qt::RichText);
  QSizePolicy docs_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
  docs_policy.setHeightForWidth(true);
  docs->setSizePolicy(docs_policy);
  docs->setText(trs(
      "<b>How assistant memory works</b><br>"
      "This memory is shared across every session of the assistant. It is folded from "
      "the recent messages of all sessions (bounded by <i>Trigger tokens</i>) together "
      "with each session's own summary (bounded by <i>Session pool</i>). Older sessions "
      "may therefore be forgotten.<br><br>"
      "<b>Summarize now</b> ignores the automatic trigger and folds on demand. It is "
      "idempotent: running it again with no new messages reads the same set and produces "
      "the same memory. Custom instructions are kept separate and are never touched."
  ));
  memory_layout->addWidget(docs);

  layout->addWidget(memory_block);
  // Keep the form and memory blocks at their natural height instead of letting the
  // form's own trailing stretch absorb the extra space.
  layout->addStretch(1);

  connect(assistant_summarize_, &QPushButton::clicked, this, [this]() {
    summarize_assistant_now();
  });

  return make_page(
      trs("Assistant"),
      trs("The assistant open in this window. It is fixed for this run, "
          "but its configuration and memory can be edited here."),
      content
  );
}

QWidget* SettingsWindow::build_account_page() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 12, 0);
  layout->setSpacing(12);

  auto* session_card = make_card(content, trs("Current session"));
  {
    QVBoxLayout* body = card_body(session_card);
    account_session_name_ = value_label(session_card, QString(), true);
    account_session_id_ = value_label(session_card, QString(), false);
    body->addWidget(account_session_name_);
    body->addWidget(account_session_id_);
  }
  layout->addWidget(session_card);

  auto* startup_card = make_card(content, trs("Startup"));
  {
    QVBoxLayout* body = card_body(startup_card);

    auto* auto_login = new QCheckBox(
        trs("Open at startup"), startup_card
    );
    auto_login->setObjectName(QStringLiteral("AutoLogin"));
    auto_login->setToolTip(
        trs("Open this assistant automatically at startup. When off, the "
            "assistant picker is shown every time.")
    );
    auto_login->setChecked(pending_.auto_login);
    body->addWidget(auto_login);

    config_writers_.push_back([this, auto_login]() {
      pending_.auto_login = auto_login->isChecked();
    });
    config_reloaders_.push_back([this, auto_login]() {
      auto_login->setChecked(pending_.auto_login);
    });
  }
  layout->addWidget(startup_card);

  layout->addStretch(1);

  update_account_page();

  return make_page(
      trs("Account"),
      trs("The session you are in, and how the app starts."),
      content
  );
}

QWidget* SettingsWindow::build_about_page() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 12, 0);
  layout->setSpacing(12);

  auto* card = make_card(content, QString());
  {
    QVBoxLayout* body = card_body(card);

    auto* name = value_label(card, QStringLiteral("Local Notebook"), true);
    apply_derived_font(name, 6.0, true);
    body->addWidget(name);
    body->addSpacing(2);

    body->addWidget(value_label(
        card, trs("A local, assistant-backed notebook."), false
    ));
    body->addSpacing(10);

    body->addWidget(value_label(
        card, trs("Version %1").arg(QApplication::applicationVersion()), false
    ));
    body->addWidget(value_label(
        card, trs("Built with Qt %1").arg(QString::fromUtf8(qVersion())), false
    ));
  }
  layout->addWidget(card);

  auto* mit = make_card(content, trs("License"));
  {
    QVBoxLayout* body = card_body(mit);
    body->addWidget(value_label(
        mit, trs("Local Notebook is distributed under the MIT License."), false
    ));
    body->addSpacing(6);
    body->addWidget(license_view(mit, read_license(QStringLiteral(":/licenses/mit.txt"))));
  }
  layout->addWidget(mit);

  auto* qt = make_card(content, trs("Qt License"));
  {
    QVBoxLayout* body = card_body(qt);
    body->addWidget(value_label(
        qt,
        trs("Built with the community edition of Qt, used under the terms of the "
            "GNU Lesser General Public License, version 3."),
        false
    ));
    body->addSpacing(6);
    body->addWidget(
        license_view(qt, read_license(QStringLiteral(":/licenses/lgpl-3.0.txt")))
    );
  }
  layout->addWidget(qt);

  layout->addStretch(1);

  return make_page(trs("About"), trs("Program information."), content);
}

void SettingsWindow::update_account_page() {
  if (account_session_name_ == nullptr) {
    return;
  }

  const auto session = app_.db().session(current_session_);

  if (!session.has_value()) {
    account_session_name_->setText(trs("No session selected"));
    account_session_id_->clear();
    return;
  }

  account_session_name_->setText(
      session->title_.empty() ? trs("New Session") : to_qstring(session->title_)
  );
  account_session_id_->setText(trs("Session #%1").arg(session->id_));
}

void SettingsWindow::write_config() {
  for (const auto& writer : config_writers_) {
    writer();
  }
}

void SettingsWindow::write_metrics() {
  for (const auto& writer : metric_writers_) {
    writer();
  }
}

void SettingsWindow::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);

  // Rebuild the scoped sheet first: the theme may have changed since the last
  // visit, and the dialog's own controls would otherwise keep the colours they
  // were built with.
  apply_style();

  // A cancelled edit must not survive into the next visit, so every open starts
  // from the stored state rather than from whatever was left in the widgets.
  reload_from_app();
}

void SettingsWindow::reload_from_app() {
  pending_ = app_.settings();
  pending_metrics_ = metrics();

  for (const auto& reload : config_reloaders_) {
    reload();
  }

  for (const auto& reload : metric_reloaders_) {
    reload();
  }

  if (assistant_form_ != nullptr) {
    assistant_form_->set_assistant(app_.assistant());
  }

  if (assistant_memory_ != nullptr) {
    assistant_memory_->setPlainText(to_qstring(app_.assistant().memory_));
  }

  update_account_page();
}

void SettingsWindow::restore_config_defaults() {
  // Only the fields this page shows are reset; the settings that live elsewhere
  // (last assistant, geometry, metrics) are left untouched, so a later Save does
  // not clobber them.
  const lnb::Settings defaults;

  pending_.theme = defaults.theme;
  pending_.language = defaults.language;
  pending_.ui_font = defaults.ui_font;
  pending_.ui_font_size = defaults.ui_font_size;
  pending_.font = defaults.font;
  pending_.font_size = defaults.font_size;
  pending_.mono_font = defaults.mono_font;
  pending_.mono_font_size = defaults.mono_font_size;
  pending_.enter_with_modifier = defaults.enter_with_modifier;
  pending_.rendered_message_limit = defaults.rendered_message_limit;
  pending_.history_page_size = defaults.history_page_size;
  pending_.background_url = defaults.background_url;
  pending_.background_override = defaults.background_override;

  for (const auto& reload : config_reloaders_) {
    reload();
  }
}

void SettingsWindow::restore_metrics_defaults() {
  // The theme's own defaults, before any user override: the same source
  // apply_theme() falls back to when no override is stored.
  pending_metrics_ = theme().metrics;

  for (const auto& reload : metric_reloaders_) {
    reload();
  }
}

void SettingsWindow::save() {
  // Refuse to save an unusable API endpoint: an invalid base would make every
  // request fail, so the window stays open for correction instead of persisting
  // it.
  if (assistant_form_ != nullptr) {
    QString url_error;
    if (!assistant_form_->validate_base_url(url_error)) {
      QMessageBox::warning(this, trs("Invalid API URL"), url_error);
      return;
    }
  }

  const lnb::Settings previous = app_.settings();

  write_config();
  write_metrics();

  pending_.metrics = pending_metrics_;

  app_.settings() = pending_;
  app_.save_settings();

  // A language change cannot reach the already-built windows (their text is set
  // once, at construction), so it is applied for this run and the user is offered
  // a restart to see the rest of the interface follow.
  const bool language_changed = previous.language != app_.settings().language;
  if (language_changed) {
    lnb::i18n::set_language(lnb::i18n::language_from_code(app_.settings().language));
  }

  // The assistant's own configuration is persisted to its directory, not the
  // global settings file.
  if (assistant_form_ != nullptr) {
    assistant_form_->write_into(app_.assistant());
    if (assistant_memory_ != nullptr) {
      app_.assistant().memory_ = assistant_memory_->toPlainText().toStdString();
    }
    app_.save_assistant();
  }

  // Re-apply the theme at runtime so the change is visible immediately, but only
  // when something the theme depends on actually changed. A full restyle rebuilds
  // the application stylesheet and re-decodes the background image, which is far
  // too expensive to run for a save that touched none of it.
  if (!theme_inputs_equal(previous, app_.settings())) {
    if (auto* application = qobject_cast<QApplication*>(QApplication::instance())) {
      ui::apply_theme(*application, app_.settings());
    }
  }

  if (on_saved_) {
    on_saved_();
  }

  accept();

  if (language_changed) {
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, trs("Language changed"),
        trs("The interface language will change after Local Notebook restarts. "
            "Restart now?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes
    );

    if (choice == QMessageBox::Yes) {
      QProcess::startDetached(
          QCoreApplication::applicationFilePath(),
          QCoreApplication::arguments().mid(1)
      );
      QCoreApplication::quit();
    }
  }
}

void SettingsWindow::summarize_assistant_now() {
  if (assistant_summarizing_ || !app_.has_assistant() || assistant_memory_ == nullptr) {
    return;
  }

  // The shared assistant fold occupies one slot; a fold already in flight is left
  // to finish (it can be watched — and cancelled — in the tasks window).
  if (app_.tasks().assistant_busy()) {
    return;
  }

  assistant_summarizing_ = true;
  assistant_summarize_->setEnabled(false);
  assistant_summarize_->setText(trs("Summarizing..."));

  lnb::store::database* db = &app_.db();
  const lnb::Assistant assistant = app_.assistant();

  lnb::net::Remote remote;
  remote.url = lnb::net::derive_endpoint(assistant.api_url_, assistant.protocol_);
  remote.key = assistant.authentication_token_;
  remote.proxy = assistant.proxy_;
  remote.model = assistant.model_;
  remote.protocol = assistant.protocol_;

  // The review runs on the worker thread and blocks on the interface thread, so
  // a decline is recorded here and read after the task is reclaimed.
  auto produced = std::make_shared<std::string>();
  auto declined = std::make_shared<bool>(false);

  const lnb::net::SummaryApprover approver =
      [declined](const lnb::net::SummaryPlan& plan) {
        const bool approved = review_summary(plan);
        if (!approved) {
          *declined = true;
        }
        return approved;
      };

  // The fold runs as the shared assistant task, so it appears in the tasks window
  // and can be cancelled there like any other job.
  auto task = app_.tasks().submit(
      lnb::TaskKind::AssistantSummary, 0,
      [produced, declined, db, assistant, remote, approver](
          lnb::GenerationTask& state, std::stop_token stop_token
      ) {
        *produced = lnb::net::summarize_assistant_now(
            *db, assistant, remote, state, stop_token, approver
        );
      }
  );

  if (task == nullptr) {
    assistant_summarizing_ = false;
    assistant_summarize_->setEnabled(true);
    assistant_summarize_->setText(trs("Summarize now"));
    return;
  }

  // Keep the modal window responsive while the worker folds every session: the
  // nested loop only pumps events until the worker has exited. The main window's
  // poll timer keeps firing inside it, so the tasks window updates live.
  QEventLoop loop;
  QTimer timer;

  QObject::connect(&timer, &QTimer::timeout, &loop, [&task, &loop]() {
    if (task->finished()) {
      loop.quit();
    }
  });

  timer.start(100);
  loop.exec();
  timer.stop();

  // Reclaim the worker before its captures (produced/declined) are read.
  task->reap();

  assistant_summarizing_ = false;
  assistant_summarize_->setEnabled(true);
  assistant_summarize_->setText(trs("Summarize now"));

  // A declined review is a deliberate no-op: say nothing about it.
  if (*declined) {
    return;
  }

  const lnb::GenerationTask::Snapshot snapshot = task->snapshot();

  // A cancellation was deliberate (from the tasks window): say nothing.
  if (snapshot.status == lnb::GenerationTask::Status::CANCELLED) {
    return;
  }

  if (snapshot.status == lnb::GenerationTask::Status::FAILED) {
    QMessageBox::warning(
        this, trs("Summarization Failed"), to_qstring(snapshot.error)
    );
    return;
  }

  const QString previous = assistant_memory_->toPlainText();
  const QString memory = to_qstring(*produced);

  assistant_memory_->setPlainText(memory);
  app_.assistant().memory_ = *produced;
  app_.save_assistant();
  // The fold advanced every session's assistant watermark, so the cached trigger
  // (which the composer's summarize mode reads) must be recomputed.
  app_.mark_assistant_summary_stale();

  if (memory.isEmpty() || memory == previous) {
    QMessageBox::information(
        this, trs("Nothing to Summarize"),
        trs("There is no new conversation to fold into memory yet.")
    );
  }
}

} // namespace ui
