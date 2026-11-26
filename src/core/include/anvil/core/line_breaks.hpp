#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace anvil::core {

// How lines are broken into rows when they wrap. Widths are in columns, the width of a half-width
// character.
struct WrapOptions {
    std::size_t tab_size = 4; // at least 1
    std::size_t column = 80;  // the width of a row, at least 1
    double full_width = 2;    // the width of a full-width character

    bool operator==(const WrapOptions&) const = default;
};

struct LineBreaks {
    // The byte offsets at which each row after the first begins.
    std::vector<std::size_t> starts;
    // The columns by which the rows after the first are indented; 0 when there is one row.
    std::size_t indent = 0;

    bool operator==(const LineBreaks&) const = default;
};

// The rows of a line as VS Code's monospace line breaks computer makes them, with its default break
// characters and `wrappingIndent: "same"`: a row ends before or after one of those characters, or
// next to an ideograph, and where there is no such place it ends where the row is full. The rows
// after the first are indented like the line, unless that leaves no room. `line` is valid UTF-8
// without line breaks.
LineBreaks break_line(std::string_view line, const WrapOptions& options);

// The number of rows that break_line() gives, without keeping them.
std::size_t count_rows(std::string_view line, const WrapOptions& options);

} // namespace anvil::core
