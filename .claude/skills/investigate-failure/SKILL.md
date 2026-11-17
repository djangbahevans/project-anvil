---
name: investigate-failure
description: Diagnose a failing build, test, CI job or flaky test in Anvil and fix the cause rather than the symptom. Use when a build or test fails, CI goes red, or a test passes sometimes.
---

# Investigate a failure

Find the cause before changing anything. A change that makes the failure disappear without an explanation is a guess.

## 1. Reproduce and read

- Local failure: rerun the single failing test directly (`build/<preset>/bin/anvil_core_tests --gtest_filter=<Suite.Test>`), not only through `ctest`; `ctest` reports `GTEST_SKIP` as passed.
- CI failure: fetch the log and read the first error, not the last.
  ```sh
  gh run list --limit 5
  gh run view <run-id> --log-failed
  ```
- Note which preset (Clang debug, GCC debug, ASan) and which job failed. A failure in only one points at compiler, sanitizer or environment differences.

## 2. Classify

| Symptom | Where to look first |
| --- | --- |
| Compile error about `<expected>` or concepts on Clang | The `__cpp_concepts` override in `cmake/AnvilCompileOptions.cmake` |
| Unused variable, conversion or missing-field warning | `-Werror`: fix the code; Qt sizes need explicit casts, designated initializers need member defaults |
| Qt UI link error with `vtable` or `moc` | Header not listed in the target's sources (AUTOMOC) |
| Extension host fails at startup | `ELECTRON_RUN_AS_NODE` or `VSCODE_*` inherited from the environment |
| Native module build fails intermittently | Concurrent node-gyp builds; `scripts/fetch-vscode.sh` builds them sequentially and must stay that way |
| Hang at the end of a test run | A child process holding ctest's output pipe; `sh -c` needs `exec` |
| Only the ASan job fails | A real memory error or leak: read the stack, do not suppress |
| Passes locally, fails in CI | Cold `.deps` cache, a missing package in the workflow, timing |
| Passes sometimes | A race or a timing assumption; see below |

## 3. Flaky tests

1. Loop until it fails: `for i in $(seq 200); do <test> || break; done`. Under load, run several copies at once.
2. Find the shared state or timing assumption (ports, temp paths, sleeps, event ordering, an unkilled process).
3. Replace the assumption with synchronization on an observable condition, never a longer sleep.
4. Loop again to show it no longer fails. Never mark a test skipped or retry-on-failure to make CI green.

## 4. Fix and prove

- Fix the root cause. Add a test that fails without the fix (`mutation-check`), unless the failure was in the environment.
- If the cause was an undocumented trap that cost time, add it to the Traps section of AGENTS.md.
- If the fix is out of scope for the current task, file an issue with the reproduction (`file-issue`) rather than leaving the failure in place.
- When CI infrastructure (not the code) is at fault, say so in the pull request, with the log line that shows it.
