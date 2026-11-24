#include "anvil/exthost/file_event_service.hpp"

#include "anvil/exthost/vscode_types.hpp"

#include <algorithm>

namespace anvil::exthost {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using platform::FileEvent;
using platform::FileEventKind;

constexpr const char* source = "file watcher";

// `FileChangeFilter` of VS Code.
constexpr int filter_updated = 1 << 1;
constexpr int filter_added = 1 << 2;
constexpr int filter_deleted = 1 << 3;

// The default of `files.watcherExclude`.
const std::vector<std::string>& default_workspace_excludes() {
    static const std::vector<std::string> excludes = {
        ".git/objects/**",   ".git/subtree-cache/**",   ".hg/store/**",
        "*/.git/objects/**", "*/.git/subtree-cache/**", "*/.hg/store/**"};
    return excludes;
}

bool is_below_or_equal(const fs::path& path, const fs::path& base) {
    const std::string text = path.generic_string();
    const std::string prefix = base.generic_string();
    return text == prefix || (text.size() > prefix.size() && text.starts_with(prefix) &&
                              (prefix.ends_with('/') || text[prefix.size()] == '/'));
}

// Excludes are globs relative to the watched folder, or paths that may be absolute.
std::vector<std::string> relative_excludes(const json& excludes, const fs::path& base) {
    std::vector<std::string> result;
    if (!excludes.is_array()) {
        return result;
    }
    for (const json& entry : excludes) {
        if (!entry.is_string()) {
            continue;
        }
        const std::string text = entry.get<std::string>();
        if (!text.empty() && text.front() == '/') {
            if (is_below_or_equal(text, base)) {
                std::string relative = fs::path(text).lexically_relative(base).generic_string();
                if (relative != ".") {
                    result.push_back(std::move(relative));
                }
            }
            continue;
        }
        result.push_back(text);
    }
    return result;
}

} // namespace

FileEventService::FileEventService(Session& session, RequestRouter& router,
                                   FileSystemProviders& providers,
                                   std::shared_ptr<core::Logger> logger,
                                   const std::optional<core::Workspace>& workspace)
    : session_(session), providers_(providers), logger_(std::move(logger)) {
    providers_.set_change_listener(
        [this](const std::vector<ProviderChange>& changes) { send_provider_changes(changes); });
    router.on(ProxyId::MainThreadFileSystemEventService, "$watch",
              [this](const Request&, const json& args) { return watch(args); });
    router.on(ProxyId::MainThreadFileSystemEventService, "$unwatch",
              [this](const Request&, const json& args) { return unwatch(args); });
    if (workspace) {
        workspace_watch_ = start("the workspace", workspace->folder, true,
                                 default_workspace_excludes(), std::nullopt, 0);
    }
}

FileEventService::~FileEventService() = default;

std::shared_ptr<FileEventService::Watch>
FileEventService::start(const std::string& owner, const fs::path& base, bool recursive,
                        std::vector<std::string> excludes, std::optional<double> session,
                        int filter) {
    auto watch = std::make_shared<Watch>();
    watch->base = base;
    watch->recursive = recursive;
    watch->excludes = excludes;
    watch->session = session;
    watch->filter = filter;
    auto started = platform::FileWatcher::start(
        base, platform::WatchOptions{.recursive = recursive, .excludes = std::move(excludes)});
    if (!started) {
        logger_->log(core::LogLevel::warning, source,
                     "Cannot watch " + base.string() + " for " + owner + ": " +
                         started.error().message);
        return nullptr;
    }
    watch->watcher = std::move(*started);
    watches_.push_back(watch);
    return watch;
}

std::optional<HandlerResult> FileEventService::watch(const json& args) {
    if (!args.is_array() || args.size() < 5 || !args[0].is_string() || !args[1].is_number() ||
        !args[3].is_object()) {
        return Failed{error_json("$watch needs an extension, a session, a resource, options and "
                                 "a correlation flag")};
    }
    const std::string owner = args[0].get<std::string>();
    const double session = args[1].get<double>();
    const auto uri = uri_from_json(args[2]);
    const auto base = uri ? uri->file_path() : std::nullopt;
    if (uri && !base && providers_.find(uri->scheme)) {
        if (providers_.watch(*uri, session, args[3])) {
            provider_sessions_[session] = uri->scheme;
        }
        return Ok{};
    }
    if (!base) {
        logger_->log(core::LogLevel::warning, source,
                     owner + " watches a resource that is not a file: " +
                         (uri ? uri->to_string() : "invalid") + "; it is not watched");
        return Ok{};
    }
    const json& options = args[3];
    const bool recursive = options.value("recursive", false);
    std::vector<std::string> excludes =
        relative_excludes(options.value("excludes", json::array()), *base);

    if (args[4].is_boolean() && args[4].get<bool>()) {
        int filter = filter_updated | filter_added | filter_deleted;
        if (options.contains("filter") && options["filter"].is_number_integer()) {
            filter = options["filter"].get<int>();
        }
        if (auto watch = start(owner, *base, recursive, std::move(excludes), session, filter)) {
            sessions_[session] = std::move(watch);
        }
        return Ok{};
    }

    for (const std::shared_ptr<Watch>& existing : watches_) {
        const bool covers = !existing->session &&
                            ((existing->recursive && is_below_or_equal(*base, existing->base)) ||
                             (!existing->recursive && !recursive && existing->base == *base));
        if (covers) {
            sessions_[session] = existing;
            return Ok{};
        }
    }
    if (auto watch = start(owner, *base, recursive, std::move(excludes), std::nullopt, 0)) {
        sessions_[session] = std::move(watch);
    }
    return Ok{};
}

std::optional<HandlerResult> FileEventService::unwatch(const json& args) {
    if (!args.is_array() || args.empty() || !args[0].is_number()) {
        return Failed{error_json("$unwatch needs a session")};
    }
    const double session = args[0].get<double>();
    if (const auto provided = provider_sessions_.find(session);
        provided != provider_sessions_.end()) {
        providers_.unwatch(provided->second, session);
        provider_sessions_.erase(provided);
        return Ok{};
    }
    const auto found = sessions_.find(session);
    if (found == sessions_.end()) {
        return Ok{};
    }
    const std::shared_ptr<Watch> watch = found->second;
    sessions_.erase(found);
    const bool used =
        std::ranges::any_of(sessions_, [&](const auto& entry) { return entry.second == watch; });
    if (!used && watch != workspace_watch_) {
        std::erase(watches_, watch);
    }
    return Ok{};
}

void FileEventService::send(const Watch& watch, const std::vector<FileEvent>& events) {
    json created = json::array();
    json changed = json::array();
    json deleted = json::array();
    for (const FileEvent& event : events) {
        const json uri = uri_to_json(core::Uri::file(event.path));
        switch (event.kind) {
        case FileEventKind::created:
            if (!watch.session || (watch.filter & filter_added)) {
                created.push_back(uri);
            }
            break;
        case FileEventKind::changed:
            if (!watch.session || (watch.filter & filter_updated)) {
                changed.push_back(uri);
            }
            break;
        case FileEventKind::deleted:
            if (!watch.session || (watch.filter & filter_deleted)) {
                deleted.push_back(uri);
            }
            break;
        }
    }
    send_batch(std::move(created), std::move(changed), std::move(deleted), watch.session);
}

void FileEventService::send_batch(json created, json changed, json deleted,
                                  std::optional<double> session) {
    if (created.empty() && changed.empty() && deleted.empty()) {
        return;
    }
    json batch = {{"created", std::move(created)},
                  {"changed", std::move(changed)},
                  {"deleted", std::move(deleted)}};
    if (session) {
        batch["session"] = *session;
    }
    session_.call(ProxyId::ExtHostFileSystemEventService, "$onFileEvent",
                  json::array({std::move(batch)}).dump(), [](CallResult) {});
}

void FileEventService::send_provider_changes(const std::vector<ProviderChange>& changes) {
    json created = json::array();
    json changed = json::array();
    json deleted = json::array();
    for (const ProviderChange& change : changes) {
        const json uri = uri_to_json(change.resource);
        (change.type == 1 ? created : change.type == 2 ? deleted : changed).push_back(uri);
    }
    send_batch(std::move(created), std::move(changed), std::move(deleted), std::nullopt);
}

void FileEventService::poll() {
    const std::vector<std::shared_ptr<Watch>> watches = watches_;
    for (const std::shared_ptr<Watch>& watch : watches) {
        auto polled = watch->watcher->poll(std::chrono::milliseconds(0));
        if (!polled) {
            logger_->log(core::LogLevel::warning, source,
                         "Watching " + watch->base.string() + ": " + polled.error().message);
            continue;
        }
        if (!polled->empty()) {
            send(*watch, *polled);
        }
    }
}

} // namespace anvil::exthost
