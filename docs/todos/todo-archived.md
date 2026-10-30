# TODOs - Archived

This file stores the history of TODOs.
Records should be added once they are completed.



## Update Catalog

I will try to make this list ordered by time.

- (New Function) Theme selectors and budget presets, assistant-summarization presets are also scrolling adjustable. Please disable it as we did for number/ font choosing controls.
- (New Function) Please place some screenshots in `docs/screenshots/`, then present them in `README.md`.
- (New Function) Add summarization review window before the program sends the summarization request, both munually and automatically. It only needs to display what's going to be sent in plain text, and how watermarks will be updated. It asks the user whether it should run, if user decline, cancel it (If the user cancel an automatically triggered summarization, inform them some messages may fail to be converged if they don't manually summarize before sending new messages).

- (Bug) Changing theme may still not refresh the complete program, as font colors and the colors of some controls (spinbox, one row input box, selector, etc. ) in settings window may not have been immediately updated yet. But re-starting the program can fix it.
- (Performance Issue) Historical messages should be automatically unloaded so that the number of rendered messages never exceed the configured initial rendering amount. Triggered on the arrival of new messages.
- (New Function) Add a status bar, it shows several items.
  - On the leftest side, it shows an icon, it switches the visibility of the sidebar.
  - On the right side. The number of currently running generation tasks, the icon is click-able, clicking it opens a window (unique, but does not block the main window) which shows all running generation tasks with some information (like rounds, elapsed time)
- (New Function) Sessions pinning supports. Add a catagory named `pinned` above the catagory `sessions`. Choose the best place to store this boolean property, maybe in the session database?

- (Bug) Pushing Cancel button can cause the program to lag for a quite while.
- (Bug) Pushing Save button in Settings Window can lag for a while.
- (New Function) It should show the arrival time of the message in the row of Session #id and those icons.
- (Bug) The conversation view still viberates when the user is not currently stick to the bottom, only auto-scroll (scroll to the end when it's already in the end previously). And, it unexpectedly scrolls to the end once the generation task is completed.

- (Refinement) When the side-bar is visible, the icon on the status bar can be colored like the send button.
- (Bug) add a number `summary_keep_tokens_` for session-specific memory use. That decision was a mistake. We need it, unless the memory summarization will be triggered repeatedly once the user messages exceed the limits... Though I think we don't need it for assistant-level memory yet as it advances the watermarks to the latest.
  The formalised model
  - `summary_trigger_tokens_`: the input bound and the trigger. A request carries system prompt + transcript; when that reaches the trigger, the collapse fires.
  - `summary_keep_tokens_`: the level the transcript collapses to. The newest messages kept verbatim; everything older is folded into memory_.
  - Therefore keep must be well below trigger — otherwise the collapse leaves more than the trigger and fires again immediately.

- (Bug) The watermark is not updated if the summarization job fails. So, please update only when the job has been completed successfully.
- (Refinement) Please add a validation on clicking "Save", currently we provide one check: `response_token_budget_` is not allowed to be greater than `summary_trigger_tokens_` in Session Editor, it simply opens a Qt Message Box to explain the reason.
- (Refinement) The token count in the title bar should be honest. Currently, I discovered that it can decrease it I set the trigger to a lower value. As our design triggers the summarization once the token count have exactly surpassed the trigger amount. And the oldest message in the tail will not be truncated. So, no messages will fail to converge. Remember we never decide the messages to be included into the summarization by watermarks.
- (Refinement) Alter the send button to summarize button when the messages after the watermarks have surpassed the trigger amount, then we include all messages converged by trigger amount (the one in the tail should not be included), and it triggers summarizes only.

- (Refinement) Add UI translation files for English, Simplified Chinese, Japanese and English. The language can be toggled in Settings Window - Configuration page.

- (New Function) Ripgrep-style code search tools, offered when a session has a shared directory. They walk the shared directory honouring `.gitignore`/`.ignore` files (nearest match wins, with `!` negation), skip hidden entries, and never follow symlinks; any path outside the shared directory is rejected as before.
  - `search_code_regex`: matches text files with a PCRE2 regular expression and returns matching paths, line numbers and surrounding context. Bounded like `search_code` (2 MB per file, 30 matches); unlike a naive search it is never truncated by a file count, so a match deep in a large checkout is still found. Matching is case-sensitive; the model opts into `(?i)` for case-insensitivity and can use `\b` for word boundaries. An optional `glob` restricts which files are searched.
  - `find_files`: lists files whose path matches a glob, honouring the same ignore rules, without reading their contents.
  - `search_code` is unchanged: a plain substring search that always works, including for languages and non-code files.

- (Refinement) Tasks window should not remove failed tasks, it should show the failed reasons for them. Clicking on it open a message box (content should be scrollable) which dumps the log of this task, it should not render the markdowns, and shows everything including all those are not displayed in the main window (thinking procedure, etc. )
- (Refinement) As soon as the user can reveal the failed notifications in the Tasks window, we no longer need to show the failure information in the conversation view (I think it's quite volatile, anti-intuition, and sometimes be buggy). Also, ensure generation tasks are not removed when the task has failed. But we will need a tiny message box created right above the button to indicate that a task has failed, clicking on the Tasks Window icon on the status bar to inspect the reasons. Ensure this tiny message box is re-usable, so we can use it for future icons appended to the status bar.
- (Refinement) Use full copy of MIT Lisence in About tab in Settings Window. Also, the fully version lisence of the community version of Qt. They don't need to be rendered as markdown.
- (Refinement) The background of scrollbars should be transparent. Currently they still have black background in settings window. While the scrollbar in Main Window appears to have transparent background (What looks better).
- (Refinement) Generation Task triggered by summarization (both assistant-level and session-level) should be also queued into task list. Button should be substituted to summarize button when assistant-level summarization trigger has met (currently we already have it). Also, for the case where assistant-level trigger has been met. So, in the case when both triggers have been met in the same time, user have to summarize twice before moving on. (session-specific memory is inspected first. )

- (Refinement) Manual assistant-level summarisation should be appended into Tasks Window as well.
  In the case assistant-level summarisation is not completed, for both manually initiated or automatically triggered on sending message and the user affirms it, as we have already implemented.
  - The send button is banned for all sessions.
  - The user can not cancel this task in session conversation view.
  - Please add cancel button in Tasks Window for running job, so assistant-level summarisation can be cancelled there.
  - Currently, when the assistant-level summarisation is issued through the button on the right of session, the task will show up, but no details are provided there. No error messages even though the assistant-level summarisation is failed.

  Implemented with the task abstraction below: the manual fold (Settings) runs as the shared assistant task, so it appears in the Tasks Window; every row for a running task carries a Cancel button; and a manual fold now surfaces both its request/response log (the summarizer runs on a silent task whose log is copied onto the visible one) and its failure (the manual paths fold strictly instead of swallowing the error).

- (Refinement) Icon on Wayland should be installed dynamically, so that in case that the `.desktop` cannot be installed, the icon still shows up.
- (Refinement) It now accepts multiple shared directories. It does not rely on Linux mount. It makes fake addresses for tools. Real absolute path no longer exposes to the model. Details:
  - There is no `mount(2)`, no bind mount, no mount namespace, no bwrap. The word “mount” in the code comments is a metaphor for a lookup table. The whole thing is an in-process virtual path tree inside the tool dispatcher.
    - `build_shared_roots` canonicalizes each configured directory and assigns it a visible name (its basename, with -2, -3 for duplicates).
    - `resolve_target` interprets the model’s string: in single-root mode it is a path inside the one root; in multi-root mode the first segment selects a named root.
    - `checked_repo_path` joins that onto the real directory, calls `fs::canonical`, and verifies `lexically_relative(root)` doesn’t start with ...
    - The result is formatted back into relative paths before it ever reaches the model.
  - Examples:
    - Several directories configured — named top-level entries case. `list_directory {}` returns the "mounts", named from each directory’s basename: `alpha/`, `beta-123/`. The full path is never exposed, but the folder name is.

- (Refinement) Scrollbar track. A stylesheet-only transparent track is
  not possible under Breeze: the style fills the empty track with
  `QPalette::Base` even when the QSS asks for `background: transparent`, which is
  the lighter strip that showed through. The transcript now uses `ui::ScrollBar`,
  which paints the track with the conversation surface (`conversation_background`)
  and draws only a rounded handle. For themes that use a background image the
  surface is transparent, so the bar asks `BackgroundWidget::paint_region()` for
  the exact slice of the scaled image (and tint) behind it. The settings pages,
  the assistant picker and the session editor use the same bar with the window
  colour, and the session list with the sidebar surface colour; the platform
  style never gets a chance to fill the track.
- (Refinement) Inline code. The rounded plate now drops its horizontal
  padding on a side where the neighbouring character is not whitespace, so the
  background no longer runs into adjacent text (padding is kept next to spaces
  and at line/paragraph edges). Inline code is also sized as
  `inline_code_scale` x the surrounding text instead of using the configured
  monospace size; the ratio is configurable under Appearance as "Inline code
  size" (default 0.9, range 0.50-1.50) and is persisted in `settings.json` as
  `inline_code_scale`.

- (CI) Windows Build CI https://github.com/sleepy-muffin/local-notebook/actions/runs/39581150163/job/119158815237 on the step named "Install SQLite, Boost and OpenSLL". Make it cached.

- (Refinement) An assistant now talks to one of two wire protocols, chosen in the assistant form. Responses` is the default (and the only endpoint that supports tool calling for the GPT-6 family); `Chat Completions` remains for OpenAI-compatible servers that expose only that shape. The choice is stored in the assistant JSON and, absent a key, an older assistant routes through Responses.
  - The API URL control now takes a base like `xxx.com/v1`; it requires an explicit
    `http://` or `https://`, and `https://` is prepended on focus-out when it is
    missing. Saving is refused unless the value is an http(s) base ending in `/v1`.
    A legacy full-endpoint value normalises to its base on load.
  - The endpoint derived from the base is `/responses` or `/chat/completions`,
    matching the assistant's protocol. Summarisation follows the same protocol.
  - `reasoning_effort` is now user-visible: a borderless, fully transparent selector
    with a right-hand chevron sits to the left of the composer's send button (clicking
    anywhere on it opens the dropdown), and a matching selector appears on the
    assistant settings page (JSON) and the session settings page (DB column). Each
    list is inferred from the model id, leading with the cheapest level.
  - A stored level the current model no longer offers is **rejected at send time**,
    never reset to an available one behind the user's back. The selectors still show
    such a level so it can be corrected.
  - (Open) The second `GPT-6.1 Luna` entry in the original reasoning-level spec had
    no distinct model name. Per the docs, `none` is unsupported by GPT-6 Astra and
    GPT-6.1 Sol, so the no-`none` list is applied to the GPT-6 family except Luna.
    Revisit if a real model name surfaces.

- (Refinement) Make an abstract layer for tasks? Which prevents the UI from launching a "send" task when either session-level or assistant-level summarisation trigger is met (so, we will have another protection layer). And all tasks (send tasks, summarisation tasks) must go through it, so Tasks Window can show them without problems. To reduce UI sources from directly making decisions from their own.
  Implemented as `lnb::TaskManager` / `lnb::Task` (`src/app/task_manager.*`), owned by `Application`. It is the single owner of every background worker and the single place the primary-action policy lives (`primary_action`: session overflow, then the shared assistant trigger or an in-flight assistant fold, then a plain send). Every turn and fold is started through `submit`, which refuses a `Turn` while the primary action is not a send — the extra protection layer — and keeps at most one task per slot (one per session, one for the shared assistant memory). The Tasks Window, the status-bar count and every composer now read from the manager instead of from the open views.

- (Refinement) Draw markdown with custom renderer for better performance. Implemented as a
  Qt-free renderer core (`ui/render/`: document model, layout, hit testing,
  selection and paint order) plus a Qt-backed canvas (`TranscriptCanvas`) that
  only supplies shaping (`QTextLayout`) and a `QPainter` backend. The transcript
  is one custom-painted, virtualised surface: paragraphs are shaped once, only
  lines inside the viewport are painted, streaming relayouts only the last
  message, and the per-message widget tree (and the `QTextDocument` → HTML →
  `QLabel` round trip) is gone. Selection now spans every message and code
  block. The reserved action strip is drawn by the layout and a single reusable
  hover bar handles copy/edit/delete. The "Load earlier messages" button was
  replaced by automatic paging when the reader reaches the top, and scroll
  anchors by message id keep the reader's place across rebuilds.

- (Refinement) Make strip of scrollbar (in conversation view) corner no longer rounded to match the overall styles.
  `ui::ScrollBar` now paints the handle with `drawRect`, without antialiasing or
  a corner radius. The change is shared by every custom scrollbar (conversation,
  sidebar and settings), since the rest of the chrome is square-cornered.
- (Bug) Currently the screenshots produced by the tool does not provide example conversations. And the names mismatch the ones recorded in `README.md`.
  The tool now seeds three example conversations (including a featured one with a
  code block for the review dialog) and writes exactly the two images the README
  references: `docs/screenshots/main-window.png` and
  `docs/screenshots/summary-review.png`. The summary-review capture runs a real
  `lnb::SummaryPlan`; `append_message` was added to the core interface for
  headless seeding.

- (Bug) On any connections involving TLS on Windows: TLS handshake failed: certificate verify failed (SSL routines)
  The HTTP client now imports the certificates from the Windows ROOT store into
  the connection's X509 store before verifying. The vcpkg OpenSSL build ships no
  CA bundle and `set_default_verify_paths()` leaves the store empty on Windows;
  the `SSL_CERT_FILE`/`SSL_CERT_DIR` overrides are still honoured, and `crypt32`
  is linked on Windows.
- (Refinement) Default language should decided by detecting system language.
  `load_settings()` now defaults the UI language from `QLocale::system()`: Chinese
  maps to `zh`, Japanese to `ja`, and everything else to `en`. An explicitly
  stored language still wins.
- (New Function) Make the shared directories write-able by the models. In session-settings, it should provide switches to turn on or turn off certain permissions (read, write, search online, all 3 kinds of tools) for the current session. (the tools run efficiently even though the repo is large) (Store them persistantly with DB column)
  Shared directories can now be modified through `write_file`, `edit_file` and
  `delete_file`, all sandboxed to the configured directories (symlink escapes
  rejected, the parent must exist, directories are never removed, and a single
  write is capped at 4 MiB). The Session Editor has a "Tool permissions" section
  with a master switch plus read, write and online toggles, persisted in new
  `session` columns (schema version 6, with ALTER TABLE migrations for older
  databases). Reads and online access keep the previous behavior; writing
  defaults to off.

- (Refinement) It now shows reviewer launched by the altered button on the right of the input box when either the session-level trigger or the assistant-level trigger is met.

- (Refinement) Scrollbar strip should move when history is loaded. Currently, it will if it's scrolled, but the strip will not move if it's because the user drags the strip.
  The transcript no longer pages history while the scrollbar thumb is held. A drag
  that reaches the top defers its single page load until `sliderReleased`, so the
  anchored restore moves the thumb down by the inserted page instead of Qt
  snapping it back to the pointer and paging again. Wheel, keyboard and track
  scrolling keep paging as before.
- (Refinement) Summarization settings should be separated from budget settings.
  The session editor now groups its fields into *Summarisation* (summary trigger,
  retained tokens), *Budgets* (memory budget, reasoning headroom, response
  budget) and *Tool limits* (output cap, rounds, calls). The assistant form
  separates *Summarisation* (trigger tokens, session pool) from *Budgets* (memory
  budget, reasoning headroom). Presets still fill every group.
- (Refinement) Generation tasks should not block other sessions, only assistant-level summerization should block others.
  The core keeps one task slot per session plus one slot for the shared
  assistant fold. A turn or session summarisation in one session no longer
  cancels or blocks another; the old single `task`/`worker` pair and its global
  "busy" refusal are gone. The assistant fold is the only cross-session
  operation: while it runs, new prompts are refused (`ERR_ASSISTANT_BUSY`) and
  it waits for the session turns already in flight before reading their rows.
  `start_generation()` now takes the session id explicitly (no shared "pending
  prompt" slot), `cancel_generation()` became `cancel_session()` and
  `cancel_assistant()`, and the UI uses `session_busy()`, `assistant_busy()` and
  `assistant_task()` instead of inferring the fold from one global task.
- (New Function) Make lock files to prevent the same assistant (so it's stored in the assistant directory, which is under `UUID/`) to be opened by more than one instances. Automatically remove it if it's stale (The program should check it, the lock file might do this by saving a PID for later programs to check if it's stale, on the case it's stale, the program can remove it). Afterall, it's the first time I make draft for such specific demand, you can always propose a "better" design.
  `interface::open_assistant()` takes an exclusive operating-system lock
  (`flock` on POSIX, `LockFileEx` on Windows) on `data/<uuid>/lock` and holds it
  for the process lifetime. The file records the owner's pid and start time for
  diagnostics only. Staleness needs no pid-liveness check: the kernel releases
  the lock when the owner exits, even after a crash, so a leftover file never
  blocks the next instance (and a pid check would be wrong across pid reuse). A
  second instance is refused; `open_assistant()` returns false and
  `last_error()` names the owner. The file is deliberately not deleted on
  release, so every process always opens the same inode.
- (Refinement) Add a toggle to make assistant-level memory summarisation ignore this session. (Store them persistantly with DB column)
  `session.include_in_assistant_memory` (schema version 7, migrated with ALTER
  TABLE) defaults to on. The Session Editor has an *Include in assistant memory*
  switch. An opted-out session is skipped by the cross-session watermark query,
  by `assistant_trigger_reached()` and by the assistant fold itself; its own
  conversation summarization is untouched, and opting back in reads only messages
  newer than its unchanged watermark.
- (Refinement) Add more default themes (Ordinary).
  `themes/theme.json` now also ships Plain Light, Plain Dark, GitHub Light,
  GitHub Dark, One Dark, Nord, Solarized Light and Solarized Dark, all with the
  complete colour key set and no transparency, so they behave like the plain
  Catppuccin themes.

- (Refinement) Disable and remove `max_tool_rounds`, `max_tool_calls` from settings, as they usually don't help limiting budgets.
- (Refinement) Rename `tool_output_max_tokens` to `input_token_produced_midway`
- (Refinement) Disable and remove `summary_reasoning_headroom_tokens`.
- (Refinement) Make summarizer reviewer show the estimated tokens.
- (Refinement) Make separate reasoning effort switches (2 switches, one in assistant settings, one in session summarization settings) for both assistant and session summarization. Default for them: `none`.
  `Assistant::summary_reasoning_effort_` (settings.json, snake_case key
  `summary_reasoning_effort`) and `session.summary_reasoning_effort` (schema
  version 10, migrated with ALTER TABLE) both default to `none`. The assistant
  settings page and the session editor's Summarization page each gained a
  *Summarization reasoning* selector, independent of the chat level, and both
  summarisers read those fields instead of the chat level. A legacy settings file
  without the key loads as `none`; an unsupported level is still refused at
  summarisation time rather than silently replaced.

- (Refinement) Refactor: rename session-level memory to summarization

  Rename the per-session "memory" concept to "summarization" across the core,
  UI and docs. Assistant-level memory is deliberately left untouched.
  
  The two concepts were easy to confuse: session summarization compresses one
  conversation, while assistant memory is shared across every session. The name
  now says what the feature actually does.
  
  Core / DB:
  - Session::summary_, summary_watermark_ and summary_trigger_tokens_, plus the
    set_session_summary / messages_after_summary_watermark /
    has_session_summary_trigger_met APIs.
  - ERR_SESSION_SUMMARY_TRIGGER_HAS_MET, fold_session_summary,
    maybe_summarize_session / summarize_session_now.
  - Prompts and logs now use "[SESSION SUMMARY]" / "[EXISTING SUMMARY]",
    "session summarization" and "Updating summarization...".
  - Schema version 11 renames session.memory -> summary and
    session.memory_watermark -> summary_watermark. Older databases are migrated
    in the constructor, so existing data stays in place.
  
  UI:
  - SessionEditor, ConversationView/Controller, MainWindow, SummaryReviewDialog
    and TasksWindow now talk about summarization; summarize.svg replaces the old
    icon and the screenshots follow.
  - i18n keys renamed in every locale with no duplicate entries.
  
  Docs:
  - README.md, docs/architecture.md and docs/todos/todo-archived.md updated.
  
  Assistant-level memory keeps all of its names: Assistant::memory_,
  [ASSISTANT MEMORY], assistant_memory_watermark_,
  include_in_assistant_memory_, fold_assistant_memory, set_assistant_memory.

- (Refinement) Tasks window is now 2 column.
- (Refinement) Remove the right pane of Assistant Picker.
- (Bug) Fix the case where the review is not triggered.
  Review every summarization request: each fold batch is previewed and approved, and the automatic pre-reply fold a send may trigger now passes the approver too.
- (Refinement) Tasks window shows live input/output estimates for the request in flight, added to the measured totals that accumulate across a task's requests.
- (Refinement) There are limits for both summarization (called summarization budget) and memory (called memory budget), so please make theirs inspect-able in Tasks window as well.
