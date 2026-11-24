# Anvil

A native C++ code editor for the VS Code workflow, with a Qt 6 UI and a Node.js extension host. See [docs/design.md](docs/design.md) for the architecture and [docs/adr/](docs/adr/) for the decisions behind it.

## Requirements

- CMake 3.25 or later and Ninja
- GCC 13 or later, or Clang 18 or later
- GoogleTest, nlohmann-json, PCRE2 and, for benchmarks, Google Benchmark (system packages, or vcpkg when `VCPKG_ROOT` is set)
- Qt 6.4 or later with the Widgets module, needed only for the UI and the `anvil` executable. When Qt is not found, only the libraries and tests are built; `-DANVIL_BUILD_UI=ON` makes a missing Qt an error.

On Debian and Ubuntu: `sudo apt install build-essential clang cmake ninja-build libgtest-dev nlohmann-json3-dev libpcre2-dev qt6-base-dev`.

## Build and test

```sh
cmake --preset linux-clang-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
```

Presets: `linux-clang-debug`, `linux-clang-release`, `linux-clang-asan`, `linux-gcc-debug`, `windows-msvc-debug`, `windows-msvc-release`, `macos-debug`, `macos-release`. Build output goes to `build/<preset>/`.

## Running

```sh
build/linux-clang-debug/bin/anvil [folder] [file...]
```

The first argument that is a directory is the workspace folder: its name is in the window title and in the box in the middle of the title bar, and extensions see it as `workspace.workspaceFolders[0]`. It is not opened as a file; File > Open Folder… and Close Folder change it in a running window by restarting the extension host, which keeps the tabs and documents; another directory is reported and ignored, and so is an argument that ends in a separator but does not exist. The editor opens the given files in one window with VS Code's menus (items whose feature does not exist yet are shown disabled) and a command palette (Ctrl+Shift+P runs a command). Ctrl+F opens a find bar above the editor, with the selected text or the word at the caret already in its field: matches are highlighted and counted as the pattern is typed, Enter and Shift+Enter (or F3 and Shift+F3) step through them and wrap around, and the options match case, whole word and regular expression are toggles (Alt+C, Alt+W and Alt+R, Cmd+Alt on macOS, while the bar is open), and Escape closes the bar from the editor as well as from its fields. With nothing selected, Ctrl+C and Ctrl+X copy and cut the caret's line, and pasting that text inserts it above the line of the caret. With several cursors, copying takes each selection and the line of each caret (once per line), in document order; pasting into as many cursors as pieces gives each cursor its own piece, and text from another program whose line count equals the number of cursors goes one line to each cursor (a final line break is not counted). Any other text goes to every cursor. Ctrl+H adds a replace field with Replace and Replace All, where regular expressions can use `$1`-style groups. Ctrl+D (Cmd+D on macOS) selects the word at the caret and then adds the next occurrence of it as another cursor, wrapping around the document; with a selection it searches the selected text with the find bar's match case and whole word options, and with a lone caret it matches whole words with the same case. Ctrl+Shift+L selects every occurrence at once, and Alt+Enter with the find bar open selects every match of the bar's pattern, regular expression included, and moves the focus to the editor; at most 20000 are selected, and the status bar says so when there are more. Keys run commands through VS Code's default key bindings for the platform (Ctrl+S, Ctrl+W, Ctrl+B, Ctrl+J, Ctrl+\, Ctrl+Z and Ctrl+Y, F1 and the rest), including chords, and the menus show the key of each command; a chord in progress is shown in the status bar. Your own bindings go in `keybindings.json` (see Key bindings). Commands that extensions contribute are in the command palette under `Category: Title`, and their key bindings apply. It uses the extension host when a Node.js binary and the checkout from `scripts/fetch-vscode.sh` are found, and reads these environment variables:

View > Split Editor Right (Ctrl+\) and Split Editor Down add an empty editor group next to the focused one, each group with its own tabs; Focus Next Group, Focus Previous Group and Close Editor Group move between groups and remove one. Opening a file that is open in another group focuses its tab there, because a document is shown in one group at a time. Dragging a tab onto the tabs of another group moves it there, onto the middle of a group moves it to the end, and onto the nearest quarter of a group's edge splits the group and puts the tab in the new half; Escape cancels the drag.

| Variable | Meaning |
| --- | --- |
| `ANVIL_EXTENSIONS` | unpacked extension directories, separated like `PATH` entries |
| `ANVIL_NODE` | the Node.js binary (default: `node` on `PATH`) |
| `ANVIL_VSCODE_DIR` | the VS Code checkout (default: `.deps/vscode` of the source tree) |
| `ANVIL_EXTENSION_HOST=off` | run the editor without the extension host |
| `ANVIL_NATIVE_FRAME=1` | give the window the system's frame and menu bar instead of its own title bar (Linux and Windows) |

Extension host messages are printed to standard error. Under WSLg the window appears through Wayland or X11; `QT_QPA_PLATFORM=xcb` forces X11.

## Settings

The editor reads a user settings file at startup and again whenever it changes, so edits apply without a restart. The file is JSON with comments and trailing commas, with VS Code's key names and defaults:

| Platform | File |
| --- | --- |
| Linux | `$XDG_CONFIG_HOME/anvil/settings.json`, or `~/.config/anvil/settings.json` |
| macOS | `~/Library/Application Support/anvil/settings.json` |
| Windows | `%APPDATA%\anvil\settings.json` |

A missing file means the defaults; the editor never creates it. Keys can be written flat (`"editor.fontSize": 16`) or nested (`"editor": {"fontSize": 16}`), and keys that nothing reads are kept as written.

| Key | Meaning | Default |
| --- | --- | --- |
| `editor.fontFamily` | Comma-separated font families; the first installed one is used, `monospace` and an unknown list stand for the system's fixed-width font | `'Droid Sans Mono', monospace` on Linux, `Menlo, Monaco, 'Courier New', monospace` on macOS, `Consolas, 'Courier New', monospace` on Windows |
| `editor.fontSize` | Font size in pixels, clamped to 6-100 | 14 (12 on macOS) |
| `editor.tabSize` | Columns per tab stop, an integer of at least 1 (values above 1024 are treated as 1024) | 4 |
| `editor.insertSpaces` | Tab inserts spaces up to the next tab stop when `true`, a tab character when `false` | `true` |
| `editor.multiCursorModifier` | The key that makes a click add a cursor: `"alt"` (Alt+click), or `"ctrlCmd"` (Ctrl+click, Cmd+click on macOS). Shift with it, or Shift+Alt with `"ctrlCmd"`, selects a rectangle | `"alt"` |
| `editor.columnSelection` | Dragging the mouse selects a rectangle, one selection per line, instead of a stream of text | `false` |
| `editor.renderWhitespace` | Which spaces and tabs get a mark (a dot for a space, an arrow for a tab, in `editorWhitespace.foreground`): `"none"`, `"boundary"` (all but a single space between words), `"selection"` (inside selections), `"trailing"` (at the end of the line) or `"all"` | `"selection"` |
| `editor.renderControlCharacters` | Control characters are drawn as symbols (white on red) when `true` | `true` |
| `workbench.colorTheme` | The color theme, by id or label: one of the ten that ship with the editor, or one that an extension contributes in `contributes.themes` (the `uiTheme` of the manifest decides its kind) | `"Dark 2026"` |
| `workbench.editor.focusRecentEditorAfterClose` | Closing the active tab shows the tab that was shown before it when `true`, its right neighbor (the left one for the last tab) when `false` | `true` |
| `window.commandCenter` | Shows the box in the middle of the title bar that shows the workspace name and opens the command palette (custom title bar only) | `true` |
| `workbench.layoutControl.enabled` | Shows the buttons in the title bar that show and hide the side bar and the panel (custom title bar only) | `true` |
| `window.titleBarStyle` | `"custom"`: on Linux and Windows the window has no frame and draws its own title bar, with the menus and the window buttons in it; `"native"`: the window has the system's frame and the menus below it. macOS always has the system's frame. Read when the window opens, so a change applies after a restart | `"custom"` |
| `files.eol` | Line ending of new documents: `"\n"`, `"\r\n"` or `"auto"` (CRLF on Windows, LF elsewhere). Opened files keep their own | `"auto"` |
| `workbench.statusBar.visible` | Shows the status bar | `true` |
| `workbench.sideBar.location` | The edge of the window the primary side bar and the activity bar are at: `"left"` or `"right"` | `"left"` |
| `workbench.activityBar.location` | `"default"` shows the activity bar beside the side bar and `"hidden"` hides it; `"top"` and `"bottom"` are accepted and show it as `"default"` | `"default"` |
| `files.autoSave` | When a document with a file and unsaved changes is saved by itself: `"off"`, `"afterDelay"` (`files.autoSaveDelay` after its last change), `"onFocusChange"` (when its editor loses the focus, another tab replaces it, or the window loses the focus) or `"onWindowChange"` (when the window loses the focus). Untitled documents are never saved. File > Auto Save switches between `"off"` and `"afterDelay"` | `"off"` |
| `files.autoSaveDelay` | Milliseconds between the last change and the save, with `"afterDelay"` (0 to 86400000) | `1000` |
| `files.associations` | An object from a glob to a language id that decides the language of the files it matches before any language's own rules, as in `{"*.cfg": "ini", "**/.config/*": "json"}`. A pattern with a `/` is matched against the whole path and any other against the file name, ignoring case. The language shows in the status bar. An entry whose value is not a language id is skipped and logged | `{}` |

The tab size and `insertSpaces` are also what extensions see in `TextEditor.options`. A file with a syntax error, or a value of the wrong type, leaves the previous settings in force; the problem is shown once in the status bar with its line and column or key name, and is written to standard error.

A command that changes a setting edits `settings.json` in place, so your comments, order and formatting stay, and the change is in force at once. When the file cannot be edited safely (it has a syntax error, it is open in a tab with unsaved changes, or it changed while the command ran), the file is left as it is, the setting changes for the running window only, and the status bar says why.

## Key bindings

The editor reads `keybindings.json` from the same directory as `settings.json` at startup and again whenever it changes. The file is JSON with comments and trailing commas, and an array of entries as in VS Code:

```jsonc
[
  { "key": "ctrl+alt+p", "command": "workbench.action.showCommands" },
  { "key": "ctrl+k ctrl+t", "command": "workbench.action.togglePanel", "when": "editorTextFocus" },
  { "key": "ctrl+b", "command": "-workbench.action.toggleSidebarVisibility" }
]
```

A key is one to four steps (`ctrl+k ctrl+c`), a `when` is VS Code's context key expression, and a command that starts with `-` removes the default bindings of that command. Your bindings win over the default ones, and among them the later entry wins. A missing file means the default bindings; the editor never creates it. A file with a syntax error leaves the previous bindings in force, and an entry that cannot be used is skipped while the others apply; each problem is shown once in the status bar and written to standard error. Only the key spelling of the platform you run on is read, and the `mac`, `win` and `linux` fields of an entry are not.

## Extension host tests

`scripts/fetch-vscode.sh` fetches and transpiles the pinned VS Code release into `.deps/vscode` and builds the two native modules the extension host uses (this needs make, a C++ compiler and Python). With it and Node.js on `PATH`, the integration test starts the real extension host and runs an extension command; without them that test is skipped.

## Benchmarks

`cmake --preset linux-clang-release -DANVIL_BUILD_BENCHMARKS=ON` builds `bin/anvil_text_bench` (Google Benchmark), which compares the text buffer with a plain string.

## Layout

| Path | Contents |
| --- | --- |
| `src/core` | Text buffer, documents, editing commands, encoding, JSON with comments and settings, with no UI or OS dependencies |
| `src/exthost` | Extension host protocol: codecs, session, and host process launcher |
| `src/platform` | Per-OS code behind portable interfaces |
| `src/ui` | Qt Widgets user interface: main window, editor view, extension host bridge |
| `src/app` | The `anvil` executable |
| `resources` | Files the editor ships with: the color themes and the icon font of VS Code |
| `tests` | Unit tests, mirroring `src` |

## License

Anvil is released under the [MIT License](LICENSE). The extension host it runs comes from the VS Code repository (also MIT) and is fetched at build time by `scripts/fetch-vscode.sh`; it is not part of this repository.

The color themes in `resources/themes` are VS Code's default themes, copied unchanged from its `theme-defaults` extension under the VS Code MIT license (`resources/themes/LICENSE-vscode.txt`); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The default theme is the `workbench.colorTheme` setting's default, `Dark 2026`, VS Code's default in the pinned release.

The icons are from VS Code's [Codicons](https://github.com/microsoft/vscode-codicons), Copyright (c) Microsoft Corporation, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The font is embedded unchanged; the license text is `resources/codicons/LICENSE-codicons.txt`.
