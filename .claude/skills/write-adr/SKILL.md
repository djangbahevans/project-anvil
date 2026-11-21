---
name: write-adr
description: Draft an architecture decision record for a significant design choice in Anvil, including proposing one for the maintainer to decide. Use when work requires a choice that others will build on, or an issue is labeled needs-decision.
---

# Write an ADR

Record a decision when it is hard to reverse, constrains other modules, or when a reasonable person would have chosen differently. Small choices local to one function do not need one.

## Procedure

1. Read [docs/adr/README.md](../../../docs/adr/README.md) and the ADRs related to the area. An accepted ADR is followed, and changed only by a new ADR that supersedes it (the old one gets status "Superseded by NNNN" and nothing else).
2. Copy [docs/adr/template.md](../../../docs/adr/template.md) to `docs/adr/NNNN-short-title.md` with the next free number.
3. Fill the sections:
   - **Context**: the requirement, the constraints, and the forces that pull apart.
   - **Decision**: the choice, in the present tense.
   - **Consequences**: what gets easier, what gets harder, what must stay true. Rejected alternatives appear here as hypotheticals ("a global registry would couple ...").
4. Add a row to the index in `docs/adr/README.md`.
5. Set the status. **Proposed** means the direction is chosen but rests on an assumption the ADR states, or the maintainer has not yet accepted it. **Accepted** is set only when the maintainer has decided; an agent drafting an ADR for an open decision uses Proposed. Merging a Proposed ADR records the proposal and does not accept it. When the maintainer decides, a follow-up pull request sets the status to Accepted in the ADR and in the index row, removes `needs-decision` from the issue, and files the implementation issues.
6. Back claims with evidence when it is cheap to get: a measurement, a prototype, the upstream source file. Cite the commands or paths.
7. Open a pull request (`open-pull-request`) that contains the ADR alone, or the ADR with the work it enables when the decision is already settled. For a `needs-decision` issue, link the pull request from the issue and leave the label until the ADR is Accepted.

## Style

Self-contained and present tense: the reader has only this file. No history ("we first tried"), no process words ("as discussed"), no first-person pronouns.
