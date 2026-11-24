#include "anvil/core/go_to_line.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace anvil::core {
namespace {

// JavaScript's parseInt for decimal text: leading white space, an optional sign, the digits, and
// whatever follows them is ignored.
std::optional<long long> parse_int(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) {
        return std::nullopt;
    }
    long long value = 0;
    constexpr long long limit = 1'000'000'000'000LL;
    for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) {
        value = std::min(limit, value * 10 + (text[i] - '0'));
    }
    return negative ? -value : value;
}

std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',' || text[i] == ':' || text[i] == '#') {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

struct Character {
    // Visible columns before and after the character, and UTF-16 code units before and after.
    std::size_t visible_before;
    std::size_t visible_after;
    std::size_t units_before;
    std::size_t units_after;
};

std::vector<Character> characters(std::string_view line, std::size_t tab_size) {
    std::vector<Character> result;
    std::size_t visible = 0;
    std::size_t units = 0;
    for (std::size_t i = 0; i < line.size();) {
        const auto lead = static_cast<unsigned char>(line[i]);
        const std::size_t bytes = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        const std::size_t length = bytes == 4 ? 2 : 1;
        const std::size_t next =
            line[i] == '\t' ? visible + tab_size - visible % tab_size : visible + 1;
        result.push_back({visible, next, units, units + length});
        visible = next;
        units += length;
        i += bytes;
    }
    return result;
}

} // namespace

GoToLine parse_go_to_line(const Text& text, std::size_t tab_size, std::string_view input) {
    GoToLine result;
    result.line_count = std::max<std::size_t>(1, text.line_count());
    tab_size = std::max<std::size_t>(1, tab_size);

    const std::vector<std::string_view> parts = split(input);
    const std::optional<long long> number = parse_int(parts[0]);
    if (!number) {
        return result;
    }
    result.has_line = true;
    const auto count = static_cast<long long>(result.line_count);
    const long long wanted = *number >= 0 ? *number : count + 1 + *number;
    result.line = static_cast<std::size_t>(std::clamp(wanted, 1LL, count)) - 1;

    const std::string line = text.line(result.line);
    const std::vector<Character> chars = characters(line, tab_size);
    const std::size_t width = chars.empty() ? 0 : chars.back().visible_after;
    result.max_column = width + 1;
    result.target = {result.line, 0};

    result.has_separator = parts.size() > 1;
    if (!result.has_separator) {
        return result;
    }
    const std::optional<long long> column = parse_int(parts[1]);
    if (!column) {
        return result;
    }
    result.has_column = true;
    const auto max_column = static_cast<long long>(result.max_column);
    const long long wanted_column = *column >= 0 ? *column : max_column + *column;
    result.column = static_cast<std::size_t>(std::clamp(wanted_column, 1LL, max_column));

    const std::size_t visible = result.column - 1;
    std::size_t units = chars.empty() ? 0 : chars.back().units_after;
    if (visible == 0) {
        units = 0;
    } else {
        for (const Character& character : chars) {
            if (character.visible_after >= visible) {
                const std::size_t before = visible - character.visible_before;
                const std::size_t after = character.visible_after - visible;
                units = after < before ? character.units_after : character.units_before;
                break;
            }
        }
    }
    result.target.column = units;
    return result;
}

} // namespace anvil::core
