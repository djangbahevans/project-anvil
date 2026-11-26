# 0019. The editor keeps its window state in a store of its own

- Status: Accepted, in part superseded by 0021
- Date: 2026-11-26

## Context

The editor reopens as it was left: the size and position of the window, the sizes and visibility of its parts and the active view containers (#86), the expanded folders of the Explorer (#254) and the recently used commands of the palette (#100). [0014](0014-workbench-layout.md) has the workbench persist the sizes and visibility of the parts and leaves the location to #15.

#15 is the storage of extensions: one JSON object per extension id, below the storage directory of the extension host, written by a `MainThread` service for a host that is running.

The state of the window has other needs:

- It exists without an extension host (`ANVIL_EXTENSION_HOST=off`, or no Node.js), and the layout is applied before the window is first shown, which is before a host runs.
- Some of it belongs to a folder and some to the user, and there is state to keep with no folder open.
- `settings.json` is the user's own file, which the editor writes only when a command changes a setting ([0017](0017-commands-change-settings-by-editing-the-file.md)).
- The editor has one window per process ([0016](0016-opening-a-folder-restarts-the-extension-host.md)) and File > New Window starts another, so several processes use the same files.

## Decision

The editor has a state store, `workbench::StateStore`, with no Qt, separate from the settings and from the storage of extensions. What the user configures stays in `settings.json`; the store holds what follows from using the editor.

- **Files.** They are below `state` in the user's configuration directory (`platform::user_config_dir`): `state/global.json`, `state/workspaces/<workspace id>.json` for a folder (`core::Workspace::id`), and `state/no-folder.json` for a window without one. With no configuration directory the store keeps state in memory only.
- **Format.** A file is one JSON object of dotted keys and JSON values, such as `{"layout.sidebar.visible": true}`. A missing key means the default, and so does a value of the wrong type or out of range. A key that the editor does not know is kept. A file that is not a JSON object is read as empty, logged once and replaced at the next write.
- **Writes.** Changes are collected and written after a short delay and when the window closes. A write reads the file again, changes only the keys that this process has set, and replaces the file with `platform::write_file`. A key that a process never set keeps the value another one wrote; for a key that two processes set, the last write wins.
- **Scopes** follow those of VS Code (`LayoutStateKeys` in `src/vs/workbench/browser/layout.ts`). The global file holds the size and position of the window, the sizes of the parts and the recently used commands. The file of the workspace holds the visibility of the parts, the position of the panel, whether it is maximized, the active view containers and the expanded folders of the Explorer. A workspace without saved state gets the default layout with the saved sizes.
- **Changing the folder.** Opening or closing a folder in a running window writes the pending state to the files of the workspace being left. The window keeps its layout, as 0016 requires, and what it saves afterwards goes to the file of the new workspace. State that describes the content of a folder, such as its expanded folders, is read from the new workspace's file.

## Consequences

- #86, #254 and #100 do not depend on #15, and #15 keeps its own files. The location that 0014 leaves to #15 is the one above.
- The layout that a folder was saved with is restored when the editor starts on the folder. A folder opened in a running window keeps the layout on screen, which then becomes the folder's saved layout; applying the saved one would move the parts under the user's hands, in a window that 0016 keeps as it is.
- The read and the replacement of a write are not locked, so a write of another process between the two is lost. `platform::write_file` names its temporary file after the target (#323), so two processes writing at the same instant can leave a damaged file, which is read as empty. Only conveniences are at stake: nothing that the store holds is the user's work.
- State follows the path of a folder: a moved or renamed folder starts with none. The files of workspaces that are gone stay.
- Keeping the state in the storage of extensions, under a reserved id, would tie it to the directory and the lifetime of the extension host, and the window needs it without one.
- Keeping it in `settings.json` would rewrite the user's file whenever a sash moves.
- `QSettings` would store it in a format and place that differ by platform (the registry on Windows), has no scope per workspace, and would put the store in `ui`.
- A separate state directory (`$XDG_STATE_HOME` on Linux) would match the convention there, but needs a second directory function on every platform and splits the editor's files between two places.
