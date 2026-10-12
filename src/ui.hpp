#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include "db.hpp"
#include "ui/metrics.hpp"

#include <mmc/async.hpp>
#include <mmc/sync.hpp>

namespace lnb {


struct color4 {
  float r;
  float g;
  float b;
  float a;

  template <typename T> T get() const { return {r, g, b, a}; }
};

inline lnb::color4 load_color(const QJsonObject &json, std::string_view name,
                              lnb::color4 fallback) {
  const QJsonValue value =
      json.value(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));

  if (!value.isArray()) {
    return fallback;
  }

  const QJsonArray array = value.toArray();

  if (array.size() < 4) {
    return fallback;
  }

  // Qt JSON stores every number as a double, so channel values above 1 are
  // interpreted as 0-255 components while smaller values are already normalised.
  auto rgb = [](const QJsonValue &channel) {
    const double number = channel.toDouble();
    return static_cast<float>(number > 1.0 ? number / 255.0 : number);
  };

  return {
      rgb(array.at(0)),
      rgb(array.at(1)),
      rgb(array.at(2)),
      static_cast<float>(array.at(3).toDouble()),
  };
}

struct general {
  // Name of the theme to load from theme.json (embedded, or from the working
  // directory as a fallback).
  std::string theme_ = "Catppuccin Mocha";

  // Optional background image, resolved relative to the current working
  // directory. When set, the window and transcript canvas become transparent
  // and the image is tinted with background_override_.
  std::optional<std::string> background_url_ = std::nullopt;

  // Colour painted over the whole background image, right above the image and
  // below every component. It only affects that layer; components keep their
  // palette colours. Set it to null in the JSON to show the image untinted.
  // std::optional<color4> background_override_ = color4{0.1f, 0.1f, 0.1f, 0.25f};

  general(std::string_view path);
  void save() const;
  ~general();

  uint32_t session_id_ = 1;

  bool enter_with_modifier_ = true;

  // Chat font: used only by the conversation transcript and composer.
  std::string font_ = {};
  int font_size_ = 12;

  // General UI font: used by every other widget. A size of 0 keeps the
  // application's default size.
  std::string ui_font_ = {};
  int ui_font_size_ = 10;

  // Monospace font for code in replies. An empty family keeps the system
  // fixed-pitch font and a size of 0 keeps the chat size.
  std::string mono_font_ = {};
  int mono_font_size_ = 11;

  // Generation/context budgets. These are intentionally configurable because
  // the advertised context window differs between local model servers.
  std::size_t model_context_tokens_ = 128000;
  std::size_t response_token_budget_ = 12000;
  std::size_t context_safety_margin_tokens_ = 4000;
  std::size_t summary_trigger_tokens_ = 80000;
  std::size_t summary_token_budget_ = 4000;

  int max_tool_rounds_ = 8;
  int max_tool_calls_ = 32;

  // Transcript rendering window. Only the most recent messages are rendered at
  // once; older messages are revealed a page at a time on demand, so a very
  // long conversation stays responsive. This only bounds rendering: the full
  // history is still loaded from the database and sent to the model.
  int rendered_message_limit_ = 20; // newest messages rendered at once
  int history_page_size_ = 20; // messages revealed per "load earlier"

  // Optional override of the theme's layout metrics. When set, these values are
  // persisted under the "metrics" section of local-notebook.json and override
  // the values loaded from theme.json; when unset the theme's defaults apply.
  std::optional<ui::ThemeMetrics> metrics_ = std::nullopt;

  const std::string path_;
};

struct ApplicationState {
  std::unique_ptr<lnb::db> uni_db_ = nullptr;
  std::unique_ptr<lnb::general> uni_config_ = nullptr;

  uint32_t self_id_ = 0;
  std::unique_ptr<mmc::async::multi_executor<void()>> multi_executor_;

  void shutdown() {
    shutting_down_ = true;
    multi_executor_->shutdown();
  }

  [[nodiscard]] bool shutting_down() const { return shutting_down_; }

private:
  std::atomic_bool shutting_down_ = false;
};

} // namespace lpf
