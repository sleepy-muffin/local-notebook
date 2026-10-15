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
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFile>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleOptionViewItem>
#include <QToolButton>
#include <QWheelEvent>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "app/application.hpp"
#include "app/metrics.hpp"
#include "app/settings.hpp"
#include "model.hpp"
#include "store/util.hpp"
#include "ui/AssistantForm.hpp"
#include "ui/ConversationView.hpp"
#include "ui/MainWindow.hpp"
#include "ui/SessionEditor.hpp"
#include "ui/SessionItemDelegate.hpp"
#include "ui/SessionListModel.hpp"
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
  metrics.composer_height = 96;
  metrics.composer_height_expanded = 280;
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
    CHECK(loaded.metrics->composer_height == 96);
    CHECK(loaded.metrics->composer_height_expanded == 280);
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

  auto* title_bar = window.findChild<QWidget*>(QStringLiteral("TitleBar"));
  CHECK(title_bar != nullptr);
  if (title_bar != nullptr) {
    CHECK(title_bar->minimumHeight() == 38);
    CHECK(title_bar->maximumHeight() == 38);
    CHECK(title_bar->findChild<QLabel*>(QStringLiteral("Brand")) != nullptr);
  }

  // The flat sidebar: a New Session row (styled like a list item) above the
  // SESSIONS category and the list; the banner now lives in the unified title
  // bar, so the sidebar has no header band of its own.
  auto* new_session = window.findChild<QPushButton*>(QStringLiteral("NewSession"));
  CHECK(new_session != nullptr);

  // The foot-of-sidebar card shows the current assistant (name and model), not a
  // session.
  auto* current_name = window.findChild<QLabel*>(QStringLiteral("CurrentName"));
  CHECK(current_name != nullptr);
  if (current_name != nullptr) {
    CHECK(current_name->text() == QStringLiteral("Tester"));
  }

  auto* current_model = window.findChild<QLabel*>(QStringLiteral("CurrentModel"));
  CHECK(current_model != nullptr);
  if (current_model != nullptr) {
    CHECK(current_model->text() == QStringLiteral("test-model"));
  }

  // Each session row carries the time of its newest message on the second line.
  auto* sessions_view = window.findChild<QListView*>(QStringLiteral("Sessions"));
  CHECK(sessions_view != nullptr);
  if (sessions_view != nullptr && sessions_view->model() != nullptr &&
      sessions_view->model()->rowCount() == 1) {
    const QString time =
        sessions_view->model()->index(0, 0).data(ui::SessionListModel::TimeRole).toString();
    CHECK(!time.isEmpty());
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

// --- Assistant log ------------------------------------------------------------

// Opening an assistant redirects logging into its own file, which lives beside
// the assistant's settings and database.
void test_assistant_log_file() {
  const std::filesystem::path directory = enter_scratch("assistant-log");

  lnb::Application notebook;
  notebook.open_assistant("44444444-4444-4444-4444-444444444444", make_assistant());

  const std::filesystem::path path = directory / "data" /
      "44444444-4444-4444-4444-444444444444" / "local-notebook.log";

  CHECK(std::filesystem::exists(path));

  std::ifstream in(path);
  const std::string contents(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
  );

  // The line that records the opened assistant is written into the file.
  CHECK(
      contents.find("opened assistant 44444444-4444-4444-4444-444444444444") !=
      std::string::npos
  );
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

  // The row holds two lines (title and time), so its height never falls below the
  // configured metric even when that metric is set small.
  QStyleOptionViewItem hint_option;
  const QSize hint = delegate.sizeHint(hint_option, QModelIndex());
  CHECK(hint.height() >= ui::metrics().session_item_height);
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

// --- Settings window: reload, restore, presets -------------------------------

// A cancelled edit must not survive into the next visit: every show re-reads the
// stored settings.
void test_settings_reload_on_show(QApplication& app) {
  enter_scratch("settings-reload");
  ui::apply_theme(app, lnb::Settings{});

  lnb::Application notebook;
  notebook.open_assistant("55555555-5555-5555-5555-555555555555", make_assistant());

  ui::SettingsWindow window(notebook);

  auto* auto_login = window.findChild<QCheckBox*>(QStringLiteral("AutoLogin"));
  CHECK(auto_login != nullptr);
  if (auto_login == nullptr) {
    return;
  }

  window.show();
  QApplication::processEvents();
  CHECK(auto_login->isChecked());  // the stored default

  // An edit that is then cancelled.
  auto_login->setChecked(false);
  window.reject();
  QApplication::processEvents();

  // Reopening reloads from the stored settings, so the edit is gone.
  window.show();
  QApplication::processEvents();
  CHECK(auto_login->isChecked());

  window.close();
}

// "Restore to defaults" resets the page's own fields and never saves.
void test_settings_restore_defaults(QApplication& app) {
  enter_scratch("settings-restore");
  ui::apply_theme(app, lnb::Settings{});

  lnb::Application notebook;
  notebook.open_assistant("66666666-6666-6666-6666-666666666666", make_assistant());

  ui::SettingsWindow window(notebook);

  auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("SettingsStack"));
  CHECK(stack != nullptr);
  if (stack == nullptr) {
    return;
  }

  // Configuration page: the rendered-messages field returns to its default.
  QWidget* config_page = stack->widget(0);
  auto* rendered = config_page->findChild<QSpinBox*>(QStringLiteral("RenderedMessages"));
  auto* restore_config = config_page->findChild<QPushButton*>(QStringLiteral("RestoreConfig"));
  CHECK(rendered != nullptr);
  CHECK(restore_config != nullptr);
  if (rendered != nullptr && restore_config != nullptr) {
    rendered->setValue(9999);
    restore_config->click();
    CHECK(rendered->value() == lnb::Settings{}.rendered_message_limit);
  }

  // Metrics page: the fields return to the current theme's own defaults. The first
  // metric field is the sidebar width.
  QWidget* metrics_page = stack->widget(1);
  const QList<QSpinBox*> metric_spins = metrics_page->findChildren<QSpinBox*>();
  auto* restore_metrics = metrics_page->findChild<QPushButton*>(QStringLiteral("RestoreMetrics"));
  CHECK(!metric_spins.isEmpty());
  CHECK(restore_metrics != nullptr);
  if (!metric_spins.isEmpty() && restore_metrics != nullptr) {
    metric_spins.front()->setValue(metric_spins.front()->maximum());
    restore_metrics->click();
    CHECK(metric_spins.front()->value() == ui::theme().metrics.sidebar_width);
  }
}

// A preset only fills the fields; it never saves by itself.
void test_assistant_form_preset() {
  ui::AssistantForm form;

  auto* preset = form.findChild<QComboBox*>();
  CHECK(preset != nullptr);
  if (preset == nullptr) {
    return;
  }

  const QList<QSpinBox*> spins = form.findChildren<QSpinBox*>();
  CHECK(spins.size() == 4);
  if (spins.size() != 4) {
    return;
  }

  // Index 0 is the placeholder; "Balanced (default)" is the second real entry:
  // memory 4000, headroom 12000, trigger 80000, session pool 16000, in field order.
  preset->setCurrentIndex(2);
  CHECK(spins[0]->value() == 4000);
  CHECK(spins[1]->value() == 12000);
  CHECK(spins[2]->value() == 80000);
  CHECK(spins[3]->value() == 16000);
}

void test_session_editor_preset() {
  enter_scratch("session-preset");

  lnb::Application notebook;
  notebook.open_assistant("77777777-7777-7777-7777-777777777777", make_assistant());

  ui::SessionEditor editor(notebook, 0);

  auto* preset = editor.findChild<QComboBox*>();
  CHECK(preset != nullptr);
  if (preset == nullptr) {
    return;
  }

  // "Large codebase analysis" (index 6) sets the trigger to 96000 and the tool-call
  // budget to 128, which no other field holds.
  preset->setCurrentIndex(6);

  bool found_trigger = false;
  bool found_calls = false;
  const QList<QSpinBox*> spins = editor.findChildren<QSpinBox*>();
  for (const QSpinBox* spin : spins) {
    if (spin->value() == 96000) {
      found_trigger = true;
    }
    if (spin->value() == 128) {
      found_calls = true;
    }
  }
  CHECK(found_trigger);
  CHECK(found_calls);
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

void test_font_combo_wheel() {
  ui::FontComboBox combo;

  const auto wheel = [](int delta) {
    return QWheelEvent(
        QPointF(10, 10), QPointF(10, 10), QPoint(0, 0), QPoint(0, delta), Qt::NoButton,
        Qt::NoModifier, Qt::NoScrollPhase, false
    );
  };

  // The family under the cursor is the current one; crossing the field while
  // scrolling must never switch it, in either direction.
  const QString before = combo.currentFont().family();

  QWheelEvent up = wheel(120);
  QApplication::sendEvent(&combo, &up);
  CHECK(combo.currentFont().family() == before);

  QWheelEvent down = wheel(-120);
  QApplication::sendEvent(&combo, &down);
  CHECK(combo.currentFont().family() == before);

  CHECK(!up.isAccepted());
  CHECK(!down.isAccepted());
}

// --- Translucent theme --------------------------------------------------------

// Writes a theme file with one opaque and one translucent variant, so a theme
// change can be observed without shipping the real themes/theme.json into the
// sandbox.
void write_probe_themes() {
  std::filesystem::create_directories("themes");
  std::ofstream out("themes/theme.json");
  out << R"({
  "themes": {
    "Opaque": {
      "dark": true,
      "colors": {
        "window": [17, 17, 27, 1.0],
        "surface": [24, 24, 37, 1.0],
        "base": [30, 30, 46, 1.0],
        "text": [205, 214, 244, 1.0],
        "text_muted": [166, 173, 200, 1.0],
        "accent": [245, 194, 231, 1.0],
        "accent_hover": [242, 205, 205, 1.0],
        "accent_active": [235, 160, 172, 1.0],
        "accent_text": [17, 17, 27, 1.0],
        "bubble_user": [49, 50, 68, 1.0],
        "bubble_user_text": [205, 214, 244, 1.0],
        "bubble_assistant": [49, 50, 68, 1.0],
        "bubble_assistant_text": [205, 214, 244, 1.0],
        "bubble_assistant_code": [49, 50, 68, 1.0],
        "conversation_background": [70, 80, 90, 1.0],
        "input_background": [100, 110, 120, 1.0],
        "border": [69, 71, 90, 1.0],
        "danger": [243, 139, 168, 1.0],
        "danger_text": [17, 17, 27, 1.0]
      }
    },
    "Translucent": {
      "dark": true,
      "colors": {
        "window": [17, 17, 27, 1.0],
        "surface": [24, 24, 37, 0.25],
        "base": [30, 30, 46, 0.38],
        "text": [205, 214, 244, 1.0],
        "text_muted": [166, 173, 200, 0.85],
        "accent": [245, 194, 231, 1.0],
        "accent_hover": [242, 205, 205, 1.0],
        "accent_active": [235, 160, 172, 1.0],
        "accent_text": [17, 17, 27, 1.0],
        "bubble_user": [49, 50, 68, 0.5],
        "bubble_user_text": [205, 214, 244, 1.0],
        "bubble_assistant": [49, 50, 68, 0.5],
        "bubble_assistant_text": [205, 214, 244, 1.0],
        "bubble_assistant_code": [49, 50, 68, 0.5],
        "conversation_background": [70, 80, 90, 0.0],
        "input_background": [100, 110, 120, 0.0],
        "border": [69, 71, 90, 0.35],
        "danger": [243, 139, 168, 1.0],
        "danger_text": [17, 17, 27, 1.0]
      }
    }
  },
  "default_metrics": {}
}
)";
}

// Buttons inherit the theme's alpha through the global stylesheet. It used to
// emit QColor::name() for their backgrounds, which silently dropped it and left
// "New Session" and "Send" opaque on translucent themes.
void test_translucent_theme_buttons(QApplication& app) {
  enter_scratch("translucent-buttons");
  write_probe_themes();

  lnb::Settings settings;
  settings.theme = "Translucent";
  ui::apply_theme(app, settings);

  const QString sheet = app.styleSheet();
  const int start = sheet.indexOf(QStringLiteral("QPushButton {"));
  CHECK(start >= 0);
  if (start >= 0) {
    // The button background must keep the theme's alpha (0.38 for base).
    const QString rule = sheet.mid(start, 240);
    CHECK(rule.contains(QStringLiteral("rgba(")));
    CHECK(!rule.contains(QStringLiteral("#")));
  }
}

// A fenced code block carries its background in a stylesheet rather than in the
// rendered HTML, so a live theme change must restyle the frame explicitly; an
// in-place text update never rebuilds it.
void test_code_block_restyles_on_theme_change(QApplication& app) {
  enter_scratch("code-restyle");
  write_probe_themes();

  lnb::Settings opaque;
  opaque.theme = "Opaque";
  ui::apply_theme(app, opaque);

  ui::MessageBubble bubble(ui::MessageBubble::Role::Assistant);
  bubble.set_text(QStringLiteral("```\nint x = 0;\n```"));

  auto* frame = bubble.findChild<QFrame*>(QStringLiteral("CodeBlock"));
  CHECK(frame != nullptr);
  if (frame == nullptr) {
    return;
  }

  const QString before = frame->styleSheet();
  CHECK(before.contains(QStringLiteral("rgba(")));
  CHECK(before.contains(QStringLiteral("255")));

  lnb::Settings translucent;
  translucent.theme = "Translucent";
  ui::apply_theme(app, translucent);
  bubble.refresh_style();

  const QString after = frame->styleSheet();
  CHECK(after != before);
  CHECK(after.contains(QStringLiteral("127")));
}

// --- Conversation / input backgrounds ----------------------------------------

// The conversation pane and the composer read their own theme colours, which
// default to the window colour. On translucent themes both are fully
// transparent, so the background image (or tint) shows through.
void test_conversation_backgrounds(QApplication& app) {
  enter_scratch("conversation-backgrounds");
  write_probe_themes();

  lnb::Settings opaque;
  opaque.theme = "Opaque";
  ui::apply_theme(app, opaque);
  CHECK(ui::theme().colors.conversation_background == QColor(70, 80, 90, 255));
  CHECK(ui::theme().colors.input_background == QColor(100, 110, 120, 255));

  lnb::Settings translucent;
  translucent.theme = "Translucent";
  ui::apply_theme(app, translucent);
  CHECK(ui::theme().colors.conversation_background.alpha() == 0);
  CHECK(ui::theme().colors.input_background.alpha() == 0);
}

// --- Bundled resources -------------------------------------------------------

// The composer icons and the default theme are compiled into the binary, so a
// deployed build needs neither loose files beside the executable nor the system
// icon theme.
void test_bundled_resources() {
  CHECK(QFile::exists(QStringLiteral(":/icons/send.png")));
  CHECK(QFile::exists(QStringLiteral(":/icons/expand.png")));
  CHECK(QFile::exists(QStringLiteral(":/icons/collapse.png")));
  CHECK(QFile::exists(QStringLiteral(":/icons/stop.png")));
  CHECK(QFile::exists(QStringLiteral(":/themes/theme.json")));

  // The bundled theme carries real content, not an empty stub.
  QFile bundled_theme(QStringLiteral(":/themes/theme.json"));
  CHECK(bundled_theme.open(QIODevice::ReadOnly));
  CHECK(bundled_theme.readAll().contains("\"themes\""));

  const QIcon send = ui::resource_icon(
      QStringLiteral("send"), QColor(255, 0, 0), QColor(0, 255, 0)
  );
  CHECK(!send.isNull());
  CHECK(!send.pixmap(16, 16).isNull());

  // A name with no bundled artwork yields a null icon, so the caller falls back
  // to the system theme or a standard icon.
  CHECK(ui::resource_icon(QStringLiteral("no-such-icon"), QColor(0, 0, 0), QColor(0, 0, 0))
            .isNull());
}

// --- Composer ----------------------------------------------------------------

// The expand toggle raises the field to composer_height_expanded and back, and
// both composer actions are icon-only buttons carrying the bundled artwork.
void test_composer_expand_toggle() {
  enter_scratch("composer-expand");

  lnb::Application notebook;
  notebook.open_assistant("11111111-1111-1111-1111-111111111111", make_assistant());
  const std::uint32_t session_id = notebook.sessions().create(lnb::Session{});

  ui::ConversationView view(notebook, session_id);

  auto* input = view.findChild<QPlainTextEdit*>(QStringLiteral("Composer"));
  auto* expand = view.findChild<QToolButton*>(QStringLiteral("ComposerExpand"));
  auto* send = view.findChild<QToolButton*>(QStringLiteral("SendButton"));
  auto* cancel = view.findChild<QToolButton*>(QStringLiteral("CancelButton"));

  CHECK(input != nullptr);
  CHECK(expand != nullptr);
  CHECK(send != nullptr);
  CHECK(cancel != nullptr);

  if (input == nullptr || expand == nullptr || send == nullptr || cancel == nullptr) {
    return;
  }

  // setFixedHeight pins both bounds, so this holds before the view is shown.
  const int collapsed = ui::metrics().composer_height;
  const int raised = ui::metrics().composer_height_expanded;

  CHECK(input->minimumHeight() == collapsed);
  CHECK(input->maximumHeight() == collapsed);

  expand->click();
  CHECK(input->minimumHeight() == raised);
  CHECK(input->maximumHeight() == raised);

  expand->click();
  CHECK(input->minimumHeight() == collapsed);

  // Both actions are icon-only and carry the bundled (non-null) artwork.
  CHECK(!send->icon().isNull());
  CHECK(!expand->icon().isNull());
  CHECK(send->text().isEmpty());

  // Cancel is the send action's peer: a same-sized icon-only tool button with
  // its own bundled artwork, so the three read as one control cluster.
  CHECK(!cancel->icon().isNull());
  CHECK(cancel->text().isEmpty());
  CHECK(send->size() == expand->size());
  CHECK(send->size() == cancel->size());
  CHECK(send->size() == QSize(28, 28));
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
  test_font_combo_wheel();

  // Widgets read the theme, so it must be installed before any is created. The
  // sandbox has no theme.json, so this exercises the theme compiled into the
  // binary.
  ui::apply_theme(app, lnb::Settings{});

  test_main_window_invariants();
  test_session_list_populated_on_startup();
  test_assistant_log_file();
  test_session_action_click_flag();
  test_settings_window_nav();
  test_settings_reload_on_show(app);
  test_settings_restore_defaults(app);
  test_assistant_form_preset();
  test_session_editor_preset();
  test_translucent_theme_buttons(app);
  test_code_block_restyles_on_theme_change(app);
  test_conversation_backgrounds(app);
  test_bundled_resources();
  test_composer_expand_toggle();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }

  std::printf("All UI tests passed.\n");
  return EXIT_SUCCESS;
}
