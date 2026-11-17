# Working on Anvil

Anvil is a native C++23 code editor in the style of VS Code, built with Qt 6, that runs the non-UI part of VS Code extensions in a Node.js child process (the unmodified upstream extension host). It is a learning project that aims for low memory use and fast startup.

Read these first, in this order:

1. [README.md](README.md): build, run, environment variables.
2. [docs/design.md](docs/design.md): the layers and how the pieces fit.
3. [docs/adr/](docs/adr/README.md): why each major decision was made. Follow an accepted ADR; change one by writing a new ADR that supersedes it.
4. [docs/extension-host-protocol.md](docs/extension-host-protocol.md): the wire protocol and every behavior of the extension host that was discovered by experiment.

What is done and what comes next is tracked on GitHub, not in this repository: see the milestones and open issues of `djangbahevans/project-anvil` (`gh issue list --milestone "<title>"`). Pick work from the lowest-numbered open milestone unless told otherwise.

## Setup

```sh
sudo apt install build-essential clang cmake ninja-build libgtest-dev nlohmann-json3-dev \
    libbenchmark-dev qt6-base-dev clang-format python3 make
scripts/fetch-vscode.sh          # pinned VS Code checkout in .deps/vscode, needs Node.js 24 and pnpm
```

Without `.deps/vscode` and Node.js, everything builds and the tests that need the extension host skip themselves.

## Everyday commands

```sh
cmake --preset linux-clang-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug              # all tests
ctest --preset linux-clang-debug -R Session   # a subset
scripts/format.sh                             # format; --check only reports
```

Presets: `linux-clang-debug`, `linux-clang-release`, `linux-clang-asan`, `linux-gcc-debug`, plus Windows and macOS presets that have not been built. Test binaries are in `build/<preset>/bin/`: `anvil_core_tests`, `anvil_platform_tests`, `anvil_exthost_tests`, `anvil_exthost_integration_tests` (real extension host), `anvil_ui_tests` (Qt, offscreen). Run the editor with `build/<preset>/bin/anvil` (see the README).

## Layers

`ui` → `workbench` → `exthost` → `core` / `platform`. Dependencies point down only. `core` has no Qt and no OS code; per-OS code lives only in `platform`; Qt lives only in `ui`. The CMake targets enforce this, so a violation fails to link or include. `workbench` does not exist yet.

| Directory | Contents |
| --- | --- |
| `src/core` | rope text, documents, undo, selections, editing commands, UTF-8/UTF-16, URIs, logging interface |
| `src/platform` | processes, local sockets, files, paths |
| `src/exthost` | frame and RPC codecs, session, extension host launcher, `MainThread*` services |
| `src/ui` | editor view, main window, extension host bridge |
| `tools/exthost-probe` | a Node script that plays the editor toward the extension host, and fixture extensions |
| `scripts` | fetching VS Code, generating proxy ids, formatting |

## Conventions

- C++23, warnings are errors (`-Wall -Wextra -Wpedantic -Wconversion -Werror`) on Clang and GCC. Fallible functions return `std::expected<T, Error>`; exceptions are for programmer errors, and the JSON library's exceptions are caught where extension input is parsed.
- Formatting is `clang-format` with the checked-in config; run `scripts/format.sh` before committing.
- Comments explain a non-obvious trade-off, quirk or workaround, never what the code plainly does. Remove redundant comments from code you touch.
- Documentation and commit messages are self-contained and in the present tense. They describe the current design and its rationale, and never the history of how it was reached: no "previously", "now fixed", "as discussed". Avoid "I" and "we".
- Commit messages start with a short imperative subject, then a body explaining what and why. Do not add attribution footers or trailers of any kind.
- Commit to `main` in small steps, and keep `main` building with all tests passing. Push only when asked.
- Record a significant decision as an ADR (`docs/adr/template.md`), and record a newly discovered fact about the VS Code protocol in `docs/extension-host-protocol.md`.

## Testing practice

- Every behavior has a test. Prefer model-based tests that compare against an obviously correct reference (see `tests/core/text_model_test.cpp` and `document_model_test.cpp`), and tests that replay real recorded traffic (`tests/exthost/replay_test.cpp`).
- Prove a new test can fail: break the behavior on purpose (copy the file aside, change it, run the tests, restore it), and confirm the tests catch it. A mutation that does not compile proves nothing, so check that the build succeeded.
- Extension host services are tested twice: against `FakeHost` (the extension host's side of the connection, in `tests/exthost/fake_host.hpp`) for exact wire behavior, and against the real extension host with a fixture extension from `tools/exthost-probe/fixtures/`.
- UI tests run under Qt's offscreen platform. Set `ANVIL_SCREENSHOT_DIR` to a directory to have them save PNGs, and look at the images to check rendering.
- `ctest` reports a `GTEST_SKIP` as passed. Run the test binary directly to see whether the real-host tests actually ran.

## Traps

- Clang with libstdc++ hides `<expected>` because it reports `__cpp_concepts` as 201907L; `cmake/AnvilCompileOptions.cmake` overrides the macro.
- A shell started from a VS Code terminal inherits `ELECTRON_RUN_AS_NODE` and `VSCODE_*`, which make the extension host fail at startup under plain Node. The launcher and the probe strip them; do the same for any other process that starts the host.
- The extension host needs two native modules (`spdlog`, `native-watchdog`), which `scripts/fetch-vscode.sh` builds. Without them extension logging is empty.
- The numeric proxy ids come from the VS Code source and change between releases. `proxy_ids.hpp` is generated; `node scripts/generate-proxy-ids.mjs --check` verifies it.
- `GTEST` macros split on commas, so wrap braced initializers in parentheses. A raw string containing `)"` needs its own delimiter, such as `R"j(...)j"`.
- Designated initializers need a default member initializer on every member, or `-Wmissing-field-initializers` fires.
- Qt sizes are signed (`qsizetype`); cast them explicitly under `-Wconversion`. AUTOMOC only processes headers that are listed in the target's sources.
- Process tests that start `sh -c` must `exec` the real program, or killing the shell leaves an orphan holding ctest's output pipe open.
- Benchmarks on WSL2 vary by several times between runs; compare orders of magnitude.

## How-to guides

- [Adding a MainThread service](docs/howto/adding-a-main-thread-service.md)
- [Upgrading the pinned VS Code](docs/howto/upgrading-vscode.md)

## Definition of done

1. Builds without warnings on `linux-clang-debug` and `linux-gcc-debug`; all tests pass, including `linux-clang-asan`.
2. `scripts/format.sh --check` passes.
3. New behavior has tests that were shown to be able to fail.
4. Decisions, protocol facts and user-visible behavior are documented where they belong.
5. The relevant GitHub issue is updated or closed, and new problems found along the way are filed as issues instead of being left in a code comment.
