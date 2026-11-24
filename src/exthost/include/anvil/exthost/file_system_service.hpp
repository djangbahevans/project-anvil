#pragma once

#include "anvil/core/log.hpp"
#include "anvil/exthost/activation_service.hpp"
#include "anvil/exthost/file_system_providers.hpp"
#include "anvil/exthost/request_router.hpp"
#include "anvil/exthost/session.hpp"

#include <memory>

namespace anvil::exthost {

// The editor side of `vscode.workspace.fs` (VS Code's MainThreadFileSystem) for `file:` resources:
// stat, readdir, readFile, writeFile, rename, copy, mkdir and delete on the local file system.
//
// Failures are error replies whose `name` is the code that the extension host maps to a
// `vscode.FileSystemError`: `EntryNotFound`, `EntryExists`, `EntryNotADirectory`,
// `EntryIsADirectory`, `NoPermissions`, `Unavailable` or `Unknown`.
//
// Before an operation on a resource of another scheme the service fires `onFileSystem:<scheme>`
// and waits for the activation, as VS Code's file service does, and then has the provider of the
// scheme serve the operation, or fails with `Unavailable` for a scheme that has no file system
// provider.
//
// `$ensureActivation(scheme)` fires `onFileSystem:<scheme>` for every scheme but `file` and replies
// when the activation has finished, as `extHostFileSystemConsumer.ts` awaits it before every
// operation on a resource. A failed activation is logged and the operation then reports that no
// provider is there.
//
// The session, router, activation service and providers must outlive the service.
class FileSystemService {
public:
    FileSystemService(Session& session, RequestRouter& router, ActivationService& activation,
                      FileSystemProviders& providers, std::shared_ptr<core::Logger> logger);
};

} // namespace anvil::exthost
