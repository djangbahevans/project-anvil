# exthost-probe

A Node script that plays the editor's role toward the upstream VS Code extension host. It listens on a Unix socket, completes the handshake, sends the initialization calls the extension host waits for, activates the given extensions, executes a command, and logs every RPC request the extension host sends. It documents the protocol behavior that the C++ `exthost` layer must reproduce ([protocol reference](../../docs/extension-host-protocol.md)).

```sh
scripts/fetch-vscode.sh
node tools/exthost-probe/probe.mjs \
  --vscode .deps/vscode \
  --extension tools/exthost-probe/fixtures/hello-extension \
  --seconds 8
```

`--command <id> [--args '<json array>']` runs another command, for example one of the commands of the other fixture extensions.

A successful run shows `anvil.hello` registered through `MainThreadCommands.$registerCommand`, the reply `"hello, anvil"` to `$executeContributedCommand`, and the extension host's resident memory.

The probe reads the proxy identifier table from `extHost.protocol.ts` in the checkout, so it works only against a checkout whose source order matches the table, which is true for any single release.
