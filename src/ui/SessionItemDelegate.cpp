#include "SessionItemDelegate.hpp"

#include <QVariant>
#include "theme.hpp"
#include "../db.hpp"

#include <algorithm>
#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QPalette>
#include <QColor>
#include <qevent.h>
#include <QRect>

namespace ui {

//

SessionItemDelegate::SessionItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

void SessionItemDelegate::set_menu_handler(MenuHandler handler) {
  menu_handler_ = std::move(handler);
}

QSize SessionItemDelegate::sizeHint(
    const QStyleOptionViewItem& option,
    const QModelIndex& index
) const {
  QSize size = QStyledItemDelegate::sizeHint(option, index);
  size.setHeight(theme().metrics.session_item_height);

  return size;
}

QRect SessionItemDelegate::menu_button_rect(
    const QStyleOptionViewItem& option
) const {
  constexpr int button_size = 32;
  constexpr int right_margin = 6;

  return QRect(
      option.rect.right() - right_margin - button_size + 1,
      option.rect.center().y() - button_size / 2,
      button_size,
      button_size
  );
}

void SessionItemDelegate::paint(
    QPainter* painter,
    const QStyleOptionViewItem& option,
    const QModelIndex& index
) const {
    painter->save();

    const bool selected =
        option.state & QStyle::State_Selected;

    const bool hovered =
        option.state & QStyle::State_MouseOver;

    const ThemeColors& colors = theme().colors;

    // The overflow button only exists when a menu handler is installed; the
    // identity picker reuses this delegate without one.
    const bool show_menu = menu_handler_ != nullptr;

    const QRect item_rect = option.rect.adjusted(3, 2, -3, -2);
    const QRect button_rect = menu_button_rect(option);

    const QColor selected_background = colors.base.lighter(115);
    const QColor hover_background = colors.base.lighter(108);

    const QColor background_color =
        selected
            ? selected_background
            : hovered
                ? hover_background
                : QColor(Qt::transparent);

    if (background_color.alpha() > 0) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(background_color);
        painter->drawRoundedRect(item_rect, 10, 10);
    }

    const QString title = index.data(Qt::DisplayRole).toString();

    constexpr int left_margin = 12;
    constexpr int right_margin = 8;

    const int text_right = (show_menu ? button_rect.left() : item_rect.right()) - right_margin;

    const QRect text_rect(
        item_rect.left() + left_margin,
        item_rect.top(),
        text_right - item_rect.left() - left_margin,
        item_rect.height()
    );

    const QColor text_color = colors.text;

    painter->setPen(text_color);
    painter->setFont(option.font);

    painter->setFont(option.font);
    painter->setPen(text_color);
    painter->drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft,
                      QFontMetrics(option.font).elidedText(title, Qt::ElideRight, text_rect.width()));

    if (show_menu && (hovered || selected)) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(text_color);

        const QPoint center = button_rect.center();
        constexpr int dot_radius = 1;
        constexpr int dot_spacing = 5;

        for (int i = -1; i <= 1; ++i) {
            painter->drawEllipse(
                QPoint(
                    center.x(),
                    center.y() + i * dot_spacing
                ),
                dot_radius,
                dot_radius
            );
        }
    }

    painter->restore();
}


bool SessionItemDelegate::editorEvent(
    QEvent* event,
    QAbstractItemModel* model,
    const QStyleOptionViewItem& option,
    const QModelIndex& index
) {
  if (event->type() == QEvent::MouseButtonPress ||
      event->type() == QEvent::MouseButtonRelease) {

    auto* mouse_event = static_cast<QMouseEvent*>(event);

    if (menu_handler_ && menu_button_rect(option).contains(
            mouse_event->position().toPoint())) {

      if (event->type() == QEvent::MouseButtonRelease &&
          mouse_event->button() == Qt::LeftButton &&
          menu_handler_) {

        menu_handler_(
            index,
            mouse_event->globalPosition().toPoint()
        );
          }

      return true;
            }
      }

  return QStyledItemDelegate::editorEvent(
      event,
      model,
      option,
      index
  );
}




}
