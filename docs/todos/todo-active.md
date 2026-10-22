# TODOs - Active

This file reveals the current jobs we are working on.
If the TODO has been completed, please move the record to `todo-archived.md`.

## Pending

- (New Function) Make lock files to prevent the same assistant (so it's stored in the assistant directory, which is under `UUID/`) to be opened by more than one instances. Automatically remove it if it's stale (The program should check it, the lock file might do this by saving a PID for later programs to check if it's stale, on the case it's stale, the program can remove it). Afterall, it's the first time I make draft for such specific demand, you can always propose a "better" design.
- (Refinement) Add a toggle to make assistant-level memory summarisation ignore this session. (Store them persistantly with DB column)
- (Refinement) Add more default themes (Ordinary).
