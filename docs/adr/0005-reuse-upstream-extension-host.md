# 0005. Reuse the upstream VS Code extension host

- Status: Proposed
- Date: 2026-11-17

## Context

The `vscode` API namespace is large, and its behavior (event ordering, document synchronization, provider adapters, type classes) is subtle. Extensions depend on that behavior, not only on the declared signatures.

VS Code's extension host is a Node program in the MIT-licensed VS Code repository. It communicates with the workbench through an internal RPC protocol organized as paired `ExtHost*` (extension side) and `MainThread*` (workbench side) services.

## Decision

The editor runs the unmodified upstream extension host bundle, built from a pinned VS Code release tag, inside the Node process from [0004](0004-extension-host-node-child-process.md). The editor implements the `MainThread*` side of the protocol in C++ for the supported API surface (see [0009](0009-scope-and-non-goals.md)).

The pinned tag is a vendored dependency. Upgrading it is a deliberate change that re-validates the implemented services.

This ADR remains Proposed until a spike demonstrates the extension host starting, completing its handshake with the C++ side, and activating an extension that registers and executes a command.

## Consequences

- API semantics are inherited from upstream rather than re-implemented.
- The internal protocol is unversioned and changes between releases, so the editor is coupled to one VS Code version at a time.
- Building the extension host bundle requires a Node toolchain and the upstream build scripts.
- A self-written `vscode` shim module over a simpler protocol would avoid the coupling but would re-implement the extension-side logic that upstream already provides.
