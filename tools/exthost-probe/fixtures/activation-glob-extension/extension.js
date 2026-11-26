const vscode = require('vscode');

// Registers a command named after the extension when it activates, so that a test sees from the
// commands of the host which extensions are active. The command is not in the manifest: running it
// would not activate the extension.
exports.activate = (context) => {
    context.subscriptions.push(
        vscode.commands.registerCommand(`anvil.activation.${require('./package.json').name}`, () => true),
    );
};
