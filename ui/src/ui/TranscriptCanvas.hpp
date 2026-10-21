#pragma once

#include <QAbstractScrollArea>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "render/layout.hpp"

class QKeyEvent;
class QPaintEvent;
class QResizeEvent;

namespace ui {

// The whole conversation surface as one custom-painted, scrollable canvas.
//
// Messages are not widgets: the Qt-free renderer lays out paragraphs, wraps
// lines, resolves selection and decides the paint order, while this widget only
// supplies a QPainter backend and the viewport-sized window onto the document.
// The advantages over a widget per message are direct: there is no widget tree
// to rebuild, no QTextDocument->HTML->QLabel round trip, hit testing is a
// binary-readable scan over lines, and a selection can cross every block because
// all text lives on the same surface.
class TranscriptCanvas : public QAbstractScrollArea {
public:
  // A stable place in the transcript, used to keep the reader's position across
  // a rebuild without guessing at heights. `viewport_offset` is the distance in
  // pixels from the viewport's top edge to the message's top edge.
  struct ScrollAnchor {
    bool valid = false;
    std::uint32_t message_id = 0;
    float viewport_offset = 0.0f;
  };

  explicit TranscriptCanvas(QWidget* parent = nullptr);
  ~TranscriptCanvas() override;

  // Re-reads the current theme, fonts and metrics and relayouts in place.
  void refresh_style();

  void set_document(render::Document document, const ScrollAnchor& anchor, bool follow_bottom);
  void clear();

  // Streaming: appends an id-less assistant message, then replaces only that
  // message's blocks as text arrives. The tail relayout keeps the rest of the
  // transcript's shaped paragraphs untouched.
  void begin_streaming();
  void set_streaming_blocks(std::vector<render::Block> blocks);

  [[nodiscard]] ScrollAnchor anchor() const;
  [[nodiscard]] bool at_bottom() const;
  void scroll_to_bottom();

  // Plain text of the current selection. Code blocks keep their own line
  // breaks; unrelated blocks are joined with a newline.
  [[nodiscard]] std::u16string selected_text() const;

  // Called when the reader reaches the top and older history may exist. The view
  // pages in a window and calls set_document again; without an anchor change the
  // canvas asks again, so a short transcript fills the viewport on its own.
  void set_on_reach_top(std::function<void()> handler);

  void set_on_copy(std::function<void(std::uint32_t)> handler);
  void set_on_edit(std::function<void(std::uint32_t)> handler);
  void set_on_delete(std::function<void(std::uint32_t)> handler);

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void scrollContentsBy(int dx, int dy) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  struct Private;
  std::unique_ptr<Private> d_;

  std::function<void()> on_reach_top_;
  std::function<void(std::uint32_t)> on_copy_;
  std::function<void(std::uint32_t)> on_edit_;
  std::function<void(std::uint32_t)> on_delete_;
};

} // namespace ui
