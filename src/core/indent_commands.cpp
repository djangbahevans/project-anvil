// Tab, Indent Line and Outdent Line, ported from VS Code's `TabOperation` and `ShiftCommand` for a
// language without indentation rules of its own, so that the results can be compared with the ones
// recorded from VS Code (`tests/core/data/edit_cases.json`).

#include "anvil/core/editing.hpp"
#include "anvil/core/visible_column.hpp"
#include "cursor_commands.hpp"

namespace anvil::core {
namespace {

using namespace cursor_commands;

struct Indentation {
    bool insert_spaces = true;
    std::size_t tab_size = 4;

    // The white space that reaches `column`, a multiple of the tab size.
    std::string to(std::size_t column) const {
        return insert_spaces ? std::string(column, ' ') : std::string(column / tab_size, '\t');
    }
};

bool is_blank(const std::string& line) {
    return line.find_first_not_of(" \t\v\f\r") == std::string::npos;
}

// Moves the leading white space of the lines of the selection to the next or the previous tab stop.
Command shift(const Model& model, const Sel& selection, bool outdent, Indentation indentation) {
    const std::size_t first = selection.start().line;
    std::size_t last = selection.end().line;
    if (selection.end().column == 1 && first != last) {
        --last;
    }
    Command command{.selection = selection, .tracked = selection};
    for (std::size_t number = first; number <= last; ++number) {
        const std::string line = model.line(number);
        // An empty line is indented only when it is the one line asked for.
        if (!outdent && line.empty() && first != last) {
            continue;
        }
        const std::size_t text_at = line.find_first_not_of(" \t");
        const std::size_t width = text_at == std::string::npos ? line.size() : text_at;
        if (outdent && width == 0) {
            continue;
        }
        const std::size_t column = visible_column(line, width, indentation.tab_size);
        const std::size_t past = column % indentation.tab_size;
        const std::size_t target = !outdent ? column + indentation.tab_size - past
                                   : past   ? column - past
                                            : column - std::min(column, indentation.tab_size);
        command.ops.push_back(
            {LineRange{{number, 1}, {number, width + 1}}, indentation.to(target)});
        // A selection that begins in the indentation keeps beginning where it did.
        if (number == first && !selection.empty() && selection.start().column <= width + 1) {
            command.keep_start_column = true;
        }
    }
    // On a blank line the caret goes after the indentation.
    command.inverse = selection.empty() && is_blank(model.line(first)) && command.ops.size() == 1;
    return command;
}

Command replace(const Sel& selection, LineRange range, std::string text) {
    return {.selection = selection, .ops = {{range, std::move(text)}}, .inverse = true};
}

// What Tab does with one cursor.
Command tab_command(const Model& model, const Sel& selection, Indentation indentation) {
    const std::size_t number = selection.start().line;
    const std::string line = model.line(number);
    if (selection.empty() && is_blank(line)) {
        const std::string level = indentation.to(indentation.tab_size);
        if (!line.starts_with(level)) {
            return replace(selection, {{number, 1}, {number, model.max_column(number)}}, level);
        }
    }
    const bool whole_line = selection.start().column == 1 &&
                            selection.end().column == model.max_column(selection.end().line);
    if (!selection.empty() && (number != selection.end().line || whole_line)) {
        return shift(model, selection, false, indentation);
    }
    const std::size_t column =
        visible_column(line, selection.start().column - 1, indentation.tab_size);
    return replace(selection, {selection.start(), selection.end()},
                   indentation.insert_spaces
                       ? std::string(indentation.tab_size - column % indentation.tab_size, ' ')
                       : "\t");
}

} // namespace

EditOutcome EditController::tab(bool insert_spaces) {
    const Model model(document_.text());
    const std::vector<Sel> cursors = cursors_of(model, document_.selections());
    std::vector<Command> commands;
    for (const Sel& cursor : cursors) {
        commands.push_back(tab_command(model, cursor, {insert_spaces, tab_size_}));
    }
    return run_commands(model, commands, cursors);
}

EditOutcome EditController::shift_lines(bool outdent, bool insert_spaces) {
    const Model model(document_.text());
    const std::vector<Sel> cursors = cursors_of(model, document_.selections());
    std::vector<Command> commands;
    for (const Sel& cursor : cursors) {
        commands.push_back(shift(model, cursor, outdent, {insert_spaces, tab_size_}));
    }
    return run_commands(model, commands, cursors);
}

EditOutcome EditController::indent_lines(bool insert_spaces) {
    return shift_lines(false, insert_spaces);
}

EditOutcome EditController::outdent_lines(bool insert_spaces) {
    return shift_lines(true, insert_spaces);
}

} // namespace anvil::core
