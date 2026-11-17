# Anvil

A native C++ code editor for the VS Code workflow, with a Qt 6 UI and a Node.js extension host. See [docs/design.md](docs/design.md) for the architecture and [docs/adr/](docs/adr/) for the decisions behind it.

## Requirements

- CMake 3.25 or later and Ninja
- GCC 13 or later, or Clang 18 or later
- GoogleTest (system package, or vcpkg when `VCPKG_ROOT` is set)
- Qt 6.4 or later with the Widgets module, needed only for the UI and the `anvil` executable. When Qt is not found, only the libraries and tests are built; `-DANVIL_BUILD_UI=ON` makes a missing Qt an error.

On Debian and Ubuntu: `sudo apt install build-essential clang cmake ninja-build libgtest-dev qt6-base-dev`.

## Build and test

```sh
cmake --preset linux-clang-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
```

Presets: `linux-clang-debug`, `linux-clang-release`, `linux-clang-asan`, `linux-gcc-debug`, `windows-msvc-debug`, `windows-msvc-release`, `macos-debug`, `macos-release`. Build output goes to `build/<preset>/`.

## Layout

| Path | Contents |
| --- | --- |
| `src/core` | Text and encoding primitives with no UI or OS dependencies |
| `src/exthost` | Extension host wire protocol: frame and RPC message codecs |
| `src/platform` | Per-OS code behind portable interfaces |
| `src/ui` | Qt Widgets user interface |
| `src/app` | The `anvil` executable |
| `tests` | Unit tests, mirroring `src` |
