# 0003. Qt 6 Widgets for UI chrome

- Status: Accepted
- Date: 2026-11-17

## Context

The application needs windows, menus, tabs, tree views, dialogs, input-method (IME) support, accessibility and native platform integration on three operating systems. Building these on a custom renderer (for example Skia with a bespoke widget layer) offers full control but requires re-implementing IME, accessibility and bidirectional text, which are large and error-prone.

## Decision

The UI is built with Qt 6. Application chrome uses Qt Widgets rather than QML, because Widgets have a smaller runtime footprint and no embedded JavaScript engine in the UI process. The text editing viewport is a custom-painted widget backed by the editor core ([0006](0006-editor-core-and-text-encoding.md)), not `QPlainTextEdit`, which does not scale to very large files.

All Qt usage is confined to the `ui` layer. Core, protocol and platform-service code does not include Qt GUI headers, so that the toolkit remains replaceable.

## Consequences

- IME, accessibility, clipboard, drag and drop, and high-DPI handling come from Qt.
- Theming VS Code-style color themes requires custom painting or stylesheets on top of Widgets.
- Qt is LGPL; dynamic linking is required unless a commercial license is acquired.
- Replacing Qt later is feasible only as far as the layering rule above is respected.
