#pragma once

#include <QDialog>

#include <cstdint>
#include <functional>
#include <vector>

#include "settings/metrics.hpp"
#include "app/settings.hpp"

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QWidget;

namespace lnb {
class Application;
}

namespace ui {

class AssistantForm;

// A separate settings window: a left navigation with five pages — Configuration
// (the app-global struct), Metrics (the theme metrics), Assistant (the open
// assistant's own configuration and shared memory), Account (the current session
// and startup behaviour) and About (program information). Configuration and
// Metrics edit a pending copy; Save persists everything, re-applies the theme at
// runtime and closes the window.
//
// The assistant itself is fixed for the lifetime of the process (one assistant
// per process), but its configuration is editable here.
class SettingsWindow : public QDialog {
public:
  explicit SettingsWindow(lnb::Application& app, QWidget* parent = nullptr);

  // Tells the Account page which session is current. Called before the window is
  // shown, since the window does not track the selection itself.
  void set_current_session(std::uint32_t session_id);

  // Invoked after a successful Save, so the main window can restyle its own
  // hand-painted chrome.
  void set_on_saved(std::function<void()> handler) {
    on_saved_ = std::move(handler);
  }

protected:
  // Every open re-reads the stored settings and assistant, so an edit that was
  // cancelled (or left uncommitted) never leaks into the next visit.
  void showEvent(QShowEvent* event) override;

private:
  QWidget* build_config_page();
  QWidget* build_metrics_page();
  QWidget* build_assistant_page();
  QWidget* build_account_page();
  QWidget* build_about_page();

  // Wraps a page body with its title/subtitle inside a scrolling area.
  QWidget* make_page(const QString& title, const QString& subtitle, QWidget* content);

  // Rebuilds the window's scoped stylesheet from the current theme. The scoped
  // sheet has higher precedence than the application sheet for this dialog's
  // subtree, so it must be rebuilt on every show; otherwise a theme change would
  // leave the dialog's controls (and the cards) on the colours they were built
  // with until the application restarts.
  void apply_style();

  // Pushes the widget values back into the pending settings / metrics.
  void write_config();
  void write_metrics();

  // Repopulates every widget from the stored state (settings, metrics and the
  // assistant). Called on each show.
  void reload_from_app();

  // Resets the Configuration page's fields to the shipped defaults, and the
  // Metrics page's fields to the current theme's own defaults. Neither saves.
  void restore_config_defaults();
  void restore_metrics_defaults();

  // Applies the pending settings, persists them, and closes the window.
  void save();

  // Folds the recent conversation of every session into the assistant memory,
  // ignoring the automatic trigger (the assistant-level counterpart of the
  // session editor's "Summarize now").
  void summarize_assistant_now();

  void update_account_page();

  lnb::Application& app_;

  // The edited copies; nothing is committed until Save.
  lnb::Settings pending_;
  lnb::ThemeMetrics pending_metrics_;

  QListWidget* nav_ = nullptr;
  QStackedWidget* stack_ = nullptr;

  std::uint32_t current_session_ = 0;
  QLabel* account_session_name_ = nullptr;
  QLabel* account_session_id_ = nullptr;

  // Edits the open assistant's configuration; written back on Save.
  AssistantForm* assistant_form_ = nullptr;

  // The assistant-level memory: shared across every session, edited (and folded)
  // on the Assistant page. Written back to the assistant on Save.
  QPlainTextEdit* assistant_memory_ = nullptr;
  QPushButton* assistant_summarize_ = nullptr;
  bool assistant_summarizing_ = false;

  // One writer per editor, so write_config()/write_metrics() do not need to keep
  // a pointer to every widget. The reloaders mirror them in the opposite
  // direction: they push the stored state back into the widgets on each show.
  std::vector<std::function<void()>> config_writers_;
  std::vector<std::function<void()>> metric_writers_;
  std::vector<std::function<void()>> config_reloaders_;
  std::vector<std::function<void()>> metric_reloaders_;

  std::function<void()> on_saved_;
};

} // namespace ui
