#include "anvil/exthost/session.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <iterator>

using namespace anvil::exthost;

// The recording holds the bytes a VS Code 1.146.0 extension host sent while it started, activated
// the hello fixture extension, and ran its command. Replaying it checks the protocol
// implementation against real traffic.
TEST(SessionReplay, RealExtensionHostRecordingIsAcceptedWithoutProtocolErrors) {
    std::ifstream file(ANVIL_TEST_DATA_DIR "/exthost-1.146.0-session.bin", std::ios::binary);
    ASSERT_TRUE(file.is_open());
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), {});
    ASSERT_GT(raw.size(), 1000u);

    Session session("{}");
    bool ready = false;
    session.set_ready_handler([&] { ready = true; });
    std::vector<std::string> requests;
    session.set_request_handler([&](const Request& request) -> std::optional<HandlerResult> {
        requests.push_back(std::string(proxy_name(request.proxy_id)) + "." + request.method);
        return Ok{};
    });

    // Deliver in uneven chunks, as a socket would.
    const auto bytes = std::as_bytes(std::span{raw});
    for (std::size_t offset = 0; offset < bytes.size(); offset += 777) {
        const auto result = session.receive(
            bytes.subspan(offset, std::min<std::size_t>(777, bytes.size() - offset)));
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    EXPECT_TRUE(ready);
    EXPECT_EQ(session.state(), SessionState::running);
    EXPECT_GT(requests.size(), 10u);
    EXPECT_NE(std::find(requests.begin(), requests.end(), "MainThreadCommands.$registerCommand"),
              requests.end());
    EXPECT_NE(std::find(requests.begin(), requests.end(), "MainThreadStatusBar.$setEntry"),
              requests.end());
}
