---
name: groom-issue
description: Turn an issue that is not agent-ready into one that is, or establish exactly what blocks it. Use for any issue labeled needs-decision, needs-split, blocked or type:research, or any issue whose scope or acceptance criteria are unclear.
---

# Groom an issue

Grooming changes issues, not code. The result is an issue that either carries `agent-ready` or states precisely what stands between it and ready, with the right readiness label.

## Definition of ready

An issue is `agent-ready` when all of these hold:

1. **Context.** It says why the work is needed and where it fits, and links the ADRs, documents and source files involved. The links were checked: the files and symbols exist.
2. **Acceptance criteria.** Each is observable and testable ("typing in a second tab does not slow the first"), not a restated task ("improve tabs").
3. **Size.** One pull request that a reviewer reads in one sitting. If it needs several, it is `needs-split`.
4. **No open decision.** Nothing in it requires a choice between designs that the issue or an accepted ADR has not made.
5. **No unfinished dependency.** Every issue it depends on is closed, and the body says "Depends on: #N" for any that are not.
6. **Starting point.** It names the files, the tests to model new ones on, and any trap from AGENTS.md that applies.

## Readiness labels

| Label | Meaning | Who moves it forward |
| --- | --- | --- |
| `agent-ready` | Meets the definition above | An implementing agent |
| `needs-decision` | A design choice is open | An agent prepares options; the maintainer decides |
| `needs-split` | Too large for one pull request | A grooming agent |
| `blocked` | Waits on another issue or an external fact | Nobody, until the blocker closes |
| `needs-human` | Needs hardware, an account, a license ruling or taste | The maintainer |
| `in-progress` | Claimed by a session | The claimant |

An issue carries at most one of the first five. Remove the previous one when changing it.

## Procedure

1. Read the issue, its comments, its milestone, and the code and ADRs it touches. Open the files it mentions; descriptions go stale.
2. Check each item of the definition of ready and list what fails.
3. Resolve each failure by its kind:
   - **Thin context or criteria.** Rewrite the issue body (`gh issue edit <n> --body-file <file>`) following `.github/ISSUE_TEMPLATE/task.md`. Replace vague criteria with checks a test or a screenshot can show.
   - **Too large.** Create the child issues with `file-issue`, each independently shippable and ordered. List them in the parent as a task list (`- [ ] #41`). The parent becomes a tracking issue: remove `agent-ready`, keep it open until the children close.
   - **Open decision.** List the options with trade-offs and a recommendation, as a comment on the issue. When the decision is architectural, draft the ADR with status Proposed on a branch and open a pull request (see `write-adr`); the maintainer decides by saying so on the pull request or the issue, and a follow-up pull request then sets the status to Accepted. Label `needs-decision` until then. Do not implement before the ADR is Accepted.
   - **Blocked.** Name the blocker in the body ("Depends on: #N"), label `blocked`, and look for a smaller first slice that is not blocked.
   - **Research.** A `type:research` issue is complete when it produces a written finding (an ADR, a section of `docs/`, or a comment with measurements and commands to reproduce them) and follow-up issues. Say that in its acceptance criteria. Define the question and the form of the answer.
   - **Needs a human.** Label `needs-human` and say exactly what is needed and why an agent cannot do it.
4. When every item holds, set the label: `gh issue edit <n> --add-label agent-ready --remove-label <old>`.
5. Comment with a short summary of what changed and why the issue is now in its state.

## Recheck dependencies

Before declaring an issue blocked or ready, run `gh issue view <blocker>`: a closed blocker means the label comes off. Planning mistakes (a dependency cycle, a wrongly ready issue) are fixed in the issue and mentioned in a comment, not worked around.
