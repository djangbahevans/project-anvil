#pragma once

#include "anvil/core/file_tree.hpp"
#include "anvil/core/log.hpp"
#include "anvil/exthost/request_router.hpp"
#include "anvil/exthost/session.hpp"
#include "anvil/search/ripgrep.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anvil::exthost {

// What the searches that extensions ask for run with.
struct SearchSettings {
    // None when there is no ripgrep.
    std::optional<std::filesystem::path> ripgrep{};
    // What the settings leave out of a search.
    core::FileSearchOptions options{};
};

// The editor side of VS Code's MainThreadWorkspace: `$checkExists`, by which the extension host
// decides whether an extension with a `workspaceContains:<glob>` activation event starts. The
// answer is whether ripgrep, given the globs and the search settings, finds a file below one of
// the folders (ADR 0020).
//
// A search runs as ripgrep does, and the reply is sent by poll() when the first file has arrived,
// when every folder has been searched, or when the time limit has passed, which is shorter than
// the 7 s the extension host waits. The session, the router and the logger must outlive the
// service; destroying it ends the searches.
class WorkspaceService {
public:
    static constexpr std::chrono::milliseconds default_time_limit{5000};

    WorkspaceService(Session& session, RequestRouter& router, std::shared_ptr<core::Logger> logger,
                     SearchSettings settings);

    // Takes what the searches have found and replies to those that are over. It does not wait.
    void poll();
    // Ends every search without a reply, for a connection that is going away.
    void stop();

    // How long a request may search, for the requests that arrive from now on.
    void set_time_limit(std::chrono::milliseconds limit) {
        time_limit_ = limit;
    }
    // The requests that are waiting for a reply.
    std::size_t pending() const {
        return checks_.size();
    }

private:
    struct Check {
        std::uint32_t request = 0;
        std::vector<std::filesystem::path> folders;
        std::vector<std::string> includes;
        // The folder to search when the listing of the one before is over.
        std::size_t next = 0;
        std::optional<search::FileListing> listing{};
        std::chrono::steady_clock::time_point deadline{};
    };

    std::optional<HandlerResult> check_exists(const Request& request, const nlohmann::json& args);
    // Starts the listing of the next folder that can be searched; false when there is none left.
    bool start_next(Check& check);
    // The answer, once there is one.
    std::optional<bool> advance(Check& check);
    void warn(const std::string& message);

    Session& session_;
    std::shared_ptr<core::Logger> logger_;
    SearchSettings settings_;
    std::chrono::milliseconds time_limit_ = default_time_limit;
    std::vector<Check> checks_;
    bool reported_missing_ = false;
};

} // namespace anvil::exthost
