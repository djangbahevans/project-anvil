#pragma once

#include "anvil/core/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace anvil::exthost::wire {

// Bounds-checked big-endian reader. After any read past the end, ok() is false and every
// further read returns zero or an empty span, so callers check ok() once after a sequence of reads.
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    std::uint8_t u8() {
        if (!require(1)) {
            return 0;
        }
        return std::to_integer<std::uint8_t>(data_[pos_++]);
    }

    std::uint32_t u32() {
        if (!require(4)) {
            return 0;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value = (value << 8) | std::to_integer<std::uint32_t>(data_[pos_++]);
        }
        return value;
    }

    std::span<const std::byte> take(std::size_t count) {
        if (!require(count)) {
            return {};
        }
        const auto result = data_.subspan(pos_, count);
        pos_ += count;
        return result;
    }

    bool ok() const {
        return ok_;
    }
    bool at_end() const {
        return pos_ == data_.size();
    }

private:
    bool require(std::size_t count) {
        if (!ok_ || data_.size() - pos_ < count) {
            ok_ = false;
        }
        return ok_;
    }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

class Writer {
public:
    void u8(std::uint8_t value) {
        out_.push_back(static_cast<std::byte>(value));
    }

    void u32(std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            out_.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
        }
    }

    void raw(std::span<const std::byte> bytes) {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
    }

    Bytes take() {
        return std::move(out_);
    }

private:
    Bytes out_;
};

} // namespace anvil::exthost::wire
