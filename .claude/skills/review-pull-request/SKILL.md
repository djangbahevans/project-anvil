---
name: review-pull-request
description: Review a pull request against the Anvil conventions and the originating issue, and report concrete defects with evidence. Use when asked to review a pull request or branch, or to check another session's work.
---

# Review a pull request

The aim is to find what the author missed, with evidence. A review that approves without running anything has not reviewed.

## 1. Gather

```sh
gh pr view <n> --json title,body,headRefName,files,statusCheckRollup
gh pr diff <n>
gh issue view <closing issue>
gh pr checks <n>
git fetch origin pull/<n>/head:pr-<n> && git worktree add ../anvil-review-<n> pr-<n>
```

Review in a worktree so that other work in the main checkout is untouched.

## 2. Check against the issue

Each acceptance criterion maps to code and to a test. List any criterion with neither, and any change that no criterion asks for (scope creep).

## 3. Check against the conventions

- Layer rules: no Qt outside `src/ui`, no per-OS code outside `src/platform`, dependencies point down.
- Errors: `std::expected<T, Error>` for failures; no exceptions for expected conditions.
- Comments: only for non-obvious trade-offs; no redundant or narrating comments in touched code.
- Commit messages and docs: present tense, self-contained, no history words ("previously", "now fixed"), no attribution footers.
- ADRs and `docs/extension-host-protocol.md` updated when a decision or protocol fact was made.
- Every trap in AGENTS.md that the touched area exposes. In particular: nested event loops during modal dialogs (code must act on a captured identity), per-keystroke cost that must not scale with tabs or documents, and `DocumentsService::Observer` callbacks for new extension-driven document operations.

## 4. Run it

Build and run the tests in the worktree. Then exercise the behavior beyond the author's tests:

- Try the inputs the tests do not: empty, huge, non-ASCII, CRLF, multiple cursors, a document closed or changed mid-operation.
- Run the new tests against a deliberately broken implementation (`mutation-check`) for at least the main behavior. Tests that pass with the behavior removed are a finding.
- For UI changes, run the UI tests with `ANVIL_SCREENSHOT_DIR` and look at the images; for performance claims, measure.
- For extension host changes, run the integration binary directly and confirm real-host tests ran rather than skipped.

## 5. Report

Report findings ordered by severity. Each finding has: where (`file:line`), what is wrong, the concrete input or sequence that triggers it, and the evidence (a failing test, a measurement, a command). Distinguish defects from preferences. State what was checked and found sound, so the author knows what was covered.

Post it as a pull request comment (`gh pr comment <n> --body-file <file>`). Do not approve or merge unless the maintainer asks, and do not push to the author's branch. Clean up the worktree afterward: `git worktree remove ../anvil-review-<n>`.
