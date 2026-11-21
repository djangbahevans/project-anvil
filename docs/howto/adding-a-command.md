# Adding an editor command

An editor operation that a menu, a key binding, the command palette or an extension can run is a command in `ui::CommandRegistry`, registered under VS Code's id when VS Code has one for the operation (`undo`, `editor.action.selectAll`, `workbench.action.files.save`). The registry is the only entry point: a menu item or a shortcut calls `commands_.execute(id)` and never the operation directly.

## 1. Pick the id and the title

Look the operation up in VS Code (`.deps/vscode/src/vs/editor/contrib/**/*.ts` registers `editor.action.*` ids, `src/vs/workbench/browser/actions/` registers `workbench.action.*`) so that key bindings and extensions that name the id work unchanged. The title is the label the command palette shows, without a mnemonic ampersand.

## 2. Expose the operation

A command runs an operation that already has a public entry point on the object that owns it. `EditorView` has the editing operations (`undo`, `move`, `delete_backward`, `copy`), `FindBar` the find ones, and `MainWindow` the file ones. Add a public method when the operation has none, and cover it with a test of that object.

## 3. Register it

`MainWindow::register_commands` adds each command with `add(id, title, action)`. A command ignores the JSON arguments it does not use. The registry is copied into the extension host's command service when the host starts, so extensions run the command with `vscode.commands.executeCommand(id, ...args)` without further work.

Call the command from the menu with `commands_.execute(id)`.

## 4. Test it

- Execute the command through `window.commands().execute(id)` in `tests/ui/main_window_test.cpp` and check the effect on the editor or window.
- Check that the menu action runs the command: `TheMenusRunTheirCommands` replaces a handler with a counter and triggers the action.
- Dialogs: a command that opens a modal dialog runs a nested event loop in which the extension host keeps being serviced. It acts on the object that was current when it asked (an index or identity captured before the question), as `close_document` and `save_document` do.
