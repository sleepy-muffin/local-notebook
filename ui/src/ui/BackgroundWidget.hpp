#pragma once

#include <QPixmap>
#include <QSize>
#include <QWidget>

class QPaintEvent;
class QPainter;

namespace ui {

// The base layer of every top-level window. It paints the configured background
// image (scaled to cover the widget, keeping the aspect ratio) or, when no
// image is set, the theme's window colour. This guarantees the window is always
// fully painted.
//
// When an image is enabled the optional background tint is filled over the
// whole widget, below every component, so components keep their palette colours
// and only the canvas area shows the tinted image.
//
// use_background_image opts a window into the image. Only the main window does
// so; dialogs and pickers keep a plain window-coloured canvas, so the image
// never appears behind a form.
class BackgroundWidget : public QWidget {
public:
  explicit BackgroundWidget(QWidget* parent = nullptr, bool use_background_image = false);

  // Paints the backdrop behind `region` (in this widget's logical coordinates)
  // into `target` (in the caller's painter coordinates). Used by overlay chrome,
  // such as the custom scrollbar, that must reproduce the surface it sits on —
  // including the background image and its tint when one is configured.
  void paint_region(QPainter& painter, const QRect& target, const QRect& region);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  // Rebuilds the cached, device-pixel-ratio aware cover-fit of the source image.
  void ensure_scaled_background();

  bool use_background_image_ = false;
  QPixmap scaled_background_;
  QSize cached_size_;
  qreal cached_dpr_ = 0.0;
  // Identity of the source pixmap the cache was built from, so replacing the
  // background image with one of the same size rebuilds the cache instead of
  // keeping the stale pixmap on screen.
  qint64 cached_image_key_ = 0;
};

} // namespace ui
