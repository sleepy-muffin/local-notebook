#pragma once

// Wheel-safe spin boxes.
//
// A plain QSpinBox changes its value when the mouse wheel passes over it, which
// is a hazard inside a scrolling form: the pointer crosses the field while the
// reader scrolls and the value silently changes. These subclasses ignore the
// wheel unconditionally.
//
// Ignoring (rather than consuming) the event lets Qt forward it to the enclosing
// scroll area, so the page still scrolls while the value stays put.
//
// They are drop-in replacements: use them wherever a QSpinBox or QDoubleSpinBox
// would otherwise be created.

#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QWheelEvent>

namespace ui {

class SpinBox : public QSpinBox {
public:
  using QSpinBox::QSpinBox;

protected:
  void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class DoubleSpinBox : public QDoubleSpinBox {
public:
  using QDoubleSpinBox::QDoubleSpinBox;

protected:
  void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

} // namespace ui
