#include "anvil/platform/local_socket.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace anvil::platform {
namespace {

#ifdef __APPLE__
constexpr int send_flags = 0;
#else
constexpr int send_flags = MSG_NOSIGNAL;
#endif

Error errno_error(const std::string& what) {
    return Error{ErrorCode::io_error, what + ": " + std::strerror(errno)};
}

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() {
        reset();
    }

    int get() const {
        return fd_;
    }
    explicit operator bool() const {
        return fd_ >= 0;
    }

private:
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

// Descriptors must not leak into spawned children, which would keep the connection open.
std::expected<void, Error> configure_stream_socket(int fd) {
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        return std::unexpected(errno_error("fcntl(FD_CLOEXEC)"));
    }
#ifdef __APPLE__
    const int enable = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) < 0) {
        return std::unexpected(errno_error("setsockopt(SO_NOSIGPIPE)"));
    }
#endif
    return {};
}

// Returns true when `fd` is readable, false on timeout.
std::expected<bool, Error> wait_readable(int fd, std::chrono::milliseconds timeout) {
    pollfd descriptor{fd, POLLIN, 0};
    const int result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    if (result < 0) {
        if (errno == EINTR) {
            return false;
        }
        return std::unexpected(errno_error("poll"));
    }
    return result > 0;
}

} // namespace

struct LocalConnection::Impl {
    Fd fd;
};

struct LocalListener::Impl {
    Fd fd;
    std::filesystem::path path;

    ~Impl() {
        if (fd && !path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};

LocalConnection::LocalConnection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LocalConnection::LocalConnection(LocalConnection&&) noexcept = default;
LocalConnection& LocalConnection::operator=(LocalConnection&&) noexcept = default;
LocalConnection::~LocalConnection() = default;

std::expected<ReadResult, Error> LocalConnection::read(std::span<std::byte> buffer,
                                                       std::chrono::milliseconds timeout) {
    const auto ready = wait_readable(impl_->fd.get(), timeout);
    if (!ready) {
        return std::unexpected(ready.error());
    }
    if (!*ready) {
        return ReadResult{};
    }
    const ssize_t count = ::recv(impl_->fd.get(), buffer.data(), buffer.size(), 0);
    if (count < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return ReadResult{};
        }
        return std::unexpected(errno_error("recv"));
    }
    return ReadResult{static_cast<std::size_t>(count), count == 0};
}

std::expected<void, Error> LocalConnection::write_all(std::span<const std::byte> data) {
    while (!data.empty()) {
        const ssize_t count = ::send(impl_->fd.get(), data.data(), data.size(), send_flags);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(errno_error("send"));
        }
        data = data.subspan(static_cast<std::size_t>(count));
    }
    return {};
}

LocalListener::LocalListener(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LocalListener::LocalListener(LocalListener&&) noexcept = default;
LocalListener& LocalListener::operator=(LocalListener&&) noexcept = default;
LocalListener::~LocalListener() = default;

std::expected<LocalListener, Error> LocalListener::listen(const std::filesystem::path& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string native = path.string();
    if (native.size() >= sizeof(address.sun_path)) {
        return std::unexpected(
            Error{ErrorCode::io_error, "socket path is longer than the " +
                                           std::to_string(sizeof(address.sun_path) - 1) +
                                           " bytes a Unix socket allows: " + native});
    }
    std::memcpy(address.sun_path, native.c_str(), native.size() + 1);

    Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd) {
        return std::unexpected(errno_error("socket"));
    }
    if (auto configured = configure_stream_socket(fd.get()); !configured) {
        return std::unexpected(configured.error());
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        return std::unexpected(errno_error("bind " + native));
    }
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    if (::listen(fd.get(), 1) < 0) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return std::unexpected(errno_error("listen"));
    }
    impl->fd = std::move(fd);
    return LocalListener(std::move(impl));
}

std::expected<std::optional<LocalConnection>, Error>
LocalListener::accept(std::chrono::milliseconds timeout) {
    const auto ready = wait_readable(impl_->fd.get(), timeout);
    if (!ready) {
        return std::unexpected(ready.error());
    }
    if (!*ready) {
        return std::nullopt;
    }
    Fd client(::accept(impl_->fd.get(), nullptr, nullptr));
    if (!client) {
        if (errno == EINTR || errno == EAGAIN) {
            return std::nullopt;
        }
        return std::unexpected(errno_error("accept"));
    }
    if (auto configured = configure_stream_socket(client.get()); !configured) {
        return std::unexpected(configured.error());
    }
    auto impl = std::make_unique<LocalConnection::Impl>();
    impl->fd = std::move(client);
    return LocalConnection(std::move(impl));
}

} // namespace anvil::platform
