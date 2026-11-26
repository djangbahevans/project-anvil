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

## 5. Record the results of the ported matchers again

The core's fuzzy matcher, its file name comparer, its token theme matcher, its visible columns and its line breaks are tested against results recorded from VS Code's own code (the scorer in the checkout, and `vscode-textmate` with `vscode-oniguruma` in its `node_modules`). Record them from the new checkout and run the core tests; a difference is a change in VS Code's behavior to port.

```sh
node scripts/generate-fuzzy-cases.mjs
node scripts/generate-file-name-order-cases.mjs   # needs a Node.js with full ICU data
node scripts/generate-token-theme-cases.mjs
node scripts/generate-key-table.mjs          # the keys, then the cases that use them
node scripts/generate-key-chord-cases.mjs
node scripts/generate-keybinding-cases.mjs
node scripts/generate-languages.mjs          # the languages, then the cases that use them
node scripts/generate-language-cases.mjs
node scripts/generate-language-comments.mjs   # the comment tokens of the languages
node scripts/generate-comment-cases.mjs       # what the comment commands do, recorded from VS Code
node scripts/generate-grapheme-table.mjs     # the grapheme classes, then the cases that use them
node scripts/generate-cursor-column-cases.mjs # visible columns and how Up and Down keep them
node scripts/generate-line-break-cases.mjs    # where wrapped lines break
```

The key table (`src/core/include/anvil/core/key_table.hpp`) lists VS Code's `KeyCode` values with the names key bindings use for them, so a new key in the release is a new row. CI checks the three key scripts with `--check`.

The language table (`src/core/include/anvil/core/language_table.hpp`) lists `plaintext` and the `contributes.languages` of the built-in extensions in `.deps/vscode/extensions`, in the order the extension scanner registers them. The recorded cases come from VS Code's own `LanguageService`, loaded with those contributions. A new built-in language or a changed rule is a diff in the table, and a change in how VS Code decides a file's language shows as a failing case in `language_registry_test`. CI checks both scripts with `--check`.

The menu bar is generated too (`src/workbench/include/anvil/workbench/menu_table.hpp`). `generate-menus.mjs` loads the compiled workbench in Node, creates its workbench contributions and views service against inert services, and reads the menus back from `MenuRegistry`, with the `when`, `precondition` and `toggled` clause of each item and, from the configuration registry, the default of every setting that a clause reads as `config.<key>` (`menu_config_defaults`). It stops with a message when a menu it expects (File to Help, and a few items that stand for registrations the script must reach) is missing, when a clause reads a setting that no configuration registers, and when the menus or those defaults differ between Linux and Windows. CI checks it with `--check`.

```sh
node scripts/generate-menus.mjs
```

Review the diff for items that VS Code added, moved or renamed. A new item shows disabled until a command with its id is registered.

## 6. Refresh the color themes

The default color themes are copied from the checkout into `resources/themes`, and a test fails when the copies differ from it. Refresh them, review the diff, and check that the default theme (`EditorDefaults::color_theme`, VS Code's `ThemeSettingDefaults.COLOR_THEME_DARK` in `workbenchThemeService.ts`) is still in the list:

```sh
node scripts/sync-themes.mjs
```

## 7. Refresh the Codicons

The Codicons font, its license text and the table of icon names and code points come from the `@vscode/codicons` package in the checkout, and tests fail when the font or the table differs from it. Regenerate them and review the diff: an icon that the editor draws by name must still exist (the tests that draw every icon of the table show what changed), and the version in `THIRD_PARTY_NOTICES.md` follows the package.

```sh
node scripts/generate-codicons.mjs
```

## 8. Refresh the Seti icons

The Seti font, its notice and the table of the icons of the languages come from the `theme-seti` extension in the checkout, and tests fail when the font or the table differs from it. Regenerate them and review the diff. The script stops for a definition that is not a glyph with a color, and the table follows the rules by which VS Code turns the theme into a style sheet (`fileIconThemeData.ts`), so a change there is one to port.

```sh
node scripts/generate-seti-icons.mjs
```

## 9. Update the documentation

Update the version in `docs/extension-host-protocol.md` and every fact in it that changed, and the version in `README.md` where it appears.
