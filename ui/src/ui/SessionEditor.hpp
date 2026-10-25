#pragma once

#include <QDialog>

#include <cstdint>

#include "local-notebook-core/model.hpp"

class QComboBox;
class QCheckBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QWidget;

namespace lnb {
namespace api {
class interface;
}
}

namespace ui {
class PlainTextEdit;

class ConversationView;

// Edits one session: its title, custom instructions, shared directories, memory
// and every session-bound budget (design-1.md §8). Like the settings window, the
// dialog is a left-hand navigation rail over three stacked pages — General, Memory
// and Tools — and the memory box is directly editable, with an "Update memory
// now" action that rebuilds it on demand.
//
// Pass a session_id of 0 to create a new session; the accepted id is exposed by
// session_id().
class SessionEditor : public QDialog {
public:
  SessionEditor(
      lnb::api::interface& api, std::uint32_t session_id,
      QWidget* parent = nullptr
  );

  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }

  // The conversation view that owns the session's background worker, when one is
  // available. A manual memory update then runs as the session's own task, so it
  // appears in the tasks window and can never race a generation on the same
  // session. Unset for a brand-new session, whose update action is disabled
  // because there is nothing stored to merge.
  void set_conversation_view(ConversationView* view) { conversation_view_ = view; }

private:
  // The navigation rail and the stacked pages it selects, built like the
  // settings window's body.
  QWidget* build_body();
  QWidget* build_general_tab();
  QWidget* build_memory_tab();
  QWidget* build_tools_tab();

  // Puts one page's content inside a scroll area, matching the window's own
  // scrollbar and margins.
  QWidget* scrolled_tab(QWidget* content);

  // The dialog chrome plus the navigation rail's row states.
  QString session_stylesheet() const;

  void load_session();
  void save_session();

  // Rebuilds the reasoning list for the assistant's current model, keeping
  // `preferred` visible even when the model no longer offers it.
  void populate_reasoning(const std::string& preferred);

  // Merges the whole transcript into the memory box on demand, regardless of the
  // automatic trigger. Runs as the session's own background task (through the
  // conversation view) and reloads the persisted memory when it finishes.
  void summarize_now();

  lnb::api::interface& api_;

  // 0 while creating; the stored id once the session exists.
  std::uint32_t session_id_ = 0;
  lnb::Session session_;

  QLineEdit* title_ = nullptr;
  QPlainTextEdit* instructions_ = nullptr;

  // One directory per line; the order sets the mount name when directories
  // share a basename (the second becomes e.g. "project-2").
  PlainTextEdit* shared_directories_ = nullptr;

  // Tool permissions for this session. The master switch gates all of them; the
  // three category switches keep their values while it is off.
  QCheckBox* tools_enabled_ = nullptr;
  QCheckBox* read_tools_ = nullptr;
  QCheckBox* write_tools_ = nullptr;
  QCheckBox* online_tools_ = nullptr;

  // The reasoning level this session asks for. The list is model-specific; a
  // stale level stays selectable so it can be corrected rather than dropped.
  QComboBox* reasoning_ = nullptr;

  PlainTextEdit* memory_ = nullptr;
  QSpinBox* watermark_ = nullptr;
  QSpinBox* assistant_watermark_ = nullptr;

  // When unchecked, this session is left out of the shared assistant memory.
  QCheckBox* include_in_assistant_memory_ = nullptr;

  QPushButton* summarize_button_ = nullptr;
  QPushButton* save_button_ = nullptr;
  QPushButton* cancel_button_ = nullptr;

  // Fill the fields of their own page with a preset; neither saves by itself.
  QComboBox* memory_preset_ = nullptr;
  QComboBox* tools_preset_ = nullptr;

  // Session-bound memory-update settings and budgets.
  QSpinBox* summary_keep_ = nullptr;
  QSpinBox* summary_trigger_ = nullptr;
  QSpinBox* summary_budget_ = nullptr;
  QSpinBox* summary_headroom_ = nullptr;
  QSpinBox* midturn_input_budget_ = nullptr;
  QSpinBox* response_budget_ = nullptr;

  // The page rail and the pages it selects.
  QListWidget* nav_ = nullptr;
  QStackedWidget* stack_ = nullptr;

  // The session's worker owner, when the editor was given one. Null for a new
  // session.
  ConversationView* conversation_view_ = nullptr;

  // True while a manual memory update is running, so the action cannot be
  // started twice.
  bool summarizing_ = false;
};

} // namespace ui
