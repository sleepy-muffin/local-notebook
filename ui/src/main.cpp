#include <QApplication>
#include <QDialog>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "local_notebook/core/interface.hpp"
#include "shark/log.hpp"
#include "settings/settings.hpp"
#include "support/i18n.hpp"
#include "ui/AssistantPicker.hpp"
#include "ui/MainWindow.hpp"
#include "ui/theme.hpp"
#include "wayland_icon.hpp"

namespace {

// The per-user application data directory, or the --user-data override. This is
// resolved in the UI because it is the only place that knows about Qt's
// platform-specific locations; the core only receives the resulting root.
std::filesystem::path resolve_data_root(const QStringList& arguments) {
  for (int i = 1; i < arguments.size(); ++i) {
    const QString& argument = arguments.at(i);

    if (argument == QStringLiteral("--user-data") && i + 1 < arguments.size()) {
      return arguments.at(i + 1).toStdString();
    }

    if (argument.startsWith(QStringLiteral("--user-data="))) {
      return argument.mid(12).toStdString();
    }
  }

  const QString location =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

  return location.isEmpty() ? std::filesystem::path{"data"}
                            : std::filesystem::path{location.toStdString()};
}

} // namespace

int main(int argc, char** argv) {
  // The theme and the icons live in assets/local-notebook.qrc, which is compiled
  // into the local-notebook-ui-core static library. The linker drops a resource
  // object that nothing references, so without this call ":/themes/theme.json"
  // does not exist and a launch whose working directory has no themes/theme.json
  // gets the empty fallback theme (black colours). Q_INIT_RESOURCE pulls the
  // generated resource object into the executable.
  Q_INIT_RESOURCE(local_notebook);

  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Local Notebook"));
  QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

  // The bundled artwork becomes the icon of every window (the picker, the main
  // window and any dialog) for the platforms where that is enough. On Wayland it
  // is only the client-side decoration's icon: see install_wayland_toplevel_icon.
  QApplication::setWindowIcon(ui::application_icon());

  // A native Wayland client has no desktop entry to point the compositor at and
  // Qt 6.8 does not speak xdg-toplevel-icon-v1, so hand the icon to the
  // compositor directly. No-op off Wayland or if the protocol is unavailable.
  ui::install_wayland_toplevel_icon(app);

  // Point the whole on-disk tree at the per-user application data directory, or at
  // the --user-data override. This must run before the settings are read.
  lnb::api::set_data_root(resolve_data_root(app.arguments()));
  shark::log::info("data root: {}", lnb::api::data_root().string());

  lnb::Settings settings = lnb::load_settings();

  // Select the stored UI language before any window is created, so every string
  // is looked up in the right language the first time it is built.
  lnb::i18n::set_language(
      lnb::i18n::language_from_code(settings.language)
  );

  // Install the theme before any window is created.
  ui::apply_theme(app, settings);

  lnb::api::interface api;

  // Reopen the last assistant when auto-login is on and it is still on disk;
  // otherwise ask.
  const std::string last = settings.last_assistant;
  bool opened = false;

  if (settings.auto_login && !last.empty()) {
    const std::vector<std::string> uuids = lnb::api::scan_assistants();

    if (std::find(uuids.begin(), uuids.end(), last) != uuids.end()) {
      opened = api.open_assistant(last, lnb::api::load_assistant(last));

      if (!opened) {
        shark::log::error("cannot auto-open assistant: {}", api.last_error());
      }
    }
  }

  if (!opened) {
    ui::AssistantPicker picker(api, settings);

    if (picker.exec() != QDialog::Accepted) {
      return 0;
    }
  }

  ui::MainWindow window(api, settings);
  window.show();

  const int code = app.exec();

  lnb::save_settings(settings);
  api.wait_for_generation();
  shark::log::debug("local-notebook exited with code {}", code);
  return code;
}
