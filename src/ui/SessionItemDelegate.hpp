#pragma once

#include <QSize>
#include <QStyledItemDelegate>

#include <functional>

class QPainter;
class QStyleOptionViewItem;

namespace ui {

class SessionItemDelegate : public QStyledItemDelegate {
public:
  using MenuHandler = std::function<void(
      const QModelIndex&,
      const QPoint&
  )>;

  explicit SessionItemDelegate(
      QObject* parent = nullptr
  );

  void set_menu_handler(MenuHandler handler);

  void paint(
      QPainter* painter,
      const QStyleOptionViewItem& option,
      const QModelIndex& index
  ) const override;

  QSize sizeHint(
      const QStyleOptionViewItem& option,
      const QModelIndex& index
  ) const override;

  bool editorEvent(
      QEvent* event,
      QAbstractItemModel* model,
      const QStyleOptionViewItem& option,
      const QModelIndex& index
  ) override;

private:
  MenuHandler menu_handler_;

  QRect menu_button_rect(
      const QStyleOptionViewItem& option
  ) const;
};

}