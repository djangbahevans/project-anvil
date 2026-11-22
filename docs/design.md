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
| `ui` | Qt Widgets: the main window with a tab per open document, the editor view that paints a document and turns input into editing commands, the quick pick popup behind the command palette, and the bridge that runs the extension host from the event loop. |
| `workbench` | Commands, keybindings, configuration, workspace model, language-feature routing, terminal sessions. |
| `exthost` | Node process supervision and the C++ implementation of the `MainThread*` RPC services, including commands and the documents and editors that extensions see ([0005](adr/0005-reuse-upstream-extension-host.md)). |
| `core` | Text buffer, undo, cursors and selections, search and replace, `when` expressions, line layout, highlighting, UTF-16 boundary conversion ([0006](adr/0006-editor-core-and-text-encoding.md), [0010](adr/0010-syntax-highlighting.md), [0013](adr/0013-pcre2-for-search.md)). |
| `platform` | Pseudo-terminals, file watching, process spawning, OS paths. The only layer with per-OS code ([0008](adr/0008-cross-platform-from-the-start.md)). |

## Editor core

`core::Text` is an immutable, persistent rope of UTF-8 chunks whose nodes cache byte, UTF-16 and newline counts; copying it is a snapshot. `core::TextBuffer` holds the current text and reports each edit as a `TextChange`. The text uses `\n` line breaks only. Positions exchanged with extensions and language servers are zero-based line and UTF-16 column, converted by `Text::position_at` and `Text::offset_at` ([0006](adr/0006-editor-core-and-text-encoding.md)).

`core::EditController` implements the editing commands (typing, deleting, motion, selection, undo, whole-line copy, cut and paste) on a document, for all cursors at once; the Qt editor view only paints and forwards input ([0012](adr/0012-editing-commands-in-core.md)).

`core::Document` adds selections, an undo history and the file format (line ending, byte order mark) to a buffer. Edits are applied as validated batches and reported as ordered change lists; undo steps hold forward and inverse replacements that share storage with the text, and edits can be joined into undo groups. The history keeps a bounded number of groups, 1000 by default ([0011](adr/0011-document-model.md)).

`core::Search` is a compiled query (literal, case, whole word, regular expression) with `find_next`, `find_previous`, `find_all` and replacement expansion. It reads a `Text` snapshot in bounded windows, so it never copies a whole document and can run on a worker thread; a stop token cancels it ([0013](adr/0013-pcre2-for-search.md)). `ui::FindBar` drives it: documents up to 1 MiB are searched at once, larger ones on a worker thread whose result is discarded when the document changes, and at most 20 000 matches are collected. Replace All is one `Document::apply_edits` call, so it is one undo step.

`core::WhenExpression` parses and evaluates VS Code `when` expressions, the conditions that decide whether a keybinding, menu item or command applies (`editorTextFocus && !editorReadonly`, `resourceExtname == .md`). `evaluate` reads a `core::ContextKeys`, a map from key to a boolean, number, string or list of strings, and an unset key is falsy. The syntax, the precedence and the JavaScript-style coercions of `==`, the comparisons and `=~` follow VS Code's `contextkey.ts`, so the `when` clauses of extensions behave as they do there; `=~` patterns are compiled with PCRE2 in the JavaScript-compatible mode that [0013](adr/0013-pcre2-for-search.md) uses for search. Parsing normalizes: `&&` is distributed over `||`, negation is pushed down to the keys, and terms are sorted and de-duplicated, so two spellings of the same expression have the same `canonical()` string and compare equal, which the keybinding resolver relies on. A parse error reports the byte offset of the problem. An expression is immutable and cheap to copy, so it can be evaluated from any thread.

`core::ColorTheme` loads a VS Code color theme: the workbench colors and the token color rules of a theme file and of the files it includes (`dark_modern.json` includes `dark_plus.json`, which includes `dark_vs.json`), following `colorThemeData.ts`. An included file is applied first and the including file on top, so the including file's colors win and its token rules come later; `colors` maps a workbench color key to `#rgb`, `#rgba`, `#rrggbb` or `#rrggbbaa`, the value `"default"` removes a key an included file set, a value that is not a string is ignored, and a string that is not a color is skipped with a warning. The token rules stay the JSON that was written, for the scope matcher. The file is JSON with comments, found through a loader function so that `core` does no file I/O, includes are relative to the file that names them (up to 16 deep, with cycles and missing files reported with the chain of files), and a theme's kind (dark, light, high contrast) comes from the contributing manifest's `uiTheme`. A `tokenColors` that names a TextMate file, the legacy `settings` array and an include that is not a `.json` file are errors; `semanticTokenColors` is ignored.

The color themes that ship with the editor are VS Code's default themes, copied unchanged into `resources/themes` with a `themes.json` that lists them (`id`, `label`, `uiTheme`, file) and the VS Code license text; `THIRD_PARTY_NOTICES.md` names them. They are data the editor needs without the VS Code checkout, which is only a build-time dependency. `ui::themes_directory` finds them: `share/anvil/themes` next to the `bin` directory of an installed editor (CMake installs them there, with the license files in `share/doc/anvil`), and otherwise the source tree's `resources/themes`, for an editor run from its build directory. The `workbench.colorTheme` setting names a theme by its `id`; its default is `Dark 2026`, VS Code's default in the pinned release, and is one constant (`EditorDefaults::color_theme`). `scripts/sync-themes.mjs` refreshes the copies from the checkout, and a test fails when they differ.

`core::score_item` ranks labels against a query as VS Code's quick pick does: a port of the behavior of `fuzzyScorer.ts`. A query is prepared once (`prepare_query`: quotes, `*`, `…`, whitespace and a trailing `#` removed, words split on spaces) and then scores any number of items, each a label with an optional description and file path. A score of zero is no match; a match in the label scores above one that needed the description, a prefix of the label above any other match in it, and a query equal to the item's path highest. Matched characters are reported as ranges for highlighting, and `compare_items` orders two scored items by score, then by how compact the match is, then by label length. Text is UTF-8 and the algorithm and all reported positions use UTF-16 code units, as VS Code's do. Case is folded for ASCII letters only, and the last tie-break compares code units instead of using a locale collator; the module documents both. `tests/core/data/fuzzy_cases.json` holds results recorded from VS Code's own scorer (`scripts/generate-fuzzy-cases.mjs`), which the tests replay.

## Commands

The editor's operations are commands in `ui::CommandRegistry`, keyed by VS Code's command ids (`undo`, `cursorLeft`, `editor.action.clipboardCopyAction`, `workbench.action.files.save`) and each with a palette title. Menus run a command by its id, and `MainWindow` hands every registered command to the extension host's `CommandsService` when the host starts, so `vscode.commands.executeCommand` runs the same code as the menu. A command receives the JSON array of arguments and ignores what it does not use. [Adding an editor command](howto/adding-a-command.md) describes how to register one.

The command palette (`workbench.action.showCommands`, Ctrl+Shift+P and F1) lists the registry's commands by title and the commands extensions registered by id, ranks them with the fuzzy matcher and highlights the matched characters, and puts the recently used ones first. It is a `ui::QuickPick`: a popup over the top center of the window with a filter field and a list that filters and ranks its own items, reports the chosen item by position, and gives the focus back to where it was. The same widget serves the extension API's quick pick. A command chosen in the palette runs through the extension host's command service when a host is running, so extension commands and the editor's own take one path, and it does not run while a dialog is open.

## Window structure

The window is arranged from fixed parts, in VS Code's positions and with its part ids: the title bar above; the activity bar at the left edge; the primary sidebar; the editor area; the bottom panel; and the status bar below ([0014](adr/0014-workbench-layout.md)). Splitters resize the sidebar and the panel, and each part can be hidden. The window uses the native frame and menu bar.

`ui::Workbench` is the window's central widget and arranges the parts: the activity bar at the left edge (48 px), a horizontal splitter with the sidebar and, beside it, a vertical splitter with the editor area above the panel, while the window's own status bar stays at the bottom. Each part has a `Part` and VS Code's id (`workbench.parts.sidebar`), is shown and hidden with `set_visible` or `toggle`, and reports a size. The sidebar and the panel start hidden, as does the activity bar, because they are empty containers until views are registered; showing one gives it the size it had when it was hidden (300 px the first time), within its minimum (170 px wide, 77 px high) and what the editor area keeps (220 by 70 px), and hiding one returns its space to the editor area. The splitter handles are 4 px sashes that show the border color of the part as a 1 px line and the `sash.hoverBorder` color while hovered or dragged. The activity bar and the sidebar are filled from `ui::ViewContainerRegistry`: a view container has an id, a title, a monochrome icon, a location (sidebar or panel), an order and a function that makes its widget, and the registry knows nothing about what is inside. `ui::Sidebar` shows a button for each sidebar container in the activity bar (hidden while there are none) with the active one marked on its left edge, and the chosen container in the sidebar under its title in capitals. Choosing a container shows the sidebar with it, choosing the one that is shown hides the sidebar, and showing the sidebar another way shows the last active container, or the first. A container's widget is made the first time it is shown and kept, and removed with the container. Icons are drawn as masks in the foreground or the inactive color of the activity bar.

`ui::Panel` fills the bottom panel the same way: a 35 px header with a tab for each container registered for the panel (titles in capitals, the active one underlined), a button that maximizes the panel and one that closes it, and the chosen container below. Choosing a tab shows that container, closing hides the panel and keeps the container, and showing the panel another way shows the last active container or the first. Maximizing hides the editor area so that the panel fills it, and restoring shows the editor area again with the panel at the size it had; hiding a maximized panel restores it. The widgets of the sidebar and the panel come from `ui::ViewContainerStack`, which makes a container's widget the first time it is shown and deletes it with the container. `workbench.action.toggleMaximizedPanel` toggles the maximized state.

View > Toggle Sidebar (Ctrl+B) and Toggle Panel (Ctrl+J) run the commands `workbench.action.toggleSidebarVisibility` and `workbench.action.togglePanel`.

The sidebar and the panel host view containers: each is registered with an id, a title, an icon and a factory, and its widget is created the first time it is shown. The explorer, search, the terminal and extension tree views plug in the same way. The editor area is a tree of editor groups and splitters; the model of groups, their tabs and the split orientations is plain data with no widget types. The status bar keeps a left and a right group of items ordered by priority. Every part reads its colors by workbench color key through one lookup, `ui::WorkbenchColors`, that a color theme fills: a part asks for `statusBar.background` or `tab.activeBackground` and never holds a color. The keys that exist are one table in `src/ui/workbench_colors.cpp`, ordered by key, each with VS Code's name, a description of where it is used, and its fallback: what the theme gives another key (`sash.hoverBorder` follows `focusBorder`, as in VS Code) or a role of the palette, so the window looks as it does today when no theme is loaded. A theme replaces all its values at once and every part repaints once; asking for a key that is not in the table is a programming error and answers magenta. The quick pick and the command palette read their colors this way; the editor view still reads the palette. To give a part a color, add the key to the table, with a fallback, and read it through the lookup.

## Settings

`core::parse_jsonc` reads JSON with comments and trailing commas and reports a syntax error with its line and column; the settings, keybinding and theme files share it. `core::Settings` holds the parsed object of the user settings file, finds a dotted key (`editor.fontSize`) whether the file writes it flat or nested, and keeps keys that nothing reads. `core::EditorSettings` reads the keys the editor uses with VS Code's defaults and ranges; per-platform defaults come from `platform::editor_defaults()` so that `core` has no per-OS code. A value of the wrong type is an error, not a silent default, so that a typo never changes the editor unnoticed.

`ui::SettingsStore` reads `<user config dir>/settings.json` and watches it with `QFileSystemWatcher`. It watches the file and the nearest existing directory above it, and sets both watches again after every change, so a file that is created later, deleted, or replaced by an editor that writes a temporary file and renames it is followed. Changes are debounced. A file that cannot be used leaves the previous values in force and is reported once per distinct problem, through a signal that the window turns into a status bar message, and in the log. `ui::MainWindow` applies the values: `EditorView::apply_settings` takes the font, tab size and Tab key behavior and keeps the caret and the top visible line, the document service reports the tab size and `insertSpaces` to extensions, and new untitled documents take their line ending from `files.eol`. Applying settings happens when the file changes, never per keystroke.

Settings are global. Per-language overrides (`"[python]": {...}`), workspace settings and a settings editor are not implemented; the parsed JSON keeps its nesting so that they can be added.

## Extension compatibility

The upstream extension host runs unmodified from a pinned VS Code release. The editor answers its RPC calls for the in-scope API surface and reports unsupported API use explicitly. Extensions are installed from Open VSX or from local `.vsix` files ([0007](adr/0007-extension-registry-open-vsx.md)).

## Build

C++23, CMake with Ninja and per-platform presets, vcpkg manifest for dependencies other than Qt ([0002](adr/0002-language-and-build-system.md)).

## Roadmap

The roadmap is tracked as milestones and issues on GitHub (`djangbahevans/project-anvil`), where it stays current. The milestones group the work into the editor's basics, language support, the breadth of the extension API, the workbench (explorer, search, themes, terminal), cross-platform support, and performance and memory.
