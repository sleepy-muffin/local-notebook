# TODOs - Archived

## Bugs

### Windows CI fails on the step named "Test"

Linux has passed the building.

Run ctest --test-dir build --output-on-failure
Test project D:/a/local-notebook/local-notebook/build
    Start 1: store
1/2 Test #1: store ............................   Passed    0.59 sec
    Start 2: ui
2/2 Test #2: ui ...............................***Failed    1.18 sec
FAIL line 427: inner->minimumSizeHint().width() <= area->viewport()->width()
FAIL line 427: inner->minimumSizeHint().width() <= area->viewport()->width()
FAIL line 427: inner->minimumSizeHint().width() <= area->viewport()->width()
FAIL line 427: inner->minimumSizeHint().width() <= area->viewport()->width()
4 check(s) failed


50% tests passed, 1 tests failed out of 2

Total Test time (real) =   1.78 sec

The following tests FAILED:
	  2 - ui (Failed)
Errors while running CTest
Error: Process completed with exit code 1.

## Experience Improvements

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
