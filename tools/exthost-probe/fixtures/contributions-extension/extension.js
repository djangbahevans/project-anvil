const vscode = require('vscode');

// Each command writes its own name at the start of the active document, which is what the editor's
// tests look at. The extension is activated by the commands it contributes, and by nothing else.
exports.activate = (context) => {
    const writes = (name) =>
        vscode.commands.registerCommand(`anvil.contrib.${name}`, () =>
            vscode.window.activeTextEditor.edit((builder) =>
                builder.insert(new vscode.Position(0, 0), `[${name}]`),
            ),
        );
    context.subscriptions.push(writes('lazy'), writes('hidden'), writes('gated'), writes('keyed'), writes('typo'));
};
