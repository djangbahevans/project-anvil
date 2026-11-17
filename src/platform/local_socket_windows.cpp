#include "anvil/platform/local_socket.hpp"

// Local sockets are not implemented on Windows yet; every operation reports `unsupported`.
namespace anvil::platform {

namespace {
Error unsupported() {
    return Error{ErrorCode::unsupported, "local sockets are not implemented on Windows"};
}
} // namespace

struct LocalConnection::Impl {};
struct LocalListener::Impl {};

LocalConnection::LocalConnection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LocalConnection::LocalConnection(LocalConnection&&) noexcept = default;
LocalConnection& LocalConnection::operator=(LocalConnection&&) noexcept = default;
LocalConnection::~LocalConnection() = default;

std::expected<ReadResult, Error> LocalConnection::read(std::span<std::byte>,
                                                       std::chrono::milliseconds) {
    return std::unexpected(unsupported());
}

std::expected<void, Error> LocalConnection::write_all(std::span<const std::byte>) {
    return std::unexpected(unsupported());
}

LocalListener::LocalListener(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LocalListener::LocalListener(LocalListener&&) noexcept = default;
LocalListener& LocalListener::operator=(LocalListener&&) noexcept = default;
LocalListener::~LocalListener() = default;

std::expected<LocalListener, Error> LocalListener::listen(const std::filesystem::path&) {
    return std::unexpected(unsupported());
}

std::expected<std::optional<LocalConnection>, Error>
LocalListener::accept(std::chrono::milliseconds) {
    return std::unexpected(unsupported());
}

} // namespace anvil::platform
