#pragma once

#include "anvil/exthost/request_router.hpp"

namespace anvil::exthost {

// The editor side of `vscode.workspace.fs` (VS Code's MainThreadFileSystem) for `file:` resources:
// stat, readdir, readFile, writeFile, rename, copy, mkdir and delete on the local file system.
//
// Failures are error replies whose `name` is the code that the extension host maps to a
// `vscode.FileSystemError`: `EntryNotFound`, `EntryExists`, `EntryNotADirectory`,
// `EntryIsADirectory`, `NoPermissions`, `Unavailable` or `Unknown`. A resource of another scheme
// fails with `Unavailable`, as for a scheme that has no file system provider.
//
// The router must outlive the service.
class FileSystemService {
public:
    explicit FileSystemService(RequestRouter& router);
};

} // namespace anvil::exthost
