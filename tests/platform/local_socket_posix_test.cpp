#include "anvil/core/bytes.hpp"
#include "anvil/platform/local_socket.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using anvil::platform::LocalListener;
using std::chrono::milliseconds;

namespace {

class LocalSocket : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "anvil-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        directory_ = pattern;
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path socket_path() const {
        return directory_ / "s.sock";
    }

    int connect_raw() const {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string path = socket_path().string();
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        EXPECT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
        return fd;
    }

    std::filesystem::path directory_;
};

} // namespace

TEST_F(LocalSocket, AcceptTimesOutWithoutAClient) {
    auto listener = LocalListener::listen(socket_path());
    ASSERT_TRUE(listener.has_value()) << listener.error().message;
    const auto accepted = listener->accept(milliseconds(20));
    ASSERT_TRUE(accepted.has_value());
    EXPECT_FALSE(accepted->has_value());
}

TEST_F(LocalSocket, ExchangesBytesInBothDirections) {
    auto listener = LocalListener::listen(socket_path());
    ASSERT_TRUE(listener.has_value());
    const int client = connect_raw();

    auto accepted = listener->accept(milliseconds(2000));
    ASSERT_TRUE(accepted.has_value() && accepted->has_value());
    auto& connection = **accepted;

    ASSERT_EQ(::send(client, "ping", 4, 0), 4);
    std::array<std::byte, 16> buffer{};
    const auto received = connection.read(buffer, milliseconds(2000));
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(anvil::as_text(std::span{buffer}.first(received->bytes)), "ping");

    ASSERT_TRUE(connection.write_all(anvil::to_bytes("pong")).has_value());
    char reply[4] = {};
    ASSERT_EQ(::recv(client, reply, sizeof(reply), MSG_WAITALL), 4);
    EXPECT_EQ(std::string(reply, 4), "pong");
    ::close(client);
}

TEST_F(LocalSocket, ReadReportsTimeoutAndThenClose) {
    auto listener = LocalListener::listen(socket_path());
    ASSERT_TRUE(listener.has_value());
    const int client = connect_raw();
    auto accepted = listener->accept(milliseconds(2000));
    ASSERT_TRUE(accepted.has_value() && accepted->has_value());
    auto& connection = **accepted;

    std::array<std::byte, 16> buffer{};
    const auto idle = connection.read(buffer, milliseconds(20));
    ASSERT_TRUE(idle.has_value());
    EXPECT_EQ(idle->bytes, 0u);
    EXPECT_FALSE(idle->closed);

    ::close(client);
    const auto closed = connection.read(buffer, milliseconds(2000));
    ASSERT_TRUE(closed.has_value());
    EXPECT_TRUE(closed->closed);
}

TEST_F(LocalSocket, ListenerRemovesItsSocketFile) {
    {
        auto listener = LocalListener::listen(socket_path());
        ASSERT_TRUE(listener.has_value());
        EXPECT_TRUE(std::filesystem::exists(socket_path()));
    }
    EXPECT_FALSE(std::filesystem::exists(socket_path()));
}

TEST_F(LocalSocket, ListenRejectsPathsTooLongForASocket) {
    const auto listener = LocalListener::listen(directory_ / std::string(200, 'x'));
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, anvil::ErrorCode::io_error);
}
