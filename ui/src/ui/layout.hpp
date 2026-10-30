#pragma once

// Shared narrow-window adaptation.
//
// The windows are laid out for a desktop: two panes side by side, dialogs that
// open at 760x520 and refuse to shrink below ~620. A phone in portrait is about
// 360 logical pixels wide, so every one of those minimums pushes the dialog off
// both edges — the login panel ended up centred with white bands above and below
// it, and the buttons in its footer were half off screen.
//
// The adaptation is driven by the window's own width, not by the platform: a
// narrow desktop window gets the same compact arrangement as a phone, which is
// what the desktop interface wants anyway.

#include <QSize>

class QLayout;
class QWidget;

namespace ui {

// Below this width (logical pixels) a window switches to its compact
// arrangement: one column, no navigation rail, no preview pane.
constexpr int k_compact_width = 600;

inline bool is_compact(int width) { return width < k_compact_width; }

// The primary screen's available size, in logical pixels (empty when there is no
// screen, as on a headless test run).
QSize screen_size();

// True when the display itself is narrow enough to want the compact
// arrangement. Dialogs are sized from this before they have a width of their own.
bool compact_screen();

// Applies a dialog's usual desktop minimum size, but never larger than the
// screen: a 620 px minimum on a 360 px phone is a minimum the window cannot
// honour, and Qt then lays the content out past the screen edges.
void set_minimum_size(QWidget* widget, int width, int height);

// Removes every item from a layout, leaving the widgets it held alive and
// parented: the dialogs use this to rearrange the same controls into a compact
// order and back.
void clear_layout(QLayout* layout);

} // namespace ui
