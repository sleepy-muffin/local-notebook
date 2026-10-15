// Generates the interface screenshots used by README.md.
//
// This is a development tool, not a test: it seeds a scratch data directory,
// renders each window under the offscreen platform plugin and writes PNGs to the
// directory given as the first argument. Run it from the repository root:
//
//   QT_QPA_PLATFORM=offscreen \
//     ./build/linux-x64-debug/local-notebook-screenshots docs/screenshots
//
// It never touches the real data/ tree: it changes into a fresh temporary
// directory before constructing the application.

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QListWidget>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "app/application.hpp"
#include "app/settings.hpp"
#include "model.hpp"
#include "net/client.hpp"
#include "store/util.hpp"
#include "ui/AssistantPicker.hpp"
#include "ui/MainWindow.hpp"
#include "ui/SessionEditor.hpp"
#include "ui/SettingsWindow.hpp"
#include "ui/SummaryReviewDialog.hpp"
#include "ui/theme.hpp"

namespace {

// Renders `widget` at `size` and writes it to `path`.
void shoot(QWidget& widget, const std::filesystem::path& path, const QSize& size) {
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

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: local-notebook-screenshots <output-directory>\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path output = std::filesystem::absolute(argv[1]);

  // Seed in a scratch directory so the real data/ tree is never touched.
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / ("lnb-screenshots-" + lnb::store::make_uuid());
  std::filesystem::create_directories(scratch);
  std::filesystem::current_path(scratch);
  std::filesystem::create_directories(output);

  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Local Notebook"));
  QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

  lnb::Application notebook;
  notebook.open_assistant("a1b2c3d4-0000-0000-0000-000000000001", make_assistant());

  const auto add_session = [&](const std::string& title, const std::string& memory) {
    lnb::Session session;
    session.title_ = title;
    session.memory_ = memory;
    return notebook.sessions().create(session);
  };

  // Background sessions, so the sidebar reads like a real notebook.
  const std::uint32_t rust = add_session(
      "Rust lifetimes",
      "The user is learning Rust. They already know C++ and keep reaching for "
      "move semantics; explain ownership in terms of what they know."
  );
  notebook.messages(rust).append(lnb::Role::User, "Why does this borrow live so long?");
  notebook.messages(rust).append(
      lnb::Role::Assistant,
      "Because the returned reference is tied to the input's lifetime, not to a "
      "value created inside the function."
  );

  const std::uint32_t perf = add_session(
      "Qt performance", "The user cares about keeping long transcripts responsive."
  );
  notebook.messages(perf).append(lnb::Role::User, "How do I keep a long chat list smooth?");
  notebook.messages(perf).append(
      lnb::Role::Assistant, "Render only what is visible, and reuse the rows as they scroll."
  );

  // The featured session is created last, so its messages are the newest and it
  // opens by default.
  lnb::Session featured;
  featured.title_ = "Summarisation design";
  featured.custom_instructions_ =
      "When discussing design, compare the options and recommend the most mature one.";
  featured.memory_ =
      "The session is designing a two-level memory: a session memory and a shared "
      "assistant memory. Watermarks are bookkeeping only.";
  featured.shared_directory_ = "/home/user/src/local-notebook";
  const std::uint32_t design = notebook.sessions().create(featured);

  notebook.messages(design).append(
      lnb::Role::User, "Why can a long conversation truncate the model's reply?"
  );
  notebook.messages(design).append(
      lnb::Role::Assistant,
      "Because the completion budget is shared with hidden reasoning. If the "
      "reasoning headroom is too small, the visible answer is cut off.\n\n"
      "```cpp\nconst std::size_t max_tokens =\n    session.summary_token_budget_ +\n"
      "    session.summary_reasoning_headroom_tokens_;\n```\n\n"
      "The fix is to raise `summary_reasoning_headroom_tokens`, not the answer budget."
  );
  notebook.messages(design).append(
      lnb::Role::User,
      "Should the input bound be derived from the model context window?"
  );
  notebook.messages(design).append(
      lnb::Role::Assistant,
      "No. Deriving it creates a second source of truth. The input bound is the "
      "session's own `summary_trigger_tokens_`; nothing else feeds it."
  );

  ui::apply_theme(app, lnb::Settings{});

  {
    ui::MainWindow window(notebook);
    shoot(window, output / "main-window.png", QSize(1180, 760));
  }

  {
    ui::SettingsWindow window(notebook);
    window.set_current_session(design);

    auto* nav = window.findChild<QListWidget*>(QStringLiteral("SettingsNav"));

    const auto show_page = [&](int row, const char* name) {
      if (nav != nullptr) {
        nav->setCurrentRow(row);
      }
      shoot(window, output / name, QSize(820, 620));
    };

    show_page(0, "settings-configuration.png");
    show_page(1, "settings-metrics.png");
    show_page(2, "settings-assistant.png");
    show_page(3, "settings-account.png");
    show_page(4, "settings-about.png");
  }

  {
    ui::SessionEditor editor(notebook, design);
    shoot(editor, output / "session-editor.png", QSize(640, 720));
  }

  {
    ui::AssistantPicker picker(notebook);
    shoot(picker, output / "assistant-picker.png", QSize(760, 520));
  }

  {
    lnb::net::SummaryPlan plan;
    plan.kind = "session memory";
    plan.automatic = false;
    plan.watermark_text = "Session memory watermark: 12 -> 18";

    QJsonArray messages;
    messages.append(QJsonObject{
        {"role", "system"},
        {"content",
         "You maintain a running memory of a long conversation between a user and "
         "an assistant. Rewrite the memory so it preserves every durable fact."},
    });
    messages.append(QJsonObject{
        {"role", "user"},
        {"content",
         "New conversation to fold into memory:\n\n"
         "User: Why can a long conversation truncate the model's reply?\n"
         "Assistant: Because the completion budget is shared with hidden reasoning."},
    });
    plan.request = QJsonObject{
        {"model", "gpt-5.6-luna"},
        {"max_tokens", 16000},
        {"messages", messages},
    };

    ui::SummaryReviewDialog dialog(plan);
    shoot(dialog, output / "summary-review.png", QSize(760, 640));
  }

  return EXIT_SUCCESS;
}
