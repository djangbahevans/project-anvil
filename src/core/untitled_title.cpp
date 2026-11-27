#include "anvil/core/untitled_title.hpp"

#include "anvil/core/char_class.hpp"
#include "anvil/core/visible_column.hpp"

#include <algorithm>
#include <string_view>

namespace anvil::core {
namespace {

constexpr std::size_t candidate_units = 400;
constexpr std::size_t title_units = 40;
constexpr char32_t right_to_left_override = 0x202E;
// VS Code's `USUAL_WORD_SEPARATORS`.
constexpr std::u32string_view word_separators = U"`~!@#$%^&*()-=+[{]}\\|;:'\",.<>/?";

// What `\s` matches in JavaScript.
bool is_white_space(char32_t c) {
    return c == U' ' || (c >= U'\t' && c <= U'\r') || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
           c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

std::size_t units_of(char32_t c) {
    return c >= 0x10000 ? 2 : 1;
}

std::size_t bytes_of(std::string_view text, std::size_t at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    return lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
}

std::size_t units_in(std::string_view text) {
    std::size_t units = 0;
    for (std::size_t at = 0; at < text.size(); at += bytes_of(text, at)) {
        units += units_of(decode_code_point(text, at));
    }
    return units;
}

} // namespace

std::optional<std::string> untitled_title(const Text& text) {
    // No more of the line than the title can come from, so that a long one costs nothing.
    std::size_t read = std::min(text.line_end(0), 4 * candidate_units);
    while (!text.is_char_boundary(read)) {
        --read;
    }
    const std::string line = text.slice(0, read);

    std::string collapsed;
    bool space = false;
    bool override_removed = false;
    std::size_t units = 0;
    for (std::size_t at = 0; at < line.size() && units < candidate_units;) {
        const std::size_t length = bytes_of(line, at);
        const char32_t c = decode_code_point(line, at);
        units += units_of(c);
        if (is_white_space(c)) {
            space = !collapsed.empty() || override_removed;
        } else {
            if (space) {
                collapsed += ' ';
                space = false;
            }
            if (c == right_to_left_override) {
                override_removed = true;
            } else {
                collapsed.append(line, at, length);
            }
        }
        at += length;
    }

    std::size_t cut = 0;
    std::size_t used = 0;
    while (cut < collapsed.size()) {
        const std::size_t end = cluster_end(collapsed, cut);
        used += units_in(std::string_view(collapsed).substr(cut, end - cut));
        if (used > title_units) {
            break;
        }
        cut = end;
    }
    collapsed.resize(cut);

    for (std::size_t at = 0; at < collapsed.size(); at += bytes_of(collapsed, at)) {
        const char32_t c = decode_code_point(collapsed, at);
        if (!is_white_space(c) && word_separators.find(c) == std::u32string_view::npos) {
            return collapsed;
        }
    }
    return std::nullopt;
}

} // namespace anvil::core
