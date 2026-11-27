#pragma once

// Editing commands that VS Code runs one for each cursor through its cursor machinery: each command
// gives edits in lines and columns and says where its selection goes, and `finish` makes the edits
// of all of them one batch, drops the cursors whose edits overlap and merges those that meet, as VS
// Code does. Lines and columns are counted from 1, and a column is a byte offset in the line plus
// one.

#include "anvil/core/document.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace anvil::core::cursor_commands {

using Signed = std::ptrdiff_t;

struct Pos {
    std::size_t line = 1;
    std::size_t column = 1;

    friend bool operator==(const Pos&, const Pos&) = default;
    friend auto operator<=>(const Pos&, const Pos&) = default;
};

struct Sel {
    Pos anchor;
    Pos active;

    Pos start() const {
        return std::min(anchor, active);
    }
    Pos end() const {
        return std::max(anchor, active);
    }
    bool empty() const {
        return anchor == active;
    }
    bool reversed() const {
        return active < anchor;
    }
    // The selection with its end moved, in the direction it had.
    Sel with_end(Pos end) const {
        return reversed() ? Sel{end, start()} : Sel{start(), end};
    }
};

struct LineRange {
    Pos start;
    Pos end;

    // A range whose end comes first is turned around, as VS Code's is.
    static LineRange of(Pos a, Pos b) {
        return a <= b ? LineRange{a, b} : LineRange{b, a};
    }
    bool empty() const {
        return start == end;
    }
};

// An edit in lines and columns: the text replaces the range.
struct Op {
    LineRange range;
    std::string text;
    // False for an edit that changes nothing but still takes part in deciding which cursors
    // conflict.
    bool applies = true;
};

class Model {
public:
    explicit Model(const Text& text) : text_(text) {}

    std::size_t line_count() const {
        return text_.line_count();
    }
    std::string line(std::size_t number) const {
        const std::size_t begin = text_.line_start(number - 1);
        return text_.slice(begin, text_.line_end(number - 1) - begin);
    }
    std::size_t max_column(std::size_t number) const {
        return text_.line_end(number - 1) - text_.line_start(number - 1) + 1;
    }
    std::size_t line_start_offset(std::size_t number) const {
        return text_.line_start(number - 1);
    }
    std::size_t line_end_offset(std::size_t number) const {
        return text_.line_end(number - 1);
    }
    // Columns past the line are the end of it, and column 0 is column 1, as for VS Code's ranges.
    std::size_t offset(Pos position) const {
        const std::size_t line = std::clamp<std::size_t>(position.line, 1, line_count());
        const std::size_t column = std::clamp<std::size_t>(position.column, 1, max_column(line));
        return text_.line_start(line - 1) + column - 1;
    }
    Pos position(std::size_t offset) const {
        const std::size_t line = text_.position_at(offset).line;
        return {line + 1, offset - text_.line_start(line) + 1};
    }
    Sel selection(const Selection& selection) const {
        return {position(selection.anchor), position(selection.active)};
    }

private:
    const Text& text_;
};

// What a command decided for one selection.
struct Command {
    Sel selection{};
    std::vector<Op> ops{};

    // The selection is followed through the edits of all the commands, as a tracked range.
    bool inverse = false;
    Sel tracked{};
    bool move_end_down = false;
    std::size_t delta_column = 0;
    // With `inverse` the selection goes where the edits left their text instead: between the text
    // of two edits, or for one edit after what it inserted, less the end token of a block comment
    // and its space.
    std::size_t used_end_token_size = 0;
    bool used_end_token = false;
    // The start of the tracked selection goes no further right than the column it had.
    bool keep_start_column = false;
};

struct Finished {
    std::vector<Edit> edits;
    // The selections of the cursors, in the order of the commands; the first is the primary one,
    // as in VS Code.
    std::vector<Selection> selections;
    // No command has an edit: nothing changes, not even the selections.
    bool cancelled = false;
};

// The edits of all the commands, as one batch, and the selection each command leaves. Each command
// belongs to the cursor of the same number in `cursors_before`, the primary one first.
Finished finish(const Model& model, const std::vector<Command>& commands,
                const std::vector<Sel>& cursors_before);

// The selections as VS Code lists its cursors: the primary one first and the others in the order of
// the text.
std::vector<Sel> cursors_of(const Model& model, const SelectionSet& selections);

} // namespace anvil::core::cursor_commands
