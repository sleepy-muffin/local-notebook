#include "ui/ScrollBar.hpp"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>

#include "ui/BackgroundWidget.hpp"
#include "ui/theme.hpp"

namespace ui {

ScrollBar::ScrollBar(Qt::Orientation orientation, ScrollBarTrack track, QWidget* parent)
    : QScrollBar(orientation, parent), track_(track) {
  // The track is painted by this class; never let the style or the backing store
  // fill the widget first.
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_Hover, true);
  setAutoFillBackground(false);
}

QColor ScrollBar::handle_color() const {
  QColor color = theme().colors.border;

  if (hovered_ || isSliderDown()) {
    color = color.lighter(130);
  }

  return color;
}

void ScrollBar::paint_track(QPainter& painter) const {
  if (track_ == ScrollBarTrack::None) {
    return;
  }

  const ThemeColors& colors = theme().colors;
  QColor color = colors.window;

  switch (track_) {
    case ScrollBarTrack::None:
      return;
    case ScrollBarTrack::Window:
      color = colors.window;
      break;
    case ScrollBarTrack::Surface:
      color = colors.surface;
      break;
    case ScrollBarTrack::Conversation:
      color = colors.conversation_background;
      break;
  }

  if (color.alpha() == 255) {
    painter.fillRect(rect(), color);
    return;
  }

  // A fully transparent surface means the theme shows the window's background
  // image through it. Reproduce the slice of that image behind the bar rather
  // than leaving the platform's Base strip in the picture. This covers both the
  // conversation surface and the transparent sidebar in the translucent themes.
  if (color.alpha() == 0) {
    if (auto* background = dynamic_cast<BackgroundWidget*>(window())) {
      const QRect region(mapTo(background, QPoint(0, 0)), size());
      background->paint_region(painter, rect(), region);
      return;
    }
  }

  // A translucent surface sits over the plain window colour.
  painter.fillRect(rect(), colors.window);
  painter.fillRect(rect(), color);
}

void ScrollBar::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);

  if (maximum() <= minimum()) {
    return;
  }

  QStyleOptionSlider option;
  initStyleOption(&option);

  const QRect slider = style()->subControlRect(
      QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, this
  );

  if (!slider.isValid()) {
    return;
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  // The style may have filled the widget before this event ran; the track paint
  // replaces that fill with whatever is actually behind the bar.
  paint_track(painter);

  painter.setPen(Qt::NoPen);
  painter.setBrush(handle_color());

  const bool vertical = orientation() == Qt::Vertical;
  const QRectF handle = QRectF(slider).adjusted(
      vertical ? 1.0 : 0.0, vertical ? 0.0 : 1.0, vertical ? -1.0 : 0.0, vertical ? 0.0 : -1.0
  );
  const qreal radius = (vertical ? handle.width() : handle.height()) / 2.0;

  painter.drawRoundedRect(handle, radius, radius);
}

bool ScrollBar::event(QEvent* event) {
  switch (event->type()) {
    case QEvent::HoverEnter:
      hovered_ = true;
      update();
      break;
    case QEvent::HoverLeave:
      hovered_ = false;
      update();
      break;
    default:
      break;
  }

  return QScrollBar::event(event);
}

void install_scroll_bars(QAbstractScrollArea* area, ScrollBarTrack track) {
  area->setVerticalScrollBar(new ScrollBar(Qt::Vertical, track));
  area->setHorizontalScrollBar(new ScrollBar(Qt::Horizontal, track));
}

} // namespace ui
