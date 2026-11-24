---
name: Task
about: A piece of planned work
labels: ""
---

## Context

Why this is needed and where it fits. Link the ADRs, documents and files involved (`path:line`).

## Acceptance criteria

- [ ] Observable behavior that shows the work is done.
- [ ] Tests that were shown to be able to fail.
- [ ] Documentation updated where the change belongs (ADR, protocol reference, README).

## Starting point

Files to read, tests to model new ones on, traps from AGENTS.md that apply.

## Dependencies

"Depends on: #N" for each unfinished prerequisite, or "None". Set each as a relation too: `scripts/issue-links.sh blocked-by <this issue> <prerequisite>`.

## Readiness

Add `agent-ready` only when every item holds; otherwise use `needs-decision`, `needs-split`, `blocked` or `needs-human` (see `.claude/skills/groom-issue/SKILL.md`).

- [ ] Context explains why, and the linked files and symbols exist.
- [ ] Every criterion is observable and testable.
- [ ] Fits in one pull request.
- [ ] No open design decision.
- [ ] No unfinished dependency.
- [ ] Starting point is named.
