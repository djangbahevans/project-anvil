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
| `ui` | Qt Widgets: main window, editor widget painting, trees, dialogs, theming. |
| `workbench` | Commands, keybindings, configuration, workspace model, language-feature routing, terminal sessions. |
| `exthost` | Node process supervision and the C++ implementation of the `MainThread*` RPC services, including commands and the documents and editors that extensions see ([0005](adr/0005-reuse-upstream-extension-host.md)). |
| `core` | Text buffer, undo, cursors and selections, line layout, highlighting, UTF-16 boundary conversion ([0006](adr/0006-editor-core-and-text-encoding.md), [0010](adr/0010-syntax-highlighting.md)). |
| `platform` | Pseudo-terminals, file watching, process spawning, OS paths. The only layer with per-OS code ([0008](adr/0008-cross-platform-from-the-start.md)). |

## Editor core

`core::Text` is an immutable, persistent rope of UTF-8 chunks whose nodes cache byte, UTF-16 and newline counts; copying it is a snapshot. `core::TextBuffer` holds the current text and reports each edit as a `TextChange`. The text uses `\n` line breaks only. Positions exchanged with extensions and language servers are zero-based line and UTF-16 column, converted by `Text::position_at` and `Text::offset_at` ([0006](adr/0006-editor-core-and-text-encoding.md)).

`core::Document` adds selections, an undo history and the file format (line ending, byte order mark) to a buffer. Edits are applied as validated batches and reported as ordered change lists; undo steps hold forward and inverse replacements that share storage with the text, and edits can be joined into undo groups ([0011](adr/0011-document-model.md)).

## Extension compatibility

The upstream extension host runs unmodified from a pinned VS Code release. The editor answers its RPC calls for the in-scope API surface and reports unsupported API use explicitly. Extensions are installed from Open VSX or from local `.vsix` files ([0007](adr/0007-extension-registry-open-vsx.md)).

## Build

C++23, CMake with Ninja and per-platform presets, vcpkg manifest for dependencies other than Qt ([0002](adr/0002-language-and-build-system.md)).

## Milestones

1. **Editor slice:** window, open a file, edit in the custom editor widget, save.
2. **Extension-host session:** codecs, session, launcher and startup sequence in `src/exthost`, verified by an end-to-end test that activates an extension and runs its command ([0005](adr/0005-reuse-upstream-extension-host.md)).
3. **Language features:** LSP client, tree-sitter highlighting, diagnostics, completion.
4. **Workbench:** explorer, search, command palette, settings, themes, terminal.
5. **Extension breadth:** widen the supported `vscode` API surface, driven by real extensions.
