#pragma once

#include <QIcon>
#include <QSize>
#include <QStyledItemDelegate>

#include <functional>

class QPainter;
class QStyleOptionViewItem;

namespace ui {

// Paints one session row: a subtle hover, a slightly brighter selected row (the
// way Zed's chrome behaves), the title, and two hover-only actions — an edit
// pencil that opens the session editor and a trash can that deletes it.
//
// The action icons come from the icon theme (with standard-icon fallbacks),
// exactly as the per-message actions in the transcript do, so the row does not
// hand-paint its own artwork.
class SessionItemDelegate : public QStyledItemDelegate {
public:
  // Invoked with the row's model index when an action icon is clicked.
  using ActionHandler = std::function<void(const QModelIndex&)>;

  explicit SessionItemDelegate(QObject* parent = nullptr);

  void set_edit_handler(ActionHandler handler);
  void set_delete_handler(ActionHandler handler);

  void paint(
      QPainter* painter, const QStyleOptionViewItem& option,
      const QModelIndex& index
  ) const override;

  QSize sizeHint(
      const QStyleOptionViewItem& option, const QModelIndex& index
  ) const override;

  bool editorEvent(
      QEvent* event, QAbstractItemModel* model,
      const QStyleOptionViewItem& option, const QModelIndex& index
  ) override;

  // A release inside an action rect sets this flag (and invokes the handler).
  // The owning view reads it in its `clicked` handler to avoid also switching
  // to the session when an action icon was clicked; `QAbstractItemView` emits
  // `clicked` for any release on the pressed index regardless of editorEvent's
  // return value.
  [[nodiscard]] bool take_action_click();
  void clear_action_click();

  // The two action hit areas within a row. Public so hit-testing is possible
  // without duplicating the layout constants.
  [[nodiscard]] QRect delete_button_rect(const QStyleOptionViewItem& option) const;
  [[nodiscard]] QRect edit_button_rect(const QStyleOptionViewItem& option) const;

private:
  ActionHandler edit_handler_;
  ActionHandler delete_handler_;

  QIcon edit_icon_;
  QIcon delete_icon_;

  bool action_click_ = false;
};

} // namespace ui
