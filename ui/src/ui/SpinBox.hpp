#pragma once

// Wheel-safe spin boxes, combo boxes and font selectors.
//
// A plain QSpinBox, QComboBox or QFontComboBox changes its value when the mouse
// wheel passes over it, which is a hazard inside a scrolling form: the pointer
// crosses the field while the reader scrolls and the value silently changes.
// These subclasses ignore the wheel unconditionally.
//
// Ignoring (rather than consuming) the event lets Qt forward it to the enclosing
// scroll area, so the page still scrolls while the value stays put.
//
// They are drop-in replacements: use them wherever a QSpinBox, QDoubleSpinBox,
// QComboBox or QFontComboBox would otherwise be created. The combo boxes also cap
// their own minimum width (eliding a long current item instead), so no single
// field can force a form wider than its viewport.

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QWheelEvent>

#include <algorithm>

namespace ui {

namespace detail {

  // A spin box's preferred width follows its widest possible value, which for the
  // token budgets is "1,000,000,000" — wide enough to push a form past its viewport
  // on platforms with large font metrics. Cap the *minimum* width at a
  // representative number instead: the field still grows to show the full value
  // whenever the form has room, but it can also shrink (eliding the value) rather
  // than widening the whole page.
  inline QSize capped_spin_minimum(const QWidget *widget, const QSize &natural) {
    const int width = widget->fontMetrics().horizontalAdvance(QStringLiteral("000,000")) + 32;
    QSize size = natural;
    size.setWidth(std::min(size.width(), width));
    return size;
  }

} // namespace detail

class SpinBox : public QSpinBox {
public:
  using QSpinBox::QSpinBox;

  explicit SpinBox(QWidget *parent = nullptr) : QSpinBox(parent) {
    setButtonSymbols(QAbstractSpinBox::NoButtons);
  }

protected:
  void wheelEvent(QWheelEvent *event) override { event->ignore(); }

  QSize minimumSizeHint() const override {
    return detail::capped_spin_minimum(this, QSpinBox::sizeHint());
  }
};

class DoubleSpinBox : public QDoubleSpinBox {
public:
  using QDoubleSpinBox::QDoubleSpinBox;

  explicit DoubleSpinBox(QWidget *parent = nullptr) : QDoubleSpinBox(parent) {
    setButtonSymbols(QAbstractSpinBox::NoButtons);
  }

protected:
  void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

// A wheel-safe combo box whose minimum width is bounded: the size hint follows a
// modest contents length rather than the longest item, so a long theme or preset
// name (or wide platform font metrics) cannot push a form past its viewport.
class ComboBox : public QComboBox {
public:
  explicit ComboBox(QWidget *parent = nullptr) : QComboBox(parent) {
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(6);

    setStyleSheet(
        styleSheet() + QStringLiteral(
                           "QComboBox::drop-down { width: 0px; border: none; }"
                           "QComboBox::down-arrow { image: none; }"));
  }

protected:
  void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

class FontComboBox : public QFontComboBox {
public:
  explicit FontComboBox(QWidget *parent = nullptr) : QFontComboBox(parent) {
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(6);

    setStyleSheet(
        styleSheet() + QStringLiteral(
                           "QComboBox::drop-down { width: 0px; border: none; }"
                           "QComboBox::down-arrow { image: none; }"));
  }

protected:
  void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

class PlainTextEdit : public QPlainTextEdit {
public:
  using QPlainTextEdit::QPlainTextEdit;

protected:
  void wheelEvent(QWheelEvent *event) override {
    if (!hasFocus()) {
      event->ignore();
      return;
    }

    QPlainTextEdit::wheelEvent(event);
  }
};

} // namespace ui
