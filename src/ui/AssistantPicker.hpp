#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QCheckBox;
class QListWidget;
class QPushButton;

namespace lnb {
class Application;
}

namespace ui {

// The startup assistant picker: choose an existing assistant, or create a new
// one, before the main window opens.
//
// Only each assistant's name and model are shown here. The API URL and the
// authentication token are deliberately never displayed in this window — they
// are edited solely inside the (separate) assistant form.
class AssistantPicker : public QDialog {
public:
  explicit AssistantPicker(lnb::Application& app, QWidget* parent = nullptr);

private:
  // Rebuilds the list from disk and selects the last-opened assistant.
  void refresh();
  // Updates the right-hand preview from the current selection.
  void update_preview();
  // Opens the selected assistant into the application and accepts the dialog.
  void open_selected();
  // Edits the selected assistant in place, reusing its UUID.
  void edit_selected();
  // Creates a fresh assistant under a new UUID.
  void create_new();

  [[nodiscard]] QString selected_uuid() const;
  void select_uuid(const QString& uuid);

  lnb::Application& app_;

  QListWidget* list_ = nullptr;
  QLabel* preview_name_ = nullptr;
  QLabel* preview_model_ = nullptr;
  QLabel* preview_hint_ = nullptr;
  QCheckBox* auto_login_ = nullptr;
  QPushButton* edit_button_ = nullptr;
  QPushButton* open_button_ = nullptr;
};

} // namespace ui
