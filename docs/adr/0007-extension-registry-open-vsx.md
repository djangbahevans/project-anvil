# 0007. Open VSX as the extension registry

- Status: Proposed
- Date: 2026-11-17

## Context

The Visual Studio Marketplace terms restrict use of its service to Microsoft's own products, so third-party editors cannot rely on it. Open VSX is an open registry of VS Code extensions operated by the Eclipse Foundation and used by other VS Code-compatible editors.

## Decision

Extension discovery and installation use the Open VSX HTTP API. Extensions are installed as `.vsix` packages into a per-user extensions directory and described by their `package.json` manifest.

The decision remains Proposed until the registry's current terms and API stability are reviewed against the editor's needs.

## Consequences

- Not every extension available on the Visual Studio Marketplace is available on Open VSX, and some publishers do not distribute there.
- Side-loading a local `.vsix` file is supported independently of the registry.
- The registry endpoint is configurable so that a self-hosted instance can be used.
