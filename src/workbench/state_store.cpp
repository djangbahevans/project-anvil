#include "anvil/workbench/state_store.hpp"

#include "anvil/platform/file.hpp"

#include <limits>
#include <system_error>
#include <utility>

namespace anvil::workbench {

StateStore::StateStore(std::filesystem::path directory,
                       const std::optional<std::string>& workspace_id,
                       std::shared_ptr<core::Logger> logger)
    : directory_(std::move(directory)), logger_(std::move(logger)) {
    if (!directory_.empty()) {
        global_.path = directory_ / "global.json";
    }
    workspace_.path = workspace_file(workspace_id);
    load(global_);
    load(workspace_);
}

std::filesystem::path
StateStore::workspace_file(const std::optional<std::string>& workspace_id) const {
    if (directory_.empty()) {
        return {};
    }
    return workspace_id ? directory_ / "workspaces" / (*workspace_id + ".json")
                        : directory_ / "no-folder.json";
}

const nlohmann::json* StateStore::find(StateScope scope, std::string_view key) const {
    const nlohmann::json& values = scope == StateScope::global ? global_.values : workspace_.values;
    const auto found = values.find(key);
    return found == values.end() ? nullptr : &*found;
}

std::optional<bool> StateStore::boolean(StateScope scope, std::string_view key) const {
    const nlohmann::json* value = find(scope, key);
    if (!value || !value->is_boolean()) {
        return std::nullopt;
    }
    return value->get<bool>();
}

std::optional<std::int64_t> StateStore::integer(StateScope scope, std::string_view key) const {
    const nlohmann::json* value = find(scope, key);
    if (!value || !value->is_number_integer() ||
        (value->is_number_unsigned() &&
         value->get<std::uint64_t>() >
             static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
        return std::nullopt;
    }
    return value->get<std::int64_t>();
}

std::optional<std::string> StateStore::string(StateScope scope, std::string_view key) const {
    const nlohmann::json* value = find(scope, key);
    if (!value || !value->is_string()) {
        return std::nullopt;
    }
    return value->get<std::string>();
}

void StateStore::set(StateScope scope, std::string_view key, nlohmann::json value) {
    File& file = scope == StateScope::global ? global_ : workspace_;
    const auto found = file.values.find(key);
    if (found != file.values.end() && *found == value) {
        return;
    }
    file.values[std::string(key)] = std::move(value);
    file.pending.emplace(key);
}

std::expected<nlohmann::json, Error> StateStore::read(const std::filesystem::path& path) {
    std::error_code error;
    if (path.empty() ||
        std::filesystem::status(path, error).type() == std::filesystem::file_type::not_found) {
        return nlohmann::json::object();
    }
    const auto contents = platform::read_file(path);
    if (!contents) {
        return std::unexpected(contents.error());
    }
    nlohmann::json parsed = nlohmann::json::parse(*contents, nullptr, false);
    if (!parsed.is_object()) {
        if (reported_damaged_.insert(path).second) {
            warn(path.string() + " is not a JSON object and is read as empty");
        }
        return nlohmann::json::object();
    }
    return parsed;
}

void StateStore::load(File& file) {
    auto values = read(file.path);
    if (values) {
        file.values = std::move(*values);
    } else {
        warn(values.error().message);
        file.values = nlohmann::json::object();
    }
    file.pending.clear();
}

std::expected<void, Error> StateStore::write(File& file) {
    if (file.path.empty() || file.pending.empty()) {
        return {};
    }
    auto current = read(file.path);
    if (!current) {
        return std::unexpected(current.error());
    }
    for (const std::string& key : file.pending) {
        (*current)[key] = file.values.at(key);
    }
    std::error_code error;
    std::filesystem::create_directories(file.path.parent_path(), error);
    if (error) {
        return std::unexpected(
            Error{ErrorCode::io_error,
                  "cannot make " + file.path.parent_path().string() + ": " + error.message()});
    }
    const std::string text =
        current->dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    if (auto written = platform::write_file(file.path, text); !written) {
        return written;
    }
    file.pending.clear();
    return {};
}

std::expected<void, Error> StateStore::flush() {
    std::expected<void, Error> result;
    for (File* file : {&global_, &workspace_}) {
        if (auto written = write(*file); !written) {
            warn(written.error().message);
            result = std::move(written);
        }
    }
    return result;
}

void StateStore::set_workspace(const std::optional<std::string>& workspace_id) {
    // A failure has been logged, and the values cannot wait: they belong to another file.
    static_cast<void>(flush());
    workspace_.path = workspace_file(workspace_id);
    load(workspace_);
}

void StateStore::warn(const std::string& message) {
    if (logger_) {
        logger_->log(core::LogLevel::warning, "state", message);
    }
}

} // namespace anvil::workbench
