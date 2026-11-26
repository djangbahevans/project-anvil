# 0021. A folder opened in a running window takes its own layout

- Status: Accepted
- Date: 2026-11-26

## Context

The editor keeps the layout of a window for each workspace ([0019](0019-editor-state-store.md)): the visibility of the side bar and the panel, the position of the panel, whether it is maximized and the active view containers. Opening or closing a folder keeps the window and restarts the extension host ([0016](0016-opening-a-folder-restarts-the-extension-host.md)).

A window that keeps the layout on screen when its folder changes restores the layout of a folder only at startup. A user who starts the editor without a folder and then opens one never gets the folder back as it was left, and the next write replaces what was saved for it with the layout of the empty window. The default that shows the side bar for a folder has no effect there either.

VS Code reloads the window to open a folder, so the folder's layout comes back, and a folder without one gets the default layout (`LayoutStateKeys` in `src/vs/workbench/browser/layout.ts`).

This decision replaces the paragraph "Changing the folder" of 0019 with its consequence on when a layout is restored, and the statement of 0016 that the layout stays as it is. The rest of both is in force.

## Decision

Opening or closing a folder in a running window gives the window the layout of the workspace that it enters.

- The pending state is written to the files of the workspace that is left first.
- The visibility of the side bar and the panel, the position of the panel, whether it is maximized and the active view containers are those saved for the workspace entered, which for Close Folder is the window without a folder.
- Each of these that is not saved takes its default, so nothing of the layout of the workspace left carries over: the side bar shows with a folder and is hidden without one, the panel is hidden, at the bottom and not maximized, and each part shows its first view container.
- The sizes of the parts and of the window stay, as they belong to the user, and so does what a setting governs.
- Zen Mode, which is not saved, is left.
- The tabs, groups and open documents stay as 0016 has them.
- The keyboard focus stays where it is when that still shows, and goes to the editor otherwise.

## Consequences

- A folder comes back as it was left however it is opened, and a window has the same layout for a folder whether it started on it or opened it later.
- The parts move when a folder is opened. This happens at a moment that the user chose, when the content of the window changes anyway.
- A layout arranged in a window without a folder does not follow to a folder opened from it.
- Whatever else the store keeps for a workspace and the window shows has to be applied when the folder changes as well as at startup.
- Keeping the layout on screen for a folder that has none saved would let a side bar that is hidden in an empty window hide the Explorer of a folder never seen before, and save it so.
- Rebuilding the window, as a reload does, would give the layout without a separate path, but 0016 keeps the window for the documents that it holds.
