#include "row_motion.hpp"

#include "anvil/core/visible_column.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace anvil::core {
namespace {

// The rows of one line, and the one among them that a caret is on.
struct Line {
    std::size_t number = 0;
    std::size_t start = 0;
    std::string content;
    LineBreaks breaks;
    std::size_t part = 0;

    // The bytes [begin(), end()) of the line that row `part` shows.
    std::size_t begin() const {
        return part == 0 ? 0 : breaks.starts[part - 1];
    }
    std::size_t end() const {
        return part < breaks.starts.size() ? breaks.starts[part] : content.size();
    }
    bool continues() const {
        return part < breaks.starts.size();
    }
    std::size_t indent() const {
        return part == 0 ? 0 : breaks.indent;
    }
    // The row as VS Code's view line has it: its indent as spaces, then its text.
    std::string shown() const {
        return std::string(indent(), ' ') + content.substr(begin(), end() - begin());
    }
};

Line line_at(const Text& text, const RowMap& rows, std::size_t number) {
    Line line;
    line.number = number;
    line.start = text.line_start(number);
    line.content = text.line(number);
    line.breaks = break_line(line.content, rows.options());
    return line;
}

Line place(const Text& text, const RowMap& rows, RowCaret caret) {
    Line line = line_at(text, rows, text.position_at(caret.offset).line);
    const std::size_t byte = caret.offset - line.start;
    line.part = static_cast<std::size_t>(std::ranges::upper_bound(line.breaks.starts, byte) -
                                         line.breaks.starts.begin());
    if (caret.ends_row && line.part > 0 && line.breaks.starts[line.part - 1] == byte) {
        --line.part;
    }
    return line;
}

std::size_t column_on(const Line& line, std::size_t offset, std::size_t tab_size) {
    return visible_column(line.shown(), line.indent() + offset - line.start - line.begin(),
                          tab_size);
}

} // namespace

std::size_t row_visible_column(const Text& text, const RowMap& rows, RowCaret caret,
                               std::size_t tab_size) {
    return column_on(place(text, rows, caret), caret.offset, tab_size);
}

RowCaret row_vertical(const Text& text, const RowMap& rows, RowCaret caret, int direction,
                      std::optional<std::size_t> goal, std::size_t tab_size) {
    Line from = place(text, rows, caret);
    const std::size_t row = rows.first_row(from.number) + from.part;
    if (direction < 0 && row == 0) {
        return {0};
    }
    if (direction > 0 && row + 1 >= rows.row_count()) {
        return {text.size()};
    }
    const std::size_t column = goal ? *goal : column_on(from, caret.offset, tab_size);
    const std::size_t target = direction < 0 ? row - 1 : row + 1;
    const std::size_t number = rows.line_of(target);
    // The next row is mostly of the same line, whose breaks cost its length to find.
    Line to = number == from.number ? std::move(from) : line_at(text, rows, number);
    to.part = target - rows.first_row(to.number);
    const std::size_t at = offset_at_visible_column(to.shown(), column, tab_size);
    // A column inside the indent is the start of the row.
    const std::size_t byte = to.begin() + std::max(at, to.indent()) - to.indent();
    return {to.start + byte, to.continues() && byte == to.end()};
}

std::optional<RowCaret> row_home(const Text& text, const RowMap& rows, RowCaret caret) {
    const Line line = place(text, rows, caret);
    if (line.part == 0) {
        return std::nullopt;
    }
    const std::size_t found = line.content.find_first_not_of(" \t", line.begin());
    const bool blank = found >= line.end();
    const std::size_t first = line.start + (blank ? line.begin() : found);
    if (!blank && caret.offset == first) {
        return std::nullopt;
    }
    return RowCaret{first};
}

std::optional<RowCaret> row_end(const Text& text, const RowMap& rows, RowCaret caret) {
    const Line line = place(text, rows, caret);
    if (!line.continues() || caret.offset == line.start + line.end()) {
        return std::nullopt;
    }
    return RowCaret{line.start + line.end(), true};
}

} // namespace anvil::core
