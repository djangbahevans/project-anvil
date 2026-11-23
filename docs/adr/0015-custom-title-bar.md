# 0015. Custom title bar on Linux and Windows, native frame and global menu on macOS

- Status: Accepted
- Date: 2026-11-23

## Context

[0014](0014-workbench-layout.md) keeps the native window frame and the native `QMenuBar` and leaves a custom title bar "for later". In VS Code the top of the window is one row: the app icon, the menu bar, the window title or command center, the layout buttons and the minimize, maximize and close controls, colored by the `titleBar.*` theme keys. With the native frame, Anvil shows a system title bar with the controls and a second row for the menus, which looks different from VS Code on Linux and Windows and cannot follow the color theme. On macOS, VS Code puts the menus in the system menu bar and keeps the native traffic lights, which the native `QMenuBar` already does.

A custom title bar needs a frameless window, which moves the work of moving, resizing, maximizing and snapping from the window manager to the application, in a way that differs per platform. Qt 6.4 offers `QWindow::startSystemMove()` and `QWindow::startSystemResize(Qt::Edges)`, which hand an interactive move or resize to the window manager, so the application does not move or resize the window itself.

A throwaway prototype (a `Qt::FramelessWindowHint` widget with a 35 px bar holding eight menu buttons, a draggable title and three window controls) was run under WSLg with both the Wayland and the X11 Qt platform plugins. The results that this decision rests on:

- `startSystemMove()` moves the window, a double click on the bar maximizes and restores it, dragging the bar of a maximized window restores it under the pointer, the maximize button works, and Win+arrow keys and dragging to the screen edge snap the window.
- `startSystemResize()` resizes from all four edges and four corners when the handles are the outermost pixels of the window itself.
- A resize handle in a transparent margin outside the painted frame (a translucent window with a 6 px layout margin) receives no mouse input under WSLg, so the margin does not work as a handle.
- Menu buttons that open a `QMenu` do not follow the pointer to a neighboring button while a menu is open. The popup holds the mouse grab, and a handler for mouse moves on the popup that opens the menu under the pointer restores the behavior of a menu bar.
- Whether the compositor draws a shadow around a frameless window was not established.

Windows snap layouts, the Windows system menu and DWM shadow, and macOS were not exercised.

## Decision

Linux and Windows use a frameless window with a `TitleBar` part (`workbench.parts.titlebar`, the part id of 0014) as the top row of the workbench, holding the app icon, the menu bar, an empty draggable area that shows the window title, and minimize, maximize and close buttons drawn with the Codicons font. The close button turns red under the pointer. macOS keeps the native frame and the native `QMenuBar`, which Qt places in the system menu bar.

- **Moving.** A left press on an empty part of the bar calls `startSystemMove()`; a double click toggles maximized. Dragging a maximized window is handled by the window manager.
- **Resizing.** The outermost 5 px of the window are resize handles while the window is neither maximized nor fullscreen. An application-wide event filter handles mouse moves and presses over them before the child widgets do, sets the matching resize cursor, and calls `startSystemResize()` with the edges. No transparent margin is used. The handles overlap the outer pixels of the content, so the top 5 px of the bar resize instead of moving.
- **Menu bar.** The menu data of [#130](https://github.com/djangbahevans/project-anvil/issues/130) builds one `QMenuBar`. On macOS it is the native menu bar; elsewhere the same widget sits in the title bar row, styled with the `titleBar.*` and `menu.*` colors, so Alt, mnemonics, arrow keys, hover switching, the overflow extension and the accessibility roles are Qt's. If the styling or the keyboard behavior of an embedded `QMenuBar` cannot match VS Code closely enough, the bar is drawn as a row of flat buttons that open `QMenu` popups, with the hover handler that the prototype shows, and the keyboard handling is then written in the part.
- **Windows.** The maximize button answers `WM_NCHITTEST` with `HTMAXBUTTON` so that the snap layout flyout appears, the window keeps the DWM shadow, and a right click on the bar opens the system menu.
- **Setting.** `window.titleBarStyle` is `custom` or `native`, as in VS Code. `native` shows the system frame with the `QMenuBar` below it, and is the only mode on macOS.
- **Fullscreen and maximized.** The resize handles are off in both. The title bar is hidden in fullscreen and shown while the pointer is at the top edge, as VS Code does.
- **Accessibility.** The title bar and the window buttons have `QAccessibleInterface`s with the roles and names of the system's own controls.

The decision holds if an embedded `QMenuBar` or the fallback buttons can reach VS Code's look, and if Windows behaves as the platform documents it. Both are checked when the title bar is implemented, and a failure is recorded in a new ADR.

## Consequences

- The top of the window looks like VS Code's on Linux and Windows and follows the color theme, and the same menu data serves the custom bar, the native bar and the macOS menu bar.
- The project owns window chrome that the system supplied: the controls, the maximized and fullscreen layouts, resize handles, the focus order of the bar and its accessibility. Each is written once in the title bar part.
- A vertical scroll bar or a splitter at the very edge of the window is partly covered by a resize handle. VS Code has the same overlap.
- A window manager that ignores `startSystemMove()` or `startSystemResize()` (a return value of `false`) leaves the window fixed. The part falls back to the native frame when either call reports failure on the first use.
- A frameless window on Linux receives whatever shadow the compositor gives it. The part draws a 1 px border (`titleBar.border`, `window.activeBorder`) so that the edge is visible without one.
- A transparent margin as the resize handle would give a larger target outside the window but receives no input under WSLg.
- Keeping the native frame and moving only the menu into the content area would match neither VS Code's single row nor its theme colors, and `QMainWindow` cannot place a menu bar in a system title bar.
- Drawing the title bar and its menu with QML would add a JavaScript engine to the UI process, which [0003](0003-ui-toolkit-qt6-widgets.md) rejects.
- This decision replaces the "Title bar and menu" decision of 0014 for Linux and Windows; the other decisions of 0014 stand.
