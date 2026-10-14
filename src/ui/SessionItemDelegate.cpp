#include "ui/SessionItemDelegate.hpp"

#include "ui/SessionListModel.hpp"
#include "ui/theme.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QFontMetrics>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace ui {

namespace {

// The two hover actions sit at the right edge, delete outermost. Each hit area
// mirrors the per-message actions (a 20x20 button holding a 15x15 icon).
constexpr int kActionSize = 20;
constexpr int kIconSize = 15;
constexpr int kActionMargin = 4;
constexpr int kActionGap = 2;

// The inner rect the icon is centred in.
QRect icon_rect(const QRect& button) {
  const int inset = (kActionSize - kIconSize) / 2;

  return button.adjusted(inset, inset, -inset, -inset);
}

} // namespace

SessionItemDelegate::SessionItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {
  // Take the artwork from the icon theme, with a standard-icon fallback, exactly
  // as ConversationView::MessageRow::add_action does for the message actions.
  edit_icon_ = QIcon::fromTheme(QStringLiteral("document-edit"));

  if (edit_icon_.isNull()) {
    edit_icon_ = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
  }

  delete_icon_ = QIcon::fromTheme(QStringLiteral("edit-delete"));

  if (delete_icon_.isNull()) {
    delete_icon_ = QApplication::style()->standardIcon(QStyle::SP_TrashIcon);
  }
}

void SessionItemDelegate::set_edit_handler(ActionHandler handler) {
  edit_handler_ = std::move(handler);
}

void SessionItemDelegate::set_delete_handler(ActionHandler handler) {
  delete_handler_ = std::move(handler);
}

QRect SessionItemDelegate::delete_button_rect(const QStyleOptionViewItem& option) const {
  return QRect(
      option.rect.right() - kActionSize - kActionMargin,
      option.rect.center().y() - kActionSize / 2,
      kActionSize,
      kActionSize
  );
}

QRect SessionItemDelegate::edit_button_rect(const QStyleOptionViewItem& option) const {
  const QRect del = delete_button_rect(option);

  return QRect(
      del.left() - kActionSize - kActionGap,
      del.top(),
      kActionSize,
      kActionSize
  );
}

void SessionItemDelegate::paint(
    QPainter* painter, const QStyleOptionViewItem& option,
    const QModelIndex& index
) const {
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);

  const QRect rect = option.rect;
  const bool selected = option.state & QStyle::State_Selected;
  const bool hovered = option.state & QStyle::State_MouseOver;

  // Low-contrast states: a barely-visible hover and a slightly brighter
  // selected row. No strong accent highlight.
  if (selected) {
    painter->setPen(Qt::NoPen);
    painter->setBrush(row_selected_background());
    painter->drawRoundedRect(rect.adjusted(4, 1, -4, -1), 6, 6);
  } else if (hovered) {
    painter->setPen(Qt::NoPen);
    painter->setBrush(row_hover_background());
    painter->drawRoundedRect(rect.adjusted(4, 1, -4, -1), 6, 6);
  }

  const QString title = index.data(SessionListModel::TitleRole).toString();

  QFont font = ui_font();
  painter->setFont(font);
  painter->setPen(theme().colors.text);

  // Reserve the trailing room the two action icons need, so a long title is
  // elided before it can reach them.
  const int reserved = kActionSize * 2 + kActionGap + kActionMargin;
  const QRect text_rect = rect.adjusted(12, 0, -reserved, 0);
  const QString elided = QFontMetrics(font).elidedText(
      title, Qt::ElideRight, text_rect.width()
  );
  painter->drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, elided);

  // The actions appear only while the row is hovered, so even the current
  // session's row stays calm at rest.
  if (hovered) {
    edit_icon_.paint(
        painter, icon_rect(edit_button_rect(option)), Qt::AlignCenter,
        QIcon::Active, QIcon::Off
    );
    delete_icon_.paint(
        painter, icon_rect(delete_button_rect(option)), Qt::AlignCenter,
        QIcon::Active, QIcon::Off
    );
  }

  painter->restore();
}

QSize SessionItemDelegate::sizeHint(
    const QStyleOptionViewItem& option, const QModelIndex& index
) const {
  Q_UNUSED(index);
  Q_UNUSED(option);
  return QSize(0, metrics().session_item_height);
}

bool SessionItemDelegate::editorEvent(
    QEvent* event, QAbstractItemModel* model,
    const QStyleOptionViewItem& option, const QModelIndex& index
) {
  Q_UNUSED(model);

  if (event->type() != QEvent::MouseButtonRelease) {
    return false;
  }

  auto* mouse = static_cast<QMouseEvent*>(event);
  if (mouse->button() != Qt::LeftButton) {
    return false;
  }

  const QPoint position = mouse->pos();

  if (edit_button_rect(option).contains(position)) {
    action_click_ = true;

    if (edit_handler_) {
      edit_handler_(index);
    }

    return true;
  }

  if (delete_button_rect(option).contains(position)) {
    action_click_ = true;

    if (delete_handler_) {
      delete_handler_(index);
    }

    return true;
  }

  action_click_ = false;
  return false;
}

bool SessionItemDelegate::take_action_click() {
  const bool value = action_click_;
  action_click_ = false;
  return value;
}

void SessionItemDelegate::clear_action_click() {
  action_click_ = false;
}

} // namespace ui
