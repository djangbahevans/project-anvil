# Anvil design

Anvil is a native C++ code editor aimed at the VS Code workflow, with lower memory use and faster startup and input latency than an Electron application. It runs a subset of VS Code extensions in a separate Node.js process. Scope is defined in [ADR 0009](adr/0009-scope-and-non-goals.md); the rationale for each choice below is in the linked ADRs.

## Process model

```text
+---------------------------- anvil (C++/Qt 6) ----------------------------+
|  ui (Qt Widgets)                                                         |
|     |                                                                    |
|  workbench services: commands, config, workspace, language, terminal     |
|     |                         |                                          |
|  editor core            ext-host bridge  ---- pipe / socket ----+        |
|  (buffer, cursors,      (MainThread* services)                  |        |
|   layout, highlight)                                            |        |
|  platform (pty, fs watch, process)                              |        |
+-----------------------------------------------------------------|--------+
                                                                  |
      LSP servers, ripgrep, shells: child processes     +---------v--------+
                                                        | node: upstream   |
                                                        | extension host   |
                                                        +------------------+
```

- The editor is one native process. Extensions run in a separate Node process started on first use ([0004](adr/0004-extension-host-node-child-process.md)).
- Language servers, ripgrep and shells are child processes managed by the editor.

## Layers and dependency rule

Dependencies point downward only; `core`, `workbench` and `platform` never include Qt GUI headers ([0003](adr/0003-ui-toolkit-qt6-widgets.md)).

| Layer | Responsibility |
| --- | --- |
| `ui` | Qt Widgets: the main window with a tab per open document, the editor view that paints a document and turns input into editing commands, and the bridge that runs the extension host from the event loop. |
| `workbench` | Commands, keybindings, configuration, workspace model, language-feature routing, terminal sessions. |
| `exthost` | Node process supervision and the C++ implementation of the `MainThread*` RPC services, including commands and the documents and editors that extensions see ([0005](adr/0005-reuse-upstream-extension-host.md)). |
| `core` | Text buffer, undo, cursors and selections, search and replace, line layout, highlighting, UTF-16 boundary conversion ([0006](adr/0006-editor-core-and-text-encoding.md), [0010](adr/0010-syntax-highlighting.md), [0013](adr/0013-pcre2-for-search.md)). |
| `platform` | Pseudo-terminals, file watching, process spawning, OS paths. The only layer with per-OS code ([0008](adr/0008-cross-platform-from-the-start.md)). |

## Editor core

`core::Text` is an immutable, persistent rope of UTF-8 chunks whose nodes cache byte, UTF-16 and newline counts; copying it is a snapshot. `core::TextBuffer` holds the current text and reports each edit as a `TextChange`. The text uses `\n` line breaks only. Positions exchanged with extensions and language servers are zero-based line and UTF-16 column, converted by `Text::position_at` and `Text::offset_at` ([0006](adr/0006-editor-core-and-text-encoding.md)).

`core::EditController` implements the editing commands (typing, deleting, motion, selection, undo, whole-line copy, cut and paste) on a document, for all cursors at once; the Qt editor view only paints and forwards input ([0012](adr/0012-editing-commands-in-core.md)).

`core::Document` adds selections, an undo history and the file format (line ending, byte order mark) to a buffer. Edits are applied as validated batches and reported as ordered change lists; undo steps hold forward and inverse replacements that share storage with the text, and edits can be joined into undo groups. The history keeps a bounded number of groups, 1000 by default ([0011](adr/0011-document-model.md)).

`core::Search` is a compiled query (literal, case, whole word, regular expression) with `find_next`, `find_previous`, `find_all` and replacement expansion. It reads a `Text` snapshot in bounded windows, so it never copies a whole document and can run on a worker thread; a stop token cancels it ([0013](adr/0013-pcre2-for-search.md)). `ui::FindBar` drives it: documents up to 1 MiB are searched at once, larger ones on a worker thread whose result is discarded when the document changes, and at most 20 000 matches are collected. Replace All is one `Document::apply_edits` call, so it is one undo step.

## Extension compatibility

The upstream extension host runs unmodified from a pinned VS Code release. The editor answers its RPC calls for the in-scope API surface and reports unsupported API use explicitly. Extensions are installed from Open VSX or from local `.vsix` files ([0007](adr/0007-extension-registry-open-vsx.md)).

## Build

C++23, CMake with Ninja and per-platform presets, vcpkg manifest for dependencies other than Qt ([0002](adr/0002-language-and-build-system.md)).

## Roadmap

The roadmap is tracked as milestones and issues on GitHub (`djangbahevans/project-anvil`), where it stays current. The milestones group the work into the editor's basics, language support, the breadth of the extension API, the workbench (explorer, search, themes, terminal), cross-platform support, and performance and memory.
