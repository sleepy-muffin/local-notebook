#pragma once

#include <QFrame>
#include <QString>

#include <functional>
#include <utility>

class QLabel;
class QMouseEvent;

namespace ui {

// A small inline notice that points the reader at one of the status bar's icons
// ("A generation task failed - open the tasks window from the status bar"). It sits
// just above the composer, so it is visible without pushing the transcript around.
//
// It is deliberately generic: the caller names the icon and supplies the message,
// so any future status-bar icon can raise a matching notice without a new widget.
// An optional click action turns the whole bar into a shortcut back to that icon's
// window.
class StatusNotice : public QFrame {
public:
  explicit StatusNotice(QWidget* parent = nullptr);

  // Sets the glyph (a bundled icon name) and the message. An empty message hides
  // the notice.
  void set_content(const QString& icon_name, const QString& text);

  // Runs when the notice is clicked (when set), so it can open the window the
  // status-bar icon belongs to.
  void set_on_click(std::function<void()> handler) {
    on_click_ = std::move(handler);
    refresh_cursor();
  }

  // Re-applies the theme colours after a theme change.
  void refresh_style();

protected:
  void mouseReleaseEvent(QMouseEvent* event) override;

private:
  void apply_style();
  void refresh_cursor();

  QLabel* icon_ = nullptr;
  QLabel* text_ = nullptr;
  QString icon_name_;
  std::function<void()> on_click_;
};

} // namespace ui
