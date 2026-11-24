#pragma once

#include "anvil/core/log.hpp"
#include "anvil/core/workspace.hpp"
#include "anvil/exthost/file_system_providers.hpp"
#include "anvil/exthost/request_router.hpp"
#include "anvil/exthost/session.hpp"
#include "anvil/platform/file_watcher.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anvil::exthost {

// The editor side of `workspace.createFileSystemWatcher` (VS Code's
// MainThreadFileSystemEventService) for `file:` resources: `$watch` and `$unwatch`, and the
// batches of events that go back as `ExtHostFileSystemEventService.$onFileEvent`.
//
// The extension host does not ask to watch the workspace folder for a watcher that is created with
// a string pattern or a pattern inside the workspace: it expects the editor to report the changes
// of the workspace folder at all times. The service therefore watches the workspace folder
// itself, recursively, without the paths that `files.watcherExclude` excludes by default.
//
// A watch of a resource that an earlier watch covers (the same folder, or below a folder that is
// watched recursively) shares the earlier watcher, so that an extension host that gets the events
// of every watch never gets one change twice. What the covered watch excludes is not applied, and
// what the earlier watch excludes is not reported to it. A watch that asks for
// correlated events gets a watcher and a session of its own, and its events carry the session.
// The extension host filters every event with the pattern of each of its watchers, so a watcher
// that reports more than a watch asked for does no harm. A `file:` resource is watched by the
// editor; a resource of a scheme with a file system provider is watched by the provider, which is
// asked to, and the changes it reports are sent on as events. Another scheme is not watched. A
// watch that cannot start is logged and leaves the others alone.
//
// The session, the router and the logger must outlive the service. Events are collected by the
// watchers' worker threads and sent when poll() is called.
class FileEventService {
public:
    FileEventService(Session& session, RequestRouter& router, FileSystemProviders& providers,
                     std::shared_ptr<core::Logger> logger,
                     const std::optional<core::Workspace>& workspace);
    ~FileEventService();

    // Sends the events that have arrived since the last call, one `$onFileEvent` per watcher.
    void poll();

    // The number of platform watchers that run, which is less than the number of sessions when
    // watches share one.
    std::size_t watcher_count() const {
        return watches_.size();
    }

private:
    struct Watch {
        std::filesystem::path base;
        bool recursive = false;
        std::vector<std::string> excludes;
        // Set for a correlated watch, whose events carry it.
        std::optional<double> session;
        // Which kinds of events a correlated watch reports: the `FileChangeFilter` bits.
        int filter = 0;
        std::unique_ptr<platform::FileWatcher> watcher;
    };

    std::shared_ptr<Watch> start(const std::string& owner, const std::filesystem::path& base,
                                 bool recursive, std::vector<std::string> excludes,
                                 std::optional<double> session, int filter);
    std::optional<HandlerResult> watch(const nlohmann::json& args);
    std::optional<HandlerResult> unwatch(const nlohmann::json& args);
    void send(const Watch& watch, const std::vector<platform::FileEvent>& events);
    void send_batch(nlohmann::json created, nlohmann::json changed, nlohmann::json deleted,
                    std::optional<double> session);
    void send_provider_changes(const std::vector<ProviderChange>& changes);

    Session& session_;
    FileSystemProviders& providers_;
    std::shared_ptr<core::Logger> logger_;
    std::vector<std::shared_ptr<Watch>> watches_;
    // The watcher of the workspace folder, which stays whatever the extensions do.
    std::shared_ptr<Watch> workspace_watch_;
    // The watcher that each `$watch` session uses.
    std::map<double, std::shared_ptr<Watch>> sessions_;
    // The scheme of the provider that watches for each session that asked for one.
    std::map<double, std::string> provider_sessions_;
};

} // namespace anvil::exthost
