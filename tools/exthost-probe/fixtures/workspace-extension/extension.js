const vscode = require('vscode');

// Reports what an extension sees of the workspace and of the documents. The argument is the path
// of a resource to ask about; `storage` is the workspace storage of this extension, whose last
// segment is the workspace id.
exports.activate = (context) => {
    context.subscriptions.push(
        vscode.commands.registerCommand('anvil.workspace.report', (path) => {
            const uri = vscode.Uri.file(path);
            const folder = vscode.workspace.getWorkspaceFolder(uri);
            return {
                name: vscode.workspace.name ?? null,
                folders: (vscode.workspace.workspaceFolders ?? []).map((f) => ({
                    path: f.uri.path,
                    name: f.name,
                    index: f.index,
                })),
                folderOfResource: folder ? folder.uri.path : null,
                relativePath: vscode.workspace.asRelativePath(uri, false),
                workspaceFile: vscode.workspace.workspaceFile?.toString() ?? null,
                trusted: vscode.workspace.isTrusted,
                storage: context.storageUri?.path ?? null,
                pid: process.pid,
                activeEditor: vscode.window.activeTextEditor?.document.uri.toString() ?? null,
                documents: vscode.workspace.textDocuments.map((d) => ({ uri: d.uri.toString(), text: d.getText() })),
            };
        }),
        // A request that is still in flight when the editor restarts the host.
        vscode.commands.registerCommand('anvil.workspace.hang', () => new Promise(() => {})),
        vscode.commands.registerCommand('anvil.workspace.die', () => process.kill(process.pid, 'SIGKILL')),
    );
};
