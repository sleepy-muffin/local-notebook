// Offscreen UI tests for the reconstructed interface (design-1.md).
//
// Dependency-free, like the store tests: a tiny CHECK macro and a main() that
// returns non-zero on the first failure. Everything runs under the offscreen
// platform plugin and inside a scratch working directory, so the real data/ tree
// is never touched.
//
// Run via CTest (`ctest --test-dir build/linux-x64-debug`) or directly with
// QT_QPA_PLATFORM=offscreen set.

#include <QApplication>
#include <QFrame>
#include <QListView>
#include <QListWidget>
#include <QMouseEvent>
#include <QPoint>
#include <QRect>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleOptionViewItem>
#include <QWheelEvent>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "app/application.hpp"
#include "app/metrics.hpp"
#include "app/settings.hpp"
#include "model.hpp"
#include "store/util.hpp"
#include "ui/AssistantForm.hpp"
#include "ui/MainWindow.hpp"
#include "ui/SessionItemDelegate.hpp"
#include "ui/SettingsWindow.hpp"
#include "ui/SpinBox.hpp"
#include "ui/theme.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

std::filesystem::path g_original_directory;

// Creates a fresh directory under the system temp root and makes it the current
// directory, so anything that resolves paths relatively (data/, themes/) lands
// in the sandbox.
std::filesystem::path enter_scratch(const std::string& name) {
  const std::filesystem::path directory =
    std::filesystem::temp_directory_path() /
    ("lnb-ui-" + name + "-" + lnb::store::make_uuid());
  std::filesystem::create_directories(directory);
  std::filesystem::current_path(directory);
  return directory;
}

// An assistant whose only meaningful field for these tests is its identity.
lnb::Assistant make_assistant() {
  lnb::Assistant assistant;
  assistant.name_ = "Tester";
  assistant.model_ = "test-model";
  assistant.api_url_ = "https://example.invalid/v1";
  return assistant;
}

// --- Settings persistence -----------------------------------------------------

void test_settings_round_trip() {
  enter_scratch("settings");

  lnb::Settings original;
  original.auto_login = false;
  original.rendered_message_limit = 7;
  original.history_page_size = 11;
  original.enter_with_modifier = false;

  lnb::ThemeMetrics metrics;
  metrics.sidebar_width = 301;
  metrics.bubble_radius = 13;
  metrics.line_height_self = 135;
  metrics.message_max_width = 512;
  original.metrics = metrics;

  lnb::save_settings(original);

  const lnb::Settings loaded = lnb::load_settings();
  CHECK(loaded.auto_login == false);
  CHECK(loaded.rendered_message_limit == 7);
  CHECK(loaded.history_page_size == 11);
  CHECK(loaded.enter_with_modifier == false);

  CHECK(loaded.metrics.has_value());
  if (loaded.metrics.has_value()) {
    CHECK(loaded.metrics->sidebar_width == 301);
    CHECK(loaded.metrics->bubble_radius == 13);
    CHECK(loaded.metrics->line_height_self == 135);
    CHECK(loaded.metrics->message_max_width == 512);
  }

  // auto_login defaults to on when the key is absent.
  CHECK(lnb::Settings{}.auto_login == true);
}

// --- AssistantForm ------------------------------------------------------------

void test_assistant_form_round_trip() {
  ui::AssistantForm form;

  lnb::Assistant original;
  original.name_ = "Alpha";
  original.model_ = "model-x";
  original.api_url_ = "https://example.invalid/v1";
  original.authentication_token_ = "secret";
  original.proxy_ = "socks5h://127.0.0.1:1080";
  original.custom_instructions_ = "Be terse.";
  original.summary_token_budget_ = 111;
  original.summary_reasoning_headroom_tokens_ = 222;
  original.summary_trigger_tokens_ = 333;
  original.summary_token_budget_for_session_summarization_ = 444;
  original.memory_ = "Remembered across sessions.";

  form.set_assistant(original);

  // Layer the form over an existing assistant: every field the form owns is
  // overwritten, and every field it does not own survives.
  lnb::Assistant written;
  written.memory_ = "Remembered across sessions.";

  form.write_into(written);

  CHECK(written.name_ == "Alpha");
  CHECK(written.model_ == "model-x");
  CHECK(written.api_url_ == "https://example.invalid/v1");
  CHECK(written.authentication_token_ == "secret");
  CHECK(written.proxy_.has_value() && *written.proxy_ == "socks5h://127.0.0.1:1080");
  CHECK(written.custom_instructions_ == "Be terse.");
  CHECK(written.summary_token_budget_ == 111);
  CHECK(written.summary_reasoning_headroom_tokens_ == 222);
  CHECK(written.summary_trigger_tokens_ == 333);
  CHECK(written.summary_token_budget_for_session_summarization_ == 444);

  // The one field the form must never touch.
  CHECK(written.memory_ == "Remembered across sessions.");

  // An emptied name falls back to a placeholder rather than persisting blank.
  form.set_assistant(lnb::Assistant{});
  lnb::Assistant blank;
  form.write_into(blank);
  CHECK(blank.name_ == "Untitled assistant");
  CHECK(!blank.proxy_.has_value());
}

// --- Main window chrome -------------------------------------------------------

void test_main_window_invariants() {
  enter_scratch("main-window");

  lnb::Application notebook;
  notebook.open_assistant("11111111-1111-1111-1111-111111111111", make_assistant());

  const std::uint32_t session_id = notebook.sessions().create(lnb::Session{});
  notebook.messages(session_id).append(lnb::Role::User, "Hello there.");
  notebook.messages(session_id).append(lnb::Role::Assistant, "Hi! How can I help?");
  notebook.messages(session_id).append(
      lnb::Role::User, "Tell me a short story about a very small robot."
  );

  ui::MainWindow window(notebook);
  window.resize(1080, 720);
  window.show();
  QApplication::processEvents();

  auto* sidebar = window.findChild<QWidget*>(QStringLiteral("Sidebar"));
  CHECK(sidebar != nullptr);
  if (sidebar != nullptr) {
    // The sidebar is pinned to the metric width, so it can never drift.
    CHECK(sidebar->minimumWidth() == ui::metrics().sidebar_width);
    CHECK(sidebar->maximumWidth() == ui::metrics().sidebar_width);
  }

  auto* separator = window.findChild<QWidget*>(QStringLiteral("SidebarSeparator"));
  CHECK(separator != nullptr);
  if (separator != nullptr) {
    CHECK(separator->minimumWidth() == 1);
    CHECK(separator->maximumWidth() == 1);
  }

  auto* header = window.findChild<QWidget*>(QStringLiteral("Header"));
  CHECK(header != nullptr);
  if (header != nullptr) {
    CHECK(header->minimumHeight() == 38);
    CHECK(header->maximumHeight() == 38);
  }

  const QList<QFrame*> bubbles =
    window.findChildren<QFrame*>(QStringLiteral("MessageBubble"));
  CHECK(bubbles.size() == 3);

  if (bubbles.size() >= 2) {
    auto* transcript = window.findChild<QWidget*>(QStringLiteral("TranscriptInner"));
    CHECK(transcript != nullptr);

    // Every message body is the same width and shares one left edge: a message's
    // size never depends on how much was written.
    const int width = bubbles.front()->width();
    const int left = transcript != nullptr
        ? bubbles.front()->mapTo(transcript, QPoint(0, 0)).x()
        : 0;

    CHECK(width > 0);

    for (const QFrame* bubble : bubbles) {
      CHECK(bubble->width() == width);
      if (transcript != nullptr) {
        CHECK(bubble->mapTo(transcript, QPoint(0, 0)).x() == left);
      }
    }
  }
}

// --- Startup sidebar ----------------------------------------------------------

// The sidebar is populated from the database as soon as the window is built; it
// used to stay empty until a session was added.
void test_session_list_populated_on_startup() {
  enter_scratch("startup-list");

  lnb::Application notebook;
  notebook.open_assistant("33333333-3333-3333-3333-333333333333", make_assistant());

  notebook.sessions().create(lnb::Session{});
  notebook.sessions().create(lnb::Session{});
  notebook.sessions().create(lnb::Session{});

  ui::MainWindow window(notebook);
  window.resize(1080, 720);
  window.show();
  QApplication::processEvents();

  auto* view = window.findChild<QListView*>(QStringLiteral("Sessions"));
  CHECK(view != nullptr);

  if (view != nullptr && view->model() != nullptr) {
    CHECK(view->model()->rowCount() == 3);
  }
}

// --- Session row actions ------------------------------------------------------

// A release on a row's edit/delete icon is claimed by the delegate and flagged,
// so the owning view can tell it apart from a click on the row body (which opens
// the session). The icons themselves come from the icon theme.
void test_session_action_click_flag() {
  ui::SessionItemDelegate delegate;

  const QRect row(0, 0, 200, 36);
  QStyleOptionViewItem option;
  option.rect = row;

  const auto release_at = [&](const QPointF& position) {
    QMouseEvent event(
        QEvent::MouseButtonRelease, position, position, Qt::LeftButton, Qt::LeftButton,
        Qt::NoModifier
    );
    return delegate.editorEvent(&event, nullptr, option, QModelIndex());
  };

  // A release inside the edit icon is claimed and flagged.
  CHECK(release_at(delegate.edit_button_rect(option).center()));
  CHECK(delegate.take_action_click());
  CHECK(!delegate.take_action_click()); // reading the flag resets it

  // The delete icon behaves the same way.
  CHECK(release_at(delegate.delete_button_rect(option).center()));
  CHECK(delegate.take_action_click());

  // A release on the row body is not an action click.
  CHECK(!release_at(QPointF(20, 18)));
  CHECK(!delegate.take_action_click());
}

// --- Settings window ----------------------------------------------------------

void test_settings_window_nav() {
  enter_scratch("settings-window");

  lnb::Application notebook;
  notebook.open_assistant("22222222-2222-2222-2222-222222222222", make_assistant());

  ui::SettingsWindow window(notebook);

  auto* nav = window.findChild<QListWidget*>(QStringLiteral("SettingsNav"));
  CHECK(nav != nullptr);
  if (nav != nullptr) {
    CHECK(nav->count() == 5);
    CHECK(nav->item(0)->text() == QStringLiteral("Configuration"));
    CHECK(nav->item(1)->text() == QStringLiteral("Metrics"));
    CHECK(nav->item(2)->text() == QStringLiteral("Assistant"));
    CHECK(nav->item(3)->text() == QStringLiteral("Account"));
    CHECK(nav->item(4)->text() == QStringLiteral("About"));
  }

  auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("SettingsStack"));
  CHECK(stack != nullptr);
  if (stack != nullptr) {
    // One page per navigation entry, in the same order.
    CHECK(stack->count() == 5);
  }

  // The Assistant page embeds the shared form, so the picker and the settings
  // window can never drift apart.
  CHECK(window.findChild<QWidget*>(QStringLiteral("AssistantFormWidget")) != nullptr);

  window.resize(780, 580);
  window.show();
  QApplication::processEvents();

  // No page's content may be wider than its viewport, or the (hidden) horizontal
  // scrollbar would clip the right-hand column. Each page is activated first,
  // since a hidden page in the stack is never laid out.
  if (stack != nullptr) {
    for (int i = 0; i < stack->count(); ++i) {
      stack->setCurrentIndex(i);
      QApplication::processEvents();

      QWidget* page = stack->widget(i);
      const QList<QScrollArea*> areas = page->findChildren<QScrollArea*>();
      for (const QScrollArea* area : areas) {
        const QWidget* inner = area->widget();
        if (inner == nullptr || area->viewport()->width() <= 0) {
          continue;
        }
        CHECK(inner->minimumSizeHint().width() <= area->viewport()->width());
      }
    }
  }
}

// --- Wheel-safe spin boxes ----------------------------------------------------

void test_spin_box_wheel() {
  ui::SpinBox spin;
  spin.setRange(0, 1000);
  spin.setSingleStep(10);
  spin.setValue(100);

  const auto wheel = [](int delta) {
    return QWheelEvent(
        QPointF(10, 10), QPointF(10, 10), QPoint(0, 0), QPoint(0, delta), Qt::NoButton,
        Qt::NoModifier, Qt::NoScrollPhase, false
    );
  };

  // Crossing the field while scrolling must never change the value, in either
  // direction.
  QWheelEvent up = wheel(120);
  QApplication::sendEvent(&spin, &up);
  CHECK(spin.value() == 100);

  QWheelEvent down = wheel(-120);
  QApplication::sendEvent(&spin, &down);
  CHECK(spin.value() == 100);

  // The event is ignored (not consumed), so an enclosing scroll area still scrolls.
  CHECK(!up.isAccepted());
  CHECK(!down.isAccepted());
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Local Notebook"));
  QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

  // Restore the working directory even if an assertion below fails.
  struct restore_working_directory {
    std::filesystem::path path;
    ~restore_working_directory() {
      std::error_code error;
      std::filesystem::current_path(path, error);
    }
  } guard{std::filesystem::current_path()};

  test_settings_round_trip();
  test_assistant_form_round_trip();
  test_spin_box_wheel();

  // Widgets read the theme, so it must be installed before any is created. With
  // no theme.json in the sandbox, this exercises the compiled-in fallback.
  ui::apply_theme(app, lnb::Settings{});

  test_main_window_invariants();
  test_session_list_populated_on_startup();
  test_session_action_click_flag();
  test_settings_window_nav();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }

  std::printf("All UI tests passed.\n");
  return EXIT_SUCCESS;
}
