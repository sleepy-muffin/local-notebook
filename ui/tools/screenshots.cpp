#include <QApplication>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "local-notebook-core/interface.hpp"
#include "local-notebook-core/model.hpp"
#include "local-notebook-core/summary.hpp"
#include "settings/settings.hpp"
#include "support/i18n.hpp"
#include "ui/MainWindow.hpp"
#include "ui/SummaryReviewDialog.hpp"
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
      "You are a careful engineering assistant. Answer concisely and precisely, "
      "and prefer showing the smallest correct change.";
  assistant.memory_ =
      "The user is building Local Notebook, a C++23 / Qt Widgets application. "
      "They prefer British spelling in comments and flat, Zed-like interfaces "
      "with no rounded chrome.";
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

void seed_design_session(lnb::api::interface& api, std::uint32_t session_id) {
  api.append_message(
      session_id, lnb::Role::User,
      "Why can a long conversation truncate the model's reply?"
  );
  api.append_message(
      session_id, lnb::Role::Assistant,
      "Because the completion budget is shared with hidden reasoning. If the "
      "reasoning headroom is too small, the visible answer is cut off.\n\n"
      "```cpp\nconst std::size_t max_tokens =\n"
      "    session.summary_token_budget_ +\n"
      "    session.summary_reasoning_headroom_tokens_;\n```\n\n"
      "The fix is to raise `summary_reasoning_headroom_tokens`, not the answer "
      "budget."
  );
  api.append_message(
      session_id, lnb::Role::User,
      "Should the input bound be derived from the model context window?"
  );
  api.append_message(
      session_id, lnb::Role::Assistant,
      "No. Deriving it creates a second source of truth. The input bound is the "
      "session's own `summary_trigger_tokens_`; nothing else feeds it."
  );
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

  // Background sessions, so the sidebar reads like a real notebook.
  const std::uint32_t rust = add_session(
      api,
      "Placeholder",
      "Placeholder"
  );
  api.append_message(
      rust, lnb::Role::User, "Placeholder"
  );
  api.append_message(
      rust, lnb::Role::Assistant,
      "Placeholder"
  );

  const std::uint32_t performance = add_session(
      api,
      "Qt performance",
      "The user cares about keeping long transcripts responsive."
  );
  api.append_message(
      performance, lnb::Role::User,
      "How do I keep a long chat list smooth?"
  );
  api.append_message(
      performance, lnb::Role::Assistant,
      "Render only what is visible, and reuse the rows as they scroll."
  );

  // The featured session is created last, so its newest message is what the
  // main window opens. Its retained window is deliberately small, which gives
  // the summarisation review below a real overflow to fold.
  lnb::Session design;
  design.title_ = "Summarisation design";
  design.custom_instructions_ =
      "When discussing design, compare the options and recommend the most mature "
      "one.";
  design.memory_ =
      "The session is designing a two-level memory: a session memory and a "
      "shared assistant memory. Watermarks are bookkeeping only.";
  design.summary_keep_tokens_ = 120;

  const std::uint32_t design_id = api.create_session(design);
  seed_design_session(api, design_id);

  // Capture the request the review dialog normally shows, without sending it:
  // the approver runs before the first byte leaves the process, and declining
  // leaves the memory and its watermark untouched.
  lnb::SummaryPlan plan;

  if (auto task = api.summarize_session(
          design_id, /*force=*/true,
          [&plan](const lnb::SummaryPlan& proposed) {
            plan = proposed;
            return false;
          }
      );
      task != nullptr) {
    api.wait_for_generation();
  }

  if (plan.request.empty()) {
    std::cerr << "failed to capture a summarisation request\n";
    return EXIT_FAILURE;
  }

  {
    ui::MainWindow window(api, settings);
    shoot(window, output / "main-window.png", QSize(1180, 760));
  }

  {
    ui::SummaryReviewDialog dialog(plan);
    shoot(dialog, output / "summary-review.png", QSize(760, 640));
  }

  api.wait_for_generation();

  return EXIT_SUCCESS;
}
