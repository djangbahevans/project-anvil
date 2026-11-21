# Working on Anvil

Anvil is a native C++23 code editor in the style of VS Code, built with Qt 6, that runs the non-UI part of VS Code extensions in a Node.js child process (the unmodified upstream extension host). It is a learning project that aims for low memory use and fast startup.

Read these first, in this order:

1. [README.md](README.md): build, run, environment variables.
2. [docs/design.md](docs/design.md): the layers and how the pieces fit.
3. [docs/adr/](docs/adr/README.md): why each major decision was made. Follow an accepted ADR; change one by writing a new ADR that supersedes it.
4. [docs/extension-host-protocol.md](docs/extension-host-protocol.md): the wire protocol and every behavior of the extension host that was discovered by experiment.

What is done and what comes next is tracked on GitHub, not in this repository: see the milestones and open issues of `djangbahevans/project-anvil` (`gh issue list --milestone "<title>"`). Pick work from the first open milestone in title order (`M5`, `M5b`, `M6`, ...) unless told otherwise.

**Start with the `start-session` playbook** below; it leads to the right next step whether or not a task was given.

## Playbooks

Step-by-step procedures live in `.claude/skills/<name>/SKILL.md`. They are plain Markdown and apply to any agent; Claude Code loads them as skills.

| Playbook | Use when |
| --- | --- |
| [start-session](.claude/skills/start-session/SKILL.md) | Beginning any session, or unsure what to do next |
| [groom-issue](.claude/skills/groom-issue/SKILL.md) | An issue is not `agent-ready`: undecided, too large, blocked, research, or vague |
| [implement-issue](.claude/skills/implement-issue/SKILL.md) | Implementing a ready issue, from worktree to finished branch |
| [open-pull-request](.claude/skills/open-pull-request/SKILL.md) | Pushing a branch, writing the description, handling CI and review |
| [review-pull-request](.claude/skills/review-pull-request/SKILL.md) | Reviewing a pull request or another session's work |
| [investigate-failure](.claude/skills/investigate-failure/SKILL.md) | A build, test or CI failure, or a flaky test |
| [mutation-check](.claude/skills/mutation-check/SKILL.md) | Proving that new tests can fail |
| [file-issue](.claude/skills/file-issue/SKILL.md) | Recording a bug, follow-up or planned work |
| [write-adr](.claude/skills/write-adr/SKILL.md) | A design choice that others will build on |

## Issue labels

- Area: `area:core`, `area:platform`, `area:exthost`, `area:ui`, `area:build`, `area:docs`.
- Type: `type:feature`, `type:tech-debt`, `type:performance`, `type:research`.
- Readiness, at most one per issue: `agent-ready` (can be implemented as written), `needs-decision` (a design choice is open; the maintainer decides), `needs-split` (too large for one pull request), `blocked` (waits on another issue), `needs-human` (needs hardware, an account or a judgment only the maintainer can make).
- `in-progress`: a session has claimed the issue. Remove it when work stops without a merged pull request.

An issue without a readiness label has not been groomed: run `groom-issue` on it before implementing.

## Setup

```sh
sudo apt install build-essential clang cmake ninja-build libgtest-dev nlohmann-json3-dev \
    libpcre2-dev libbenchmark-dev qt6-base-dev clang-format python3 make
scripts/fetch-vscode.sh          # pinned VS Code checkout in .deps/vscode, needs Node.js 24 and pnpm
```

The script also builds the two native modules the extension host loads, which needs `make`, a C++ compiler and Python, and fails if it cannot. Set `ANVIL_ALLOW_MISSING_NATIVE_MODULES=1` to continue without them (the extension host then writes no log files and does not watch its parent process).

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
| `src/core` | rope text, documents, undo, selections, editing commands, search and replace, UTF-8/UTF-16, URIs, JSON with comments, settings, logging interface |
| `src/platform` | processes, local sockets, files, paths, per-platform editor defaults |
| `src/exthost` | frame and RPC codecs, session, extension host launcher, `MainThread*` services |
| `src/ui` | editor view, find bar, main window, extension host bridge |
| `tools/exthost-probe` | a Node script that plays the editor toward the extension host, and fixture extensions |
| `scripts` | fetching VS Code, generating proxy ids, formatting |

## Conventions

- C++23, warnings are errors (`-Wall -Wextra -Wpedantic -Wconversion -Werror`) on Clang and GCC. Fallible functions return `std::expected<T, Error>`; exceptions are for programmer errors, and the JSON library's exceptions are caught where extension input is parsed.
- Formatting is `clang-format` with the checked-in config; run `scripts/format.sh` before committing.
- Comments explain a non-obvious trade-off, quirk or workaround, never what the code plainly does. Remove redundant comments from code you touch.
- Documentation and commit messages are self-contained and in the present tense. They describe the current design and its rationale, and never the history of how it was reached: no "previously", "now fixed", "as discussed". Avoid "I" and "we".
- Commit messages start with a short imperative subject, then a body explaining what and why. Do not add attribution footers or trailers of any kind.
- Work on a branch named `<issue>-<slug>` in its own `git worktree`, in small commits. Changes reach `main` through pull requests (`open-pull-request`), which the maintainer merges; `main` always builds with all tests passing. Do not push to `main`, merge your own pull request, or force-push anything but your own feature branch. A maintainer who works directly in the main checkout may commit to `main` and push when asked.
- Never stage with `git add -A` or `git commit -a` in a checkout that may hold another session's changes; stage files by name.
- What extensions do to a document the window shows (edit it, save it, ask to show it) reaches the window through callbacks on `DocumentsService::Observer`, carried by `ExtensionHostBridge::Callbacks`. A new operation that extensions can perform on documents needs its own callback, or the tabs, title and views go stale.
- The project is MIT licensed ([LICENSE](LICENSE)); contributions are made under the same license. Do not copy code from sources with an incompatible license, and keep the notices of any third-party code that is added.
- Record a significant decision as an ADR (`docs/adr/template.md`), and record a newly discovered fact about the VS Code protocol in `docs/extension-host-protocol.md`.

## Testing practice

- Every behavior has a test. Prefer model-based tests that compare against an obviously correct reference (see `tests/core/text_model_test.cpp` and `document_model_test.cpp`), and tests that replay real recorded traffic (`tests/exthost/replay_test.cpp`).
- Prove a new test can fail: break the behavior on purpose (copy the file aside, change it, run the tests, restore it and `touch` it so that the build does not keep the mutated object), and confirm the tests catch it. A mutation that does not compile proves nothing, so check that the build succeeded.
- Extension host services are tested twice: against `FakeHost` (the extension host's side of the connection, in `tests/exthost/fake_host.hpp`) for exact wire behavior, and against the real extension host with a fixture extension from `tools/exthost-probe/fixtures/`.
- UI tests run under Qt's offscreen platform. Set `ANVIL_SCREENSHOT_DIR` to a directory to have them save PNGs, and look at the images to check rendering.
- `ctest` reports a `GTEST_SKIP` as passed. Run the test binary directly to see whether the real-host tests actually ran.

## Traps

- Clang with libstdc++ hides `<expected>` because it reports `__cpp_concepts` as 201907L; `cmake/AnvilCompileOptions.cmake` overrides the macro.
- A shell started from a VS Code terminal inherits `ELECTRON_RUN_AS_NODE` and `VSCODE_*`, which make the extension host fail at startup under plain Node. The launcher and the probe strip them; do the same for any other process that starts the host.
- The two native modules (`spdlog`, `native-watchdog`) are built by `scripts/fetch-vscode.sh` one after the other. node-gyp unpacks one shared header download for both, so building them at the same time races and fails intermittently. Do not parallelize those builds, and do not turn a failed build into a warning in CI.
- Processes inherit the working directory. One whose directory was deleted (a shell in a build directory that was cleared) makes Node print `ENOENT ... uv_cwd` and Fontconfig print `out of memory`, which are the same cause. The editor moves to a directory that exists at startup, and the extension host gets one explicitly through `ProcessSpec::working_directory`; give any other child process one too, and use absolute paths for its executable.
- Modal dialogs (`QMessageBox::exec`, file dialogs) run a nested event loop in which the extension host keeps being serviced. An extension can show another document, edit one or save one while a prompt is open. Code that asks a question acts on the object the question was about (an index or identity captured before asking), never on "the current" one.
- Work on a per-keystroke path must not grow with the number of open tabs or documents. Setting a `QTabBar` tab's text or tooltip re-lays out the whole bar even when the value is unchanged, so set them only when they differ. Measure typing cost with hundreds of tabs when touching such code.
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
6. Anything that was missing, wrong or misleading in AGENTS.md or a playbook while doing the work is fixed in the same pull request, or reported in its "gaps found" section.
