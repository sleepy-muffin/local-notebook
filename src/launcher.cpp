#include <QApplication>

#include "ui/MainWindow.hpp"
#include "ui.hpp"
#include "external.hpp"

#include <memory>

#include "ui/Logon.hpp"
#include "ui/theme.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  auto state = std::make_unique<lnb::ApplicationState>();

  state->uni_config_ = std::make_unique<lnb::general>("local-notebook.json");
  mmc::log::debug("local-notebook.json loaded");

  state->uni_db_ = std::make_unique<lnb::db>("local-notebook.db");
  mmc::log::debug("local-notebook.db loaded");

  state->self_id_ = 0;

  state->multi_executor_ =
      std::make_unique<mmc::async::multi_executor<void()>>(3);

  ui::apply_theme(app, *state->uni_config_);

  std::unique_ptr<ui::MainWindow> main_window;

  ui::Logon logon(
      *state,
      [&]() {
          main_window = std::make_unique<ui::MainWindow>(*state);

          main_window->show();
          logon.hide();
      }
  );

  logon.show();

  const int result = app.exec();

  mmc::log::debug("Application is shutting down");

  main_window.reset();

  state->shutdown();
  state.reset();

  mmc::log::kill();

  return result;
}
