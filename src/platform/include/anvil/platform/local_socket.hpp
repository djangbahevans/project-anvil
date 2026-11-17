#pragma once

#include "anvil/core/error.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace anvil::platform {

struct ReadResult {
    std::size_t bytes = 0;
    // True once the peer has closed its end and all data has been read.
    bool closed = false;
};

// A connected stream socket on a filesystem path (Unix domain socket).
class LocalConnection {
public:
    LocalConnection(LocalConnection&&) noexcept;
    LocalConnection& operator=(LocalConnection&&) noexcept;
    ~LocalConnection();

    // Waits up to `timeout` for data. A result with zero bytes and `closed` false is a timeout.
    std::expected<ReadResult, Error> read(std::span<std::byte> buffer,
                                          std::chrono::milliseconds timeout);

    std::expected<void, Error> write_all(std::span<const std::byte> data);

private:
    friend class LocalListener;
    struct Impl;
    explicit LocalConnection(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Listens on a filesystem path and removes the socket file when destroyed.
class LocalListener {
public:
    static std::expected<LocalListener, Error> listen(const std::filesystem::path& path);

    LocalListener(LocalListener&&) noexcept;
    LocalListener& operator=(LocalListener&&) noexcept;
    ~LocalListener();

    // Waits up to `timeout` for one connection; std::nullopt on timeout.
    std::expected<std::optional<LocalConnection>, Error> accept(std::chrono::milliseconds timeout);

private:
    struct Impl;
    explicit LocalListener(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace anvil::platform
