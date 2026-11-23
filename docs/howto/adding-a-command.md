# Adding an editor command

An editor operation that a menu, a key binding, the command palette or an extension can run is a command in `ui::CommandRegistry`, registered under VS Code's id when VS Code has one for the operation (`undo`, `editor.action.selectAll`, `workbench.action.files.save`). The registry is the only entry point: a menu item or a shortcut calls `commands_.execute(id)` and never the operation directly.

## 1. Pick the id and the title

Look the operation up in VS Code (`.deps/vscode/src/vs/editor/contrib/**/*.ts` registers `editor.action.*` ids, `src/vs/workbench/browser/actions/` registers `workbench.action.*`) so that key bindings and extensions that name the id work unchanged. The title is the label the command palette shows, without a mnemonic ampersand.

## 2. Expose the operation

A command runs an operation that already has a public entry point on the object that owns it. `EditorView` has the editing operations (`undo`, `move`, `delete_backward`, `copy`), `FindBar` the find ones, and `MainWindow` the file ones. Add a public method when the operation has none, and cover it with a test of that object.

## 3. Register it

`MainWindow::register_commands` adds each command with `add(id, title, action)`. A command ignores the JSON arguments it does not use. The registry is copied into the extension host's command service when the host starts, so extensions run the command with `vscode.commands.executeCommand(id, ...args)` without further work.

The command appears in the command palette under its title with no further work. A menu item runs the command with its id: the menus come from VS Code's menu table, so the id of an operation that VS Code has in a menu enables that item as soon as the command is registered, and an item without a command is shown disabled.

## 4. Test it

- Execute the command through `window.commands().execute(id)` in `tests/ui/main_window_test.cpp` and check the effect on the editor or window.
- When VS Code lists the command in a menu, add its id to `TheMenusRunTheirCommands`, which replaces every handler with a counter and triggers the item.
- Dialogs: a command that opens a modal dialog runs a nested event loop in which the extension host keeps being serviced. It acts on the object that was current when it asked (an index or identity captured before the question), as `close_document` and `save_document` do.
