#pragma once

#include "anvil/core/editor_settings.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace anvil::core {

// A range of UTF-16 columns of a line, `begin` included and `end` not.
struct ColumnRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

// The columns of `line` whose space or tab the editor draws a mark for, in ascending order, when
// `mode` is in force. The rules are VS Code's:
//
//  - `none`: no column. `all`: every space and tab.
//  - `boundary`: all leading and trailing spaces and tabs, every tab, and the spaces inside the
//    text except a single space between two characters that are not spaces or tabs.
//  - `trailing`: the spaces and tabs at the end of the line, all of the line if it is blank.
//  - `selection`: the spaces and tabs inside `selections`, which are columns of this line.
//
// Only columns in [`from`, `to`) are returned. `all` and `selection` read nothing else, so their
// cost is that of the window; `boundary` and `trailing` also read the blanks at the two ends of the
// line, which for a line that is mostly blank is the line.
std::vector<std::size_t> whitespace_columns(std::u16string_view line, RenderWhitespace mode,
                                            std::span<const ColumnRange> selections,
                                            std::size_t from, std::size_t to);

// The same for the whole line.
std::vector<std::size_t> whitespace_columns(std::u16string_view line, RenderWhitespace mode,
                                            std::span<const ColumnRange> selections = {});

} // namespace anvil::core
