---
name: open-pull-request
description: Push a branch and open a pull request for the Anvil repository, including the pre-flight checks, the description, CI, and what to do after review. Use when implementation work is finished.
---

# Open a pull request

Work reaches `main` through pull requests. An agent never pushes to `main` and never merges its own pull request; the maintainer merges (rebase-merge, so history stays linear).

## Pre-flight

1. `git fetch origin && git rebase origin/main`; resolve conflicts, then rebuild and retest.
2. The definition of done in AGENTS.md holds: both compilers, all tests including ASan, `scripts/format.sh --check`.
3. `git log origin/main..HEAD` shows only commits that belong to this change, each with a subject and an explaining body, and no attribution footers.
4. `git diff origin/main --stat` contains no files outside the issue's scope (generated files, editor settings, stray notes).
5. Read the whole diff once as a reviewer would. Remove leftover debugging and redundant comments.

## Push and open

Pushing a feature branch is part of this task. Pushing any other branch, or force-pushing, is not.

```sh
git push -u origin <branch>
gh pr create --base main --title "<imperative subject>" --body-file <file>
```

Use [the pull request template](../../../.github/PULL_REQUEST_TEMPLATE.md). The description must:

- Start with `Closes #<n>` so the issue closes on merge (`Refs #<n>` for partial work).
- State what changed and why, in present tense, self-contained: a reader sees only the description and the diff.
- List the evidence: which tests were added, the mutation each was shown to catch, and for UI work the screenshots or measurements. Paste numbers, not "seems fast".
- Name decisions the reviewer may question, with the reason, and anything deliberately left out with the issue that tracks it.
- Fill the "gaps found" section: places where AGENTS.md or a playbook was missing, wrong or misleading while doing this work, so the next agent does not hit them.

Do not add attribution lines to the title or body.

## After opening

```sh
gh pr checks <n> --watch
```

- Red CI is the author's problem: follow `investigate-failure`, push a fix commit, and watch again. Do not retry a failing job hoping it passes unless the log shows an infrastructure error, and say so in a comment if so.
- Keep `in-progress` on the issue until the pull request merges, and link the pull request in an issue comment.
- Respond to review comments with commits, not argument. Add new commits during review; squashing and force-pushing is the maintainer's call. Resolve a conversation only after the change it asks for is pushed.
- When another reviewing agent's findings arrive, verify each against the code before acting; fix the real ones and state why the others are not.

## If the pull request becomes stale

When `main` moves, rebase and push with `git push --force-with-lease` to the feature branch only, and note it in a comment.
