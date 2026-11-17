# 0004. Run extensions in a Node child process

- Status: Accepted
- Date: 2026-11-17

## Context

Running non-UI VS Code extensions requires a Node.js runtime. Node can be linked into the application as a library (libnode) or launched as a separate process. Linking in-process complicates the build, shares an address space between untrusted extension code and the editor, and ties editor stability to extension stability.

## Decision

Extensions run in a separate Node.js process, started by the editor and connected over a pipe or local socket. The editor ships a pinned `node` binary. The process is started lazily when the first extension needs to activate, and is restarted if it exits unexpectedly.

## Consequences

- A crash or runaway extension cannot take down the editor; the host process can be killed and restarted.
- Editors that never activate an extension pay no memory cost for Node.
- Every interaction between editor and extensions crosses a serialization boundary, so the protocol must be designed for batching and avoid chatty round trips.
- An in-process embedding would remove that boundary but is rejected because the isolation benefit outweighs the latency cost.
