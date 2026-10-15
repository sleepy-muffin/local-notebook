#include "ui/BackgroundWidget.hpp"

#include "ui/theme.hpp"

#include <QPainter>
#include <QPixmap>
#include <QRect>
#include <QSizeF>

namespace ui {

BackgroundWidget::BackgroundWidget(QWidget* parent, bool use_background_image)
    : QWidget(parent),
      use_background_image_(use_background_image) {
  // The widget paints itself, so the stylesheet must not fill it first.
  setAttribute(Qt::WA_StyledBackground, false);
  setAutoFillBackground(false);
}

void BackgroundWidget::ensure_scaled_background() {
  const QPixmap& image = background_image();

  const QSize logical_size = size();
  const qreal dpr = devicePixelRatioF();

  // The cache is only rebuilt when the logical size or the scale factor changes.
  if (!scaled_background_.isNull() && cached_size_ == logical_size &&
      qFuzzyCompare(cached_dpr_, dpr)) {
    return;
  }

  cached_size_ = logical_size;
  cached_dpr_ = dpr;

  if (image.isNull() || logical_size.isEmpty()) {
    scaled_background_ = QPixmap{};
    return;
  }

  // Scale in device pixels so the image stays sharp on scaled displays, then
  // tag the result with the same ratio so it is drawn at the logical size.
  const QSize device_size = (QSizeF(logical_size) * dpr).toSize();

  const QPixmap scaled = image.scaled(
      device_size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation
  );

  const QPoint offset(
      (scaled.width() - device_size.width()) / 2,
      (scaled.height() - device_size.height()) / 2
  );

  scaled_background_ = scaled.copy(QRect(offset, device_size));
  scaled_background_.setDevicePixelRatio(dpr);
}

void BackgroundWidget::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);

  QPainter painter(this);

  if (use_background_image_ && background_enabled() && !background_image().isNull()) {
    ensure_scaled_background();

    if (!scaled_background_.isNull()) {
      painter.drawPixmap(0, 0, scaled_background_);
    }

    // The tint covers the entire background, below every component. It only
    // affects the area right above the image, never the components themselves.
    if (background_tint().has_value()) {
      painter.fillRect(rect(), *background_tint());
    }

    return;
  }

  painter.fillRect(rect(), theme().colors.window);
}

} // namespace ui
