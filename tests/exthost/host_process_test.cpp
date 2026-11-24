#include "anvil/exthost/host_process.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using std::chrono::milliseconds;

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
    const auto checkout = std::filesystem::temp_directory_path() /
                          ("anvil-fake-vscode-" + std::to_string(::getpid()));
    std::filesystem::create_directories(checkout / "out");
    std::ofstream(checkout / "out" / "bootstrap-fork.js").put('\n');

    const auto host =
        HostProcess::start(HostOptions{"/bin/false", checkout, "{}", milliseconds(5000)});
    std::filesystem::remove_all(checkout);

    ASSERT_FALSE(host.has_value());
    EXPECT_NE(host.error().message.find("exited with code 1"), std::string::npos)
        << host.error().message;
}
