#pragma once

#include <QPixmap>
#include <QSize>
#include <QWidget>

class QPaintEvent;

namespace ui {

// The base layer of every top-level window. It paints the configured background
// image (scaled to cover the widget, keeping the aspect ratio) or, when no
// image is set, the theme's window colour. This guarantees the window is always
// fully painted, which also removes the thin unpainted line some window
// managers showed above the content.
//
// When an image is enabled the optional background tint is filled over the
// whole widget, below every component, so components keep their palette colours
// and only the canvas area shows the tinted image.
class BackgroundWidget : public QWidget {
public:
  explicit BackgroundWidget(QWidget* parent = nullptr);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  // Rebuilds the cached, device-pixel-ratio aware cover-fit of the source image.
  void ensure_scaled_background();

  QPixmap scaled_background_;
  QSize cached_size_;
  qreal cached_dpr_ = 0.0;
};

} // namespace ui
