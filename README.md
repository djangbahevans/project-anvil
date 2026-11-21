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
build/linux-clang-debug/bin/anvil [file...]
```

The editor opens the given files in one window with menus for files, editing and extension commands (Ctrl+Shift+P runs a command). Ctrl+F opens a find bar above the editor, with the selected text or the word at the caret already in its field: matches are highlighted and counted as the pattern is typed, Enter and Shift+Enter (or F3 and Shift+F3) step through them and wrap around, and the options match case, whole word and regular expression are toggles (Alt+C, Alt+W and Alt+R, Cmd+Alt on macOS, while the bar is open), and Escape closes the bar from the editor as well as from its fields. With nothing selected, Ctrl+C and Ctrl+X copy and cut the caret's line, and pasting that text inserts it above the line of the caret. Ctrl+H adds a replace field with Replace and Replace All, where regular expressions can use `$1`-style groups. It uses the extension host when a Node.js binary and the checkout from `scripts/fetch-vscode.sh` are found, and reads these environment variables:

| Variable | Meaning |
| --- | --- |
| `ANVIL_EXTENSIONS` | unpacked extension directories, separated like `PATH` entries |
| `ANVIL_NODE` | the Node.js binary (default: `node` on `PATH`) |
| `ANVIL_VSCODE_DIR` | the VS Code checkout (default: `.deps/vscode` of the source tree) |
| `ANVIL_EXTENSION_HOST=off` | run the editor without the extension host |

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
| `files.eol` | Line ending of new documents: `"\n"`, `"\r\n"` or `"auto"` (CRLF on Windows, LF elsewhere). Opened files keep their own | `"auto"` |

The tab size and `insertSpaces` are also what extensions see in `TextEditor.options`. A file with a syntax error, or a value of the wrong type, leaves the previous settings in force; the problem is shown once in the status bar with its line and column or key name, and is written to standard error.

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
| `tests` | Unit tests, mirroring `src` |

## License

Anvil is released under the [MIT License](LICENSE). The extension host it runs comes from the VS Code repository (also MIT) and is fetched at build time by `scripts/fetch-vscode.sh`; it is not part of this repository.
