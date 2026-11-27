# 0022. A folder opened in a running window brings its own editors

- Status: Accepted
- Date: 2026-11-27

## Context

The editor keeps the state of a window for each workspace ([0019](0019-editor-state-store.md)): the layout ([0021](0021-layout-of-an-opened-folder.md)) and the editor groups with their tabs, which a start restores. Opening or closing a folder in a running window keeps the window and restarts the extension host ([0016](0016-opening-a-folder-restarts-the-extension-host.md)).

Editors that stay on screen when the folder changes are saved as those of the folder entered, which replaces the editors that were saved for it. A user who opens a folder from another one would lose the tabs of the folder opened.

VS Code reloads the window to open a folder: it closes the editors of the workspace left, after asking about or backing up unsaved ones, and restores those of the workspace entered (`shouldRestoreEditors` in `src/vs/workbench/browser/layout.ts`).

This decision replaces the statements of 0016 that the tabs, groups and open documents stay and that nothing is asked, and that of 0021 that they stay as 0016 has them. The rest of both is in force.

## Decision

Opening or closing a folder in a running window switches the editors.

- Each unsaved document is asked about first. Cancel leaves the folder, the editors and the saved state as they were.
- The editor groups and file tabs of the workspace left are saved with its state, and every editor is closed.
- The window then shows the editors of the workspace entered as a start on it does, and one untitled document when there is none to show.
- Only files come back. An untitled document and a change that was discarded are gone.
- What leads back to a closed editor is forgotten: the navigation history, the closed editors and the recently opened list.
- The extension host restarts as 0016 has it and is given the documents of the workspace entered.

## Consequences

- A folder comes back with its editors however it is opened, and a window has the same editors for a folder whether it started on it or opened it later.
- A document cannot be carried from one folder to another by opening the other, a file outside both included.
- A change of folder can be refused at a question about an unsaved document.
- Whatever refers to an open document has to cope with its closing at a change of folder.
- Keeping the editors of the workspace left on screen would overwrite what was saved for the one entered.
- Switching the editors only where the folder entered has some saved would leave those of another folder on screen in a folder opened for the first time, and save them as its own.
- Keeping unsaved text across the change, without a question, needs backups of unsaved documents, which the editor does not make.
