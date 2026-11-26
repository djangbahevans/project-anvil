#include "anvil/core/visible_column.hpp"

#include "anvil/core/char_class.hpp"
#include "grapheme_table.hpp"

#include <algorithm>
#include <iterator>

namespace anvil::core {
namespace {

// VS Code's `isFullWidthCharacter` and `isEmojiImprecise`.
bool is_two_columns(char32_t c) {
    return (c >= 0x2E80 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF01 && c <= 0xFF5E) || (c >= 0xFFE0 && c <= 0xFFE6) ||
           (c >= 0x1F1E6 && c <= 0x1F1FF) || c == 8986 || c == 8987 || c == 9200 || c == 9203 ||
           (c >= 9728 && c <= 10175) || c == 11088 || c == 11093 || (c >= 127744 && c <= 128591) ||
           (c >= 128640 && c <= 128764) || (c >= 128992 && c <= 129008) ||
           (c >= 129280 && c <= 129535) || (c >= 129648 && c <= 129782);
}

GraphemeClass grapheme_class(char32_t c) {
    if (c < 0x20) {
        return c == '\n'   ? GraphemeClass::lf
               : c == '\r' ? GraphemeClass::cr
                           : GraphemeClass::control;
    }
    if (c < 0x7F) {
        return GraphemeClass::other;
    }
    const auto after = std::ranges::upper_bound(grapheme_ranges, c, {}, &GraphemeRange::first);
    if (after == std::begin(grapheme_ranges) || std::prev(after)->last < c) {
        return GraphemeClass::other;
    }
    return std::prev(after)->type;
}

// Whether a cluster ends between code points of these classes: the rules of Unicode's annex 29 as
// VS Code applies them, which look at the two neighbors only.
bool breaks_between(GraphemeClass a, GraphemeClass b) {
    using enum GraphemeClass;
    // VS Code's shortcut, which also parts `other` from a joiner that follows it.
    if (a == other) {
        return b != extend && b != spacingmark;
    }
    if (a == cr && b == lf) {
        return false;
    }
    if (a == control || a == cr || a == lf || b == control || b == cr || b == lf) {
        return true;
    }
    if ((a == l && (b == l || b == v || b == lv || b == lvt)) ||
        ((a == lv || a == v) && (b == v || b == t)) || ((a == lvt || a == t) && b == t)) {
        return false;
    }
    if (b == extend || b == zwj || b == spacingmark || a == prepend) {
        return false;
    }
    return !((a == zwj && b == pictographic) ||
             (a == regional_indicator && b == regional_indicator));
}

std::size_t code_point_length(char lead) {
    const auto byte = static_cast<unsigned char>(lead);
    return byte < 0x80 ? 1 : byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : 2;
}

// The end of the grapheme cluster that starts at `offset`, which is before the end of `line`.
std::size_t cluster_end(std::string_view line, std::size_t offset) {
    std::size_t end = offset + code_point_length(line[offset]);
    GraphemeClass previous = grapheme_class(decode_code_point(line, offset));
    while (end < line.size()) {
        const GraphemeClass next = grapheme_class(decode_code_point(line, end));
        if (breaks_between(previous, next)) {
            break;
        }
        end += code_point_length(line[end]);
        previous = next;
    }
    return std::min(end, line.size());
}

std::size_t column_after(std::string_view line, std::size_t offset, std::size_t column,
                         std::size_t tab_size) {
    if (line[offset] == '\t') {
        return column + tab_size - column % tab_size;
    }
    return column + (is_two_columns(decode_code_point(line, offset)) ? 2 : 1);
}

} // namespace

std::size_t visible_column(std::string_view line, std::size_t offset, std::size_t tab_size) {
    std::size_t column = 0;
    for (std::size_t at = 0; at < offset; at = cluster_end(line, at)) {
        column = column_after(line, at, column, tab_size);
    }
    return column;
}

std::size_t offset_at_visible_column(std::string_view line, std::size_t column,
                                     std::size_t tab_size) {
    std::size_t before = 0;
    for (std::size_t at = 0; at < line.size() && column > 0;) {
        const std::size_t end = cluster_end(line, at);
        const std::size_t after = column_after(line, at, before, tab_size);
        if (after >= column) {
            return after - column < column - before ? end : at;
        }
        at = end;
        before = after;
    }
    return column > 0 ? line.size() : 0;
}

} // namespace anvil::core
