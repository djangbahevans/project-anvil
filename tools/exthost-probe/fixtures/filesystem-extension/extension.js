const vscode = require('vscode');

const uri = (path) => vscode.Uri.file(path);

// The extension host serves `file:` resources itself for stat, readDirectory, readFile, writeFile,
// createDirectory and delete, with Node's file system, and asks the editor only for rename, copy,
// a delete with `useTrash` and for resources of other schemes. The commands cover both paths.
//
// Each command runs one `workspace.fs` call and reports its result, or its failure as the
// extension sees it: the code of the `FileSystemError` and whether it is one at all. Bytes travel
// as base64.
const outcome = async (run) => {
    try {
        return { ok: (await run()) ?? null };
    } catch (error) {
        return {
            error: {
                code: error.code ?? null,
                isFileSystemError: error instanceof vscode.FileSystemError,
                name: error.name,
                message: error.message,
            },
        };
    }
};

exports.activate = (context) => {
    const fs = vscode.workspace.fs;
    const command = (id, handler) =>
        context.subscriptions.push(vscode.commands.registerCommand(`anvil.fs.${id}`, (...args) => outcome(() => handler(...args))));

    command('stat', async (path) => ({ ...(await fs.stat(uri(path))) }));
    command('statUri', async (text) => ({ ...(await fs.stat(vscode.Uri.parse(text))) }));
    command('readDirectory', async (path) => fs.readDirectory(uri(path)));
    command('readFile', async (path) => Buffer.from(await fs.readFile(uri(path))).toString('base64'));
    command('writeFile', async (path, base64) => {
        await fs.writeFile(uri(path), Buffer.from(base64, 'base64'));
    });
    command('createDirectory', async (path) => {
        await fs.createDirectory(uri(path));
    });
    command('delete', async (path, recursive, useTrash) => {
        await fs.delete(uri(path), { recursive: !!recursive, useTrash: !!useTrash });
    });
    command('rename', async (from, to, overwrite) => {
        await fs.rename(uri(from), uri(to), { overwrite: !!overwrite });
    });
    command('copy', async (from, to, overwrite) => {
        await fs.copy(uri(from), uri(to), { overwrite: !!overwrite });
    });
};
