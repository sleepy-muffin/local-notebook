#pragma once

#include <QScrollBar>

class QAbstractScrollArea;

namespace ui {

// What a scrollbar paints behind its handle.
//
// The platform style is bypassed deliberately. Breeze (and some other styles)
// fill the empty track with the palette's Base colour, which shows up as a solid
// strip that is lighter than the surface behind it. Painting the track with the
// colour that is actually behind the bar keeps it invisible on every style.
enum class ScrollBarTrack {
  // Leave the track untouched (the platform style shows through).
  None,
  // The window colour: dialogs and other plain surfaces.
  Window,
  // The sidebar/list surface: the session list over the sidebar.
  Surface,
  // The conversation surface. When the theme has no background image this is
  // the conversation colour; with an image it follows the window's background
  // (image and tint) so the bar stays part of the picture.
  Conversation,
};

class ScrollBar : public QScrollBar {
public:
  explicit ScrollBar(
      Qt::Orientation orientation, ScrollBarTrack track = ScrollBarTrack::None,
      QWidget* parent = nullptr
  );

protected:
  void paintEvent(QPaintEvent* event) override;
  bool event(QEvent* event) override;

private:
  void paint_track(QPainter& painter) const;
  [[nodiscard]] QColor handle_color() const;

  ScrollBarTrack track_ = ScrollBarTrack::None;
  bool hovered_ = false;
};

// Replaces both scrollbars of an area with ScrollBar instances. Call once,
// after the area's scrollbar policies are set.
void install_scroll_bars(
    QAbstractScrollArea* area, ScrollBarTrack track = ScrollBarTrack::None
);

} // namespace ui
