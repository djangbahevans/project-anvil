#include "anvil/exthost/workspace_service.hpp"

#include "anvil/exthost/vscode_types.hpp"

#include <utility>

namespace anvil::exthost {
namespace {

using nlohmann::json;

HandlerResult answer(bool exists) {
    return OkJson{exists ? "true" : "false"};
}

} // namespace

WorkspaceService::WorkspaceService(Session& session, RequestRouter& router,
                                   std::shared_ptr<core::Logger> logger, SearchSettings settings)
    : session_(session), logger_(std::move(logger)), settings_(std::move(settings)) {
    router.on(
        ProxyId::MainThreadWorkspace, "$checkExists",
        [this](const Request& request, const json& args) { return check_exists(request, args); });
}

void WorkspaceService::warn(const std::string& message) {
    if (logger_) {
        logger_->log(core::LogLevel::warning, "workspace", message);
    }
}

// `$checkExists(folders: UriComponents[], includes: string[])`, with a cancellation token that is
// not among the arguments.
std::optional<HandlerResult> WorkspaceService::check_exists(const Request& request,
                                                            const json& args) {
    if (!args.is_array() || args.size() < 2 || !args[0].is_array() || !args[1].is_array()) {
        warn("$checkExists: the arguments are not folders and patterns");
        return answer(false);
    }
    Check check{.request = request.req, .folders = {}, .includes = {}};
    for (const json& folder : args[0]) {
        // Only folders on this machine can be searched.
        if (const auto uri = uri_from_json(folder)) {
            if (const auto path = uri->file_path()) {
                check.folders.push_back(*path);
            }
        }
    }
    for (const json& include : args[1]) {
        if (include.is_string() && !include.get<std::string>().empty()) {
            check.includes.push_back(include.get<std::string>());
        }
    }
    // Without a pattern every file would match.
    if (check.folders.empty() || check.includes.empty()) {
        return answer(false);
    }
    if (!settings_.ripgrep) {
        if (!std::exchange(reported_missing_, true)) {
            warn("extensions that start for a pattern of files in the workspace are not started: "
                 "ripgrep, which searches for them, was not found");
        }
        return answer(false);
    }
    check.deadline = std::chrono::steady_clock::now() + time_limit_;
    if (!start_next(check)) {
        return answer(false);
    }
    checks_.push_back(std::move(check));
    return std::nullopt;
}

bool WorkspaceService::start_next(Check& check) {
    check.listing.reset();
    while (check.next < check.folders.size()) {
        const std::filesystem::path& folder = check.folders[check.next++];
        auto listing = search::FileListing::start(*settings_.ripgrep, folder, settings_.options,
                                                  check.includes);
        if (listing) {
            check.listing = std::move(*listing);
            return true;
        }
        warn("cannot search " + folder.string() + ": " + listing.error().message);
    }
    return false;
}

std::optional<bool> WorkspaceService::advance(Check& check) {
    const search::ListingBatch batch = check.listing->read(std::chrono::milliseconds(0));
    if (!batch.files.empty()) {
        return true;
    }
    if (batch.finished) {
        if (batch.error) {
            warn("searching " + check.folders[check.next - 1].string() +
                 " failed: " + batch.error->message);
        }
        if (!start_next(check)) {
            return false;
        }
    }
    if (std::chrono::steady_clock::now() >= check.deadline) {
        warn("gave up searching for " + check.includes.front() + " after " +
             std::to_string(time_limit_.count()) + " ms");
        return false;
    }
    return std::nullopt;
}

void WorkspaceService::poll() {
    std::erase_if(checks_, [this](Check& check) {
        const std::optional<bool> exists = advance(check);
        if (!exists) {
            return false;
        }
        session_.reply(check.request, answer(*exists));
        return true;
    });
}

void WorkspaceService::stop() {
    checks_.clear();
}

} // namespace anvil::exthost
