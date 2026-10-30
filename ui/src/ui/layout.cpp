#include "ui/layout.hpp"

#include <QGuiApplication>
#include <QLayout>
#include <QScreen>
#include <QWidget>

#include <algorithm>

namespace ui {

QSize screen_size() {
  if (const QScreen *screen = QGuiApplication::primaryScreen()) {
    return screen->availableGeometry().size();
  }

  return {};
}

bool compact_screen() {
  const QSize size = screen_size();

  return size.isValid() && is_compact(size.width());
}

void set_minimum_size(QWidget *widget, int width, int height) {
  const QSize screen = screen_size();

  const int max_width = screen.isValid() ? screen.width() : width;
  const int max_height = screen.isValid() ? screen.height() : height;

  widget->setMinimumSize(std::min(width, max_width), std::min(height, max_height));
}

void clear_layout(QLayout *layout) {
  while (QLayoutItem *item = layout->takeAt(0)) {
    // Deleting a QWidgetItem only deletes the item, not the widget it holds, so
    // the caller can insert the same widget into another layout afterwards.
    delete item;
  }
}

} // namespace ui
