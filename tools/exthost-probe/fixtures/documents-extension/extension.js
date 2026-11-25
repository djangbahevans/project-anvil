const vscode = require('vscode');

const changes = [];
const saves = [];
const closes = [];

const pair = (position) => [position.line, position.character];

function describe(document, editor) {
    return {
        uri: document.uri.toString(),
        text: document.getText(),
        lineCount: document.lineCount,
        languageId: document.languageId,
        version: document.version,
        isDirty: document.isDirty,
        eol: document.eol === vscode.EndOfLine.CRLF ? 'crlf' : 'lf',
        options: editor ? { tabSize: editor.options.tabSize, insertSpaces: editor.options.insertSpaces } : null,
        selections: editor ? editor.selections.map((s) => [...pair(s.anchor), ...pair(s.active)]) : [],
    };
}

exports.activate = (context) => {
    const active = () => vscode.window.activeTextEditor;
    context.subscriptions.push(
        vscode.workspace.onDidChangeTextDocument((event) =>
            changes.push({
                uri: event.document.uri.toString(),
                version: event.document.version,
                isDirty: event.document.isDirty,
                reason: event.reason ?? null,
                contentChanges: event.contentChanges.map((c) => ({
                    start: pair(c.range.start),
                    end: pair(c.range.end),
                    rangeOffset: c.rangeOffset,
                    rangeLength: c.rangeLength,
                    text: c.text,
                })),
            }),
        ),
        vscode.workspace.onDidSaveTextDocument((document) => saves.push(document.uri.toString())),
        vscode.workspace.onDidCloseTextDocument((document) => closes.push(document.uri.toString())),

        vscode.commands.registerCommand('anvil.docs.active', () => {
            const editor = active();
            return editor ? describe(editor.document, editor) : null;
        }),
        vscode.commands.registerCommand('anvil.docs.insert', (text) =>
            active().edit((builder) => builder.insert(new vscode.Position(0, 0), text)),
        ),
        vscode.commands.registerCommand('anvil.docs.replaceTwo', () =>
            active().edit((builder) => {
                builder.replace(new vscode.Range(0, 0, 0, 1), 'X');
                builder.delete(new vscode.Range(1, 0, 1, 1));
            }),
        ),
        vscode.commands.registerCommand('anvil.docs.open', async (path) => {
            const document = await vscode.workspace.openTextDocument(vscode.Uri.file(path));
            return describe(document, undefined);
        }),
        vscode.commands.registerCommand('anvil.docs.show', async (path) => {
            const document = await vscode.workspace.openTextDocument(vscode.Uri.file(path));
            const editor = await vscode.window.showTextDocument(document);
            return { shown: editor.document.uri.toString(), active: active().document.uri.toString() };
        }),
        vscode.commands.registerCommand('anvil.docs.create', async (language, content) => {
            const document = await vscode.workspace.openTextDocument({ language, content });
            await vscode.window.showTextDocument(document);
            return describe(document, active());
        }),
        vscode.commands.registerCommand('anvil.docs.changes', () => changes.splice(0)),
        vscode.commands.registerCommand('anvil.docs.saves', () => saves.splice(0)),
        vscode.commands.registerCommand('anvil.docs.closes', () => closes.splice(0)),
        vscode.commands.registerCommand('anvil.docs.openDocuments', () =>
            vscode.workspace.textDocuments.map((document) => document.uri.toString()),
        ),
        vscode.commands.registerCommand('anvil.docs.select', (a, b, c, d) => {
            active().selection = new vscode.Selection(a, b, c, d);
            return true;
        }),
        vscode.commands.registerCommand('anvil.docs.save', () => active().document.save()),
        vscode.commands.registerCommand('anvil.docs.workspaceEdit', (path) => {
            const edit = new vscode.WorkspaceEdit();
            edit.insert(vscode.Uri.file(path), new vscode.Position(0, 0), '>>');
            return vscode.workspace.applyEdit(edit);
        }),
        // Applies a WorkspaceEdit made of the given steps: {op: 'create' | 'rename' | 'delete' |
        // 'insert', path, to?, options?, text?}. `text` is the content of a created file and the
        // text an insert puts at the start of the file.
        vscode.commands.registerCommand('anvil.docs.fileEdit', (steps) => {
            const edit = new vscode.WorkspaceEdit();
            for (const step of steps) {
                const uri = vscode.Uri.file(step.path);
                const options = { ...(step.options || {}) };
                if (step.op === 'create') {
                    if (step.text !== undefined) {
                        options.contents = Buffer.from(step.text);
                    }
                    edit.createFile(uri, options);
                } else if (step.op === 'rename') {
                    edit.renameFile(uri, vscode.Uri.file(step.to), options);
                } else if (step.op === 'delete') {
                    edit.deleteFile(uri, options);
                } else {
                    edit.insert(uri, new vscode.Position(0, 0), step.text);
                }
            }
            return vscode.workspace.applyEdit(edit);
        }),
        vscode.commands.registerCommand('anvil.docs.execute', (id, ...args) =>
            vscode.commands.executeCommand(id, ...args),
        ),
    );
};
