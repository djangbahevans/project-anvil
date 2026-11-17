# Adding a MainThread service

The extension host calls the editor through `MainThread*` services. Most of them are accepted with an empty reply until they are implemented. This guide adds or extends one.

## 1. Find out what is needed

Run the extension that needs the service and list the requests nothing handled:

- In a test, `ExtensionHost::unhandled_requests()` returns the `"Service.method"` names, and the `RealExtensionHost.ReportsRequestsNoServiceHandled` test prints them.
- With the probe: `node tools/exthost-probe/probe.mjs --vscode .deps/vscode --extension <dir> --command <id>` logs every request with its arguments.

Read the service's shape (`MainThread<Name>Shape`) in `.deps/vscode/src/vs/workbench/api/common/extHost.protocol.ts`, and the VS Code implementation in `.deps/vscode/src/vs/workbench/api/browser/mainThread<Name>.ts` for the exact semantics, including what the extension host does with each reply.

## 2. Implement it

Services live in `src/exthost`. Follow `commands_service.cpp` (small) or `documents_service.cpp` (larger):

- Register handlers on the `RequestRouter` with `router.on(ProxyId::MainThread<Name>, "$method", handler)`. The proxy ids are generated from the pinned source.
- A handler receives the parsed JSON arguments and returns `Ok{}` (empty reply), `OkJson{json}`, `Failed{error_json(message)}`, or `std::nullopt` to reply later with `Session::reply`.
- Call the extension host with `Session::call(ProxyId::ExtHost<Name>, "$method", args_json, callback)`.
- When a reply must arrive after something the extension host observes, send the notification before the reply; both travel in order on one connection.
- Keep the service free of Qt and of workbench types. Reach editor state through a small interface or observer, as `DocumentsService::Observer` does.
- Create the service in `ExtensionHost` (member order matters: it must be declared after what it uses) and expose an accessor.
- A service that must be known to the extension host before extensions activate belongs in the startup sequence in `extension_host.cpp`.

## 3. Test it twice

1. Against `FakeHost` for exact wire behavior: add `tests/exthost/<name>_service_test.cpp` modeled on `commands_service_test.cpp` or `documents_service_test.cpp`, and register it in `tests/CMakeLists.txt`. Check the replies, the calls the service makes, and their order.
2. Against the real extension host: add a fixture extension under `tools/exthost-probe/fixtures/<name>-extension/` that exercises the API through commands, add its path as a compile definition next to the others in `tests/CMakeLists.txt`, and write tests in `tests/exthost/<name>_integration_test.cpp` using `RealDocuments` in `documents_integration_test.cpp` as the pattern.

Run the integration test binary directly to confirm the tests ran and did not skip.

## 4. Prove the tests can fail

Break the service on purpose, for example by returning the wrong reply or sending the notification after the reply. Confirm that both kinds of test fail, then restore the code. Fix tests that still pass.

## 5. Document it

Add what was learned about the extension host's behavior to `docs/extension-host-protocol.md`: argument shapes, required replies, ordering, limits. Update the table of implemented services there.
