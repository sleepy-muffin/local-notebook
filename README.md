# Local Notebook

A local-first, assistant-backed notebook. One **assistant** per process, many
**sessions**, and a two-level memory that keeps long conversations coherent
without ever growing the prompt without bound.

The interface is a flat, IDE-like shell in the spirit of [Zed](https://zed.dev) —
low contrast, hand-drawn chrome, no speech bubbles or centred column — built with
Qt Widgets, with no JavaScript anywhere in the stack.

> **Status.** The application has been reconstructed from the ground up. The design
> is frozen in `.local-notes/design-1.md`; the previous implementation is parked,
> unbuilt, under `src/.old/`. The data format is new: no migration from the old
> database or settings file is performed.

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
| **Local first** | Everything lives under `data/`. Secrets are plaintext for now; no cloud account, no telemetry. |

## Design principles

- **Explicit, reviewable memory.** The stored memory is shown in the session editor
  and the assistant settings, where it can be read, edited and regenerated on demand.
- **No derived budgets.** There is no "model context window" field feeding a
  computed request size. The input bound is the session's own
  `summary_trigger_tokens_`; the output bound is `response_token_budget_`. Nothing
  is derived, so nothing can drift.
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

- `context::plan_window` — the verbatim window (the newest messages that fit).
- `context::should_summarize` — the watermark-counted automatic trigger.
- `context::summary_candidates` — the overflow to fold.
- `context::session_summary_request` / `assistant_summary_request` — the internal
  summariser payloads.
- `context::clamp_tokens` — the per-tool-result bound.

`net::client` speaks **Server-Sent Events** only: every request (chat *and* the
internal summariser) asks for a stream, so there is a single, well-tested transport
path.

## Data on disk

All paths are relative to the process working directory:

```
data/
  settings.json            app-global settings (shared by every assistant)
  <uuid>/
    settings.json          assistant-level settings (identity, connection, memory, budgets)
    data.db                SQLite: this assistant's sessions and messages
```

There is **no root registry**: the assistant picker discovers assistants by scanning
`data/*/settings.json`. The `uuid` exists only to avoid directory collisions and
never appears in the interface.

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
from every session, interleaved by time.

## Configuration

Configuration lives at three levels, matching the three things it configures.

### Global — `data/settings.json`

Theme, fonts, window geometry, background image and tint, and the transcript
rendering window. It also holds `auto_login` (reopen the last assistant at startup,
or always show the picker).

Sizing and spacing are **theme metrics** (`ThemeMetrics`): sidebar width, page
margin, message spacing, composer height, title font size, session item height,
reading rhythm (line heights and paragraph spacing), flow max width and bubble
radius. The theme file (`themes/theme.json`) supplies defaults; `settings.json`
stores only overrides.

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
every session-bound budget (`summary_trigger_tokens_`, `summary_token_budget_`,
`summary_reasoning_headroom_tokens_`, `max_tool_rounds_`, `max_tool_calls_`,
`tool_output_max_tokens_`, `response_token_budget_`). Edited in the scrollable
session editor.

## Memory and summarisation

A request for a prompt in session `S` of assistant `A` is assembled in order:

1. **system** — `A.custom_instructions_` + `S.custom_instructions_`, under separate headers.
2. **assistant memory** — `A.memory_`, shared across sessions.
3. **session memory** — `S.memory_`.
4. **verbatim window** — the newest messages of `S` that fit `S.summary_trigger_tokens_`,
   with the message that crosses the boundary kept whole.
5. **tools** — network tools always; filesystem tools when the session has a shared directory.

### Session summarisation

`S.summary_trigger_tokens_` is a single knob with three jobs: the verbatim **window**,
the automatic **trigger** (new tokens above `memory_watermark_`), and the summarisation
**granularity** (everything older than the window is the overflow that gets folded).
The summariser rewrites `S.memory_` from the previous memory plus that overflow, in
batches if it does not fit one call, so nothing is silently dropped. Manual
summarisation ignores the trigger but reads the same overflow — pressing it twice with
no new messages produces the same memory.

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

A two-pane shell:

- **Sidebar** — a calm `Local Notebook` banner, a `SESSIONS` category label, the
  session list (newest activity first, ordered by the time of the last message), a
  `New Session` button, and the current-session block (name, `Session #id`, a settings
  button). Each session row reveals an **edit** pencil and a **trash** action on hover;
  deleting asks for confirmation. The selected row is only slightly brighter — no
  strong accent highlight.
- **Chat pane** — a narrow title bar (session title and id on the left; context-token
  usage and the last-message time on the right) above the transcript. All messages
  share one width and one left edge, the way Zed's agent panel reads; the user's own
  message is a rounded, bordered block matching the composer, and assistant replies are
  rendered Markdown with code blocks and a copy button. Hovering a message reveals its
  `#id` and its copy/edit/delete actions.
- **Composer** — shares the conversation flow's background; the input field uses the
  same surface as the user's message block. Enter behaviour is configurable
  (`enter_with_modifier`).

Windows:

- **Assistant picker** — choose, create or edit an assistant at startup, with an
  auto-login checkbox. Never shows API keys or URLs.
- **Settings** — a left navigation with **Configuration**, **Metrics**, **Assistant**,
  **Account** and **About**. The Assistant page edits the open assistant's
  configuration *and* shows its memory, with a manual **Summarize now**. Save applies
  everything, re-applies the theme at runtime, and closes the window.
- **Session editor** — title, shared directory, custom instructions, conversation
  memory (with **Summarize now**), watermarks and budgets. Scrollable.

Two performance measures keep long conversations responsive: the transcript renders
only the most recent messages (older ones are revealed a page at a time), and the
background is a cached pixmap.

## Building

Requirements: a C++23 compiler (Clang is configured), CMake ≥ 3.31, Ninja, Qt 6
(`Core`, `Gui`, `Widgets`, `Network`) and SQLite 3.

```sh
cmake --preset linux-x64-debug
cmake --build build/linux-x64-debug -j 8
```

A `linux-x64-release` preset is also provided.

## Running

`data/` is resolved relative to the working directory, so run the binary from the
directory that should hold the data:

```sh
cd run
../build/linux-x64-debug/local-notebook
```

On first run the assistant picker appears. Create an assistant (name, model, API URL,
token, optional proxy, instructions and budgets), then start a session.

## Tests

Two dependency-free suites, registered with CTest:

```sh
ctest --test-dir build/linux-x64-debug --output-on-failure
```

- **`store`** — headless: the store layer, session/message round-trips, ordering,
  cascade delete, and the summariser payload shape.
- **`ui`** — offscreen (`QT_QPA_PLATFORM=offscreen`): window chrome invariants, the
  sidebar populating at startup, settings persistence, the shared assistant form, the
  settings navigation, and the wheel-safe spin boxes.

## License

MIT — see [LICENSE.txt](LICENSE.txt).
