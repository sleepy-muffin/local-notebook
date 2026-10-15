#include <QApplication>
#include <QDialog>

#include <algorithm>
#include <string>
#include <vector>

#include "app/application.hpp"
#include "app/assistant_io.hpp"
#include "app/data_location.hpp"
#include "store/util.hpp"
#include "support/log.hpp"
#include "ui/AssistantPicker.hpp"
#include "ui/MainWindow.hpp"
#include "ui/theme.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Local Notebook"));
  QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

  // The bundled artwork becomes the icon of every window (the picker, the main
  // window and any dialog), so the taskbar and the dock show it too.
  QApplication::setWindowIcon(ui::application_icon());

  // Point the whole on-disk tree at the per-user application data directory, or at
  // the --user-data override. This must run before the Application reads the
  // global settings.
  lnb::store::paths::set_data_root(lnb::app::resolve_data_root(app.arguments()));
  lnb::log::info("data root: {}", lnb::store::paths::data_root().string());

  lnb::Application notebook;

  // Install the theme before any window is created.
  ui::apply_theme(app, notebook.settings());

  // Reopen the last assistant when auto-login is on and it is still on disk;
  // otherwise ask.
  const std::string last = notebook.settings().last_assistant;
  bool opened = false;

  if (notebook.settings().auto_login && !last.empty()) {
    const std::vector<std::string> uuids = lnb::store::scan_assistants();

    if (std::find(uuids.begin(), uuids.end(), last) != uuids.end()) {
      notebook.open_assistant(last, lnb::load_assistant(last));
      opened = true;
    }
  }

  if (!opened) {
    ui::AssistantPicker picker(notebook);

    if (picker.exec() != QDialog::Accepted) {
      return 0;
    }
  }

  ui::MainWindow window(notebook);
  window.show();

  const int code = app.exec();

  notebook.shutdown();
  lnb::log::debug("local-notebook exited with code {}", code);
  return code;
}
