#include "anvil/core/utf.hpp"

#include <format>
#include <optional>

namespace anvil::core {
namespace {

struct Decoded {
    char32_t code_point;
    std::size_t length;
};

std::optional<Decoded> decode(std::string_view s, std::size_t i) {
    const auto lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) {
        return Decoded{lead, 1};
    }

    std::size_t length = 0;
    char32_t code_point = 0;
    char32_t minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        code_point = lead & 0x1Fu;
        minimum = 0x80;
    } else if ((lead & 0xF0u) == 0xE0u) {
        length = 3;
        code_point = lead & 0x0Fu;
        minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        code_point = lead & 0x07u;
        minimum = 0x10000;
    } else {
        return std::nullopt;
    }

    if (s.size() - i < length) {
        return std::nullopt;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto continuation = static_cast<unsigned char>(s[i + k]);
        if ((continuation & 0xC0u) != 0x80u) {
            return std::nullopt;
        }
        code_point = (code_point << 6) | (continuation & 0x3Fu);
    }

    const bool surrogate = code_point >= 0xD800 && code_point <= 0xDFFF;
    if (code_point < minimum || code_point > 0x10FFFF || surrogate) {
        return std::nullopt;
    }
    return Decoded{code_point, length};
}

} // namespace

std::expected<std::size_t, Error> utf16_length(std::string_view utf8) {
    std::size_t units = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto decoded = decode(utf8, i);
        if (!decoded) {
            return std::unexpected(Error{
                ErrorCode::invalid_encoding,
                std::format("invalid UTF-8 sequence at byte {}", i),
            });
        }
        units += decoded->code_point >= 0x10000 ? 2 : 1;
        i += decoded->length;
    }
    return units;
}

} // namespace anvil::core
