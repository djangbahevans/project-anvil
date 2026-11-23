const vscode = require('vscode');

// Reports what an extension sees of the workspace. The argument is the path of a resource to ask
// about; `storage` is the workspace storage of this extension, whose last segment is the workspace id.
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
            };
        }),
    );
};
