#pragma once

#include "../ui.hpp"

#include <QDialog>

#include <functional>
#include <vector>

class QListWidget;
class QStackedWidget;
class QWidget;

namespace ui {

// A separate, non-modal settings window: a left navigation rail with four pages
// (Configuration, Appearance, Account and About). The Configuration and
// Appearance pages edit the in-memory configuration and theme metrics; Apply
// re-applies the theme at runtime. Account shows the current session and About
// shows program information.
class SettingsWindow : public QDialog {
public:
  explicit SettingsWindow(lnb::ApplicationState& state, QWidget* parent = nullptr);

private:
  QWidget* build_config_page();
  QWidget* build_metrics_page();
  QWidget* build_account_page();
  QWidget* build_about_page();

  // Wraps a page body with its title/subtitle inside a scrolling area.
  QWidget* make_page(const QString& title, const QString& subtitle, QWidget* content);

  // Re-runs every editor writer, pushing the widget values back into the
  // in-memory configuration / theme metrics.
  void write_config();
  void write_metrics();

  // Applies the edited configuration and metrics to the running application.
  void apply();

  lnb::ApplicationState& state_;

  QListWidget* nav_ = nullptr;
  QStackedWidget* stack_ = nullptr;

  // One writer per editor, so Apply (and the live change handlers) can push the
  // widget values back without keeping a pointer to every widget.
  std::vector<std::function<void()>> config_writers_;
  std::vector<std::function<void()>> metric_writers_;
};

} // namespace ui
