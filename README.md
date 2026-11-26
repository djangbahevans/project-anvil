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

Ctrl+P (Cmd+P on macOS) or Go > Go to File... opens a file by name: type parts of its name or path. Typing `>` there lists the commands instead, which is what Ctrl+Shift+P opens with. It lists the files you opened first, and searches the opened folder with ripgrep, leaving out what `files.exclude`, `search.exclude` and `.gitignore` files say (see the settings below). Without a folder it lists the open documents.

The first button of the activity bar opens the Explorer: the folder as a tree, folders first. Enter or a double click opens a file in a tab and expands or collapses a folder, and a single click only selects. With no folder open it offers Open Folder. `files.exclude` (see the settings below) decides what the tree hides. The tree follows the disk: files that other programs create, delete and rename in the folders that are open appear and disappear without a refresh, and a problem with the watch (the system's limit of watches, lost events) is said in the status bar.

The Explorer creates, renames and deletes files and folders. The commands have VS Code's ids, are in the command palette and in the context menu of the tree (right click), and apply to the selection; a command on one item takes the row that has the focus:

| Command | What it does | Keys (Linux and Windows, macOS) |
| --- | --- | --- |
| `explorer.newFile`, `explorer.newFolder` | New File..., New Folder...: a row with a field for the name at the top of the selected folder, of the folder of the selected file, or of the opened folder when nothing is selected. Enter creates it (a new file opens in an editor) and Escape drops the row | |
| `renameFile` | Rename...: edits the name in its row, without the extension selected. Open files keep their tabs and unsaved changes under the new name | F2, Enter |
| `moveFileToTrash` | Delete: asks, then moves the item to the trash. Without a trash (`files.enableTrash` is `false`) it deletes for good. The tab of a deleted file closes, except when it has unsaved changes, which stay open | Delete, Cmd+Backspace |
| `deleteFile` | Delete Permanently: asks, then deletes the item and what is in it for good | Shift+Delete, Cmd+Alt+Backspace |
| `revealFileInOS` | Opens the file manager on the folder that holds the selected row, or the file of the active editor when the Explorer does not have the focus | |

The keys apply while the tree has the focus and no name is being edited. A name that is empty, starts with a slash, has a separator, is `.` or `..`, or is taken is refused with the reason under the field, as it is typed, and nothing changes. A failure of the system (a folder that cannot be written to, a trash that cannot take the item) is said in the status bar.

Several rows can be selected (Ctrl+click, Shift+click). Dragging them onto a folder, or onto the space below the rows for the opened folder, moves them there after asking (`explorer.confirmDragAndDrop`), or copies them with Ctrl (Cmd on macOS) held when the button is released; a drop on a file is a drop on its folder. The folder under the pointer is highlighted, a closed one opens when the pointer rests on it, and Escape ends the drag. A folder cannot be dropped onto itself or into itself, and a name that is taken asks before it is replaced. Open files follow a move as they follow a rename, a copy leaves them alone, and a move does not replace a file that has unsaved changes. Typing the first letters of a name selects the next row that starts with them, ignoring case, among the rows that show; the letters start again after a short pause.

View > Split Editor Right (Ctrl+\) and Split Editor Down add an empty editor group next to the focused one, each group with its own tabs; Focus Next Group, Focus Previous Group and Close Editor Group move between groups and remove one. Opening a file that is open in another group focuses its tab there, because a document is shown in one group at a time. Dragging a tab onto the tabs of another group moves it there, onto the middle of a group moves it to the end, and onto the nearest quarter of a group's edge splits the group and puts the tab in the new half; Escape cancels the drag.

The status bar shows the language of the shown document, from VS Code's languages, the languages that extensions contribute and `files.associations` (see the settings below). Clicking it, the command palette's Change Language Mode, or Ctrl+K M (Cmd+K M on macOS) opens a list that starts with Auto Detect (for a document that has text), which gives up a language picked by hand and lets the file name and the first line decide again, and, for a file with an extension, Configure File Association for '.ext'…, which asks for a language and writes `"*.ext": "<language>"` to `files.associations` in the settings file, for every file with that extension. Under the line `languages (identifier)` come the languages by name, with the id after each and the current one marked: choosing one sets the language of that document until it is closed, whatever its file is called, and the settings do not change it. VS Code's entries for the settings of a language and for the Marketplace are not there.

| Variable | Meaning |
| --- | --- |
| `ANVIL_EXTENSIONS` | unpacked extension directories, separated like `PATH` entries |
| `ANVIL_NODE` | the Node.js binary (default: `node` on `PATH`) |
| `ANVIL_VSCODE_DIR` | the VS Code checkout (default: `.deps/vscode` of the source tree) |
| `ANVIL_RG` | the ripgrep binary that file search runs (default: the one in the VS Code checkout, else `rg` on `PATH`) |
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
| `window.zoomLevel` | The zoom of the window, in steps of a factor of 1.2: -8 to 8, decimals allowed, written by View > Appearance > Zoom In, Zoom Out and Reset Zoom (Ctrl+=, Ctrl+-, Ctrl+NumPad0). Every size of the window follows it: the text, the bars, the side bar, the panel, the menus and the quick pick | 0 |
| `editor.tabSize` | Columns per tab stop, an integer of at least 1 (values above 1024 are treated as 1024) | 4 |
| `editor.insertSpaces` | Tab inserts spaces up to the next tab stop when `true`, a tab character when `false` | `true` |
| `editor.multiCursorModifier` | The key that makes a click add a cursor: `"alt"` (Alt+click), or `"ctrlCmd"` (Ctrl+click, Cmd+click on macOS). Shift with it, or Shift+Alt with `"ctrlCmd"`, selects a rectangle | `"alt"` |
| `editor.columnSelection` | Dragging the mouse selects a rectangle, one selection per line, instead of a stream of text | `false` |
| `editor.renderWhitespace` | Which spaces and tabs get a mark (a dot for a space, an arrow for a tab, in `editorWhitespace.foreground`): `"none"`, `"boundary"` (all but a single space between words), `"selection"` (inside selections), `"trailing"` (at the end of the line) or `"all"` | `"selection"` |
| `editor.wordWrap` | Where long lines wrap: `"off"` (nowhere), `"on"` (at the width of the editor), `"wordWrapColumn"` (at `editor.wordWrapColumn`) or `"bounded"` (at the smaller of the two). View > Word Wrap (Alt+Z) turns wrapping on or off for one file until it is closed, without changing the setting | `"off"` |
| `editor.wordWrapColumn` | The column that `"wordWrapColumn"` and `"bounded"` wrap at, an integer of at least 1 | 80 |
| `editor.renderControlCharacters` | Control characters are drawn as symbols (white on red) when `true` | `true` |
| `workbench.colorTheme` | The color theme, by id or label: one of the ten that ship with the editor, or one that an extension contributes in `contributes.themes` (the `uiTheme` of the manifest decides its kind) | `"Dark 2026"` |
| `workbench.editor.focusRecentEditorAfterClose` | Closing the active tab shows the tab that was shown before it when `true`, its right neighbor (the left one for the last tab) when `false` | `true` |
| `window.commandCenter` | Shows the box in the middle of the title bar that shows the workspace name and opens the command palette (custom title bar only) | `true` |
| `workbench.layoutControl.enabled` | Shows the buttons in the title bar that show and hide the side bar and the panel (custom title bar only) | `true` |
| `window.titleBarStyle` | `"custom"`: on Linux and Windows the window has no frame and draws its own title bar, with the menus and the window buttons in it; `"native"`: the window has the system's frame and the menus below it. macOS always has the system's frame. Read when the window opens, so a change applies after a restart | `"custom"` |
| `files.eol` | Line ending of new documents: `"\n"`, `"\r\n"` or `"auto"` (CRLF on Windows, LF elsewhere). Opened files keep their own | `"auto"` |
| `editor.comments.insertSpace` | A space follows the token of a line comment and goes inside a block comment, when `true` | `true` |
| `editor.comments.ignoreEmptyLines` | Toggle Line Comment leaves lines with nothing on them alone, when `true` | `true` |
| `workbench.statusBar.visible` | Shows the status bar | `true` |
| `workbench.sideBar.location` | The edge of the window the primary side bar and the activity bar are at: `"left"` or `"right"` | `"left"` |
| `workbench.activityBar.location` | `"default"` shows the activity bar beside the side bar and `"hidden"` hides it; `"top"` and `"bottom"` are accepted and show it as `"default"` | `"default"` |
| `workbench.editor.showTabs` | How a group shows its editors: `"multiple"` (a tab for each), `"single"` (only the active one, as a tab as wide as the group) or `"none"` (no strip of tabs; the group keeps its editor). View > Appearance > Tab Bar sets it | `"multiple"` |
| `zenMode.showTabs` | The same, while Zen Mode is on | `"multiple"` |
| `zenMode.fullScreen` | Zen Mode also puts the window into full screen (and leaves it when Zen Mode ends, unless the window was full screen before) | `true` |
| `zenMode.centerLayout` | Zen Mode also centers the editor area | `true` |
| `zenMode.hideStatusBar` | Zen Mode also hides the status bar | `true` |
| `zenMode.hideActivityBar` | Zen Mode also hides the activity bar | `true` |
| `files.autoSave` | When a document with a file and unsaved changes is saved by itself: `"off"`, `"afterDelay"` (`files.autoSaveDelay` after its last change), `"onFocusChange"` (when its editor loses the focus, another tab replaces it, or the window loses the focus) or `"onWindowChange"` (when the window loses the focus). Untitled documents are never saved. File > Auto Save switches between `"off"` and `"afterDelay"` | `"off"` |
| `files.autoSaveDelay` | Milliseconds between the last change and the save, with `"afterDelay"` (0 to 86400000) | `1000` |
| `files.associations` | An object from a glob to a language id that decides the language of the files it matches before any language's own rules, as in `{"*.cfg": "ini", "**/.config/*": "json"}`. A pattern with a `/` is matched against the whole path and any other against the file name, ignoring case. The language shows in the status bar. An entry whose value is not a language id is skipped and logged | `{}` |
| `files.exclude` | An object from a glob to `true` or `false` of the files and folders the Explorer hides, matched against the path relative to the opened folder, as in `{"**/node_modules": true}`. The entries are laid over VS Code's defaults (`**/.git`, `**/.svn`, `**/.hg`, `**/.jj`, `**/.DS_Store`, `**/Thumbs.db`), so a default stays hidden unless it is set to `false`. An entry whose value is an object (a `when` condition) is skipped and logged. A change applies to the open tree without collapsing it | the defaults above |
| `search.exclude` | An object from a glob to `true` or `false` of what file search leaves out besides `files.exclude`, over which it lies: `**/node_modules`, `**/bower_components` and `**/*.code-search` unless set to `false` | those three |
| `search.useIgnoreFiles` | Whether file search respects `.gitignore` and `.ignore` files | `true` |
| `search.useGlobalIgnoreFiles` | Whether it also respects the user's global ignore file of git | `false` |
| `search.useParentIgnoreFiles` | Whether it also respects the ignore files in the folders above the searched one | `false` |
| `search.followSymlinks` | Whether file search follows symbolic links | `true` |
| `explorer.confirmDelete` | Whether the Explorer asks before it deletes | `true` |
| `explorer.confirmDragAndDrop` | Whether the Explorer asks before dragging moves items; "Do not ask me again" in the question writes `false` | `true` |
| `files.enableTrash` | Whether the Explorer moves what it deletes to the trash; without it a delete is for good | `true` |

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

## Window state

The window opens with the size, position and layout it had when it was last closed: the sidebar and the panel, their sizes and the views they show. The visibility, the views and the expanded folders of the Explorer are remembered for each folder. The editor keeps this in the `state` directory next to `settings.json`; deleting it restores the defaults.

## Extension host tests

`scripts/fetch-vscode.sh` fetches and transpiles the pinned VS Code release into `.deps/vscode` and builds the two native modules the extension host uses (this needs make, a C++ compiler and Python). With it and Node.js on `PATH`, the integration test starts the real extension host and runs an extension command; without them that test is skipped.

## Syntax highlighting

`scripts/fetch-tree-sitter.sh` downloads the pinned versions of the tree-sitter runtime (a commit of its development line) and the pinned releases of the grammars into `.deps/tree-sitter`, and checks each archive against its SHA-256 (`--check` verifies what is already there without downloading). When that directory exists at configure time the build compiles them as C into the core library and defines `ANVIL_TREE_SITTER`; this needs a C compiler besides the C++ one. Without it everything else builds, the editor has no syntax highlighting, and the tests of the highlighter are skipped. Documents in these languages are highlighted, with the colors that the color theme gives the scopes of VS Code's grammars: JSON, C, C++, JavaScript (and JSX), TypeScript (and TSX) and Markdown (the blocks and what is inside them, not the code in fences). A document of more than 5 MiB is not highlighted, and neither is a line of more than 10,000 characters.

## Benchmarks

`cmake --preset linux-clang-release -DANVIL_BUILD_BENCHMARKS=ON` builds `bin/anvil_text_bench` (Google Benchmark), which compares the text buffer with a plain string.

## Layout

| Path | Contents |
| --- | --- |
| `src/core` | Text buffer, documents, editing commands, encoding, JSON with comments and settings, with no UI or OS dependencies |
| `src/exthost` | Extension host protocol: codecs, session, and host process launcher |
| `src/platform` | Per-OS code behind portable interfaces |
| `src/search` | Listing the files of a folder with ripgrep |
| `src/ui` | Qt Widgets user interface: main window, editor view, extension host bridge |
| `src/app` | The `anvil` executable |
| `resources` | Files the editor ships with: the color themes and the icon font of VS Code |
| `tests` | Unit tests, mirroring `src` |

## License

Anvil is released under the [MIT License](LICENSE). The extension host it runs comes from the VS Code repository (also MIT) and is fetched at build time by `scripts/fetch-vscode.sh`; it is not part of this repository.

The color themes in `resources/themes` are VS Code's default themes, copied unchanged from its `theme-defaults` extension under the VS Code MIT license (`resources/themes/LICENSE-vscode.txt`); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The default theme is the `workbench.colorTheme` setting's default, `Dark 2026`, VS Code's default in the pinned release.

The icons are from VS Code's [Codicons](https://github.com/microsoft/vscode-codicons), Copyright (c) Microsoft Corporation, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The font is embedded unchanged; the license text is `resources/codicons/LICENSE-codicons.txt`.
