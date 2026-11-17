# Anvil

A native C++ code editor for the VS Code workflow, with a Qt 6 UI and a Node.js extension host. See [docs/design.md](docs/design.md) for the architecture and [docs/adr/](docs/adr/) for the decisions behind it.

## Requirements

- CMake 3.25 or later and Ninja
- GCC 13 or later, or Clang 18 or later
- GoogleTest, nlohmann-json and, for benchmarks, Google Benchmark (system packages, or vcpkg when `VCPKG_ROOT` is set)
- Qt 6.4 or later with the Widgets module, needed only for the UI and the `anvil` executable. When Qt is not found, only the libraries and tests are built; `-DANVIL_BUILD_UI=ON` makes a missing Qt an error.

On Debian and Ubuntu: `sudo apt install build-essential clang cmake ninja-build libgtest-dev nlohmann-json3-dev qt6-base-dev`.

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

The editor opens the given files in one window with menus for files, editing and extension commands (Ctrl+Shift+P runs a command). It uses the extension host when a Node.js binary and the checkout from `scripts/fetch-vscode.sh` are found, and reads these environment variables:

| Variable | Meaning |
| --- | --- |
| `ANVIL_EXTENSIONS` | unpacked extension directories, separated like `PATH` entries |
| `ANVIL_NODE` | the Node.js binary (default: `node` on `PATH`) |
| `ANVIL_VSCODE_DIR` | the VS Code checkout (default: `.deps/vscode` of the source tree) |
| `ANVIL_EXTENSION_HOST=off` | run the editor without the extension host |

Extension host messages are printed to standard error. Under WSLg the window appears through Wayland or X11; `QT_QPA_PLATFORM=xcb` forces X11.

## Extension host tests

`scripts/fetch-vscode.sh` fetches and transpiles the pinned VS Code release into `.deps/vscode` and builds the two native modules the extension host uses (this needs make, a C++ compiler and Python). With it and Node.js on `PATH`, the integration test starts the real extension host and runs an extension command; without them that test is skipped.

## Benchmarks

`cmake --preset linux-clang-release -DANVIL_BUILD_BENCHMARKS=ON` builds `bin/anvil_text_bench` (Google Benchmark), which compares the text buffer with a plain string.

## Layout

| Path | Contents |
| --- | --- |
| `src/core` | Text buffer, documents, editing commands and encoding, with no UI or OS dependencies |
| `src/exthost` | Extension host protocol: codecs, session, and host process launcher |
| `src/platform` | Per-OS code behind portable interfaces |
| `src/ui` | Qt Widgets user interface: main window, editor view, extension host bridge |
| `src/app` | The `anvil` executable |
| `tests` | Unit tests, mirroring `src` |

## License

Anvil is released under the [MIT License](LICENSE). The extension host it runs comes from the VS Code repository (also MIT) and is fetched at build time by `scripts/fetch-vscode.sh`; it is not part of this repository.
