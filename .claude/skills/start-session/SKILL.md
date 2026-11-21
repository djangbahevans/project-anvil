---
name: start-session
description: Orient in the Anvil repository and decide what to work on. Use at the start of any session, when handed no specific task, or when unsure what to do next.
---

# Start a session

Run this before touching code. It ends with either a claimed issue to implement, an issue to groom, or a report that nothing can proceed.

## 1. Learn the rules

Read [AGENTS.md](../../../AGENTS.md) and the documents it lists. Every other playbook in `.claude/skills/` assumes them.

## 2. Look at the current state

```sh
git fetch origin
git status --short --branch
git worktree list
git log --oneline origin/main -5
gh pr list --state open
gh issue list --label in-progress
```

- Uncommitted changes, or a branch that is not yours, mean another session is working in this checkout. Leave them alone. Do all work in a separate worktree: `git worktree add ../anvil-<issue> -b <issue>-<slug> origin/main`.
- Open pull requests and `in-progress` issues are taken. Do not start work that overlaps them.

## 3. Choose the work

1. A task named by the user wins. Find its issue (`gh issue view <n>`); if it has none, use `file-issue` first.
2. Otherwise take the first open milestone in title order (`M5`, `M5b`, `M6`, ..., `M10`) that has candidates. GitHub's own milestone numbers follow creation order and do not reflect the title:
   ```sh
   gh api repos/djangbahevans/project-anvil/milestones --jq '[.[] | select(.state=="open")] | sort_by(.title | capture("^M(?<n>[0-9]+)(?<s>[a-z]*)") | [(.n | tonumber), .s]) | .[] | "\(.number) \(.title) (\(.open_issues) open)"'
   gh issue list --milestone "<title>" --label agent-ready --search "-label:in-progress -label:blocked"
   ```
3. Take the lowest-numbered `agent-ready` issue in it that no open pull request covers.
4. If the milestone has no `agent-ready` issue, run `groom-issue` on its first unready issue. Moving an issue to ready is useful work.
5. If nothing can proceed, report why (what is blocked on what, which decisions are pending) instead of inventing work.

## 4. Check the baseline

Build and test a clean checkout of `origin/main` before changing anything, so a failure is attributable:

```sh
cmake --preset linux-clang-debug && cmake --build --preset linux-clang-debug && ctest --preset linux-clang-debug
```

A red baseline is the task: follow `investigate-failure`.

## 5. Claim it

```sh
gh issue edit <n> --add-label in-progress
gh issue comment <n> --body "Started. Branch: <branch>."
```

Then continue with `implement-issue` (or `groom-issue`). Remove `in-progress` if work stops without a merged pull request, and say why in a comment.
