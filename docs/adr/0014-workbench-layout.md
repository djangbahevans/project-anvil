# 0014. Workbench window built from fixed parts in a purpose-built layout

- Status: Accepted
- Date: 2026-11-21

## Context

VS Code's window is a fixed set of parts: title bar, activity bar, primary sidebar, editor area, bottom panel, secondary sidebar and status bar (`Parts` in `src/vs/workbench/services/layout/browser/layoutService.ts`, one directory each under `src/vs/workbench/browser/parts/`). Parts sit in fixed positions, are resized with sashes, can be shown or hidden, and keep their sizes between sessions. The editor area is a grid of editor groups, each with its own tab strip and editor. The sidebar and the panel host views that are registered by id: the explorer, search and the terminal are built in, and extensions contribute tree views.

The window today is a `QMainWindow` whose central widget stacks one `QTabBar`, the find bar and one `EditorView`, with the stock menu bar and status bar (`src/ui/main_window.cpp`). The explorer, workspace search, the terminal, extension views, split editors and extension status bar items all need a place to go. Each of them would otherwise choose its own placement, and the choice is costly to change once several features depend on it.

Constraints that pull apart:

- The look and behavior of the parts follow VS Code ([0009](0009-scope-and-non-goals.md) puts the explorer, tabs and splits, the panel and the status bar in scope), and every part is colored by workbench color keys from the theme (`sideBar.background`, `activityBar.background`, `statusBar.background`, `tab.activeBackground`).
- Qt Widgets are the toolkit for the chrome ([0003](0003-ui-toolkit-qt6-widgets.md)), with custom painting where the stock widgets do not match.
- Startup time and memory are goals of the project: a part or view that is not shown costs nothing.
- `ui` depends on `workbench`, `exthost` and `core`, and never the reverse. The model of the layout must be testable without Qt.

## Decision

- **A purpose-built layout, not `QDockWidget`.** The workbench is a widget in `src/ui` that arranges the parts with `QSplitter`s: the activity bar at the edge, then the sidebar, then a vertical splitter holding the editor area above the panel. The title bar sits above and the status bar below. Each part is a small widget class with a stable id (`workbench.parts.sidebar` and so on, matching VS Code's names), a visibility flag and a size that the workbench persists in workspace state (storage location: [#15](https://github.com/djangbahevans/project-anvil/issues/15), settings: [#5](https://github.com/djangbahevans/project-anvil/issues/5)).
- **View containers.** The sidebar and the panel host *view containers*. A container is registered with an id, a title, an icon and a factory that creates its widget; the factory runs the first time the container is shown. The activity bar lists the sidebar's containers, and the panel shows a strip of its own. The container knows nothing about the type of the view it hosts, so the explorer, search, the terminal and extension tree views plug in the same way.
- **Editor groups.** The editor area is a tree: a node is either a group or a splitter of nodes with an orientation and ratios. A group has its own tab strip, an optional breadcrumb strip and one `EditorView`. A window starts with one group. The model (groups, their tabs in order, the active tab of each group, the split tree) is plain data with no widget types, so it can be tested without Qt and persisted. It lives in the `workbench` layer once that layer exists.
- **Status bar.** A custom widget with a left and a right group of items, each item ordered by priority, with text, tooltip, optional colors and an optional command. It replaces the `QStatusBar` so that extension entries ([#13](https://github.com/djangbahevans/project-anvil/issues/13)) and the built-in entries (language, position, encoding, line ending) share one model.
- **Colors from the theme.** A part reads its colors by workbench color key through one lookup that the theme work ([#23](https://github.com/djangbahevans/project-anvil/issues/23)) fills; until a theme is loaded the lookup returns values derived from `QPalette`. No part hard-codes a color, so applying a theme changes no layout code.
- **Title bar and menu.** The window uses the native frame and the native `QMenuBar` on every platform. A custom title bar with the menu inside it is a separate decision for later: it needs frameless-window handling (resize edges, snapping, the system menu, dragging) that differs per platform, and no other part depends on it.
- **Keyboard focus.** The workbench owns movement of focus between the parts and between editor groups, so that the commands for them (focus the sidebar, focus the next group, toggle the panel) are ordinary commands that [#25](https://github.com/djangbahevans/project-anvil/issues/25) can bind.

## Consequences

- The layout matches VS Code's: fixed part positions, sash resizing and toggling work the same way, and persistence stores a size per part id rather than a binary window state.
- A view that has never been shown is never created, so the explorer, the terminal and extension views add no startup cost until used.
- Editor groups are a data structure first, so splits ([#18](https://github.com/djangbahevans/project-anvil/issues/18)) and persisting open tabs can be built and tested before any widget exists.
- The project owns what a dock framework would give: focus order between parts, accessibility roles and names for custom-painted parts (`QAccessibleInterface` for the activity bar, tab strips and status bar), high-DPI painting, and restoring sizes. These are written once in the workbench and its part widgets.
- A `QDockWidget` based layout would allow floating, tabbed and rearranged panels that VS Code does not have, draw its own title bars that are hard to restyle, and restore from a binary state that does not map to per-part sizes; each of these works against matching VS Code.
- A QML scene graph for the chrome would add a JavaScript engine to the UI process, which [0003](0003-ui-toolkit-qt6-widgets.md) rejects.
- The native frame keeps the first implementation small and keeps platform window behavior (snap layouts, system menu, accessibility) correct. A custom title bar can be added later behind the same part id without changing the other parts. If the native menu bar proves unacceptable on a platform, that decision is revisited with its own ADR.
- Work on the per-keystroke path must not grow with the number of parts, groups or tabs: the status bar updates its position item only when the text of that item differs, as the tab strip does for tab text.
