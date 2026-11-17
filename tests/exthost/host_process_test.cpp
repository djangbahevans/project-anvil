#include "anvil/exthost/host_process.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace anvil::exthost;
using nlohmann::json;
using std::chrono::milliseconds;

namespace {

json file_uri(const std::filesystem::path& path) {
    return {{"$mid", 1}, {"scheme", "file"}, {"path", path.generic_string()}};
}

json extension_identifier(const std::string& id) {
    return {{"value", id}, {"_lower", id}};
}

json read_json(const std::filesystem::path& path) {
    std::ifstream file(path);
    return json::parse(file);
}

std::string build_init_data(const std::filesystem::path& vscode_dir,
                            const std::filesystem::path& extension_dir,
                            const std::filesystem::path& scratch) {
    json manifest = read_json(extension_dir / "package.json");
    const std::string id =
        manifest["publisher"].get<std::string>() + "." + manifest["name"].get<std::string>();
    manifest["identifier"] = extension_identifier(id);
    manifest["extensionLocation"] = file_uri(extension_dir);
    manifest["isBuiltin"] = false;
    manifest["isUserBuiltin"] = false;
    manifest["isUnderDevelopment"] = false;
    manifest["targetPlatform"] = "linux-x64";

    return json{
        {"version", "1.146.0"},
        {"parentPid", 0},
        {"environment",
         {{"isExtensionDevelopmentDebug", false},
          {"appName", "Anvil"},
          {"appHost", "desktop"},
          {"appRoot", file_uri(vscode_dir)},
          {"appLanguage", "en"},
          {"isExtensionTelemetryLoggingOnly", true},
          {"appUriScheme", "anvil"},
          {"globalStorageHome", file_uri(scratch / "globalStorage")},
          {"workspaceStorageHome", file_uri(scratch / "workspaceStorage")}}},
        {"workspace", nullptr},
        {"extensions",
         {{"versionId", 1},
          {"allExtensions", json::array({manifest})},
          {"activationEvents", {{id, manifest["activationEvents"]}}},
          {"myExtensions", json::array({extension_identifier(id)})}}},
        {"telemetryInfo",
         {{"sessionId", "test"},
          {"machineId", "test"},
          {"sqmId", ""},
          {"devDeviceId", ""},
          {"firstSessionDate", "2026-01-01T00:00:00.000Z"}}},
        {"logLevel", 2},
        {"loggers", json::array()},
        {"logsLocation", file_uri(scratch / "logs")},
        {"autoStart", true},
        {"remote", {{"isRemote", false}, {"connectionData", nullptr}}},
        {"consoleForward", {{"includeStack", false}, {"logNative", false}}},
        {"uiKind", 1},
    }
        .dump();
}

std::string empty_configuration() {
    const json model = {
        {"contents", json::object()}, {"keys", json::array()}, {"overrides", json::array()}};
    return json::array({{{"defaults", model},
                         {"policy", model},
                         {"application", model},
                         {"userLocal", model},
                         {"userRemote", model},
                         {"workspace", model},
                         {"folders", json::array()},
                         {"configurationScopes", json::array()}}})
        .dump();
}

std::string args_text(const Request& request) {
    if (const auto* json_args = std::get_if<JsonArgs>(&request.args)) {
        return json_args->json;
    }
    return "<mixed>";
}

} // namespace

TEST(ExtensionHostEnvironment, DropsElectronAndVsCodeVariablesAndAddsConnectionEntries) {
    const auto environment =
        extension_host_environment({"PATH=/usr/bin", "ELECTRON_RUN_AS_NODE=1", "VSCODE_PID=42",
                                    "VSCODE_IPC_HOOK=/x", "HOME=/home/u"},
                                   "/tmp/anvil-x/host.sock");

    EXPECT_EQ(environment, (std::vector<std::string>{
                               "PATH=/usr/bin",
                               "HOME=/home/u",
                               "VSCODE_ESM_ENTRYPOINT=vs/workbench/api/node/extensionHostProcess",
                               "VSCODE_EXTHOST_IPC_HOOK=/tmp/anvil-x/host.sock",
                               "VSCODE_HANDLES_UNCAUGHT_ERRORS=true",
                           }));
}

TEST(HostProcess, StartFailsWhenTheVsCodeCheckoutIsMissing) {
    const auto host = HostProcess::start(HostOptions{"/usr/bin/env", "/nonexistent/vscode", "{}"});
    ASSERT_FALSE(host.has_value());
    EXPECT_NE(host.error().message.find("fetch-vscode.sh"), std::string::npos);
}

TEST(HostProcess, StartReportsAnExtensionHostThatExitsBeforeConnecting) {
    const auto checkout = std::filesystem::temp_directory_path() / "anvil-fake-vscode";
    std::filesystem::create_directories(checkout / "out");
    std::ofstream(checkout / "out" / "bootstrap-fork.js").put('\n');

    const auto host =
        HostProcess::start(HostOptions{"/bin/false", checkout, "{}", milliseconds(5000)});
    std::filesystem::remove_all(checkout);

    ASSERT_FALSE(host.has_value());
    EXPECT_NE(host.error().message.find("exited with code 1"), std::string::npos)
        << host.error().message;
}

// Drives the unmodified VS Code extension host from the C++ session: startup sequence, extension
// activation, and a command round trip. Needs the pinned checkout from scripts/fetch-vscode.sh.
TEST(HostProcess, RunsAnExtensionCommandThroughTheRealExtensionHost) {
    const std::filesystem::path vscode_dir = ANVIL_VSCODE_DIR;
    const std::filesystem::path node = ANVIL_NODE_EXECUTABLE;
    if (node.empty() || !std::filesystem::exists(vscode_dir / "out" / "bootstrap-fork.js")) {
        GTEST_SKIP()
            << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
    }

    const auto scratch = std::filesystem::temp_directory_path() / "anvil-host-test";
    std::filesystem::create_directories(scratch);

    auto started = HostProcess::start(HostOptions{
        node, vscode_dir, build_init_data(vscode_dir, ANVIL_FIXTURE_EXTENSION, scratch)});
    ASSERT_TRUE(started.has_value()) << started.error().message;
    auto& host = **started;
    Session& session = host.session();

    std::vector<std::string> registered_commands;
    session.set_request_handler([&](const Request& request) -> std::optional<HandlerResult> {
        const std::string service(proxy_name(request.proxy_id));
        if (service == "MainThreadWindow" && request.method == "$getInitialState") {
            return OkJson{R"({"isFocused":true,"isActive":true})"};
        }
        if (service == "MainThreadChatSkills" && request.method == "$getTools") {
            return OkJson{"[]"};
        }
        if (service == "MainThreadCommands" && request.method == "$registerCommand") {
            registered_commands.push_back(args_text(request));
        }
        return Ok{};
    });

    std::optional<CallResult> command_result;
    std::optional<std::string> startup_failure;
    auto step = [&](CallResult result, auto next) {
        if (!result) {
            startup_failure = result.error();
            return;
        }
        next();
    };

    session.set_ready_handler([&] {
        session.call(ProxyId::ExtHostConfiguration, "$initializeConfiguration",
                     empty_configuration(), [&](CallResult r) {
                         step(std::move(r), [&] {
                             session.call(
                                 ProxyId::ExtHostWorkspace, "$initializeWorkspace", "[null,true]",
                                 [&](CallResult r) {
                                     step(std::move(r), [&] {
                                         session.call(
                                             ProxyId::ExtHostExtensionService, "$activateByEvent",
                                             R"(["*",0])", [&](CallResult r) {
                                                 step(std::move(r), [&] {
                                                     session.call(ProxyId::ExtHostCommands,
                                                                  "$executeContributedCommand",
                                                                  R"(["anvil.hello","anvil"])",
                                                                  [&](CallResult r) {
                                                                      command_result = std::move(r);
                                                                  });
                                                 });
                                             });
                                     });
                                 });
                         });
                     });
    });

    const auto deadline = std::chrono::steady_clock::now() + milliseconds(30000);
    while (!command_result && !startup_failure && std::chrono::steady_clock::now() < deadline) {
        const auto pumped = host.pump(milliseconds(100));
        ASSERT_TRUE(pumped.has_value()) << pumped.error().message;
        if (!*pumped) {
            break;
        }
    }

    ASSERT_FALSE(startup_failure.has_value()) << *startup_failure;
    ASSERT_TRUE(command_result.has_value()) << "the command did not complete in time";
    ASSERT_TRUE(command_result->has_value()) << command_result->error();
    EXPECT_EQ(**command_result, R"("hello, anvil")");
    EXPECT_NE(
        std::find(registered_commands.begin(), registered_commands.end(), R"(["anvil.hello"])"),
        registered_commands.end());

    std::filesystem::remove_all(scratch);
}
