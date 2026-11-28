#include "anvil/core/editing.hpp"
#include "cursor_commands.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Toggle Line Comment, Add Line Comment, Remove Line Comment and Toggle Block Comment, ported from
// VS Code's `LineCommentCommand` and `BlockCommentCommand`. The arithmetic keeps VS Code's
// 1-based lines and columns (a column is a byte offset in the line plus one) and its JavaScript
// string search, so that the results can be compared with the ones recorded from VS Code
// (`tests/core/data/comment_cases.json`).

namespace anvil::core {
namespace {

using namespace cursor_commands;

using Signed = std::ptrdiff_t;

// --- strings, as JavaScript searches them ---

Signed index_of(const std::string& text, const std::string& token, Signed from) {
    from = std::clamp<Signed>(from, 0, static_cast<Signed>(text.size()));
    const std::size_t found = text.find(token, static_cast<std::size_t>(from));
    return found == std::string::npos ? -1 : static_cast<Signed>(found);
}

Signed last_index_of(const std::string& text, const std::string& token, Signed from) {
    from = std::max<Signed>(from, 0);
    const std::size_t found = text.rfind(token, static_cast<std::size_t>(from));
    return found == std::string::npos ? -1 : static_cast<Signed>(found);
}

// `text.substring(a, b)`: out of range ends are clamped, and ends in the wrong order are swapped.
std::string substring(const std::string& text, Signed a, Signed b) {
    const Signed size = static_cast<Signed>(text.size());
    a = std::clamp<Signed>(a, 0, size);
    b = std::clamp<Signed>(b, 0, size);
    if (a > b) {
        std::swap(a, b);
    }
    return text.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
}

char char_at(const std::string& text, Signed index) {
    return index >= 0 && static_cast<std::size_t>(index) < text.size()
               ? text[static_cast<std::size_t>(index)]
               : '\0';
}

std::optional<std::size_t> first_non_blank(const std::string& text) {
    const std::size_t at = text.find_first_not_of(" \t");
    return at == std::string::npos ? std::nullopt : std::optional<std::size_t>(at);
}

// The column of the first character that is not a space or a tab, or 0 on a blank line.
std::size_t first_non_blank_column(const std::string& text) {
    const auto at = first_non_blank(text);
    return at ? *at + 1 : 0;
}

// Whether `token` is in `text` at `offset`, ignoring the case of ASCII letters.
bool token_at(const std::string& text, const std::string& token, std::size_t offset) {
    if (offset + token.size() > text.size()) {
        return false;
    }
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (lower(text[offset + i]) != lower(token[i])) {
            return false;
        }
    }
    return true;
}

// --- block comments ---

std::vector<Op> remove_block_ops(const LineRange& range, const std::string& start_token,
                                 const std::string& end_token) {
    std::vector<Op> ops;
    const auto before = [](Pos position, std::size_t size) {
        return Pos{position.line, position.column > size ? position.column - size : 0};
    };
    if (!range.empty()) {
        ops.push_back({LineRange::of(before(range.start, start_token.size()), range.start), ""});
        ops.push_back(
            {LineRange::of(range.end, {range.end.line, range.end.column + end_token.size()}), ""});
    } else {
        ops.push_back({LineRange::of(before(range.start, start_token.size()),
                                     {range.end.line, range.end.column + end_token.size()}),
                       ""});
    }
    return ops;
}

std::vector<Op> add_block_ops(const LineRange& range, const std::string& start_token,
                              const std::string& end_token, bool insert_space) {
    std::vector<Op> ops;
    if (!range.empty()) {
        ops.push_back({{range.start, range.start}, start_token + (insert_space ? " " : "")});
        ops.push_back({{range.end, range.end}, (insert_space ? " " : "") + end_token});
    } else {
        ops.push_back({range, start_token + "  " + end_token});
    }
    return ops;
}

// The block comment that surrounds or touches the selection, as the operations that remove it.
std::optional<std::vector<Op>> attempt_remove_block(const Model& model, const Sel& selection,
                                                    std::string start_token,
                                                    std::string end_token) {
    std::size_t start_line = selection.start().line;
    std::size_t end_line = selection.end().line;

    const Signed allowed_before =
        static_cast<Signed>(end_token.size()) +
        static_cast<Signed>(
            std::max(first_non_blank_column(model.line(start_line)), selection.start().column));
    Signed start_index = last_index_of(model.line(start_line), start_token, allowed_before - 1);
    Signed end_index = index_of(model.line(end_line), end_token,
                                static_cast<Signed>(selection.end().column) - 1 -
                                    static_cast<Signed>(start_token.size()));

    if (start_index != -1 && end_index == -1) {
        end_index = index_of(model.line(start_line), end_token,
                             start_index + static_cast<Signed>(start_token.size()));
        end_line = start_line;
    }
    if (start_index == -1 && end_index != -1) {
        start_index = last_index_of(model.line(end_line), start_token, end_index);
        start_line = end_line;
    }
    if (selection.empty() && (start_index == -1 || end_index == -1)) {
        start_index = index_of(model.line(start_line), start_token, 0);
        if (start_index != -1) {
            end_index = index_of(model.line(start_line), end_token,
                                 start_index + static_cast<Signed>(start_token.size()));
        }
    }

    // Spaces inside the tokens are part of them.
    if (start_index != -1 &&
        char_at(model.line(start_line), start_index + static_cast<Signed>(start_token.size())) ==
            ' ') {
        start_token += ' ';
    }
    if (end_index != -1 && char_at(model.line(end_line), end_index - 1) == ' ') {
        end_token = ' ' + end_token;
        end_index -= 1;
    }

    if (start_index != -1 && end_index != -1) {
        return remove_block_ops(LineRange::of({start_line, static_cast<std::size_t>(start_index) +
                                                               start_token.size() + 1},
                                              {end_line, static_cast<std::size_t>(end_index) + 1}),
                                start_token, end_token);
    }
    return std::nullopt;
}

// Toggle Block Comment on one selection.
Command block_comment(const Model& model, const Sel& selection, const CommentOptions& options) {
    Command command{.selection = selection, .inverse = true};
    if (!options.tokens.block) {
        command.inverse = false;
        command.tracked = selection;
        return command;
    }
    std::string start_token = options.tokens.block->open;
    std::string end_token = options.tokens.block->close;
    const Pos start = selection.start();
    const Pos end = selection.end();
    const std::string start_text = model.line(start.line);
    const std::string end_text = model.line(end.line);

    Signed start_index = last_index_of(start_text, start_token,
                                       static_cast<Signed>(start.column) - 1 +
                                           static_cast<Signed>(start_token.size()));
    Signed end_index =
        index_of(end_text, end_token,
                 static_cast<Signed>(end.column) - 1 - static_cast<Signed>(end_token.size()));

    if (start_index != -1 && end_index != -1) {
        const Signed after_start = start_index + static_cast<Signed>(start_token.size());
        if (start.line == end.line) {
            if (index_of(substring(start_text, after_start, end_index), end_token, 0) >= 0) {
                start_index = end_index = -1;
            }
        } else if (index_of(substring(start_text, after_start, Signed(start_text.size())),
                            end_token, 0) >= 0 ||
                   index_of(substring(end_text, 0, end_index), end_token, 0) >= 0) {
            start_index = end_index = -1;
        }
    }

    if (start_index != -1 && end_index != -1) {
        const bool space = options.insert_space;
        const std::size_t after = static_cast<std::size_t>(start_index) + start_token.size();
        if (space && after < start_text.size() && start_text[after] == ' ') {
            start_token += ' ';
        }
        if (space && end_index > 0 && end_text[static_cast<std::size_t>(end_index) - 1] == ' ') {
            end_token = ' ' + end_token;
            end_index -= 1;
        }
        command.ops =
            remove_block_ops(LineRange::of({start.line, static_cast<std::size_t>(start_index) +
                                                            start_token.size() + 1},
                                           {end.line, static_cast<std::size_t>(end_index) + 1}),
                             start_token, end_token);
    } else {
        command.ops =
            add_block_ops(LineRange::of(start, end), start_token, end_token, options.insert_space);
        if (command.ops.size() == 1) {
            command.used_end_token = true;
            command.used_end_token_size = end_token.size();
        }
    }
    return command;
}

// --- line comments ---

struct LineData {
    bool ignore = false;
    std::size_t offset = 0;
    std::size_t length = 0;
};

// Lines the comment goes on get the same column, the smallest indentation among them rounded down
// to a tab stop, so that the markers stay under one another.
void normalize_insertion_points(const Model& model, std::vector<LineData>& lines,
                                std::size_t first_line, std::size_t indent_size) {
    indent_size = std::max<std::size_t>(indent_size, 1);
    const auto next_visible = [&](std::size_t visible, bool tab) {
        return tab ? visible + (indent_size - visible % indent_size) : visible + 1;
    };
    constexpr std::size_t unbounded = std::size_t{1} << 30;
    std::size_t minimum = unbounded;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].ignore) {
            continue;
        }
        const std::string content = model.line(first_line + i);
        std::size_t visible = 0;
        for (std::size_t j = 0; visible < minimum && j < lines[i].offset; ++j) {
            visible = next_visible(visible, content[j] == '\t');
        }
        minimum = std::min(minimum, visible);
    }
    minimum = minimum / indent_size * indent_size;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].ignore) {
            continue;
        }
        const std::string content = model.line(first_line + i);
        std::size_t visible = 0;
        std::size_t j = 0;
        for (; visible < minimum && j < lines[i].offset; ++j) {
            visible = next_visible(visible, content[j] == '\t');
        }
        // The minimum is a multiple of the tab size, which a tab never steps over.
        lines[i].offset = j;
    }
}

// Comments the selection with block tokens, for a language that has no line token, or removes
// the block comment that is there.
void line_command_as_block(const Model& model, Command& command, const Sel& s,
                           const CommentOptions& options) {
    if (!options.tokens.block) {
        return;
    }
    const std::string& start_token = options.tokens.block->open;
    const std::string& end_token = options.tokens.block->close;
    std::optional<std::vector<Op>> ops = attempt_remove_block(model, s, start_token, end_token);
    if (!ops) {
        if (s.empty()) {
            const std::string content = model.line(s.start().line);
            const std::size_t first = first_non_blank(content).value_or(content.size());
            ops = add_block_ops(
                LineRange::of({s.start().line, first + 1}, {s.start().line, content.size() + 1}),
                start_token, end_token, options.insert_space);
        } else {
            ops = add_block_ops(
                // Column 0, on a blank line, is kept: it counts in deciding which cursors conflict,
                // and is the first column when the edit is made.
                LineRange::of({s.start().line, first_non_blank_column(model.line(s.start().line))},
                              {s.end().line, model.max_column(s.end().line)}),
                start_token, end_token, options.insert_space);
        }
        if (ops->size() == 1) {
            command.delta_column = start_token.size() + 1;
        }
    }
    command.ops = std::move(*ops);
}

Command line_comment(const Model& model, const Sel& selection, LineCommentType type,
                     const CommentOptions& options, bool ignore_first_line) {
    Command command{.selection = selection};
    Sel s = selection;

    if (s.start().line == s.end().line && ignore_first_line) {
        // VS Code replaces the line break after the line with itself. It changes nothing, but the
        // range reaches into the next line, where it conflicts with the edits of other cursors.
        const std::size_t line = s.start().line;
        command.ops.push_back({{{line, model.max_column(line)}, {line + 1, 1}}, "", false});
        command.tracked = s;
        return command;
    }
    if (s.start().line < s.end().line && s.end().column == 1) {
        command.move_end_down = true;
        s = s.with_end({s.end().line - 1, model.max_column(s.end().line - 1)});
    }
    command.tracked = s;

    if (!options.tokens.line) {
        line_command_as_block(model, command, s, options);
        return command;
    }

    const std::string& comment = *options.tokens.line;
    const bool no_indent = options.tokens.line_no_indent;
    const std::size_t first_line = s.start().line;
    std::vector<LineData> lines(s.end().line - first_line + 1);
    for (LineData& line : lines) {
        line.length = comment.size();
    }

    bool remove = type != LineCommentType::add;
    bool only_blank = true;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        LineData& data = lines[i];
        if (i == 0 && ignore_first_line) {
            data.ignore = true;
            continue;
        }
        const std::string content = model.line(first_line + i);
        const auto indent = first_non_blank(content);
        if (!indent) {
            data.ignore = options.ignore_empty_lines;
            data.offset = no_indent ? 0 : content.size();
            continue;
        }
        only_blank = false;
        data.ignore = false;
        data.offset = no_indent ? 0 : *indent;
        if (remove && !token_at(content, comment, data.offset)) {
            if (type == LineCommentType::toggle) {
                remove = false;
            } else if (type == LineCommentType::remove) {
                data.ignore = true;
            }
        }
        if (remove && options.insert_space) {
            // The space is looked for after the indentation, also where the token is not.
            const std::size_t end = *indent + data.length;
            if (end < content.size() && content[end] == ' ') {
                data.length += 1;
            }
        }
    }
    if (type == LineCommentType::toggle && only_blank) {
        remove = false;
        for (LineData& data : lines) {
            data.ignore = false;
        }
    }

    if (remove) {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!lines[i].ignore) {
                command.ops.push_back({{{first_line + i, lines[i].offset + 1},
                                        {first_line + i, lines[i].offset + lines[i].length + 1}},
                                       ""});
            }
        }
    } else {
        normalize_insertion_points(model, lines, first_line, options.indent_size);
        const std::string inserted = comment + (options.insert_space ? " " : "");
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!lines[i].ignore) {
                const Pos at{first_line + i, lines[i].offset + 1};
                command.ops.push_back({{at, at}, inserted});
            }
        }
    }

    // A caret at the end of a line that gets a comment there goes after it.
    const Pos cursor = s.active;
    for (const Op& op : command.ops) {
        if (op.range.empty() && op.range.start == cursor &&
            model.line(cursor.line).size() + 1 == cursor.column) {
            command.delta_column = op.text.size();
        }
    }
    return command;
}

} // namespace

EditOutcome EditController::comment_lines(LineCommentType type, const CommentOptions& options) {
    const Model model(document_.text());
    const std::vector<Selection>& selections = document_.selections().all();
    const std::size_t primary = document_.selections().primary_index();

    const std::vector<Sel> cursors = cursors_of(model, document_.selections());
    std::vector<std::size_t> rank(selections.size());
    for (std::size_t i = 0; i < selections.size(); ++i) {
        rank[i] = i == primary ? 0 : (i < primary ? i + 1 : i);
    }

    // Selections that share a line comment it once, for the cursor that comes first. VS Code
    // compares each selection with the first one, or with the last one that won a line.
    std::vector<bool> ignore_first_line(selections.size(), false);
    std::size_t previous = 0;
    for (std::size_t i = 1; i < selections.size(); ++i) {
        if (model.selection(selections[previous]).end().line ==
            model.selection(selections[i]).start().line) {
            if (rank[previous] < rank[i]) {
                ignore_first_line[i] = true;
            } else {
                ignore_first_line[previous] = true;
                previous = i;
            }
        }
    }

    // The commands are in the order of the text, which pairs each with the cursor of the same
    // number, and not always with its own.
    std::vector<Command> commands;
    for (std::size_t i = 0; i < selections.size(); ++i) {
        commands.push_back(line_comment(model, model.selection(selections[i]), type, options,
                                        ignore_first_line[i]));
    }
    return run_commands(model, commands, cursors);
}

EditOutcome EditController::toggle_block_comment(const CommentOptions& options) {
    const Model model(document_.text());
    const std::vector<Sel> cursors = cursors_of(model, document_.selections());
    std::vector<Command> commands;
    for (const Sel& cursor : cursors) {
        commands.push_back(block_comment(model, cursor, options));
    }
    return run_commands(model, commands, cursors);
}

} // namespace anvil::core
