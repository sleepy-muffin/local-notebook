# Local Notebook

A local-first, assistant-backed notebook. One **assistant** per process, many
**sessions**, and a two-level memory that keeps long conversations coherent
without ever growing the prompt without bound.

The interface is a flat, IDE-like shell in the spirit of [Zed](https://zed.dev) —
low contrast, hand-drawn chrome, no speech bubbles or centred column — built with
Qt Widgets, with no JavaScript anywhere in the stack.

---

## Contents

- [Key ideas](#key-ideas)
- [Design principles](#design-principles)
- [Architecture](#architecture)
- [Data on disk](#data-on-disk)
- [Configuration](#configuration)
- [Memory and summarisation](#memory-and-summarisation)
- [Tools and networking](#tools-and-networking)
- [Interface](#interface)
- [Screenshots](#screenshots)
- [Building](#building)
- [Running](#running)
- [Tests](#tests)
- [License](#license)

---

## Key ideas

| Idea | Meaning |
|---|---|
| **One assistant per process** | The assistant is chosen at startup and fixed for the process's lifetime. Its *configuration* stays editable; its identity does not change mid-run. |
| **Many sessions per assistant** | Each session is an independent conversation with its own transcript, instructions, shared directory and budgets. |
| **Two-level memory** | A **session memory** (this conversation) and an **assistant memory** (what the assistant has learned about you across *all* sessions). Both are generated summaries; both are visible and editable. |
| **Single-sourced configuration** | Every tunable has exactly one home. A value never has two sources of truth; a second appearance may only ever be a *validation*. |
| **Local first** | Everything lives under one data root (the per-user application data directory by default). Secrets are plaintext for now; no cloud account, no telemetry. |

## Design principles

- **Explicit, reviewable memory.** The stored memory is shown in the session editor
  and the assistant settings, where it can be read, edited and regenerated on demand.
- **No derived budgets.** There is no "model context window" field feeding a
  computed request size. The input bound is the session's own
  `summary_trigger_tokens_`; the output bound is `response_token_budget_`. Nothing
  is derived, so nothing can drift.
- **One conversation count.** Every conversation limit — the payload window, the
  collapse trigger, the level a collapse retains, and the title-bar readout —
  measures the **conversation alone**. The system prompt (instructions and both
  memories) is never included, so the number means exactly one thing and no hidden
  input can move it. The memory's own size is governed solely by the summariser
  budgets, never by these limits.
- **Watermarks are bookkeeping only.** They count what is new so the automatic
  summariser trigger can fire. They never decide what is sent, and never decide what
  a summariser reads — so repeated manual summarisation is idempotent.
- **Chrome drawn by hand.** Title bar, sidebar, session rows and toolbars are painted
  with `QPalette` and `paintEvent`; the stylesheet is kept to small, scoped pieces.
- **No `moc`.** There is no `Q_OBJECT` anywhere, so the build is a plain C++23
  compile (with `AUTOMOC` off) and a single static core library.

## Architecture

```
src/
  model.hpp                 Role, Message, Session, Assistant
  support/                  logging, the cancellable GenerationTask
  store/                    SQLite wrapper, database, SessionManager/MessageManager
  app/                      global settings, assistant I/O, the Application object
  context/                  the pure context layer: windowing, triggers, summariser payloads
  net/                      streaming client, proxy parsing, tools (filesystem + network)
  ui/                       theme, chrome, main window, editors, settings
```

The layering keeps the interesting rules in **pure, testable functions** rather than
inside the request loop:

- `context::plan_window` — the verbatim window: the newest messages that fit a
  conversation-token bound.
- `context::should_summarize` / `context::transcript_tokens` — the unsummarised
  conversation's estimated cost and the automatic trigger it drives (the same
  number the title bar reports).
- `context::summary_candidates` — the overflow to fold.
- `context::session_summary_request` / `assistant_summary_request` — the internal
  summariser payloads.
- `context::clamp_tokens` — the per-tool-result bound.

`net::client` speaks **Server-Sent Events** only: every request (chat *and* the
internal summariser) asks for a stream, so there is a single, well-tested transport
path.

## Data on disk

All state lives under a single **data root**. By default it is the per-user
application data directory, resolved with Qt's `QStandardPaths`:

| Platform | Default root |
|---|---|
| Linux | `~/.local/share/Local Notebook` (honours `XDG_DATA_HOME`) |
| Windows | `%APPDATA%\Local Notebook` |

Pass `--user-data <dir>` to override it (for example `--user-data ./run`), which is
how the tests and the screenshot tool sandbox themselves.

```
<root>/
  settings.json            app-global settings (shared by every assistant)
  <uuid>/
    settings.json          assistant-level settings (identity, connection, memory, budgets)
    data.db                SQLite: this assistant's sessions and messages
    local-notebook.log     assistant-level log (appended across runs)
```

There is **no root registry**: the assistant picker discovers assistants by scanning
`<root>/*/settings.json`. The `uuid` exists only to avoid directory collisions and
never appears in the interface.

### Logging

Log output goes to the open assistant's `local-notebook.log`, not to the command
line. Lines emitted before an assistant is chosen (theme, picker, startup errors)
are buffered and flushed to the top of that file once one is opened, so nothing
from startup is lost. The client also records every request it sends **verbatim** —
the exact request body, tagged `chat`, `session-summary` or `assistant-summary` —
so a surprising reply or a failed summarisation can be traced back to precisely what
the model was sent.

### Schema

One SQLite file per assistant.

```sql
CREATE TABLE session (
  id                                INTEGER PRIMARY KEY AUTOINCREMENT,
  title                             TEXT    NOT NULL DEFAULT '',
  custom_instructions               TEXT    NOT NULL DEFAULT '',
  memory                            TEXT    NOT NULL DEFAULT '',
  memory_watermark                  INTEGER NOT NULL DEFAULT 0,
  assistant_memory_watermark        INTEGER NOT NULL DEFAULT 0,
  shared_directory                  TEXT,
  pinned                            INTEGER NOT NULL DEFAULT 0,
  summary_keep_tokens               INTEGER NOT NULL DEFAULT 4000,
  summary_trigger_tokens            INTEGER NOT NULL DEFAULT 16000,
  summary_token_budget              INTEGER NOT NULL DEFAULT 4000,
  summary_reasoning_headroom_tokens INTEGER NOT NULL DEFAULT 12000,
  max_tool_rounds                   INTEGER NOT NULL DEFAULT 8,
  max_tool_calls                    INTEGER NOT NULL DEFAULT 32,
  tool_output_max_tokens            INTEGER NOT NULL DEFAULT 8000,
  response_token_budget             INTEGER NOT NULL DEFAULT 8000
);

CREATE TABLE message (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL REFERENCES session(id) ON DELETE CASCADE,
  role       INTEGER NOT NULL,   -- 0 = user, 1 = assistant
  content    TEXT    NOT NULL,
  made_at    TEXT    NOT NULL    -- ISO-8601 UTC, so sessions interleave by time
);
```

A session's position in the sidebar is a **query** (`MAX(made_at)` per session), not
a stored column. `made_at` is required because the assistant memory gathers messages
from every session, interleaved by time. `pinned` only lifts a session into the
`PINNED` category; the ordering within a category is still that query. The schema is
versioned through `PRAGMA user_version`, and an older file is migrated forward when it
is opened.

## Configuration

Configuration lives at three levels, matching the three things it configures.

### Global — `data/settings.json`

Theme, fonts, window geometry, background image and tint, and the transcript
rendering window. It also holds `auto_login` (reopen the last assistant at startup,
or always show the picker).

Some bundled themes are marked **(Translucent)**: their surfaces carry an alpha so
the background image shows through. The standard controls (buttons, input fields,
menus) and the code blocks honour that alpha as well, so nothing stays opaque on a
translucent theme.

The background image itself is painted by the **main window only** — dialogs,
the assistant picker and the settings window keep a plain window-coloured canvas,
so the image never shows behind a form. Two further colours, `conversation_background`
and `input_background`, control the conversation surface and the composer field
independently; the translucent themes set both fully transparent so the tinted
image reaches them, while the opaque themes use the window colour.

Sizing and spacing are **theme metrics** (`ThemeMetrics`): sidebar width, page
margin, message spacing, composer height (and its expanded height), title font size,
session item height, reading rhythm (line heights and paragraph spacing), flow max
width and bubble radius. The theme file (`themes/theme.json`) supplies defaults;
`settings.json` stores only overrides.

### Assistant — `data/<uuid>/settings.json`

Identity and connection (`name`, `model`, `api_url`, `authentication_token`,
`proxy`), `custom_instructions`, the assistant `memory`, and the assistant-level
summariser budgets:

| Field | Job |
|---|---|
| `summary_token_budget_` | Target size of the generated assistant memory. |
| `summary_reasoning_headroom_tokens_` | Completion headroom for a reasoning model's hidden thinking. |
| `summary_trigger_tokens_` | New tokens across **all** sessions that trigger the summariser; also the cap on the recent-message pool. |
| `summary_token_budget_for_session_summarization_` | Cap on the per-session summaries included in the assistant fold. |

### Session — the `session` row

`title`, `custom_instructions`, `memory`, the two watermarks, `shared_directory`, and
every session-bound budget (`summary_keep_tokens_`, `summary_trigger_tokens_`,
`summary_token_budget_`, `summary_reasoning_headroom_tokens_`, `max_tool_rounds_`,
`max_tool_calls_`, `tool_output_max_tokens_`, `response_token_budget_`). Edited in the
scrollable session editor.

## Memory and summarisation

A request for a prompt in session `S` of assistant `A` is assembled in order:

1. **system** — `A.custom_instructions_` + `S.custom_instructions_`, under separate headers.
2. **assistant memory** — `A.memory_`, shared across sessions.
3. **session memory** — `S.memory_`.
4. **verbatim window** — the newest messages of `S` that fit
   `S.summary_trigger_tokens_` (measured on the conversation alone), with the
   message that crosses the boundary kept whole.
5. **tools** — network tools always; filesystem tools when the session has a shared directory.

### The conversation token count

Every conversation bound and the title bar share **one number**: the estimated
cost of the unsummarised conversation — every message above the watermark, plus a
small fixed per-message overhead — and **nothing else**. The system prompt is not
part of it. This is deliberate. With the memory counted too, the number had two
controllers (the conversation *and* the memory), so a summarisation that grew the
memory could hold the readout high and make a low `keep` inexplicable. Excluding
the system prompt leaves exactly one controller and makes the readout identical to
the quantity that fires the trigger. The estimate is conservative — about four
ASCII characters per token, one token per non-ASCII codepoint — so a multibyte
reply is counted, if anything, on the high side.

### Session summarisation

Session summarisation has **two knobs with one job each**, and both measure the
conversation alone. `S.summary_trigger_tokens_` is the **input bound**: the
unsummarised conversation is never allowed past it, and the automatic collapse
fires the moment that conversation reaches it. `S.summary_keep_tokens_` is the
**level the conversation collapses to**: the summariser rewrites `S.memory_` from
the previous memory plus everything older than the newest `keep` tokens, and the
watermark advances past it. The conversation therefore sits at `keep` after a
collapse and grows back towards the trigger, instead of the entire window staying
above the watermark and re-triggering on the next turn. A large overflow is
folded in several calls (batched by the trigger), so nothing is silently dropped.
Manual summarisation ignores the trigger but reads the same overflow — pressing it
twice with no new messages produces the same memory.

The system prompt is deliberately **outside** both bounds: the memory may grow
without shrinking either the sent conversation or the retained one, and the
title-bar readout stays exactly the quantity that drives the trigger. A larger
memory does cost tokens, but that cost is the user's to set through the
summariser budgets — never a hidden second input to the conversation limits, and
the reason a low `keep` always means a small retained conversation.

### Assistant summarisation

Gathers across **all** sessions into one `A.memory_`, by recency and independent of the
watermarks, so it too is idempotent. Two **separate** pools are gathered and never
mixed: the **recent messages** (interleaved by time, capped by `summary_trigger_tokens_`)
and the **compacted historical messages** (each session's memory, newest session first,
capped by `summary_token_budget_for_session_summarization_`). A session may contribute
either block, both, or neither; sessions that do not fit are omitted, so older sessions
may be forgotten.

The summariser is an **internal LLM call**, not a tool call: it runs on a silent task
so its text never reaches the visible transcript.

Every summarisation request — automatic or manual — is shown for review before it is
sent: the exact payload and the watermark updates the pass will make (see
[Interface](#interface)). Declining cancels the pass; declining an *automatic* one
warns that the messages may not be folded into memory.

## Tools and networking

- **Filesystem** (only when the session has a `shared_directory`): `search_code` and
  `list_directory`. Paths are resolved against the shared directory and anything that
  would escape it — including through symlinks — is rejected.
- **Network** (always available): `fetch_url` (fetch a page and return its readable
  text) and `web_search`.

Each tool result is bounded by `tool_output_max_tokens_`, and the in-turn loop by
`max_tool_rounds_` / `max_tool_calls_`, so the context cannot grow without limit
during a turn.

**Proxy.** A session may configure a proxy; the scheme selects the type, and a bare
`host:port` keeps the historical SOCKS5 meaning:

| Spec | Meaning |
|---|---|
| `host:port` | SOCKS5 |
| `socks5://host:port` | SOCKS5 |
| `socks5h://host:port` | SOCKS5, DNS resolved by the proxy |
| `http://host:port`, `https://host:port` | HTTP CONNECT tunnel |
| `scheme://user:password@host:port` | the same, with credentials |

## Interface

A flat shell in the spirit of Zed: a full-width title bar over a sidebar and a chat
pane, with components separated by hairlines and background bands, not by frames or
heavy highlights.

- **Title bar** — one full-width band across the very top of the window, on the
  `surface` colour with a hairline beneath it. The `Local Notebook` banner sits at
  its left (inset to line up with the sidebar content); the conversation-specific
  information sits at its right — the session id, the conversation's estimated
  token usage against the collapse trigger (the system prompt is not counted), and
  the last-message time. The session title is not repeated here; it lives only in
  the sidebar row.
- **Sidebar** — no header band of its own: below the unified title bar, a flat
  full-width **New Session** row (styled like a list item: no border, a subtle hover
  fill) sitting above a `PINNED` category (shown only while a session is pinned) and
the `SESSIONS` category, the session list (newest
  activity first, ordered by the time of the last message), and the current-assistant
  block at the foot (assistant name, model, a settings button). A single hairline
  separates the sidebar from the chat pane; the sidebar sits on the `surface` colour.
  Session rows are flat and full-width — the title on the first line and the
  last-message time on the second — with a hairline above the assistant block and no
  rounded corners or gaps anywhere. Each row reveals a **pin** toggle, an **edit**
  pencil and a **trash** action on hover; the pin moves the session between the two
  categories, and deleting asks for confirmation. The hovered and selected
  rows fill edge to edge with a slightly brighter shade — no strong accent highlight.
- **Status bar** — a narrow strip along the very bottom, on the `surface` colour with
  a hairline above it. At its left, an icon toggles the sidebar's visibility; at its
  right, the number of running generation tasks is shown, and clicking it opens a
  modeless **Running Tasks** window listing each task's session, tool-call round and
  elapsed time.
- **Chat pane** — the transcript, below the unified title bar. All messages
  share one width and one left edge, the way Zed's agent panel reads; the user's own
  message is a rounded, bordered block, and assistant replies are
  rendered Markdown with code blocks and a copy button. Hovering a message reveals
  its `#id`, its arrival time, the estimated token cost of that message and its
  copy/edit/delete actions, all on one muted meta line.
- **Composer** — a block set off from the transcript by a single hairline, on the
  conversation surface. The input field is bare (no border, no rounded corners) and
  takes the theme's `input_background`; the **Send** action is a flat, icon-only
  button at the block's bottom-right, and an **expand** toggle at the field's
  top-right raises the field to `composer_height_expanded` and back. Enter behaviour
  is configurable (`enter_with_modifier`).

Windows:

- **Assistant picker** — choose, create or edit an assistant at startup, with an
  auto-login checkbox. Never shows API keys or URLs.
- **Settings** — a left navigation with **Configuration**, **Metrics**, **Assistant**,
  **Account** and **About**. The Assistant page edits the open assistant's
  configuration *and* shows its memory, with a manual **Summarize now**. Save applies
  everything, re-applies the theme at runtime, and closes the window. Because a
  cancelled edit must not survive to the next visit, every open re-reads the stored
  settings, metrics and assistant. The Configuration and Metrics pages each carry a
  **Restore to defaults** button (the shipped defaults, and the current theme's own
  metrics respectively); it only fills the fields and never saves.
- **Session editor** — title, shared directory, custom instructions, conversation
  memory (with **Summarize now**), watermarks and budgets. A **preset** dropdown above
  the budgets fills them with a starting point (light chat, balanced, novel writing,
  role-play, abstract design, large codebase); like the settings presets, it only fills
  the fields and never saves. Scrollable.
- **Summarisation review** — shown before *any* summarisation request is sent, manual
  or automatic. It shows the exact payload as plain text together with the watermark
  updates the pass will make, and asks whether it should run. Declining an automatic
  pass additionally warns that the messages may not be folded into memory, and points
  at the manual action.

Two performance measures keep long conversations responsive: the transcript renders
only the most recent messages (older ones are revealed a page at a time), and the
background is a cached pixmap.

## Screenshots

Captured from the current build by `tools/screenshots.cpp` (see [Building](#building));
the generator seeds a scratch notebook and renders each window under the offscreen
platform plugin.

### Main window

![The main window: a flat title bar over the session sidebar and the chat pane](screenshots/main-window.png)

### Settings

| Configuration | Metrics |
|---|---|
| ![The Configuration page](screenshots/settings-configuration.png) | ![The Metrics page](screenshots/settings-metrics.png) |

| Assistant | Account |
|---|---|
| ![The Assistant page, including its memory and a manual summarise action](screenshots/settings-assistant.png) | ![The Account page](screenshots/settings-account.png) |

![The About page](screenshots/settings-about.png)

### Session editor

![The session editor: title, shared directory, instructions, memory and budgets](screenshots/session-editor.png)

### Assistant picker

![The assistant picker](screenshots/assistant-picker.png)

### Summarisation review

![The review window, showing the exact request before it is sent](screenshots/summary-review.png)

## Building

Requirements: a C++23 compiler (Clang is configured), CMake ≥ 3.31, Ninja, Qt 6
(`Core`, `Gui`, `Widgets`, `Network`) and SQLite 3.

```sh
cmake --preset linux-x64-debug
cmake --build build/linux-x64-debug -j 8
```

A `linux-x64-release` preset is also provided.

The monochrome chrome icons (the composer actions — send, the collapse-into-memory
summarize glyph, the expand toggle and stop — and the sidebar's New Session
glyph, the message hover actions — copy, edit and delete — the session-row hover
actions, the pin toggle, the status bar's sidebar toggle and running-task glyph, and
the settings cog), the application icon and the default theme are
compiled into the binaries through the Qt resource system (`assets/local-notebook.qrc`,
`CMAKE_AUTORCC`), so a deployed build carries them without loose files beside the
executable and without depending on the system icon theme.

The chrome artwork lives in `assets/icons/`: editable SVGs plus the monochrome PNGs
actually embedded, which are recoloured to the active theme at runtime. The PNG form
is used because Qt's PNG support is built in, while decoding SVG needs the optional
Qt SVG plugin; `assets/icons/rasterise.py` regenerates the PNGs from the SVGs.

The application icon is different: it is full colour and must read from a 16px
taskbar thumbnail up to a 512px launcher tile, so it is composed from primitives by
`assets/appicon/make_app_icon.py`, which writes the `app-<size>.png` set (embedded
under `:/appicon/`) plus a multi-resolution `app.ico` for Windows builds.
`ui::application_icon()` assembles them into the window/taskbar icon.

A `themes/theme.json` on disk still overrides the bundled default.

The README screenshots are generated by a small development tool, which is built
alongside the tests:

```sh
QT_QPA_PLATFORM=offscreen \
  ./build/linux-x64-debug/local-notebook-screenshots docs/screenshots
```

## Running

Data is stored in the per-user application data directory by default, so the binary
can be launched from anywhere:

```sh
./build/linux-x64-debug/local-notebook
```

To keep the data beside the repository instead (as the tests do), pass a root:

```sh
./build/linux-x64-debug/local-notebook --user-data ./run
```

On first run the assistant picker appears. Create an assistant (name, model, API URL,
token, optional proxy, instructions and budgets), then start a session.

## Tests

Two dependency-free suites, registered with CTest:

```sh
ctest --test-dir build/linux-x64-debug --output-on-failure
```

- **`store`** — headless: the store layer, session/message round-trips, ordering,
  pinned ordering and the version-1 migration, cascade delete, and the summariser
  payload shape.
- **`ui`** — offscreen (`QT_QPA_PLATFORM=offscreen`): window chrome invariants, the
  sidebar populating at startup, the pinned category split, the transcript's render
  budget, settings persistence, the shared assistant form, the
  settings navigation, settings reload-on-show and restyling on a theme change, and
  restore-to-defaults, the budget and
  summarisation presets, the wheel-safe spin boxes, combo boxes and font selectors, the
  background-image cache, the summarisation review window (including its cross-thread
  marshalling), and the composer.

## License

MIT — see [LICENSE.txt](LICENSE.txt).
