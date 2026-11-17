---
name: implement-issue
description: Implement an agent-ready GitHub issue end to end in the Anvil repository, from branch to a pull request ready for review. Use after start-session has produced a claimed issue.
---

# Implement an issue

## 1. Set up

```sh
git fetch origin
git worktree add ../anvil-<issue> -b <issue>-<slug> origin/main   # e.g. 7-copy-line
cd ../anvil-<issue>
```

A separate worktree keeps parallel sessions from sharing one working tree. Build in it with the presets from AGENTS.md; `.deps/vscode` can be shared by symlinking it from the main checkout.

## 2. Understand before editing

- Read the issue completely, including comments; later comments override the body.
- Read the ADRs and documents it links, and the code it names. Find the closest existing feature and follow its structure (see the how-to guides in `docs/howto/`).
- If the issue is not actually ready (an open decision, a missing dependency, contradictory criteria), stop and run `groom-issue`. Do not guess at design decisions that belong to the maintainer.

## 3. Work in small commits

Write the test first where the behavior is clear, then the code. After each coherent step:

```sh
scripts/format.sh
cmake --build --preset linux-clang-debug && ctest --preset linux-clang-debug
git add <files> && git commit
```

- Stage files by name; never `git add -A` in a checkout that holds other people's changes.
- Commit messages: short imperative subject, then a body with what and why, present tense, self-contained. No attribution footers or trailers.
- Remove redundant comments from every function touched. Comments stay only for non-obvious trade-offs.
- Respect the layer rules. A change that seems to need a dependency pointing up is a design problem; stop and raise it in the issue.

## 4. Prove the tests can fail

Run `mutation-check` for each new behavior. Tests that were never seen failing are not evidence.

## 5. Verify the definition of done

Run the full set from AGENTS.md: Clang and GCC debug builds without warnings, all tests including ASan, `scripts/format.sh --check`. For UI changes, run the UI tests with `ANVIL_SCREENSHOT_DIR` set and look at the images. For extension host changes, run `anvil_exthost_integration_tests` directly and confirm the real-host tests did not skip.

## 6. Record what was learned

- A decision that others will rely on becomes an ADR (`write-adr`).
- A newly found fact about the VS Code protocol goes in `docs/extension-host-protocol.md`.
- A trap that cost time goes in the Traps section of AGENTS.md.
- A problem found and not fixed becomes an issue (`file-issue`), not a code comment.

## 7. Hand over

Follow `open-pull-request`. Do not merge it yourself.
