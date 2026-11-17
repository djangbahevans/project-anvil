const vscode = require('vscode');

exports.activate = (context) => {
    context.subscriptions.push(
        vscode.commands.registerCommand('anvil.hello', async (name) => {
            vscode.window.setStatusBarMessage('hello command ran');
            return `hello, ${name ?? 'world'}`;
        }),
    );
};
