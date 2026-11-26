#pragma once

#include "anvil/core/row_map.hpp"
#include "anvil/core/text.hpp"

#include <cstddef>
#include <optional>

// Caret motion over the rows of wrapped text, as VS Code moves over its view lines.
namespace anvil::core {

struct RowCaret {
    std::size_t offset = 0;
    // Where a line breaks, an offset is both the end of one row and the start of the next; this
    // says that the caret is at the end of the earlier one.
    bool ends_row = false;
};

// The visible column of the caret on its row, counted as if the indent of the row were spaces.
std::size_t row_visible_column(const Text& text, const RowMap& rows, RowCaret caret,
                               std::size_t tab_size);

// Where Up (-1) or Down (1) takes the caret: the visible column `goal`, or the one it has, on the
// row above or below, and past the first or last row the start or end of the text.
RowCaret row_vertical(const Text& text, const RowMap& rows, RowCaret caret, int direction,
                      std::optional<std::size_t> goal, std::size_t tab_size);

// Where Home takes a caret on a row after the first of its line: to the first character of the
// row that is not a space or a tab. Nothing when Home goes by the line: from that character, and
// from the first row.
std::optional<RowCaret> row_home(const Text& text, const RowMap& rows, RowCaret caret);

// Where End takes a caret: to the end of its row. Nothing when End goes by the line: from there,
// and from the last row of the line.
std::optional<RowCaret> row_end(const Text& text, const RowMap& rows, RowCaret caret);

} // namespace anvil::core
