# 0016. Opening or closing a folder restarts the extension host

- Status: Accepted, in part superseded by 0021 and 0022
- Date: 2026-11-23

## Context

A workspace is part of what the extension host is started with, not only something it is told afterwards:

- The init data carries the workspace (`workspace` of `IExtensionHostInitData`: id and name), and `ExtHostStoragePaths` derives the storage directory that extensions get for workspace state from its id (`.deps/vscode/src/vs/workbench/api/common/extHostStoragePaths.ts`).
- `ExtHostWorkspace` learns the folders from `$initializeWorkspace` before extensions activate, and the activation events that depend on the folders, `workspaceContains:<glob>`, are decided while the host starts (`_handleWorkspaceContainsEagerExtensions` in `extHostExtensionService.ts`).
- `$acceptWorkspaceData` changes the folders of a running host, but nothing of what was fixed at start follows: the init data, the storage paths and the activation events keep describing the workspace the host started with, so an extension sees folders that differ from its own storage and from the reason it was activated.

VS Code reloads the window when the folder changes for these reasons: the extension host is created again with the new workspace.

The first folder comes from the command line when the editor starts (#153). Opening a folder while the window runs (File > Open Folder…, `workbench.action.files.openFolder`) has three possible behaviors: restarting the extension host, updating the running host, or opening a second window. The editor has one window and one extension host per process.

## Decision

Opening or closing a folder in a running window restarts the extension host with the new workspace, as the reload of VS Code does, and keeps the window.

- The window is not rebuilt: tabs, groups, the layout and the open documents stay as they are. The documents are the editor's, and the new host receives them in its first `$acceptDocumentsAndEditorsDelta`, as the first host did.
- The old host is stopped and a new one is started with the options that the first one had and the new workspace (or none for Close Folder). The workspace given on the command line takes the same path at startup.
- What lives in the old host is gone: the state that extensions hold in memory, their registered commands, and the requests in flight, which fail with an error that names the restart. Extension state that is stored (`globalState` and `workspaceState`, #15) is read again by the new host.
- The editor owns the restart: it keeps the options of the extension host and can start it again, so `ExtensionHostBridge` stops and starts a host and tells the window when the commands that extensions register have changed.
- A host that stops for any other reason can be started again the same way.
- Unsaved documents are never closed by the restart, so nothing is asked.

## Consequences

Extensions start with an init data, storage and activation events that describe the workspace, and nothing relies on a live update that the protocol only partly supports. `workspace.onDidChangeWorkspaceFolders` does not fire across a folder change; a host that starts with the new folders sees them as its initial state, as in VS Code.

The cost is a restart on every folder change, of the host's startup (the host is about 135 MB resident, see the protocol reference), and the loss of in-memory extension state, which VS Code also loses on reload. `ExtensionHostBridge` starts one host and has no restart, so it has to gain one, and the window has to forget the commands and views of the old host.

Updating the running host with `$acceptWorkspaceData` would be simpler to build and would keep extension state, but an extension would run with storage paths and activation decisions made for another folder, and the difference would show only in extensions that use workspace storage or `workspaceContains`. A second window would leave a running window alone, but the editor has one window per process, and the command-line argument already starts another process for another folder.

The restart is also the base of restoring a window's workspace after a start (#86) and of a "Restart Extension Host" command.
