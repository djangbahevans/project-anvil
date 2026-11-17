#include "anvil/core/char_class.hpp"

namespace anvil::core {

CharClass classify(char32_t c) {
    if (c == ' ' || c == '\t' || c == '\n') {
        return CharClass::space;
    }
    if (c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z')) {
        return CharClass::word;
    }
    return CharClass::punctuation;
}

char32_t decode_code_point(std::string_view utf8, std::size_t offset) {
    const auto lead = static_cast<unsigned char>(utf8[offset]);
    if (lead < 0x80) {
        return lead;
    }
    const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    char32_t code_point = lead & (0xFFu >> (length + 1));
    for (std::size_t i = 1; i < length && offset + i < utf8.size(); ++i) {
        code_point = (code_point << 6) | (static_cast<unsigned char>(utf8[offset + i]) & 0x3Fu);
    }
    return code_point;
}

} // namespace anvil::core
