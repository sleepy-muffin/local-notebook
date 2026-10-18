#pragma once

#include <QColor>
#include <QIcon>
#include <QSize>
#include <QStyledItemDelegate>

#include <functional>

class QPainter;
class QStyleOptionViewItem;

namespace ui {

// Paints one session row: a flat, full-width hover or selected shading (the way
// Zed's chrome behaves), the title on the first line and the last-message time on
// the second, plus three hover-only actions — a pin toggle, an edit pencil that
// opens the session editor and a trash can that deletes it.
//
// The action icons are bundled in the binary and recoloured to the active theme
// at runtime (see ui::resource_icon), exactly as the per-message actions in the
// transcript are, so the row does not depend on the system icon theme.
class SessionItemDelegate : public QStyledItemDelegate {
public:
  // Invoked with the row's model index when an action icon is clicked.
  using ActionHandler = std::function<void(const QModelIndex&)>;

  explicit SessionItemDelegate(QObject* parent = nullptr);

  void set_pin_handler(ActionHandler handler);
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

  // The three action hit areas within a row. Public so hit-testing is possible
  // without duplicating the layout constants.
  [[nodiscard]] QRect delete_button_rect(const QStyleOptionViewItem& option) const;
  [[nodiscard]] QRect edit_button_rect(const QStyleOptionViewItem& option) const;
  [[nodiscard]] QRect pin_button_rect(const QStyleOptionViewItem& option) const;

private:
  // Rebuilds the action icons when the theme's muted colour changes, so a
  // theme switch is picked up without an explicit refresh hook.
  void ensure_icons() const;

  ActionHandler pin_handler_;
  ActionHandler edit_handler_;
  ActionHandler delete_handler_;

  mutable QIcon pin_icon_;         // muted, for an unpinned row
  mutable QIcon pin_active_icon_;  // accent, for a pinned row
  mutable QIcon edit_icon_;
  mutable QIcon delete_icon_;
  mutable QColor icons_colour_;

  bool action_click_ = false;
};

} // namespace ui
