#include "anvil/core/whitespace.hpp"

#include <algorithm>

namespace anvil::core {
namespace {

bool is_blank_char(char16_t c) {
    return c == u' ' || c == u'\t';
}

bool selected(std::span<const ColumnRange> selections, std::size_t column) {
    return std::ranges::any_of(selections, [column](const ColumnRange& range) {
        return range.begin <= column && column < range.end;
    });
}

} // namespace

std::vector<std::size_t> whitespace_columns(std::u16string_view line, RenderWhitespace mode,
                                            std::span<const ColumnRange> selections,
                                            std::size_t from, std::size_t to) {
    std::vector<std::size_t> columns;
    if (mode == RenderWhitespace::none) {
        return columns;
    }
    to = std::min(to, line.size());
    // Only these two modes depend on where the text of the line starts and ends.
    std::size_t first = 0;
    std::size_t last = line.size();
    bool blank = false;
    if (mode == RenderWhitespace::boundary || mode == RenderWhitespace::trailing) {
        while (first < line.size() && is_blank_char(line[first])) {
            ++first;
        }
        blank = first == line.size();
        if (!blank) {
            last = line.size() - 1;
            while (is_blank_char(line[last])) {
                --last;
            }
        }
    }

    for (std::size_t column = from; column < to; ++column) {
        const char16_t c = line[column];
        if (!is_blank_char(c)) {
            continue;
        }
        bool draw = true;
        if (mode == RenderWhitespace::boundary && column > first && column < last && c == u' ') {
            const bool after_blank = is_blank_char(line[column - 1]);
            const bool before_blank = is_blank_char(line[column + 1]);
            draw = after_blank || before_blank;
        }
        if (mode == RenderWhitespace::selection) {
            draw = selected(selections, column);
        }
        if (mode == RenderWhitespace::trailing) {
            draw = blank || column > last;
        }
        if (draw) {
            columns.push_back(column);
        }
    }
    return columns;
}

std::vector<std::size_t> whitespace_columns(std::u16string_view line, RenderWhitespace mode,
                                            std::span<const ColumnRange> selections) {
    return whitespace_columns(line, mode, selections, 0, line.size());
}

} // namespace anvil::core
