#include "anvil/exthost/init_data.hpp"

#include "anvil/core/uri.hpp"
#include "anvil/platform/process.hpp"
#include "anvil/platform/target.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace anvil::exthost {
namespace {

using nlohmann::json;

json file_uri(const std::filesystem::path& path) {
    return {{"$mid", 1}, {"scheme", "file"}, {"path", path.generic_string()}};
}

json uri_components(const core::Uri& uri) {
    return {{"$mid", 1}, {"scheme", uri.scheme}, {"path", uri.path}};
}

// The extension host compares identifiers by the precomputed lowercase `_lower` field.
json extension_identifier(const std::string& id) {
    std::string lower = id;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return {{"value", id}, {"_lower", lower}};
}

Error manifest_error(const std::filesystem::path& dir, const std::string& what) {
    return Error{ErrorCode::io_error, "extension at " + dir.string() + ": " + what};
}

// The string member `key` of `object`; empty without one.
std::string string_member(const json& object, const char* key) {
    if (!object.is_object()) {
        return {};
    }
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// VS Code activates an extension for some of what it contributes, whether or not its manifest
// lists the event: `onCommand:<id>` for every command, and `onLanguage:<id>` for every language
// that comes with a language configuration. The extension host only knows the events it is given.
void add_implicit_activation_events(const json& manifest, json& events) {
    if (!events.is_array()) {
        return;
    }
    const auto contributes = manifest.find("contributes");
    if (contributes == manifest.end() || !contributes->is_object()) {
        return;
    }
    const auto add = [&events](const json& event) {
        if (std::ranges::find(events, event) == events.end()) {
            events.push_back(event);
        }
    };
    if (const auto commands = contributes->find("commands");
        commands != contributes->end() && commands->is_array()) {
        for (const json& command : *commands) {
            if (const std::string id = string_member(command, "command"); !id.empty()) {
                add("onCommand:" + id);
            }
        }
    }
    if (const auto languages = contributes->find("languages");
        languages != contributes->end() && languages->is_array()) {
        for (const json& language : *languages) {
            const std::string id = string_member(language, "id");
            if (!id.empty() && !string_member(language, "configuration").empty()) {
                add("onLanguage:" + id);
            }
        }
    }
}

struct LoadedExtension {
    std::string id;
    json description;
    json activation_events;
};

std::expected<LoadedExtension, Error> load_extension(const std::filesystem::path& dir) {
    std::ifstream file(dir / "package.json");
    if (!file) {
        return std::unexpected(manifest_error(dir, "package.json is missing"));
    }
    json manifest = json::parse(file, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("name") || !manifest.contains("publisher")) {
        return std::unexpected(manifest_error(dir, "package.json needs a publisher and a name"));
    }

    LoadedExtension extension;
    extension.id =
        manifest["publisher"].get<std::string>() + "." + manifest["name"].get<std::string>();
    extension.activation_events = manifest.value("activationEvents", json::array());
    add_implicit_activation_events(manifest, extension.activation_events);
    extension.description = std::move(manifest);
    extension.description["identifier"] = extension_identifier(extension.id);
    extension.description["extensionLocation"] = file_uri(dir);
    extension.description["isBuiltin"] = false;
    extension.description["isUserBuiltin"] = false;
    extension.description["isUnderDevelopment"] = false;
    extension.description["targetPlatform"] = platform::vscode_target_platform();
    return extension;
}

} // namespace

std::string workspace_initialization_args(const std::optional<core::Workspace>& workspace) {
    if (!workspace) {
        return "[null,true]";
    }
    const json data = {
        {"id", workspace->id},
        {"name", workspace->name},
        {"folders", json::array({{{"uri", uri_components(core::Uri::file(workspace->folder))},
                                  {"name", workspace->name},
                                  {"index", 0}}})},
        {"transient", false},
        {"isUntitled", false},
    };
    return json::array({data, true}).dump();
}

std::expected<std::string, Error> build_init_data(const InitDataOptions& options) {
    json all_extensions = json::array();
    json activation_events = json::object();
    json my_extensions = json::array();
    for (const auto& dir : options.extension_dirs) {
        auto extension = load_extension(dir);
        if (!extension) {
            return std::unexpected(extension.error());
        }
        activation_events[extension->id] = extension->activation_events;
        my_extensions.push_back(extension_identifier(extension->id));
        all_extensions.push_back(std::move(extension->description));
    }

    return json{
        {"version", options.api_version},
        // Lets the extension host exit when the editor dies.
        {"parentPid", platform::current_process_id()},
        {"environment",
         {{"isExtensionDevelopmentDebug", false},
          {"appName", options.app_name},
          {"appHost", "desktop"},
          {"appRoot", file_uri(options.app_root)},
          {"appLanguage", options.language},
          {"isExtensionTelemetryLoggingOnly", true},
          {"appUriScheme", options.uri_scheme},
          {"globalStorageHome", file_uri(options.storage_dir / "globalStorage")},
          {"workspaceStorageHome", file_uri(options.storage_dir / "workspaceStorage")}}},
        {"workspace", options.workspace ? json{{"id", options.workspace->id},
                                               {"name", options.workspace->name},
                                               {"transient", false},
                                               {"isUntitled", false}}
                                        : json(nullptr)},
        {"extensions",
         {{"versionId", 1},
          {"allExtensions", std::move(all_extensions)},
          {"activationEvents", std::move(activation_events)},
          {"myExtensions", std::move(my_extensions)}}},
        {"telemetryInfo",
         {{"sessionId", "anvil"},
          {"machineId", "anvil"},
          {"sqmId", ""},
          {"devDeviceId", ""},
          {"firstSessionDate", "2026-01-01T00:00:00.000Z"}}},
        {"logLevel", options.log_level},
        {"loggers", json::array()},
        {"logsLocation",
         file_uri(options.logs_dir.empty() ? options.storage_dir / "logs" : options.logs_dir)},
        {"autoStart", true},
        {"remote", {{"isRemote", false}, {"connectionData", nullptr}}},
        {"consoleForward", {{"includeStack", false}, {"logNative", false}}},
        {"uiKind", 1},
    }
        .dump();
}

} // namespace anvil::exthost
