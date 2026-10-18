#include "ui/StatusNotice.hpp"

#include <QColor>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>

#include "ui/theme.hpp"

namespace ui {

namespace {

// QColor's name() drops the alpha, so the colours are emitted as rgba() the way
// the rest of the hand-styled chrome does: a translucent theme stays translucent.
QString css_color(const QColor& color) {
  return QStringLiteral("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alpha());
}

constexpr int kIconSize = 15;

} // namespace

StatusNotice::StatusNotice(QWidget* parent) : QFrame(parent) {
  setObjectName(QStringLiteral("StatusNotice"));
  setAttribute(Qt::WA_StyledBackground, true);
  setAutoFillBackground(false);
  setVisible(false);

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(10, 6, 10, 6);
  layout->setSpacing(8);

  icon_ = new QLabel(this);
  icon_->setFixedSize(kIconSize, kIconSize);
  icon_->setScaledContents(true);
  layout->addWidget(icon_, 0, Qt::AlignVCenter);

  text_ = new QLabel(this);
  text_->setWordWrap(true);
  text_->setFont(ui_font());
  layout->addWidget(text_, 1);

  apply_style();
  refresh_cursor();
}

void StatusNotice::set_content(const QString& icon_name, const QString& text) {
  icon_name_ = icon_name;
  text_->setText(text);
  apply_style();
  setVisible(!text.isEmpty());
}

void StatusNotice::refresh_style() {
  apply_style();
  refresh_cursor();
}

void StatusNotice::refresh_cursor() {
  setCursor(on_click_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void StatusNotice::apply_style() {
  const ThemeColors& c = theme().colors;

  // A subdued bar rather than a loud block: the surface fill keeps it quiet in the
  // transcript's shadow, while the danger border and glyph carry the warning.
  setStyleSheet(
      QStringLiteral(
          "QFrame#StatusNotice { background: %1; border: 1px solid %2;"
          " border-radius: 6px; }"
          "QLabel { background: transparent; color: %3; }"
      )
          .arg(css_color(c.surface), css_color(c.danger), css_color(c.text))
  );

  if (!icon_name_.isEmpty()) {
    QIcon icon = resource_icon(icon_name_, c.danger, c.text_muted);
    icon_->setPixmap(icon.isNull() ? QPixmap() : icon.pixmap(kIconSize, kIconSize));
    icon_->setVisible(!icon.isNull());
  } else {
    icon_->setVisible(false);
  }
}

void StatusNotice::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && on_click_) {
    on_click_();
  }

  QFrame::mouseReleaseEvent(event);
}

} // namespace ui
