#pragma once

// Small shared helpers for the application's dialogs. They keep the dialogs'
// chrome — headings, field labels, hairlines, form spacing — identical, so every
// window reads the same way (design-1.md §12: chrome drawn consistently, QSS kept
// to small scoped pieces).

#include <QColor>
#include <QFont>
#include <QFormLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QSpinBox>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <string>

#include "ui/SpinBox.hpp"
#include "ui/theme.hpp"

namespace ui::dialog_style {

inline QString to_qstring(const std::string& value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

inline QString colour_hex(const QColor& colour) {
  return colour.name(QColor::HexRgb);
}

// The scoped stylesheet for one dialog. Scoping every rule by the dialog's object
// name keeps it from leaking into the rest of the application.
inline QString dialog_stylesheet(const QString& object_name) {
  const ThemeColors& c = theme().colors;

  const QString window = colour_hex(c.window);
  const QString surface = colour_hex(c.surface);
  const QString base = colour_hex(c.base);
  const QString text = colour_hex(c.text);
  const QString muted = colour_hex(c.text_muted);
  const QString border = colour_hex(c.border);
  const QString accent = colour_hex(c.accent);
  const QString accent_hover = colour_hex(c.accent_hover);
  const QString accent_active = colour_hex(c.accent_active);
  const QString accent_text = colour_hex(c.accent_text);

  QString sheet;

  sheet += QStringLiteral("QDialog#%1 { background: %2; }").arg(object_name, window);
  sheet += QStringLiteral("QLabel { background: transparent; }");
  sheet += QStringLiteral(
               "QLabel#PickerTitle, QLabel#FormTitle, QLabel#DialogTitle,"
               " QLabel#PreviewName, QLabel#PageTitle { color: %1; }")
               .arg(text);
  sheet += QStringLiteral(
               "QLabel#PickerSubtitle, QLabel#FormSubtitle, QLabel#DialogSubtitle,"
               " QLabel#PreviewModel, QLabel#PreviewHint, QLabel#FieldLabel,"
               " QLabel#SectionLabel, QLabel#PageSubtitle, QLabel#Muted { color: %1; }")
               .arg(muted);

  sheet += QStringLiteral("QWidget#Separator { background: %1; }").arg(border);
  sheet += QStringLiteral("QWidget#Hairline { background: %1; }").arg(border);

  sheet += QStringLiteral(
               "QListWidget#AssistantList, QListWidget#SettingsNav { background: transparent;"
               " border: none; outline: none; }");
  sheet += QStringLiteral(
               "QScrollArea { background: transparent; border: none; }"
               "QScrollArea > QWidget > QWidget { background: transparent; }");

  sheet += QStringLiteral(
               "QLineEdit, QPlainTextEdit, QSpinBox, QComboBox {"
               "  background: %1; border: 1px solid %2; border-radius: 6px;"
               "  padding: 5px 8px; selection-background-color: %3; selection-color: %4; }")
               .arg(base, border, accent, accent_text);
  sheet += QStringLiteral(
               "QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QComboBox:focus {"
               "  border: 1px solid %1; }")
               .arg(accent);

  sheet += QStringLiteral(
               "QPushButton#Primary { background: %1; color: %2; border: none;"
               "  border-radius: 6px; padding: 6px 18px; }")
               .arg(accent, accent_text);
  sheet += QStringLiteral("QPushButton#Primary:hover { background: %1; }").arg(accent_hover);
  sheet += QStringLiteral("QPushButton#Primary:pressed { background: %1; }").arg(accent_active);
  sheet += QStringLiteral("QPushButton#Primary:disabled { background: %1; color: %2; }")
               .arg(surface, muted);

  sheet += QStringLiteral(
               "QPushButton#Secondary { background: transparent; color: %1;"
               "  border: 1px solid %2; border-radius: 6px; padding: 6px 14px; }")
               .arg(text, border);
  sheet += QStringLiteral("QPushButton#Secondary:hover { background: %1; }").arg(surface);
  sheet += QStringLiteral("QPushButton#Secondary:disabled { color: %1; }").arg(muted);

  sheet += QStringLiteral("QCheckBox { color: %1; }").arg(text);

  return sheet;
}

inline QFont scaled_font(double delta, bool bold) {
  QFont font = ui_font();
  if (font.pointSizeF() > 1.0) {
    font.setPointSizeF(font.pointSizeF() + delta);
  }
  if (bold) {
    font.setWeight(QFont::DemiBold);
  }
  return font;
}

inline QLabel* section_label(QWidget* parent, const QString& text) {
  auto* label = new QLabel(text, parent);
  label->setObjectName(QStringLiteral("SectionLabel"));
  label->setFont(scaled_font(0.0, true));
  return label;
}

inline QLabel* field_label(QWidget* parent, const QString& text) {
  auto* label = new QLabel(text, parent);
  label->setObjectName(QStringLiteral("FieldLabel"));
  return label;
}

inline QFormLayout* form_layout(QWidget* parent) {
  Q_UNUSED(parent);

  // Built without a parent widget: the caller inserts it into another layout, and
  // installing it on a widget first would make Qt warn about a duplicate layout.
  auto* layout = new QFormLayout();
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setHorizontalSpacing(16);
  layout->setVerticalSpacing(10);
  layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  layout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  layout->setRowWrapPolicy(QFormLayout::DontWrapRows);
  return layout;
}

// A hairline drawn as a styled QWidget, so it is exactly one pixel and uses the
// theme's border colour.
inline QWidget* h_separator(QWidget* parent) {
  auto* line = new QWidget(parent);
  line->setObjectName(QStringLiteral("Separator"));
  line->setAttribute(Qt::WA_StyledBackground, true);
  line->setFixedHeight(1);
  line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  return line;
}

inline QWidget* v_separator(QWidget* parent) {
  auto* line = new QWidget(parent);
  line->setObjectName(QStringLiteral("Separator"));
  line->setAttribute(Qt::WA_StyledBackground, true);
  line->setFixedWidth(1);
  line->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  return line;
}

inline QSpinBox* make_spin(QWidget* parent, std::size_t value) {
  // A wheel-safe spin box: crossing the field while scrolling must not change it.
  auto* spin = new SpinBox(parent);
  spin->setRange(0, 1'000'000'000);
  spin->setSingleStep(500);
  spin->setGroupSeparatorShown(true);
  spin->setValue(static_cast<int>(value));
  return spin;
}

} // namespace ui::dialog_style
