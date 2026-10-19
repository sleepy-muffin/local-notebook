#include <QApplication>
#include <QDir>
#include <QSize>
#include <QString>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "local-notebook-core/interface.hpp"
#include "local-notebook-core/model.hpp"
#include "settings/settings.hpp"
#include "support/i18n.hpp"
#include "ui/AssistantPicker.hpp"
#include "ui/MainWindow.hpp"
#include "ui/SessionEditor.hpp"
#include "ui/SettingsWindow.hpp"
#include "ui/theme.hpp"

namespace {

void shoot(
    QWidget& widget, const std::filesystem::path& path, const QSize& size
) {
  widget.resize(size);
  widget.show();

  QApplication::processEvents();
  QApplication::processEvents();

  if (!widget.grab().save(QString::fromStdString(path.string()))) {
    std::cerr << "failed to write " << path << '\n';
    std::exit(EXIT_FAILURE);
  }

  std::cout << "wrote " << path << '\n';
}

lnb::Assistant make_assistant() {
  lnb::Assistant assistant;
  assistant.name_ = "Aurora";
  assistant.model_ = "gpt-5.6-luna";
  assistant.api_url_ = "https://api.example.invalid/v1";
  assistant.custom_instructions_ =
      "Answer concisely and precisely, and prefer the smallest correct change.";
  assistant.memory_ =
      "The user is building Local Notebook, a C++23 / Qt Widgets application.";
  return assistant;
}

std::uint32_t add_session(
    lnb::api::interface& api, const std::string& title,
    const std::string& memory
) {
  lnb::Session session;
  session.title_ = title;
  session.memory_ = memory;
  return api.create_session(session);
}

} // namespace

int main(int argc, char** argv) {
  // The tool runs from a scratch directory without themes/ or icons/ on disk,
  // so it depends on the resources compiled into local-notebook-ui-core. See
  // the note in src/main.cpp: a static library's resource object is otherwise
  // dropped by the linker.
  Q_INIT_RESOURCE(local_notebook);

  if (argc < 2) {
    std::cerr << "usage: local-notebook-screenshots <output-directory>\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path output =
      std::filesystem::absolute(argv[1]);

  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() /
      ("lnb-screenshots-" + lnb::api::make_uuid());

  std::filesystem::create_directories(scratch);
  std::filesystem::current_path(scratch);
  std::filesystem::create_directories(output);

  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Local Notebook"));
  QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
  QApplication::setWindowIcon(ui::application_icon());

  lnb::api::set_data_root(scratch / "data");

  lnb::Settings settings;
  settings.ui_font = "Carlito";
  settings.font = "Carlito";
  settings.mono_font = "JetBrains Mono Light";

  lnb::i18n::set_language(lnb::i18n::language_from_code(settings.language));
  ui::apply_theme(app, settings);

  lnb::api::interface api;
  api.open_assistant(
      "a1b2c3d4-0000-0000-0000-000000000001",
      make_assistant()
  );

  const std::uint32_t rust = add_session(
      api,
      "Rust lifetimes",
      "The user is learning Rust and already knows C++."
  );

  const std::uint32_t performance = add_session(
      api,
      "Qt performance",
      "The user cares about keeping long transcripts responsive."
  );

  const std::uint32_t design = add_session(
      api,
      "Summarisation design",
      "The session is designing a two-level memory system."
  );

  {
    ui::MainWindow window(api, settings);
    shoot(window, output / "main-window.png", QSize(1180, 760));
  }

  {
    ui::SettingsWindow window(api, settings);
    window.set_current_session(design);
    shoot(window, output / "settings.png", QSize(820, 620));
  }

  {
    ui::SessionEditor editor(api, design);
    shoot(editor, output / "session-editor.png", QSize(640, 720));
  }

  {
    ui::AssistantPicker picker(api, settings);
    shoot(picker, output / "assistant-picker.png", QSize(760, 520));
  }

  api.wait_for_generation();

  return EXIT_SUCCESS;
}