#pragma once

#include "anvil/core/bytes.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/exthost/request_router.hpp"
#include "anvil/exthost/session.hpp"

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace anvil::exthost {

// What a provider reports as a failure: a `FileSystemProviderErrorCode` (`EntryNotFound`,
// `EntryExists`, `EntryNotADirectory`, `EntryIsADirectory`, `EntryExceedsStorageQuota`,
// `EntryTooLarge`, `EntryWriteLocked`, `NoPermissions`, `Unavailable` or `Unknown`) and a message.
struct ProviderError {
    std::string name;
    std::string message;
};

// `IStat` of VS Code: `type` is the bit flags `File = 1`, `Directory = 2`, `SymbolicLink = 64`.
struct ProviderStat {
    int type = 0;
    double ctime = 0;
    double mtime = 0;
    double size = 0;
    // `FilePermission.Readonly` is 1.
    std::optional<int> permissions;
};

// One change a provider reports: `type` is `Changed = 0`, `Added = 1` or `Deleted = 2`.
struct ProviderChange {
    core::Uri resource;
    int type = 0;
};

// The file systems that extensions provide for their own schemes with
// `workspace.registerFileSystemProvider` (VS Code's MainThreadFileSystem registration, and the file
// service's provider calls). The editor keeps the providers by scheme, and calls them in the
// extension host (`ExtHostFileSystem`) the way the editor itself reads and writes a resource.
//
// A scheme has at most one provider. A write, delete, rename or directory creation on a provider
// that registered as read-only fails with `NoPermissions` without calling it, and a scheme without
// a provider fails with `Unavailable`. Operations call back with the provider's result or error,
// at once when no call is needed.
//
// The session, router and logger must outlive the service.
class FileSystemProviders {
public:
    // `FileSystemProviderCapabilities` of VS Code.
    static constexpr int readonly_capability = 1 << 11;
    static constexpr int folder_copy_capability = 1 << 3;
    static constexpr int trash_capability = 1 << 12;

    struct Provider {
        double handle = 0;
        std::string scheme;
        int capabilities = 0;
        // The JSON text of the markdown message that tells why the provider is read-only.
        std::optional<std::string> readonly_message;
    };

    FileSystemProviders(Session& session, RequestRouter& router,
                        std::shared_ptr<core::Logger> logger);

    const Provider* find(const std::string& scheme) const;
    std::size_t count() const {
        return providers_.size();
    }

    template <class T> using Callback = std::function<void(std::expected<T, ProviderError>)>;

    void stat(const core::Uri& resource, Callback<ProviderStat> done);
    void read_directory(const core::Uri& resource,
                        Callback<std::vector<std::pair<std::string, int>>> done);
    void read_file(const core::Uri& resource, Callback<Bytes> done);
    // Creates or overwrites the file; its folder must exist.
    void write_file(const core::Uri& resource, Bytes content, Callback<std::monostate> done);
    void rename(const core::Uri& resource, const core::Uri& target, bool overwrite,
                Callback<std::monostate> done);
    void copy(const core::Uri& resource, const core::Uri& target, bool overwrite,
              Callback<std::monostate> done);
    void create_directory(const core::Uri& resource, Callback<std::monostate> done);
    void remove(const core::Uri& resource, bool recursive, Callback<std::monostate> done);

    // Serves a `MainThreadFileSystem` request (`$stat`, `$readdir`, `$readFile`, `$writeFile`,
    // `$rename`, `$copy`, `$mkdir` or `$delete`) on resources of a scheme with a provider, with the
    // arguments of the request and, for `$writeFile`, its content, and replies as the handler does.
    void serve(const std::string& method, const nlohmann::json& args, std::optional<Bytes> content,
               std::function<void(HandlerResult)> reply);

    // Asks the provider of the resource's scheme to report the changes below it for `session`
    // (`$watch`), and to stop (`$unwatch`). False when the scheme has no provider.
    bool watch(const core::Uri& resource, double session, const nlohmann::json& options);
    void unwatch(const std::string& scheme, double session);

    // Called with each batch of changes that a provider reports, in the order received.
    void set_change_listener(std::function<void(const std::vector<ProviderChange>&)> listener) {
        change_listener_ = std::move(listener);
    }

private:
    using Raw = std::function<void(std::expected<BinaryReply, ProviderError>)>;
    void call(const core::Uri& resource, bool modifies, const char* method, MixedArgs args,
              Raw done);

    Session& session_;
    std::shared_ptr<core::Logger> logger_;
    std::map<std::string, Provider> providers_;
    std::function<void(const std::vector<ProviderChange>&)> change_listener_;
};

// The error of a provider call as the extension host serialized it (or a message of the editor's
// own), with its code taken from the `name` that `markAsFileSystemProviderError` gives it.
ProviderError provider_error_from(const std::string& text);

} // namespace anvil::exthost
