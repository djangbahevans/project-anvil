#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/log.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::workbench {

// Whether a value belongs to the user or to the workspace folder that the window has open.
enum class StateScope { global, workspace };

// What follows from using the editor, such as the layout of the window, kept between runs
// (ADR 0019). `global.json` holds the state of the user, `workspaces/<workspace id>.json` that of
// a folder and `no-folder.json` that of a window without one; each file is a JSON object of dotted
// keys. A write changes only the keys that were set since the last one, so that another process
// that uses the same files keeps what it wrote to the other keys.
class StateStore {
public:
    // The files are below `directory`, which is made when needed; with an empty one the state is
    // kept in memory only. `logger` is told of files that cannot be read or written.
    StateStore(std::filesystem::path directory, const std::optional<std::string>& workspace_id,
               std::shared_ptr<core::Logger> logger);

    // The value of the key; none when there is no value or it is of another type.
    std::optional<bool> boolean(StateScope scope, std::string_view key) const;
    std::optional<std::int64_t> integer(StateScope scope, std::string_view key) const;
    std::optional<std::string> string(StateScope scope, std::string_view key) const;
    std::optional<std::vector<std::string>> strings(StateScope scope, std::string_view key) const;

    // A value that the store has already is not a change: it is not written, and so does not
    // replace what another process wrote.
    void set(StateScope scope, std::string_view key, nlohmann::json value);

    // Writes the values that were set. Those of a file that cannot be written stay for the next
    // write, and a file that cannot be read is not replaced.
    std::expected<void, Error> flush();

    // Writes what is pending, then reads the state of another workspace, or of no folder. Values of
    // the workspace left that could not be written are dropped.
    void set_workspace(const std::optional<std::string>& workspace_id);

private:
    struct File {
        std::filesystem::path path;
        nlohmann::json values = nlohmann::json::object();
        std::set<std::string, std::less<>> pending;
    };

    const nlohmann::json* find(StateScope scope, std::string_view key) const;
    std::filesystem::path workspace_file(const std::optional<std::string>& workspace_id) const;
    // The object in the file: an empty one for a file that is missing or is not a JSON object,
    // and an error for one that cannot be read.
    std::expected<nlohmann::json, Error> read(const std::filesystem::path& path);
    void load(File& file);
    std::expected<void, Error> write(File& file);
    void warn(const std::string& message);

    std::filesystem::path directory_;
    std::shared_ptr<core::Logger> logger_;
    File global_;
    File workspace_;
    std::set<std::filesystem::path> reported_damaged_;
};

} // namespace anvil::workbench
