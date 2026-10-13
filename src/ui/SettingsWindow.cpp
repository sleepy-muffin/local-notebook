#include "SettingsWindow.hpp"

#include "theme.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <sqlite3.h>

#include <optional>
#include <string>

namespace ui {

namespace {

constexpr char kAppVersion[] = "0.1.0";

// A form laid out inside a titled group box.
QFormLayout* group_form(QGroupBox* box) {
  auto* form = new QFormLayout(box);
  form->setContentsMargins(0, 8, 0, 0);
  form->setSpacing(8);
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  return form;
}

QLineEdit* line_edit(QWidget* parent, const std::string& value) {
  return new QLineEdit(QString::fromStdString(value), parent);
}

QSpinBox* spin(QWidget* parent, int value, int low, int high) {
  auto* box = new QSpinBox(parent);
  box->setRange(low, high);
  box->setValue(value);
  return box;
}

// Shows only the first and last four characters of a token.
QString mask_token(const std::string& token) {
  if (token.empty()) {
    return QStringLiteral("Not set");
  }

  if (token.size() <= 8) {
    return QStringLiteral("••••••");
  }

  return QString::fromStdString(token.substr(0, 4)) + QStringLiteral("…") +
      QString::fromStdString(token.substr(token.size() - 4));
}

QString display(const std::string& value, const QString& fallback = QStringLiteral("—")) {
  return value.empty() ? fallback : QString::fromStdString(value);
}

QString display(
    const std::optional<std::string>& value,
    const QString& fallback = QStringLiteral("—")
) {
  return (value.has_value() && !value->empty()) ? QString::fromStdString(*value) : fallback;
}

} // namespace

SettingsWindow::SettingsWindow(lnb::ApplicationState& state, QWidget* parent)
    : QDialog(parent), state_(state) {
  setObjectName(QStringLiteral("SettingsWindow"));
  setWindowTitle(QStringLiteral("Settings"));
  setModal(false);
  resize(880, 600);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto* body = new QHBoxLayout();
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);

  nav_ = new QListWidget(this);
  nav_->setObjectName(QStringLiteral("SettingsNav"));
  nav_->setFixedWidth(200);
  nav_->setFocusPolicy(Qt::NoFocus);
  nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_->addItem(QStringLiteral("General"));
  nav_->addItem(QStringLiteral("Metrics"));
  nav_->addItem(QStringLiteral("Account"));
  nav_->addItem(QStringLiteral("About"));

  stack_ = new QStackedWidget(this);
  stack_->addWidget(make_page(
      QStringLiteral("Configuration"),
      QStringLiteral("Program and generation settings. Changes apply for this session."),
      build_config_page()
  ));
  stack_->addWidget(make_page(
      QStringLiteral("Appearance"),
      QStringLiteral("Theme metrics for spacing, sizing and chat text."),
      build_metrics_page()
  ));
  stack_->addWidget(make_page(
      QStringLiteral("Account"),
      QStringLiteral("The session you are currently signed in as."),
      build_account_page()
  ));
  stack_->addWidget(make_page(
      QStringLiteral("About"),
      QStringLiteral("Version and links."),
      build_about_page()
  ));

  body->addWidget(nav_);
  body->addWidget(stack_, 1);
  root->addLayout(body, 1);

  auto* footer = new QHBoxLayout();
  footer->setContentsMargins(16, 10, 16, 14);
  footer->setSpacing(8);
  footer->addStretch(1);

  auto* save_button = new QPushButton(QStringLiteral("Save"), this);
  auto* close_button = new QPushButton(QStringLiteral("Close"), this);
  close_button->setObjectName(QStringLiteral("Secondary"));
  footer->addWidget(save_button);
  footer->addWidget(close_button);

  auto* divider = new QFrame(this);
  divider->setObjectName(QStringLiteral("SettingsDivider"));
  divider->setFixedHeight(1);
  divider->setAttribute(Qt::WA_StyledBackground, true);
  root->addWidget(divider);
  root->addLayout(footer);

  connect(
      nav_,
      &QListWidget::currentRowChanged,
      this,
      [this](int row) {
        if (row >= 0) {
          stack_->setCurrentIndex(row);
        }
      }
  );

  connect(save_button, &QPushButton::clicked, this, [this]() { save(); });
  connect(close_button, &QPushButton::clicked, this, [this]() { close(); });

  nav_->setCurrentRow(0);
}

QWidget* SettingsWindow::make_page(
    const QString& title,
    const QString& subtitle,
    QWidget* content
) {
  auto* scroll = new QScrollArea(this);
  scroll->setObjectName(QStringLiteral("SettingsPage"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);

  auto* inner = new QWidget(scroll);
  inner->setObjectName(QStringLiteral("SettingsPageInner"));

  auto* layout = new QVBoxLayout(inner);
  layout->setContentsMargins(26, 22, 26, 26);
  layout->setSpacing(6);

  auto* title_label = new QLabel(title, inner);
  title_label->setObjectName(QStringLiteral("SettingsPageTitle"));
  auto* subtitle_label = new QLabel(subtitle, inner);
  subtitle_label->setObjectName(QStringLiteral("SettingsPageSubtitle"));
  subtitle_label->setWordWrap(true);

  layout->addWidget(title_label);
  layout->addWidget(subtitle_label);
  layout->addSpacing(8);
  layout->addWidget(content);
  layout->addStretch(1);

  scroll->setWidget(inner);
  return scroll;
}

QWidget* SettingsWindow::build_config_page() {
  lnb::general* cfg = state_.uni_config_.get();

  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(14);

  // ---- General ----
  auto* general = new QGroupBox(QStringLiteral("General"), content);
  auto* general_form = group_form(general);

  auto* theme_combo = new QComboBox(general);
  const QStringList names = ui::theme_names();
  theme_combo->addItems(names);

  const int theme_index = names.indexOf(QString::fromStdString(cfg->theme_));
  if (theme_index >= 0) {
    theme_combo->setCurrentIndex(theme_index);
  }

  general_form->addRow(QStringLiteral("Theme"), theme_combo);
  config_writers_.push_back([cfg, theme_combo]() {
    cfg->theme_ = theme_combo->currentText().toStdString();
  });
  connect(
      theme_combo,
      &QComboBox::currentTextChanged,
      this,
      [this](const QString&) { write_config(); }
  );

  auto* background = line_edit(general, cfg->background_url_.value_or(std::string{}));
  background->setPlaceholderText(QStringLiteral("Path to a background image (optional)"));
  general_form->addRow(QStringLiteral("Background image"), background);
  config_writers_.push_back([cfg, background]() {
    const QString value = background->text();

    if (value.isEmpty()) {
      cfg->background_url_ = std::nullopt;
    } else {
      cfg->background_url_ = value.toStdString();
    }
  });
  connect(
      background,
      &QLineEdit::textChanged,
      this,
      [this](const QString&) { write_config(); }
  );

  auto* enter_modifier = new QCheckBox(QStringLiteral("Require Ctrl to send"), general);
  enter_modifier->setChecked(cfg->enter_with_modifier_);
  general_form->addRow(QString(), enter_modifier);
  config_writers_.push_back([cfg, enter_modifier]() {
    cfg->enter_with_modifier_ = enter_modifier->isChecked();
  });
  connect(
      enter_modifier,
      &QCheckBox::toggled,
      this,
      [this](bool) { write_config(); }
  );

  layout->addWidget(general);

  // ---- Fonts ----
  auto* fonts = new QGroupBox(QStringLiteral("Fonts"), content);
  auto* fonts_form = group_form(fonts);

  auto add_font_family = [&](const QString& label, std::string lnb::general::*member) {
    auto* edit = line_edit(fonts, cfg->*member);
    edit->setPlaceholderText(QStringLiteral("System default"));
    fonts_form->addRow(label, edit);
    config_writers_.push_back([cfg, edit, member]() {
      cfg->*member = edit->text().toStdString();
    });
    connect(edit, &QLineEdit::textChanged, this, [this](const QString&) { write_config(); });
  };

  auto add_font_size = [&](const QString& label, int lnb::general::*member) {
    auto* box = spin(fonts, cfg->*member, 0, 72);
    box->setSpecialValueText(QStringLiteral("Auto"));
    fonts_form->addRow(label, box);
    config_writers_.push_back([cfg, box, member]() { cfg->*member = box->value(); });
    connect(box, &QSpinBox::valueChanged, this, [this](int) { write_config(); });
  };

  add_font_family(QStringLiteral("Chat family"), &lnb::general::font_);
  add_font_size(QStringLiteral("Chat size"), &lnb::general::font_size_);
  add_font_family(QStringLiteral("Interface family"), &lnb::general::ui_font_);
  add_font_size(QStringLiteral("Interface size"), &lnb::general::ui_font_size_);
  add_font_family(QStringLiteral("Monospace family"), &lnb::general::mono_font_);
  add_font_size(QStringLiteral("Monospace size"), &lnb::general::mono_font_size_);

  layout->addWidget(fonts);

  // ---- Transcript rendering ----
  auto* transcript = new QGroupBox(QStringLiteral("Transcript rendering"), content);
  auto* transcript_form = group_form(transcript);

  auto add_window = [&](const QString& label, int lnb::general::*member) {
    auto* box = spin(transcript, cfg->*member, 1, 1000000);
    box->setSingleStep(10);
    transcript_form->addRow(label, box);
    config_writers_.push_back([cfg, box, member]() { cfg->*member = box->value(); });
    connect(box, &QSpinBox::valueChanged, this, [this](int) { write_config(); });
  };

  add_window(QStringLiteral("Rendered messages"), &lnb::general::rendered_message_limit_);
  add_window(QStringLiteral("Load page size"), &lnb::general::history_page_size_);

  layout->addWidget(transcript);

  return content;
}

QWidget* SettingsWindow::build_metrics_page() {
  ThemeMetrics* metrics = &ui::mutable_metrics();

  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(14);

  auto add_metric = [&](QFormLayout* form, QWidget* parent, const QString& label,
                        int ThemeMetrics::*member, int low, int high) {
    auto* box = spin(parent, metrics->*member, low, high);
    form->addRow(label, box);
    metric_writers_.push_back([metrics, box, member]() { metrics->*member = box->value(); });
    connect(box, &QSpinBox::valueChanged, this, [this](int) { write_metrics(); });
  };

  auto* layout_group = new QGroupBox(QStringLiteral("Layout"), content);
  auto* layout_form = group_form(layout_group);
  add_metric(layout_form, layout_group, QStringLiteral("Sidebar width"),
             &ThemeMetrics::sidebar_width, 160, 480);
  add_metric(layout_form, layout_group, QStringLiteral("Bubble radius"),
             &ThemeMetrics::bubble_radius, 0, 40);
  add_metric(layout_form, layout_group, QStringLiteral("Message max width"),
             &ThemeMetrics::message_max_width, 0, 2000);
  add_metric(layout_form, layout_group, QStringLiteral("Message spacing"),
             &ThemeMetrics::message_spacing, 0, 80);
  add_metric(layout_form, layout_group, QStringLiteral("Page margin"),
             &ThemeMetrics::page_margin, 0, 80);
  add_metric(layout_form, layout_group, QStringLiteral("Composer height"),
             &ThemeMetrics::composer_height, 40, 240);
  add_metric(layout_form, layout_group, QStringLiteral("Session item height"),
             &ThemeMetrics::session_item_height, 24, 96);
  add_metric(layout_form, layout_group, QStringLiteral("Title font size"),
             &ThemeMetrics::title_font_size, 8, 40);
  layout->addWidget(layout_group);

  auto* text_group = new QGroupBox(QStringLiteral("Chat text"), content);
  auto* text_form = group_form(text_group);
  add_metric(text_form, text_group, QStringLiteral("Line height (assistant)"),
             &ThemeMetrics::line_height_other, 100, 300);
  add_metric(text_form, text_group, QStringLiteral("Line height (user)"),
             &ThemeMetrics::line_height_self, 100, 300);
  add_metric(text_form, text_group, QStringLiteral("Paragraph spacing"),
             &ThemeMetrics::paragraph_spacing, 0, 60);
  add_metric(text_form, text_group, QStringLiteral("List paragraph spacing"),
             &ThemeMetrics::list_paragraph_spacing, 0, 60);
  layout->addWidget(text_group);

  return content;
}

QWidget* SettingsWindow::build_account_page() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(14);

  std::optional<lnb::session> current;

  try {
    current = state_.uni_db_->get_session_by_id(state_.self_id_);
  } catch (const std::exception&) {
    current = std::nullopt;
  }

  auto* card = new QFrame(content);
  card->setObjectName(QStringLiteral("SettingsCard"));
  auto* grid = new QGridLayout(card);
  grid->setContentsMargins(18, 16, 18, 16);
  grid->setHorizontalSpacing(28);
  grid->setVerticalSpacing(10);
  grid->setColumnStretch(1, 1);

  int row = 0;

  auto add_row = [&](const QString& key, const QString& value) {
    auto* key_label = new QLabel(key, card);
    key_label->setObjectName(QStringLiteral("SettingsKey"));
    auto* value_label = new QLabel(value, card);
    value_label->setObjectName(QStringLiteral("SettingsValue"));
    value_label->setWordWrap(true);
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(key_label, row, 0, Qt::AlignTop);
    grid->addWidget(value_label, row, 1);
    ++row;
  };

  if (current.has_value()) {
    const lnb::session& session = *current;
    add_row(QStringLiteral("Display name"), display(session.title_));
    add_row(QStringLiteral("Session ID"), QString::number(session.id_));
    add_row(QStringLiteral("Model"), display(session.model_));
    add_row(QStringLiteral("API endpoint"), display(session.api_url_));
    add_row(QStringLiteral("Shared directory"), display(session.shared_directory_));
    add_row(QStringLiteral("Proxy"), display(session.proxy_));
    add_row(QStringLiteral("Authentication"), mask_token(session.authentication_token_));
    add_row(
        QStringLiteral("Custom instructions"),
        session.custom_instructions_.empty()
            ? QStringLiteral("None")
            : QStringLiteral("%1 characters").arg(
                  static_cast<qulonglong>(session.custom_instructions_.size()))
    );
    add_row(
        QStringLiteral("Memory summary"),
        session.memory_summary_.empty()
            ? QStringLiteral("None")
            : QStringLiteral("%1 characters").arg(
                  static_cast<qulonglong>(session.memory_summary_.size()))
    );
  } else {
    add_row(QStringLiteral("Status"), QStringLiteral("No session information available."));
  }

  layout->addWidget(card);
  return content;
}

QWidget* SettingsWindow::build_about_page() {
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(14);

  // ---- Header ----
  auto* header = new QFrame(content);
  header->setObjectName(QStringLiteral("SettingsCard"));
  auto* header_layout = new QVBoxLayout(header);
  header_layout->setContentsMargins(22, 20, 22, 20);
  header_layout->setSpacing(4);

  auto* name = new QLabel(QStringLiteral("Local Notebook"), header);
  name->setObjectName(QStringLiteral("AboutName"));
  auto* version = new QLabel(QStringLiteral("Version %1").arg(kAppVersion), header);
  version->setObjectName(QStringLiteral("AboutVersion"));
  auto* tagline = new QLabel(
      QStringLiteral("A local-first notebook for chatting with your own models."), header
  );
  tagline->setObjectName(QStringLiteral("AboutTagline"));
  tagline->setWordWrap(true);

  header_layout->addWidget(name);
  header_layout->addWidget(version);
  header_layout->addWidget(tagline);
  layout->addWidget(header);

  // ---- Links ----
  auto* links = new QFrame(content);
  links->setObjectName(QStringLiteral("SettingsCard"));
  auto* links_layout = new QVBoxLayout(links);
  links_layout->setContentsMargins(22, 18, 22, 18);
  links_layout->setSpacing(8);

  auto* links_title = new QLabel(QStringLiteral("Links"), links);
  links_title->setObjectName(QStringLiteral("SettingsCardTitle"));
  links_layout->addWidget(links_title);

  auto add_link = [&](const QString& label, const QString& url) {
    auto* link = new QLabel(
        QStringLiteral("<a href=\"%1\">%2</a>").arg(url, label), links
    );
    link->setObjectName(QStringLiteral("SettingsLink"));
    link->setTextFormat(Qt::RichText);
    link->setOpenExternalLinks(true);
    links_layout->addWidget(link);
  };

  add_link(QStringLiteral("Qt framework"), QStringLiteral("https://www.qt.io"));
  add_link(QStringLiteral("SQLite"), QStringLiteral("https://sqlite.org"));
  add_link(QStringLiteral("Catppuccin palette"), QStringLiteral("https://catppuccin.com"));
  layout->addWidget(links);

  // ---- Build info ----
  auto* build = new QFrame(content);
  build->setObjectName(QStringLiteral("SettingsCard"));
  auto* build_grid = new QGridLayout(build);
  build_grid->setContentsMargins(22, 18, 22, 18);
  build_grid->setHorizontalSpacing(28);
  build_grid->setVerticalSpacing(10);
  build_grid->setColumnStretch(1, 1);

  int row = 0;

  auto add_info = [&](const QString& key, const QString& value) {
    auto* key_label = new QLabel(key, build);
    key_label->setObjectName(QStringLiteral("SettingsKey"));
    auto* value_label = new QLabel(value, build);
    value_label->setObjectName(QStringLiteral("SettingsValue"));
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    build_grid->addWidget(key_label, row, 0);
    build_grid->addWidget(value_label, row, 1);
    ++row;
  };

  add_info(QStringLiteral("Application version"), QString::fromUtf8(kAppVersion));
  add_info(QStringLiteral("Qt version"), QString::fromUtf8(qVersion()));
  add_info(QStringLiteral("SQLite version"), QString::fromUtf8(sqlite3_libversion()));
  add_info(QStringLiteral("Active theme"), ui::theme().name);
  layout->addWidget(build);

  return content;
}

void SettingsWindow::write_config() {
  for (auto& writer: config_writers_) {
    writer();
  }
}

void SettingsWindow::write_metrics() {
  for (auto& writer: metric_writers_) {
    writer();
  }
}

void SettingsWindow::save() {
  // Push every edited widget value back into the in-memory configuration.
  write_config();
  write_metrics();

  auto* app = qobject_cast<QApplication*>(QApplication::instance());

  if (app == nullptr || state_.uni_config_ == nullptr) {
    close();
    return;
  }

  // Capture the edited metrics before apply_theme() reloads them from
  // theme.json, so the override survives the reload and is persisted below.
  state_.uni_config_->metrics_ = ui::theme().metrics;

  // Persist now: the file write is cheap and must not be lost even if the
  // deferred restyle below never runs (for example, the app is closing).
  state_.uni_config_->save();

  // Dismiss the dialog before re-styling. Re-applying the theme re-polishes
  // every widget in the application, which is comparatively expensive, so the
  // restyle is deferred to the next event-loop turn: the window disappears and
  // the click feels immediate instead of freezing mid-press.
  close();

  lnb::ApplicationState* state = &state_;

  QTimer::singleShot(0, app, [app, state]() {
    if (state->uni_config_ != nullptr) {
      ui::apply_theme(*app, *state->uni_config_);
    }
  });
}

} // namespace ui
