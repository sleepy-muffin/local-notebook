# TODOs - Active

This file reveals the current jobs we are working on.
If the TODO has been completed, please move the record to `todo-archived.md`.

## Small Improvement

- Make lock files to prevent the same assistant (so it's stored in the assistant directory, which is under `UUID/`) to be opened by more than one instances. Automatically remove it if it's stale (The program should check it, the lock file might do this by saving a PID for later programs to check if it's stale, on the case it's stale, the program can remove it). Afterall, it's the first time I make draft for such specific demand, you can always propose a "better" design.
- Make it support Android platforms as an Android application.
- Make the shared directories write-able by the models. In session-settings, it should provide switches to turn on or turn off certain permissions for the current session. (Store them persistantly with DB column)

## Conversation View / Renderer

- (Completed, this round) Scrollbar track. A stylesheet-only transparent track is
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
- (Completed, this round) Inline code. The rounded plate now drops its horizontal
  padding on a side where the neighbouring character is not whitespace, so the
  background no longer runs into adjacent text (padding is kept next to spaces
  and at line/paragraph edges). Inline code is also sized as
  `inline_code_scale` x the surrounding text instead of using the configured
  monospace size; the ratio is configurable under Appearance as "Inline code
  size" (default 0.9, range 0.50-1.50) and is persisted in `settings.json` as
  `inline_code_scale`.
