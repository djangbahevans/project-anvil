const vscode = require('vscode');

exports.activate = (context) => {
    context.subscriptions.push(
        vscode.commands.registerCommand('anvil.log.console', () => {
            console.log('hello', { a: 1 }, 42);
            console.warn('careful now');
            console.error('bad thing happened');
            return true;
        }),
        vscode.commands.registerCommand('anvil.log.channel', () => {
            const channel = vscode.window.createOutputChannel('Anvil Log Channel', { log: true });
            channel.info('from the log channel');
            channel.warn('warned by the log channel');
            return true;
        }),
        vscode.commands.registerCommand('anvil.log.plainChannel', () => {
            const channel = vscode.window.createOutputChannel('Anvil Plain Channel');
            channel.appendLine('a plain line');
            return true;
        }),
        vscode.commands.registerCommand('anvil.log.unhandled', () => {
            setTimeout(() => {
                throw new Error('async boom');
            }, 0);
            return true;
        }),
    );
};
