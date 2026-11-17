#include "anvil/exthost/frame.hpp"

#include "wire.hpp"

#include <format>

namespace anvil::exthost {
namespace {

bool is_valid_frame_type(std::uint8_t value) {
    switch (static_cast<FrameType>(value)) {
    case FrameType::regular:
    case FrameType::control:
    case FrameType::ack:
    case FrameType::disconnect:
    case FrameType::replay_request:
    case FrameType::pause:
    case FrameType::resume:
    case FrameType::keep_alive:
        return true;
    }
    return false;
}

Error protocol_error(std::string message) {
    return Error{ErrorCode::protocol_error, std::move(message)};
}

} // namespace

Bytes encode_frame(const Frame& frame) {
    wire::Writer writer;
    writer.u8(static_cast<std::uint8_t>(frame.type));
    writer.u32(frame.id);
    writer.u32(frame.ack);
    writer.u32(static_cast<std::uint32_t>(frame.payload.size()));
    writer.raw(frame.payload);
    return writer.take();
}

void FrameDecoder::feed(std::span<const std::byte> data) {
    // Reclaim consumed space before growing so the buffer does not retain old frames.
    if (read_pos_ > 0 && read_pos_ >= buffer_.size() / 2) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_pos_));
        read_pos_ = 0;
    }
    buffer_.insert(buffer_.end(), data.begin(), data.end());
}

std::expected<std::optional<Frame>, Error> FrameDecoder::next() {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (buffered() < frame_header_length) {
        return std::nullopt;
    }

    wire::Reader header(std::span{buffer_}.subspan(read_pos_, frame_header_length));
    const std::uint8_t type = header.u8();
    const std::uint32_t id = header.u32();
    const std::uint32_t ack = header.u32();
    const std::uint32_t length = header.u32();

    if (!is_valid_frame_type(type)) {
        failure_ = protocol_error(std::format("unknown frame type {}", type));
        return std::unexpected(*failure_);
    }
    if (length > max_payload_) {
        failure_ = protocol_error(std::format("frame payload of {} bytes exceeds the {} byte limit",
                                              length, max_payload_));
        return std::unexpected(*failure_);
    }
    if (buffered() < frame_header_length + length) {
        return std::nullopt;
    }

    const auto payload_begin =
        buffer_.begin() + static_cast<std::ptrdiff_t>(read_pos_ + frame_header_length);
    Frame frame{static_cast<FrameType>(type), id, ack,
                Bytes(payload_begin, payload_begin + static_cast<std::ptrdiff_t>(length))};
    read_pos_ += frame_header_length + length;
    return frame;
}

} // namespace anvil::exthost
