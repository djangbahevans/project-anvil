---
name: file-issue
description: Create a well-formed GitHub issue for a bug, follow-up or planned task in Anvil, with labels, milestone and enough detail for another agent to start. Use when a problem is found and not fixed, work is split, or the user describes new work.
---

# File an issue

A problem that is noticed and not recorded is lost. A problem recorded in one line is nearly as lost. Write for an agent who has not seen this session.

## 1. Avoid duplicates

```sh
gh issue list --state all --search "<keywords>"
```

Extend an existing issue with a comment when one covers it.

## 2. Write the body

Use `.github/ISSUE_TEMPLATE/task.md` (or `bug_report.md` for defects):

- **Context**: why, where it fits, links to ADRs, documents and files (`path:line`) checked to exist.
- **Acceptance criteria**: observable, testable checks, including tests shown to fail without the change and the documentation that changes.
- **Reproduction** (bugs): exact commands or steps, the expected and actual result, the preset and environment.
- **Dependencies**: "Depends on: #N" for each unfinished prerequisite, which is also set as a relation once the issue exists (see below).
- **Starting point**: files to read, tests to copy the style of, traps from AGENTS.md that apply.

Present tense, self-contained, no "we" or "I", no references to the session in which the issue was found.

## 3. Label and place it

- One `area:*` label per affected layer: `core`, `platform`, `exthost`, `ui`, `build`, `docs`.
- One `type:*` label: `feature`, `tech-debt`, `performance`, `research`.
- One readiness label (see `groom-issue`): `agent-ready` only if the definition of ready holds; otherwise `needs-decision`, `needs-split`, `blocked` or `needs-human`.
- A milestone (`gh api repos/djangbahevans/project-anvil/milestones`). When none fits, leave it unset and say why in the body.

```sh
gh issue create --title "<imperative, specific>" --body-file <file> \
  --label "area:core,type:tech-debt,agent-ready" --milestone "<title>"
```

## 4. Link it

Reference the new issue from the pull request or issue where it was found, and from a parent tracking issue's task list when it is a child. Then set the relations, which GitHub uses to show progress and blocked issues:

```sh
scripts/issue-links.sh sub-issue <parent> <new>           # when it is a child
scripts/issue-links.sh blocked-by <new> <prerequisite>... # for each "Depends on" that is not closed
```
