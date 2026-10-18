# TODOs - Delayed

This file stores the changes planned to be executed in the future.
We do not start with them now.

## Experience Improvements

- (Principle) We want the program to be as explicit as possible: surface the
  exact state (which endpoint, which reasoning level) rather than hiding it or
  guessing on the user's behalf.

- When the user edit its message while it's the latest message in current session in the case previous message failed to generate or it's currently being generated should be cancelled. Then re-generate.

- (New Function) Add in-application documentation. Which has many tabs as well. It renders markdowns under `assets/docs/manual.md`, then we will have it as the first-order truth in substitue of `docs/architecture.md`. It should describe the exact behaviors of the application.
- (Refinement) The buttons on the font selector, font size selector, numerical selector, preset selector look quite bad.
- (Refinement) There should be no divider on the status bar.
- (Refinement) Add a toggle to disable the contribution from this session to be used for assistant-level summarization. Also, a button to disable its own session-level summarization.

- (Refinement) We can make our own application chrome, though it's not demanded now.

- (Refinement) Assistant-level summarisation should produce task log on clicking it in Tasks window.
