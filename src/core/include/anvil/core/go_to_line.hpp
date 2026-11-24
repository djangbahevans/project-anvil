#pragma once

#include "anvil/core/text.hpp"

#include <optional>
#include <string_view>

namespace anvil::core {

// What the text typed in Go to Line/Column asks for, with VS Code's rules.
struct GoToLine {
    // The text names no line yet.
    bool has_line = false;
    // The line, counting from 0, kept within the text. Negative numbers count from the end:
    // -1 is the last line.
    std::size_t line = 0;
    std::size_t line_count = 1;
    // The text has a separator (`:`, `,` or `#`) after the line.
    bool has_separator = false;
    // The text names a column after the separator.
    bool has_column = false;
    // The column the text names, counting from 1, kept within the line, and the largest column of
    // the line. A column counts the width of the text before it, with a tab as far as the next
    // tab stop, and a negative number counts from the end of the line.
    std::size_t column = 1;
    std::size_t max_column = 1;
    // Where the caret goes: the line and the character the column falls on, with the column in
    // UTF-16 code units from 0 as Text::offset_at takes it. The nearer of the two characters that
    // a column inside a tab falls between.
    Position target;
};

// Reads `input`, the text after the `:` of the quick access: a line number, optionally followed by
// `:`, `,` or `#` and a column. A number that is not one leaves the line or the column unnamed.
GoToLine parse_go_to_line(const Text& text, std::size_t tab_size, std::string_view input);

} // namespace anvil::core
