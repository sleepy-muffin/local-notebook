# local-notebook

A small, local-first chat client built with Qt6 Widgets. It talks to an
OpenAI-compatible chat-completions endpoint (the code targets a llama.cpp-style
server) and stores everything in a local SQLite database.

This document records the architecture and the design decisions that are not
obvious from the code, so the project can keep evolving consistently.

## Building

The project uses CMake presets and Ninja.

```sh
cmake --preset linux-x64-debug
cmake --build build/linux-x64-debug
```

Replace `linux-x64-debug` with `linux-x64-release` for a release build.

## Running

The application opens `local-notebook.json` and `local-notebook.db` **relative to the current
working directory**, and expects the `run/` directory to be that directory
(it already contains `local-notebook.json`, `local-notebook.db` and the background image).
Launch it from `run/`:

```sh
(cd run && ../build/linux-x64-debug/local-notebook)
# or
env -C run ../build/linux-x64-debug/local-notebook
```

Alternatively use the Zed tasks defined in `.zed/tasks.json`
(`task: spawn`), which set the working directory correctly for you.

## Overview

At startup `Logon` asks you to pick an identity (one of the stored sessions).
That identity becomes `ApplicationState::self_id_`. `MainWindow` then lists all
*other* sessions as chat targets; a session can never talk to itself.

A "session" is a configured peer: title, custom instructions, API URL, token,
model, optional SOCKS5 proxy and an optional shared directory for the local
tools. Conversations are stored as messages between `(sender_id, reader_id)`
pairs, so a session's transcript is exactly the messages exchanged between the
logged-in identity and that session.

`local-notebook.json` groups the general settings under a `general` object and
an optional set of layout overrides under `metrics` (the older flat format, with
those keys at the top level, is still accepted). The `general` object also
accepts the following generation settings (omitted keys keep these production
defaults):

```json
{
  "general": {
    "model_context_tokens": 128000,
    "response_token_budget": 12000,
    "context_safety_margin_tokens": 4000,
    "summary_trigger_tokens": 95000,
    "summary_token_budget": 4000,
    "max_tool_rounds": 8,
    "max_tool_calls": 32,
    "rendered_message_limit": 50,
    "history_page_size": 50
  }
}
```

The payload builder reserves response and safety tokens, retains the newest
messages that fit, and keeps custom instructions separate from the persisted
session memory summary. Tool rounds and total tool calls are independently
bounded.

`rendered_message_limit` bounds how many of the newest messages the transcript
renders at once; older messages stay loaded in memory but out of the widget tree
until the reader reveals them, either by scrolling to the top or with the **Load
earlier messages** button. `history_page_size` is how many are revealed per
step. Both only affect rendering: the full history is still sent to the model.

Configuration is persisted: the Settings window's **Apply** button writes
`local-notebook.json` back (via `lnb::general::save()`), and the file is also
rewritten when the configuration object is destroyed at shutdown.


```
launcher.cpp            Owns ApplicationState, applies the theme, shows Logon then MainWindow.
ui/Logon.*              Identity picker + session list (also allows adding).
ui/MainWindow.*         Navigation, session list, and the stacked chat panes.
ui/ConversationView.*   Pure UI for one conversation (transcript + composer).
ui/ConversationController.*  State/glue for one conversation (one per session).
ui/SessionEditor.*      Add/edit a session dialog.
ui/theme.*              Named themes + metrics from theme.json; palette/QSS/background.
ui/BackgroundWidget.*   Base layer that paints the background image or window colour.
external.*              Payload assembly, streaming HTTP, tool-call loop.
external_tools.*        Sandboxed local tools (search_code, read_file).
db.*                    SQLite persistence (sessions, messages) + config loading.
generation_task.hpp     Thread-safe state of an in-flight generation.
external/mmc            Vendored helper library (logging, threads, strings).
themes/theme.json       Bundled theme definitions (embedded into the binary).
```

### Layers

- **`ConversationView`** is deliberately passive: it renders state and forwards
  user intent through `std::function` callbacks (`set_on_send`,
  `set_on_cancel`, `set_on_delete_message`). It contains no conversation logic.
- **`ConversationController`** owns the state of one conversation: the
  transcript, the current `GenerationTask`, and the wiring to the database and
  the worker pool. All of its methods run on the GUI thread.
- **`external.cpp`** contains the network code. `generate_reply()` runs a whole
  assistant turn, including the tool-call loop; `send_message_llama_once()`
  performs a single streaming request. Requests go through Qt's
  `QNetworkAccessManager` and the payload is built from Qt JSON
  (`QJsonObject`/`QJsonArray`), so neither Boost.Beast nor Boost.JSON is used
  by the application. The SSE response is decoded by a small `SseStream`
  helper. Cancellation is observed from inside the request's Qt event loop: a
  timer polls the stop token and aborts the reply.
- **`external_tools.*`** implement the two local tools (`search_code`,
  `read_file`) against the session's shared directory. Tool failures are
  returned to the model as text so it can correct its arguments.

> Note: the codebase does **not** use Qt's `moc`/`Q_OBJECT` (there is no
> `AUTOMOC` in `CMakeLists.txt`). New UI code follows the same convention:
> `std::function` callbacks and connections to existing Qt signals, never
> custom signals/slots.

## Theming and appearance

All colours and most layout metrics live in `themes/theme.json`, which is
**embedded into the binary** through Qt's resource system (so a fresh build
needs no extra files next to it). If `:/themes/theme.json` cannot be read, the
application falls back to a `theme.json` in the current working directory.

The file has two top-level sections:

- `themes`: a map from a theme name to `{ dark, colors, metrics }`. Eight
  Catppuccin variants ship by default: the four opaque themes (`Catppuccin
  Mocha`, `Catppuccin Macchiato`, `Catppuccin Frappe`, `Catppuccin Latte`) and
  four `... (Translucent)` counterparts meant for background-image mode (see
  below). Every theme uses Catppuccin **pink** as its accent colour.
- `default_metrics`: the fallback layout metrics, which each theme may override
  through its own `metrics` object. The knobs are `nav_width`, `sidebar_width`,
  `bubble_radius`, `message_max_width`, `message_spacing`, `page_margin`,
  `composer_height`, `title_font_size`, `session_item_height` (the height of a row
  in the session list) and the chat-text knobs `line_height_other`,
  `line_height_self` and `paragraph_spacing`.

The same metrics can be overridden per installation under the `metrics` section
of `local-notebook.json`; when that section is present it takes precedence over
the theme's values (the Settings window edits it). See
`src/ui/metrics.hpp` for the full struct.

A colour is a four-element array. Channels above `1` are read as 0-255, while
values `<= 1` are read as normalised floats; the fourth element is always the
alpha (0-1). See `lnb::load_color` in `src/ui.hpp`.

### Selecting a theme

The configuration key `theme` selects a theme **by name**:

```json
"theme": "Catppuccin Mocha"
```

An unknown name falls back to the first theme in `theme.json`, and a missing
`theme.json` falls back to a hard-coded Catppuccin Mocha palette, so the app
always starts with a coherent look. `ui::apply_theme()` (called once from
`launcher.cpp` before any window is created) installs the Breeze style, the
`QPalette`, a generated global stylesheet and the configured fonts.

`ui::theme()` exposes the active theme to widgets. The theme is chosen at startup
from the `theme` key; the Settings window (opened from the session card) can
re-apply a different theme and edited metrics at runtime through
`ui::apply_theme()`.

### Background image

The optional `background_url` key names an image **relative to the current
working directory**. When it loads successfully:

- the top-level window background and the transcript canvas become transparent
  so the image shows through, and
- `background_override` (a four-element colour) is painted over the *whole*
  background - right above the image and below every component. Components
  (navigation, sidebar, cards, composer field, menus, bubbles) keep their
  palette colours, so the override never changes them. Set
  `background_override` to `null` to show the image untinted.

The image and the tint are painted by `ui::BackgroundWidget`, which is used as
the central widget of both windows and scales the picture to *cover* the widget
(keeping the aspect ratio and centre-cropping). The cover-fit is rendered in
device pixels and tagged with the widget's device pixel ratio, so the image
stays sharp on scaled displays (for example Wayland at 125%/150%) even though
the components are drawn at logical size. This widget also paints the theme
window colour when no image is set, which is what removed the thin unpainted
line that window managers used to show above the content area.

> The old keys `dark_mode` and `background` (and an object-valued `theme`) are no
> longer used; `background` is still accepted as an alias of `background_url`.

### Translucent themes

Every opaque theme has a `... (Translucent)` counterpart, intended for
background-image mode. They keep the same pink accent but give the component
colours a low alpha (`surface` `0.55`, `base` `0.38`, bubbles `0.5`, `border`
`0.35`, and the code background `0.55`), so the navigation, sidebar, composer
field and bubbles become subtly
see-through and the tinted image shows through them. Text stays fully opaque and
`window` keeps a high alpha so dialogs stay readable. Select one with the
`theme` key:

```json
"theme": "Catppuccin Mocha (Translucent)"
```

### Fonts

Six configuration keys control typography, split between the chat area, the
code inside replies, and the rest of the interface:

| key | scope | default |
| --- | --- | --- |
| `ui_font` | every widget (the application font) | the platform default |
| `ui_font_size` | point size of `ui_font`; `0` keeps the platform default | `0` |
| `font` | the conversation transcript and composer only | inherits `ui_font` |
| `font_size` | point size of `font`; `0` keeps `ui_font_size` | `12` |
| `mono_font` | code in replies (fenced blocks and inline `code`) | the system fixed-pitch font |
| `mono_font_size` | point size of `mono_font`; `0` keeps the chat size | `0` |

The chat font is derived from the UI font, so leaving `font`/`font_size` unset
makes the chat match the interface. `ui::ui_font()`, `ui::chat_font()` and
`ui::mono_font()` expose the resolved fonts; `ConversationView` applies
`chat_font()` to the message labels and the composer and `mono_font()` to code,
while everything else follows the application font.

### Reading rhythm

Three theme metrics keep the transcript airy (ChatGPT-style) instead of compact.
They live under `default_metrics` (or a theme's own `metrics`, or the `metrics`
section of `local-notebook.json`) and are applied by `ConversationView` through
`QTextBlockFormat`, so they affect every message body:

| metric | meaning | default |
| --- | --- | --- |
| `line_height_other` | assistant line height as a percentage (`100` is normal single spacing) | `160` |
| `line_height_self` | user-message line height as a percentage | `135` |
| `paragraph_spacing` | gap left after each paragraph, in pixels | `10` |

The assistant line height is applied to assistant blocks, while user messages use
`line_height_self`. The paragraph gap is added only after prose paragraphs
and never after the final one (code blocks stay tight and the closing edge is
flush). Both line heights are clamped to at least `100` to avoid clipping lines;
`paragraph_spacing` may be `0`.

### Message width

| metric | meaning | default |
| --- | --- | --- |
| `message_max_width` | maximum width of a message body in pixels; `0` means unlimited | `640` |

`ui::message_max_width()` exposes the value (where `0` means "no limit"), and
`ConversationView` centres the transcript column once the pane is wider than it,
so bubbles pull away from both edges instead of hugging the left.

### Composer

| key | meaning | default |
| --- | --- | --- |
| `enter_with_modifier` | `true`: Ctrl+Enter sends and Enter inserts a line break; `false`: Enter sends and Shift+Enter inserts a line break | `true` |

The composer placeholder reflects the active mode, so the shortcut is always
discoverable.

### Components and hover affordances

The look leans on a few conventions that are worth keeping consistent:

- **The navigation rail is icon-only.** `MainWindow` builds it from checkable
  `QToolButton`s (object name `NavButton`) inside a `QButtonGroup`; each label is
  a tooltip rather than a caption.
- **Message actions appear on hover.** A `MessageRow` keeps a fixed-height
  action strip under every message; the icon-only buttons only appear while the
  pointer is over the entry, so revealing them never reflows the transcript.
- **Hints use native tooltips.** Labels are surfaced with `QToolTip`
  (`setToolTip`), never by resizing or restyling the widget itself.
- **Only user messages are wrapped in a bubble.** Assistant replies are rendered
  as bare text (no bubble), ChatGPT-style, and differ from user messages by
  alignment alone. Error messages keep a danger-coloured bubble.
- **Code is highlighted without extra dependencies.** Code in replies uses
  `mono_font()`. Fenced blocks are drawn as rounded `#CodeBlock` frames on the
  `bubble_assistant_code` colour (a shade darker than `base` in dark themes, a
  touch brighter in the light theme); inline spans keep their own background.
  Rich text cannot round a background, which is why fenced blocks are real
  frames; inline code stays square.

## Conversation semantics

These behaviours were chosen deliberately:

- **Per-session state.** Each conversation has its own persistent
  `ConversationView` (kept alive in a `QStackedWidget`) and its own
  `ConversationController`. Switching sessions therefore never touches the
  other session's state.
- **Streaming is not tied to visibility.** Generation runs on a worker thread
  and advances even while its session is not on screen. A single `QTimer`
  (33 ms) polls *every* controller, so background generations still progress
  and still get persisted when they finish.
- **Drafts are per session and in memory only.** Because each session has its
  own composer widget, the unsent text travels with the session when you switch
  away and back. Drafts are intentionally **not** persisted across restarts.
- **At most one generation per session.** While a session is generating its
  send button/Enter are disabled; the UI shows a Cancel button instead.
- **Cancel discards the assistant output.** Cancelling stops the request and
  does **not** write the partial reply to the database; `reload()` then removes
  it from the screen as well. The user's own message, however, is already
  persisted (and stays visible), so re-sending simply appends another turn.
- **Only completed turns are persisted.** The assistant reply is written to the
  database only when generation completes successfully with non-empty content.
- **Tool calls are executed inline.** `generate_reply()` executes the requested
  local tools against the session's `shared_directory`, feeds the results back,
  and repeats (capped at 8 rounds). The chat shows at most one transient,
  human-readable status title (for example, `Reading files...`) while a tool is
  running; tool arguments and results are never shown or persisted.
- **Assistant replies render light Markdown.** The reply Markdown is split into
  prose and fenced-code segments (`MessageBubble::parse_segments`). Each prose
  segment is imported into a `QTextDocument` (no extra dependency) and shown as a
  rich-text label, so headings, emphasis, lists and links are supported
  (blockquotes are not). Each fenced block becomes a rounded `#CodeBlock` frame
  wrapping a `mono_font()` label, because a `QTextDocument` cannot round a block
  background. User messages and errors stay plain text. While a reply streams the
  block shape is compared to the previous one and labels are updated in place
  when it is unchanged, so a growing reply neither flickers nor is rebuilt on
  every token; text is re-rendered at most every 50 ms.
- **Full-width message rows.** User and assistant messages occupy the same
  available transcript width and are left-aligned; message length no longer
  determines the row width.
  the viewport (a leading stretch in the transcript layout), so a short
  conversation does not leave a large empty gap below the last message.
- **Deleting and editing a message.** Hovering a stored message reveals an
  icon-only action strip (native tooltips: "Edit message" / "Delete message").
  Editing rewrites the row in the database; deleting removes it. Both reload the
  transcript. The strip has a constant height, so it never reflows the view.

## Threading model

- `mmc::async::multi_executor` is a small condition-variable-based thread pool
  (3 worker threads). Jobs are picked up immediately when queued.
- A generation job runs on a worker thread and shares state with the GUI
  thread only through the thread-safe `GenerationTask`. The job creates its own
  `QNetworkAccessManager` and drives a local `QEventLoop` for the request, but
  it never touches widgets or the shared Qt object graph.
- All widget updates, database writes and finalization happen on the GUI thread
  via the polling timer.
- On window close every controller is cancelled so the worker pool can be
  joined promptly during shutdown.

## Database migrations

`sqlite_interface::migrate()` runs whenever the database is opened and brings
older databases up to the current schema. In particular, databases created
before the peer→session renaming still name the lorebook owner column
`peer_id`; it is renamed to `session_id` (data-preserving, no rows touched) so
that the lorebook queries match the current code. Without this migration those
queries fail and, because they run from a Qt event handler, abort the process.

## Known limitations / possible follow-ups

- Background images are painted by the application itself (`BackgroundWidget`);
  the windows are never made translucent at the compositor level. Keeping them
  opaque avoids the earlier glitch where bubbles appeared to bleed onto the KWin
  title bar.
- The theme (including the background image path and the fonts) is read once at
  startup; there is no live theme switching yet. The only exception is the
  message action icons, which follow the system icon theme at runtime.
- Cancellation is observed from inside the request's Qt event loop, so a
  blocked DNS resolution or TLS handshake is not interruptible. A stalled
  request is bounded by the manager's 60 s transfer (inactivity) timeout.
- A message body cannot shrink below the width of its longest unbreakable
token (a long URL, identifier or code line) because `QLabel` wraps only at word
boundaries. Such a message raises the window's minimum width, so the window can
no longer be narrowed until that message scrolls out of the layout. A future
"wrap anywhere" text widget would lift this.
- The Settings and Profile pages are placeholders.
