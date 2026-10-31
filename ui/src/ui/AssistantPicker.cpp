#include "ui/AssistantPicker.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <string>

#include "local_notebook/core/interface.hpp"
#include "local_notebook/core/model.hpp"
#include "shark/log.hpp"
#include "settings/settings.hpp"
#include "support/i18n.hpp"
#include "ui/AssistantForm.hpp"
#include "ui/BackgroundWidget.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/theme.hpp"

namespace ui {
namespace {

// Item roles carried by each assistant row.
constexpr int k_uuid_role = Qt::UserRole;
constexpr int k_model_role = Qt::UserRole + 1;

QString to_qstring(const std::string& value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString colour_hex(const QColor& colour) {
  return colour.name(QColor::HexRgb);
}

// The Zed-like chrome shared by both dialogs. It is scoped by object name so it
// never leaks into the rest of the application.
QString dialog_stylesheet(const QString& object_name) {
  const ThemeColors& c = theme().colors;

  const QString window = colour_hex(c.window);
  const QString surface = colour_hex(c.surface);
  const QString base = colour_hex(c.base);
  const QString text = colour_hex(c.text);
  const QString muted = colour_hex(c.text_muted);
  const QString border = colour_hex(c.border);
  const QString accent = colour_hex(c.accent);
  const QString accent_hover = colour_hex(c.accent_hover);
  const QString accent_active = colour_hex(c.accent_active);
  const QString accent_text = colour_hex(c.accent_text);

  QString sheet;

  sheet += QStringLiteral("QDialog#%1 { background: %2; }").arg(object_name, window);
  sheet += QStringLiteral("QLabel { background: transparent; }");
  sheet += QStringLiteral("QLabel#PickerTitle, QLabel#FormTitle, QLabel#PreviewName { color: %1; }")
               .arg(text);
  sheet += QStringLiteral(
               "QLabel#PickerSubtitle, QLabel#PreviewModel, QLabel#PreviewHint,"
               " QLabel#FieldLabel, QLabel#SectionLabel { color: %1; }")
               .arg(muted);

  sheet += QStringLiteral("QWidget#Separator { background: %1; }").arg(border);

  sheet += QStringLiteral(
               "QListWidget#AssistantList { border: none; outline: none; }");
  sheet += QStringLiteral(
               "QScrollArea { border: none; }"
               "QScrollArea > QWidget > QWidget { background: transparent; }");

  sheet += QStringLiteral(
               "QLineEdit, QPlainTextEdit, QSpinBox {"
               "  background: %1; border: 1px solid %2; border-radius: 6px;"
               "  padding: 5px 8px; selection-background-color: %3; selection-color: %4; }")
               .arg(base, border, accent, accent_text);
  sheet += QStringLiteral(
               "QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus {"
               "  border: 1px solid %1; }")
               .arg(accent);

  sheet += QStringLiteral(
               "QPushButton#Primary { background: %1; color: %2; border: none;"
               "  border-radius: 6px; padding: 6px 18px; }")
               .arg(accent, accent_text);
  sheet += QStringLiteral("QPushButton#Primary:hover { background: %1; }").arg(accent_hover);
  sheet += QStringLiteral("QPushButton#Primary:pressed { background: %1; }").arg(accent_active);
  sheet += QStringLiteral("QPushButton#Primary:disabled { background: %1; color: %2; }")
               .arg(surface, muted);

  sheet += QStringLiteral(
               "QPushButton#Secondary { background: transparent; color: %1;"
               "  border: 1px solid %2; border-radius: 6px; padding: 6px 14px; }")
               .arg(text, border);
  sheet += QStringLiteral("QPushButton#Secondary:hover { background: %1; }").arg(surface);
  sheet += QStringLiteral("QPushButton#Secondary:disabled { color: %1; }").arg(muted);

  return sheet;
}

QFont scaled_font(double delta, bool bold) {
  QFont font = ui_font();
  if (font.pointSizeF() > 1.0) {
    font.setPointSizeF(font.pointSizeF() + delta);
  }
  if (bold) {
    font.setWeight(QFont::DemiBold);
  }
  return font;
}

// A hairline drawn as a styled QWidget, so it is exactly one pixel and uses the
// theme's border colour.
QWidget* h_separator(QWidget* parent) {
  auto* line = new QWidget(parent);
  line->setObjectName(QStringLiteral("Separator"));
  line->setAttribute(Qt::WA_StyledBackground, true);
  line->setFixedHeight(1);
  line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  return line;
}

// Paints one assistant row: the name in the foreground colour and the model,
// muted, beneath it. Hover and selection use the shared low-contrast row states
// so this list reads exactly like the main window's session list.
class AssistantItemDelegate : public QStyledItemDelegate {
public:
  explicit AssistantItemDelegate(QObject* parent = nullptr)
      : QStyledItemDelegate(parent) {}

  void paint(
      QPainter* painter,
      const QStyleOptionViewItem& option,
      const QModelIndex& index
  ) const override {
    painter->save();

    const ThemeColors& colours = theme().colors;
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    const QRect item = option.rect.adjusted(4, 3, -4, -3);

    QColor background = Qt::transparent;
    if (selected) {
      background = row_selected_background();
    } else if (hovered) {
      background = row_hover_background();
    }

    if (background.alpha() > 0) {
      painter->setPen(Qt::NoPen);
      painter->setBrush(background);
      painter->drawRoundedRect(item, 8, 8);
    }

    const QString name = index.data(Qt::DisplayRole).toString();
    const QString model = index.data(k_model_role).toString();

    const QRect content = item.adjusted(12, 6, -12, -6);

    const QFont name_font = scaled_font(0.0, true);
    const QFontMetrics name_metrics(name_font);
    const int name_height = name_metrics.height();

    painter->setFont(name_font);
    painter->setPen(colours.text);
    const QRect name_rect(content.left(), content.top(), content.width(), name_height);
    painter->drawText(
        name_rect,
        Qt::AlignLeft | Qt::AlignVCenter,
        name_metrics.elidedText(name, Qt::ElideRight, name_rect.width())
    );

    if (!model.isEmpty()) {
      const QFont model_font = scaled_font(-1.0, false);
      const QFontMetrics model_metrics(model_font);
      const QRect model_rect(
          content.left(),
          name_rect.bottom() + 1,
          content.width(),
          content.bottom() - name_rect.bottom()
      );
      painter->setFont(model_font);
      painter->setPen(colours.text_muted);
      painter->drawText(
          model_rect,
          Qt::AlignLeft | Qt::AlignVCenter,
          model_metrics.elidedText(model, Qt::ElideRight, model_rect.width())
      );
    }

    painter->restore();
  }

  QSize sizeHint(
      const QStyleOptionViewItem& option,
      const QModelIndex& index
  ) const override {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(metrics().session_item_height + 12, 48));
    size.setWidth(std::max(size.width(), 220));
    return size;
  }
};

// The shared editor for creating and editing an assistant. It returns a fully
// populated lnb::Assistant; the caller decides which UUID to persist it under.
class AssistantFormDialog : public QDialog {
public:
  AssistantFormDialog(
      const QString& title,
      const lnb::Assistant& initial,
      QWidget* parent = nullptr
  )
      : QDialog(parent), initial_(initial) {
    setObjectName(QStringLiteral("AssistantForm"));
    setWindowTitle(title);
    setModal(true);
    resize(600, 660);
    setMinimumSize(600, 520);
    setFont(ui_font());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* background = new BackgroundWidget(this);
    root->addWidget(background);

    auto* outer = new QVBoxLayout(background);
    outer->setContentsMargins(24, 20, 24, 16);
    outer->setSpacing(0);

    auto* heading = new QLabel(title, background);
    heading->setObjectName(QStringLiteral("FormTitle"));
    heading->setFont(scaled_font(4.0, true));
    outer->addWidget(heading);
    outer->addSpacing(16);

    auto* scroll = new QScrollArea(background);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    install_scroll_bars(scroll, ScrollBarTrack::Window);

    form_ = new AssistantForm(scroll);
    form_->set_assistant(initial_);
    scroll->setWidget(form_);

    outer->addWidget(scroll, 1);
    outer->addSpacing(12);
    outer->addWidget(h_separator(background));
    outer->addSpacing(12);

    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    footer->addStretch(1);

    auto* cancel = new QPushButton(trs("Cancel"), background);
    cancel->setObjectName(QStringLiteral("Secondary"));

    auto* save = new QPushButton(trs("Save"), background);
    save->setObjectName(QStringLiteral("Primary"));
    save->setDefault(true);

    footer->addWidget(cancel);
    footer->addWidget(save);
    outer->addLayout(footer);

    setStyleSheet(dialog_stylesheet(QStringLiteral("AssistantForm")));

    connect(cancel, &QPushButton::clicked, this, [this]() { reject(); });
    connect(save, &QPushButton::clicked, this, [this]() { accept(); });
  }

  // The edited assistant. Valid once exec() returned QDialog::Accepted.
  [[nodiscard]] lnb::Assistant assistant() const { return result_; }

  void accept() override {
    // An unusable endpoint is refused here, before anything is written: the base
    // must be an http(s) URL ending in /v1.
    QString error;
    if (!form_->validate_base_url(error)) {
      QMessageBox::warning(this, trs("Invalid API URL"), error);
      return;
    }

    // Start from the original so every field the form does not own — memory_, in
    // particular — is carried through untouched.
    lnb::Assistant value = initial_;
    form_->write_into(value);
    result_ = value;
    QDialog::accept();
  }

private:
  lnb::Assistant initial_;
  lnb::Assistant result_;
  AssistantForm* form_ = nullptr;
};

} // namespace

AssistantPicker::AssistantPicker(
    lnb::api::interface& api, lnb::Settings& settings, QWidget* parent
)
    : QDialog(parent), api_(api), settings_(settings) {
  setObjectName(QStringLiteral("AssistantPicker"));
  setWindowTitle(trs("Assistants"));
  setModal(true);
  resize(400, 520);
  setMinimumSize(300, 420);
  setFont(ui_font());

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto* background = new BackgroundWidget(this);
  root->addWidget(background);

  auto* outer = new QVBoxLayout(background);
  outer->setContentsMargins(28, 24, 28, 20);
  outer->setSpacing(0);

  // ---- Header ----
  auto* title = new QLabel(trs("Assistants"), background);
  title->setObjectName(QStringLiteral("PickerTitle"));
  title->setFont(scaled_font(4.0, true));

  auto* subtitle = new QLabel(
      trs("Choose an assistant to open, or create a new one."),
      background
  );
  subtitle->setObjectName(QStringLiteral("PickerSubtitle"));

  outer->addWidget(title);
  outer->addWidget(subtitle);
  outer->addSpacing(16);

  // ---- Body: the assistant list, full width ----
  list_ = new QListWidget(background);
  list_->setObjectName(QStringLiteral("AssistantList"));
  // The list shows the dialog behind it; the rule lives on the list so a style
  // that paints from the palette cannot fill the scrollbar column black.
  show_parent_background(list_);
  list_->setItemDelegate(new AssistantItemDelegate(list_));
  list_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  list_->setFrameShape(QFrame::NoFrame);
  list_->setUniformItemSizes(true);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  install_scroll_bars(list_, ScrollBarTrack::Window);
  list_->setSelectionRectVisible(false);
  list_->setMouseTracking(true);
  list_->viewport()->setMouseTracking(true);

  outer->addWidget(list_, 1);
  outer->addSpacing(16);

  // ---- Footer ----
  outer->addWidget(h_separator(background));
  outer->addSpacing(12);

  // The startup checkbox sits on its own line above the button row.
  auto_login_ = new QCheckBox(trs("Open automatically at startup"), background);
  auto_login_->setChecked(settings_.auto_login);
  outer->addWidget(auto_login_);
  outer->addSpacing(8);

  auto* footer = new QHBoxLayout();
  footer->setSpacing(8);

  auto* new_button = new QPushButton(trs("New assistant"), background);
  new_button->setObjectName(QStringLiteral("Secondary"));

  edit_button_ = new QPushButton(trs("Edit"), background);
  edit_button_->setObjectName(QStringLiteral("Secondary"));

  open_button_ = new QPushButton(trs("Open"), background);
  open_button_->setObjectName(QStringLiteral("Primary"));
  open_button_->setDefault(true);

  auto* cancel_button = new QPushButton(trs("Cancel"), background);
  cancel_button->setObjectName(QStringLiteral("Secondary"));

  footer->addWidget(new_button);
  footer->addStretch(1);
  footer->addWidget(edit_button_);
  footer->addWidget(open_button_);
  footer->addWidget(cancel_button);

  outer->addLayout(footer);

  setStyleSheet(dialog_stylesheet(QStringLiteral("AssistantPicker")));

  connect(list_, &QListWidget::currentRowChanged, this, [this](int) {
    update_actions();
  });
  connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem*) {
    open_selected();
  });
  connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
    open_selected();
  });
  connect(new_button, &QPushButton::clicked, this, [this]() { create_new(); });
  connect(edit_button_, &QPushButton::clicked, this, [this]() { edit_selected(); });
  connect(open_button_, &QPushButton::clicked, this, [this]() { open_selected(); });
  connect(cancel_button, &QPushButton::clicked, this, [this]() { reject(); });

  refresh();
}

void AssistantPicker::refresh() {
  list_->clear();

  const std::vector<std::string> uuids = lnb::api::scan_assistants();

  for (const std::string& uuid : uuids) {
    const lnb::Assistant assistant = lnb::api::load_assistant(uuid);

    const QString name = to_qstring(assistant.name_);
    const QString model = to_qstring(assistant.model_);

    auto* item = new QListWidgetItem(
        name.isEmpty() ? trs("Untitled assistant") : name,
        list_
    );
    item->setData(k_uuid_role, to_qstring(uuid));
    item->setData(k_model_role, model);
    item->setToolTip(name);
  }

  shark::log::debug("assistant picker listed {} assistant(s)", uuids.size());

  if (list_->count() > 0) {
    // Prefer the last-opened assistant when it is still present.
    const QString last = to_qstring(settings_.last_assistant);
    int target = 0;
    if (!last.isEmpty()) {
      for (int row = 0; row < list_->count(); ++row) {
        if (list_->item(row)->data(k_uuid_role).toString() == last) {
          target = row;
          break;
        }
      }
    }
    list_->setCurrentRow(target);
    list_->setFocus();
  }

  update_actions();
}

void AssistantPicker::update_actions() {
  const bool has_selection = !selected_uuid().isEmpty();

  edit_button_->setEnabled(has_selection);
  open_button_->setEnabled(has_selection);
}

QString AssistantPicker::selected_uuid() const {
  const QListWidgetItem* item = list_->currentItem();
  if (item == nullptr) {
    return {};
  }
  return item->data(k_uuid_role).toString();
}

void AssistantPicker::select_uuid(const QString& uuid) {
  for (int row = 0; row < list_->count(); ++row) {
    if (list_->item(row)->data(k_uuid_role).toString() == uuid) {
      list_->setCurrentRow(row);
      return;
    }
  }
}

void AssistantPicker::open_selected() {
  const QString uuid = selected_uuid();
  if (uuid.isEmpty()) {
    return;
  }

  const std::string id = uuid.toStdString();
  shark::log::info("opening assistant {}", id);

  // Another running instance may already own this assistant. The core refuses,
  // so keep the picker open and say why rather than silently doing nothing.
  if (!api_.open_assistant(id, lnb::api::load_assistant(id))) {
    QMessageBox::warning(
        this, trs("Assistant in use"), to_qstring(api_.last_error())
    );
    return;
  }

  settings_.auto_login = auto_login_->isChecked();
  settings_.last_assistant = id;
  accept();
}

void AssistantPicker::create_new() {
  AssistantFormDialog form(trs("New assistant"), lnb::Assistant{}, this);
  if (form.exec() != QDialog::Accepted) {
    return;
  }

  const lnb::Assistant assistant = form.assistant();
  const std::string uuid = lnb::api::make_uuid();

  lnb::api::save_assistant(uuid, assistant);
  shark::log::info("created assistant {}", uuid);

  if (!api_.open_assistant(uuid, assistant)) {
    QMessageBox::warning(
        this, trs("Assistant in use"), to_qstring(api_.last_error())
    );
    refresh();
    select_uuid(to_qstring(uuid));
    update_actions();
    return;
  }

  settings_.auto_login = auto_login_->isChecked();
  settings_.last_assistant = uuid;
  accept();
}

void AssistantPicker::edit_selected() {
  const QString uuid = selected_uuid();
  if (uuid.isEmpty()) {
    return;
  }

  const std::string id = uuid.toStdString();
  const lnb::Assistant existing = lnb::api::load_assistant(id);

  AssistantFormDialog form(trs("Edit assistant"), existing, this);
  if (form.exec() != QDialog::Accepted) {
    return;
  }

  const lnb::Assistant assistant = form.assistant();
  lnb::api::save_assistant(id, assistant);
  shark::log::info("updated assistant {}", id);

  // Keep the in-memory assistant in step when the edited one is the open one.
  if (api_.has_assistant() && api_.assistant_uuid() == id) {
    api_.set_assistant(assistant);
    api_.save_assistant();
  }

  refresh();
  select_uuid(uuid);
  update_actions();
}

} // namespace ui
