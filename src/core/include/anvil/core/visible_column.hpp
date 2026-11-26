#pragma once

#include <cstddef>
#include <string_view>

namespace anvil::core {

// The visible column is VS Code's rough measure of how far along a line something is shown
// (`CursorColumns`): a tab goes to the next tab stop, and every other grapheme cluster counts one
// column, or two when its first code point is a full-width character or an emoji. `line` is valid
// UTF-8 without line breaks and `tab_size` is at least 1.

// The visible column of byte `offset`, a character boundary of `line`.
std::size_t visible_column(std::string_view line, std::size_t offset, std::size_t tab_size);

// The byte offset in `line` whose visible column is nearest to `column`, the earlier one of two
// that are as near; the end of the line for a column past it.
std::size_t offset_at_visible_column(std::string_view line, std::size_t column,
                                     std::size_t tab_size);

} // namespace anvil::core
