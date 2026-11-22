# Upgrading the pinned VS Code

The extension host protocol is internal to VS Code and unversioned, so the editor is built against one release ([ADR 0005](../adr/0005-reuse-upstream-extension-host.md)). The pin lives in `scripts/fetch-vscode.sh` as `VSCODE_TAG`.

## 1. Look at what changed

Compare the two releases for the files that define the protocol, in a full clone of VS Code or on GitHub:

- `src/vs/workbench/api/common/extHost.protocol.ts`: the services, their methods and the order of the proxy identifiers.
- `src/vs/workbench/services/extensions/common/rpcProtocol.ts`: the RPC message encoding.
- `src/vs/base/parts/ipc/common/ipc.net.ts`: the frame header and acknowledgement rules.
- `src/vs/workbench/services/extensions/common/extensionHostProtocol.ts`: the handshake and the init data.
- `src/vs/workbench/api/node/extensionHostProcess.ts`: how the process connects and what environment it reads.

## 2. Fetch the new release

1. Set `VSCODE_TAG` in `scripts/fetch-vscode.sh` and in `api_version` of `InitDataOptions` (`src/exthost/include/anvil/exthost/init_data.hpp`).
2. Remove `.deps/vscode`, then run `scripts/fetch-vscode.sh`. The release may need a newer Node.js (see `.nvmrc` in the checkout) or pnpm.
3. Regenerate the proxy identifiers with `node scripts/generate-proxy-ids.mjs`, and review the diff. Every service name the code uses must still exist.

## 3. Rebuild and run the tests

Build, then run all tests, and run `anvil_exthost_integration_tests` directly to see that the real-host tests ran. Fix what the protocol changes broke: init data fields, startup calls, argument shapes.

## 4. Re-record the real traffic

The replay test feeds a recording of a real session through the session code. Record a new one:

```sh
node tools/exthost-probe/probe.mjs --vscode .deps/vscode \
  --extension tools/exthost-probe/fixtures/hello-extension --seconds 6 \
  --dump tests/exthost/data/exthost-<version>-session.bin
```

Delete the old recording and update the file name in `tests/exthost/replay_test.cpp`.

## 5. Record the fuzzy scorer's results again

The core's fuzzy matcher is tested against results recorded from VS Code's own scorer. Record them from the new checkout and run the core tests; a difference is a change in VS Code's scoring to port.

```sh
node scripts/generate-fuzzy-cases.mjs
```

## 6. Update the documentation

Update the version in `docs/extension-host-protocol.md` and every fact in it that changed, and the version in `README.md` where it appears.
