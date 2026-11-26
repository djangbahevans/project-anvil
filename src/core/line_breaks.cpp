#include "anvil/core/line_breaks.hpp"

#include "anvil/core/char_class.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace anvil::core {
namespace {

// VS Code's default `wordWrapBreakBeforeCharacters` and `wordWrapBreakAfterCharacters`.
constexpr char32_t break_before_characters[] =
    U"([{\u2018\u201C\u3008\u300A\u300C\u300E\u3010\u3014\uFF08\uFF3B\uFF5B\uFF62\u00A3"
    U"\u00A5\uFF04\uFFE1\uFFE5+\uFF0B";
constexpr char32_t break_after_characters[] =
    U" \t})]?|/&.,;\u00A2\u00B0\u2032\u2033\u2030\u2103\u3001\u3002\uFF61\uFF64\uFFE0"
    U"\uFF0C\uFF0E\uFF1A\uFF1B\uFF1F\uFF01\uFF05\u30FB\uFF65\u309D\u309E\u30FD\u30FE\u30FC"
    U"\u30A1\u30A3\u30A5\u30A7\u30A9\u30C3\u30E3\u30E5\u30E7\u30EE\u30F5\u30F6\u3041\u3043"
    U"\u3045\u3047\u3049\u3063\u3083\u3085\u3087\u308E\u3095\u3096\u31F0\u31F1\u31F2\u31F3"
    U"\u31F4\u31F5\u31F6\u31F7\u31F8\u31F9\u31FA\u31FB\u31FC\u31FD\u31FE\u31FF\u3005\u303B"
    U"\uFF67\uFF68\uFF69\uFF6A\uFF6B\uFF6C\uFF6D\uFF6E\uFF6F\uFF70\u201D\u3009\u300B\u300D"
    U"\u300F\u3011\u3015\uFF09\uFF3D\uFF5D\uFF63";

enum class BreakClass : std::uint8_t { none, before, after, ideograph };

template <std::size_t N> constexpr std::array<char32_t, N - 1> sorted(const char32_t (&text)[N]) {
    std::array<char32_t, N - 1> result{};
    std::copy_n(text, N - 1, result.begin());
    std::ranges::sort(result);
    return result;
}

constexpr auto sorted_before = sorted(break_before_characters);
constexpr auto sorted_after = sorted(break_after_characters);

constexpr BreakClass listed_class(char32_t c) {
    if (std::ranges::binary_search(sorted_after, c)) {
        return BreakClass::after;
    }
    return std::ranges::binary_search(sorted_before, c) ? BreakClass::before : BreakClass::none;
}

constexpr auto low_classes = [] {
    std::array<BreakClass, 256> classes{};
    for (char32_t c = 0; c < classes.size(); ++c) {
        classes[c] = listed_class(c);
    }
    return classes;
}();

BreakClass break_class(char32_t c) {
    if (c < low_classes.size()) {
        return low_classes[c];
    }
    // Kana and Han come first, so the small kana among the break characters count as ideographs.
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0x4E00 && c <= 0x9FFF)) {
        return BreakClass::ideograph;
    }
    // A character outside the basic plane has no class.
    return c > 0xFFFF ? BreakClass::none : listed_class(c);
}

bool can_break(BreakClass previous, char32_t c, BreakClass current) {
    using enum BreakClass;
    return c != ' ' &&
           ((previous == after && current != after) || (previous != before && current == before) ||
            (previous == ideograph && current != after) ||
            (current == ideograph && previous != before));
}

struct Character {
    char32_t code_point;
    std::size_t length;
};

Character character_at(std::string_view line, std::size_t offset) {
    const auto lead = static_cast<unsigned char>(line[offset]);
    if (lead < 0x80) {
        return {lead, 1};
    }
    return {decode_code_point(line, offset), lead >= 0xF0   ? std::size_t{4}
                                             : lead >= 0xE0 ? std::size_t{3}
                                                            : std::size_t{2}};
}

double width_of(char32_t c, double column, const WrapOptions& options) {
    if (c == '\t') {
        const auto tab = static_cast<double>(options.tab_size);
        return tab - std::fmod(column, tab);
    }
    // A character outside the basic plane takes two columns, whatever the font.
    if (c > 0xFFFF) {
        return 2;
    }
    // Control characters are drawn as symbols, which can be wide.
    return c < 32 || is_full_width(c) ? options.full_width : 1;
}

// The columns by which the continuation rows are indented: those of the leading spaces and tabs.
std::size_t continuation_indent(std::string_view line, const WrapOptions& options) {
    const std::size_t text = line.find_first_not_of(" \t");
    if (text == std::string_view::npos) {
        return 0;
    }
    std::size_t indent = 0;
    for (std::size_t i = 0; i < text; ++i) {
        indent += line[i] == '\t' ? options.tab_size - indent % options.tab_size : 1;
    }
    return static_cast<double>(indent) + options.full_width > static_cast<double>(options.column)
               ? 0
               : indent;
}

// Whether no character of the line can end up past the row, which most lines of source code
// settle without the rules. A character takes at most as many columns as it has bytes, except a
// tab and a control character.
bool surely_fits(std::string_view line, const WrapOptions& options) {
    if (line.size() > options.column || options.full_width > 3) {
        return false;
    }
    std::size_t tabs = 0;
    std::size_t controls = 0;
    for (const char c : line) {
        tabs += c == '\t' ? 1 : 0;
        controls += static_cast<unsigned char>(c) < 32 && c != '\t' ? 1 : 0;
    }
    return static_cast<double>(line.size() + tabs * (options.tab_size - 1)) +
               static_cast<double>(controls) * std::max(0.0, options.full_width - 1) <=
           static_cast<double>(options.column);
}

// Calls `emit` with the offset of each row after the first, and returns their indent. The
// arithmetic follows VS Code's in its order, so that fractional widths round the same way.
template <class Emit>
std::size_t wrap(std::string_view line, const WrapOptions& options, const Emit& emit) {
    if (line.empty() || surely_fits(line, options)) {
        return 0;
    }
    const std::size_t indent = continuation_indent(line, options);
    const double wrapped_width = static_cast<double>(options.column - indent);
    double breaking_column = static_cast<double>(options.column);

    const Character first = character_at(line, 0);
    double column = width_of(first.code_point, 0, options);
    BreakClass previous = break_class(first.code_point);
    std::size_t row_start = 0;
    // The latest place the current row can end at, or 0 when there is none.
    std::size_t candidate = 0;
    double candidate_column = 0;
    for (std::size_t at = first.length; at < line.size();) {
        const Character character = character_at(line, at);
        const BreakClass current = break_class(character.code_point);
        const double width = width_of(character.code_point, column, options);
        if (can_break(previous, character.code_point, current)) {
            candidate = at;
            candidate_column = column;
        }
        column += width;
        if (column > breaking_column) {
            if (candidate == 0 || column - candidate_column > wrapped_width) {
                candidate = at;
                candidate_column = column - width;
            }
            // A single character wider than the row stays on it.
            if (candidate > row_start) {
                emit(candidate);
                row_start = candidate;
                breaking_column = candidate_column + wrapped_width;
                candidate = 0;
            }
        }
        previous = current;
        at += character.length;
    }
    return indent;
}

} // namespace

LineBreaks break_line(std::string_view line, const WrapOptions& options) {
    LineBreaks breaks;
    const std::size_t indent =
        wrap(line, options, [&](std::size_t start) { breaks.starts.push_back(start); });
    breaks.indent = breaks.starts.empty() ? 0 : indent;
    return breaks;
}

std::size_t count_rows(std::string_view line, const WrapOptions& options) {
    std::size_t rows = 1;
    wrap(line, options, [&](std::size_t) { ++rows; });
    return rows;
}

} // namespace anvil::core
