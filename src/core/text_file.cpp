#include "anvil/core/text_file.hpp"

#include <algorithm>

namespace anvil::core {
namespace {

constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";

} // namespace

std::string_view line_ending_text(LineEnding ending) {
    switch (ending) {
    case LineEnding::lf:
        return "\n";
    case LineEnding::crlf:
        return "\r\n";
    case LineEnding::cr:
        return "\r";
    }
    return "\n";
}

std::size_t line_ending_width(LineEnding ending) {
    return line_ending_text(ending).size();
}

LineEnding detect_line_ending(std::string_view raw) {
    std::size_t lf = 0;
    std::size_t crlf = 0;
    std::size_t cr = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (raw[i] == '\n') {
            ++lf;
        }
    }
    if (crlf > lf && crlf >= cr) {
        return LineEnding::crlf;
    }
    if (cr > lf && cr > crlf) {
        return LineEnding::cr;
    }
    return LineEnding::lf;
}

std::string apply_line_ending(std::string_view normalized, LineEnding ending) {
    if (ending == LineEnding::lf) {
        return std::string(normalized);
    }
    const std::string_view replacement = line_ending_text(ending);
    std::string result;
    result.reserve(normalized.size() + normalized.size() / 16);
    for (const char c : normalized) {
        if (c == '\n') {
            result += replacement;
        } else {
            result.push_back(c);
        }
    }
    return result;
}

std::expected<DecodedFile, Error> decode_file(std::string_view bytes) {
    DecodedFile file;
    if (bytes.starts_with(utf8_bom)) {
        file.has_bom = true;
        bytes.remove_prefix(utf8_bom.size());
    }
    auto text = Text::from_utf8(bytes);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    file.text = std::move(*text);
    file.line_ending = detect_line_ending(bytes);
    return file;
}

std::string encode_file(const Text& text, LineEnding ending, bool bom) {
    std::string result;
    if (bom) {
        result = utf8_bom;
    }
    result += apply_line_ending(text.to_string(), ending);
    return result;
}

} // namespace anvil::core
