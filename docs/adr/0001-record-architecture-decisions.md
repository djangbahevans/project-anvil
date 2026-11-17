# 0001. Record architecture decisions

- Status: Accepted
- Date: 2026-11-17

## Context

Anvil is a native C++ code editor that aims to run a subset of VS Code extensions. Its architecture rests on many interdependent choices (UI toolkit, process model, compatibility strategy). The reasoning behind each is easy to lose, and a later contributor cannot tell a deliberate constraint from an accident.

## Decision

Architectural decisions are recorded as ADRs in `docs/adr/`, one decision per file, using [template.md](template.md). The current architecture is described in [../design.md](../design.md), which links to the ADRs that justify it.

## Consequences

- Each significant decision has a stable, linkable rationale.
- Reversing a decision requires a new ADR, which keeps the reversal deliberate.
- ADRs and `design.md` must be updated in the same change as the code they describe.
