#pragma once

#include "anvil/core/bytes.hpp"
#include "anvil/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace anvil::exthost {

enum class FrameType : std::uint8_t {
    regular = 1,
    control = 2,
    ack = 3,
    disconnect = 5,
    replay_request = 6,
    pause = 7,
    resume = 8,
    keep_alive = 9,
};

// One unit of the extension host socket protocol: a 13-byte big-endian header (type, id, ack,
// payload length) followed by the payload. Only `regular` frames carry an id and are counted.
struct Frame {
    FrameType type;
    std::uint32_t id = 0;
    std::uint32_t ack = 0;
    Bytes payload;
};

inline constexpr std::size_t frame_header_length = 13;

Bytes encode_frame(const Frame& frame);

// Incremental frame parser: feed it whatever the socket delivers, then drain frames with next().
// A malformed header makes the stream unrecoverable; every later call to next() returns the
// same error.
class FrameDecoder {
public:
    static constexpr std::size_t default_max_payload = std::size_t{256} * 1024 * 1024;

    explicit FrameDecoder(std::size_t max_payload = default_max_payload)
        : max_payload_(max_payload) {}

    void feed(std::span<const std::byte> data);

    // A frame when one is complete, std::nullopt when more bytes are needed.
    std::expected<std::optional<Frame>, Error> next();

private:
    std::size_t buffered() const {
        return buffer_.size() - read_pos_;
    }

    std::size_t max_payload_;
    Bytes buffer_;
    std::size_t read_pos_ = 0;
    std::optional<Error> failure_;
};

} // namespace anvil::exthost
