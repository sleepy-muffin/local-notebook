# Local Notebook

**A local-first AI notebook whose memory you can read, edit, and trust — in a
hand-drawn, native desktop UI.**

[![License: MIT](https://img.shields.io/badge/license-MIT-cba6f7.svg)](LICENSE.txt)
[![Latest release](https://img.shields.io/github/v/release/sleepy-muffin/local-notebook?color=f5c2e7)](../../releases/latest)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-cba6f7.svg)](#download)
[![Built with Qt 6](https://img.shields.io/badge/Qt-6-cba6f7.svg)](https://qt.io)

<!-- TODO: replace with a 30–60s demo GIF (see docs/architecture.md for the screenshot tool) -->
![Local Notebook](docs/screenshots/main-window-light.png)

Local Notebook is a desktop notebook for talking to a local or self-hosted model.
It runs one **assistant** per process across many **sessions**, and keeps long
conversations coherent with a **two-level memory** that you can open, edit and
regenerate yourself — so the prompt never grows without bound, and nothing about
your history is hidden from you.

No cloud account. No telemetry. No JavaScript anywhere in the stack.

---

## Download

Grab a self-contained build — no compiler, no Qt install required:

| Platform | File |
|---|---|
| **Linux** (x86_64) | [`local-notebook-*-linux-x86_64.AppImage`](../../releases/latest) |
| **Windows** (x86_64) | [`local-notebook-*-windows-x86_64.zip`](../../releases/latest) |

```sh
# Linux: make it executable and run
chmod +x local-notebook-*-linux-x86_64.AppImage
./local-notebook-*-linux-x86_64.AppImage
```

On Windows, unzip and run `local-notebook.exe`. Checksums are published alongside
each release in `SHA256SUMS`.

> Prefer to build from source? See [Building](#building).

---

## Memory you can read and edit

This is the part that makes Local Notebook different.

Every AI client eventually has to forget. Most do it invisibly, and you find out
when the model "remembers" something wrong. Local Notebook instead writes its
memory down as **plain, editable text** and shows it to you:

- **Session memory** — what this one conversation has compressed itself into.
- **Assistant memory** — what the assistant has learned about you across *every*
  session.

Both are ordinary text. You can read them, fix them, or ask for a regeneration —
and **every** summarisation, automatic or manual, is shown to you for review
*before* it is sent, together with the exact payload and the bookkeeping it will
update:

![Summarisation review: the exact request is shown before it is sent](docs/screenshots/summary-review-light.png)

Two guarantees fall out of this design:

- **Idempotent.** Summarising twice with no new messages produces the same
  memory — because the watermarks only *count* what is new; they never decide
  what gets sent.
- **No derived budgets.** The input bound is the session's own
  `summary_trigger_tokens`; the produced bound is `response_token_budget`, which
  covers the reply and the tool results it fed back during the turn. Nothing is
  computed from a "context window", so nothing drifts.

---

## What you get

- 🧠 **Two-level, editable memory** — session and assistant memory, both visible
  and both yours to change.
- 🎨 **A native, hand-drawn UI** — a flat, IDE-like shell in the spirit of
  [Zed](https://zed.dev): low contrast, hairlines, no speech bubbles, no centred
  column. Ships with **Catppuccin Mocha and Macchiato**, plus a theme system you
  can edit.
- 🌐 **Three UI languages** — English, 中文 and 日本語, chosen from
  **Settings → Configuration**. The default follows the system language on first
  run.
- 🧩 **Tools, not just chat** — `web_search` and `fetch_url` are always available;
  `search_code`, `search_code_regex`, `find_files`, `list_directory` and `read_file`
  appear when a session has shared directories, with paths sandboxed to them (symlink
  escapes rejected). When writing is enabled, `write_file`, `edit_file` and
  `delete_file` can create, change and remove files there too. Each session has its
  own switches for all tools, reading, writing and online access. The model only
  ever sees relative paths: a single directory is transparent, while several appear
  as named top-level entries. The ripgrep-style tools honour `.gitignore`/`.ignore`
  and skip hidden entries.
- 🔌 **Any OpenAI-compatible endpoint** — local or remote, with optional SOCKS5 /
  HTTP-CONNECT proxy support.
- 🗂️ **Sessions as notes** — pin, rename, edit instructions and per-session
  budgets, with a preset dropdown (light chat, balanced, novel writing, role-play,
  abstract design, large codebase).
- 🪶 **A pure C++23 / Qt Widgets codebase** — no `moc`, no `Q_OBJECT`, no JS. The
  interesting rules live in pure, testable functions.

---

## How it works

The short version: a request is assembled as **system instructions → assistant
memory → session memory → the newest messages that fit → tools**. Everything older
than the verbatim window is folded into memory, in reviewable batches.

The long version — the full architecture, the on-disk layout and SQLite schema,
the three levels of configuration, the summarisation rules, and the interface
spec — lives in **[docs/architecture.md](docs/architecture.md)**.

---

## Building

Requirements: a C++23 compiler (Clang is configured), CMake ≥ 3.31, Ninja, Qt ≥ 6.8
(`Core`, `Gui`, `Widgets`, `Network`) and SQLite 3. On Linux, the Wayland client
headers (`libwayland-client`, found through `pkg-config`) let the application set
the native Wayland window icon; without them that path is simply compiled out.

```sh
cmake --preset linux-x64-release
cmake --build build/linux-x64-release -j 8
```

A `linux-x64-debug` preset is also provided. Full build, packaging and test
details are in [docs/architecture.md](docs/architecture.md).

## Tests

Two dependency-free suites, registered with CTest:

```sh
ctest --test-dir build/linux-x64-release --output-on-failure
```

## License

MIT — see [LICENSE.txt](LICENSE.txt).
