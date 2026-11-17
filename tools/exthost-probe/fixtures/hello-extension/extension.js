const vscode = require('vscode');

exports.activate = (context) => {
    context.subscriptions.push(
        vscode.commands.registerCommand('anvil.hello', async (name) => {
            vscode.window.setStatusBarMessage('hello command ran');
            return `hello, ${name ?? 'world'}`;
        }),
        vscode.commands.registerCommand('anvil.callBuiltin', async (n) => {
            const doubled = await vscode.commands.executeCommand('anvil.test.double', n);
            return `doubled: ${doubled}`;
        }),
        vscode.commands.registerCommand('anvil.callMissing', () =>
            vscode.commands.executeCommand('anvil.test.missing', 1),
        ),
    );
};
