const vscode = require('vscode');

// Creates file system watchers and records what they report, as { watcher, kind, path }.
// `anvil.watch.create` watches a folder with a pattern, `anvil.watch.createGlob` watches with a
// string pattern, which only reports changes inside the workspace. The write, delete and rename
// commands change files through `workspace.fs`.
exports.activate = (context) => {
    const log = [];
    const watchers = new Map();
    let next = 1;

    const record = (id, watcher) => {
        for (const kind of ['Create', 'Change', 'Delete']) {
            watcher[`onDid${kind}`]((uri) => log.push({ watcher: id, kind: kind.toLowerCase(), path: uri.path }));
        }
        watchers.set(id, watcher);
        return id;
    };
    const command = (id, handler) =>
        context.subscriptions.push(vscode.commands.registerCommand(`anvil.watch.${id}`, handler));

    command('create', (folder, pattern) =>
        record(next++, vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(vscode.Uri.file(folder), pattern))));
    command('createGlob', (pattern) => record(next++, vscode.workspace.createFileSystemWatcher(pattern)));
    command('dispose', (id) => watchers.get(id)?.dispose());
    command('events', () => log.slice());
    command('write', (path, text) => vscode.workspace.fs.writeFile(vscode.Uri.file(path), Buffer.from(text)));
    command('delete', (path) => vscode.workspace.fs.delete(vscode.Uri.file(path)));
    command('rename', (from, to) => vscode.workspace.fs.rename(vscode.Uri.file(from), vscode.Uri.file(to)));
};
