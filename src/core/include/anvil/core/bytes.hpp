#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace anvil {

using Bytes = std::vector<std::byte>;

inline Bytes to_bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span{text.data(), text.size()});
    return Bytes(view.begin(), view.end());
}

inline std::string_view as_text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace anvil
