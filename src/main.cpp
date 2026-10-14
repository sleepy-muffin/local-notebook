#include <QApplication>
#include <QDialog>

#include <algorithm>
#include <string>
#include <vector>

#include "app/application.hpp"
#include "app/assistant_io.hpp"
#include "store/util.hpp"
#include "support/log.hpp"
#include "ui/AssistantPicker.hpp"
#include "ui/MainWindow.hpp"
#include "ui/theme.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Local Notebook"));
  QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

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
