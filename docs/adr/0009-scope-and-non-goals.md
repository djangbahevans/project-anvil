# 0009. Scope and non-goals

- Status: Accepted
- Date: 2026-11-17

## Context

VS Code's feature surface is very large, and much of it depends on an embedded browser engine. The editor's purpose is to deliver a fast, low-memory native editor that can run extensions whose behavior does not require rendering HTML.

## Decision

In scope:

- Text editing, multi-cursor, find and replace, workspace search, file explorer, tabs and splits, command palette, status bar, settings, keybindings, and color themes.
- Language features through the Language Server Protocol and through extensions registering providers via `vscode.languages`.
- An integrated terminal.
- The non-UI portion of the `vscode` API in a Node extension host: commands, workspace and file-system access, text documents, languages, tree views, quick pick and input box, status bar items, configuration, and source control.

Out of scope:

- Webviews and any extension feature that requires embedded HTML, CSS or JavaScript rendering.
- Notebooks, custom editors, and web-worker extension hosts.
- Remote development and Live Share.
- The Visual Studio Marketplace.

Extensions that depend on out-of-scope features are expected to fail to activate or to lose the affected functionality, and the editor reports the unsupported API rather than silently ignoring it.

## Consequences

- The `MainThread*` implementation effort ([0005](0005-reuse-upstream-extension-host.md)) is bounded by the in-scope list.
- Some popular extensions with webview-based UI will not be usable.
- Items may move into scope through a new ADR that supersedes this one.
