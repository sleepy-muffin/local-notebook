#pragma once

#include <cstdint>
#include <string>

namespace render {

struct Point {
  float x = 0.0f;
  float y = 0.0f;
};

struct Size {
  float width = 0.0f;
  float height = 0.0f;
};

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  [[nodiscard]] float right() const { return x + width; }
  [[nodiscard]] float bottom() const { return y + height; }

  [[nodiscard]] bool empty() const { return width <= 0.0f || height <= 0.0f; }

  [[nodiscard]] bool contains(Point point) const {
    return point.x >= x && point.x < right() && point.y >= y && point.y < bottom();
  }

  [[nodiscard]] bool intersects(const Rect& other) const {
    return x < other.right() && right() > other.x && y < other.bottom() &&
        bottom() > other.y;
  }
};

struct Insets {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

struct Color {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;

  bool operator==(const Color&) const = default;
};

struct Font {
  std::string family;
  float size = 12.0f;
  int weight = 400;
  bool italic = false;

  bool operator==(const Font&) const = default;
};

enum class TextAlign { Left, Center, Right };

// Who wrote a message. The role decides the body colour, the line-height rhythm
// and whether the message is drawn on a filled block.
enum class Role { User, Assistant, Notice, Error };

// The visual shape of one block of a message.
enum class BlockKind {
  Paragraph,
  Heading,
  ListItem,
  Quote,
  Code,
  Rule,
};

} // namespace render
