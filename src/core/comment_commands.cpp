#include "anvil/core/editing.hpp"

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

struct Range {
    Pos start;
    Pos end;

    // A range whose end comes first is turned around, as VS Code's is.
    static Range of(Pos a, Pos b) {
        return a <= b ? Range{a, b} : Range{b, a};
    }
    bool empty() const {
        return start == end;
    }
};

// An edit in lines and columns: the text replaces the range.
struct Op {
    Range range;
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
    // Block comment commands put the selection where the edits left their tokens instead. For one
    // edit, the caret goes before the end token that it inserted, and its space.
    std::size_t used_end_token_size = 0;
    bool used_end_token = false;
};

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

std::vector<Op> remove_block_ops(const Range& range, const std::string& start_token,
                                 const std::string& end_token) {
    std::vector<Op> ops;
    const auto before = [](Pos position, std::size_t size) {
        return Pos{position.line, position.column > size ? position.column - size : 0};
    };
    if (!range.empty()) {
        ops.push_back({Range::of(before(range.start, start_token.size()), range.start), ""});
        ops.push_back(
            {Range::of(range.end, {range.end.line, range.end.column + end_token.size()}), ""});
    } else {
        ops.push_back({Range::of(before(range.start, start_token.size()),
                                 {range.end.line, range.end.column + end_token.size()}),
                       ""});
    }
    return ops;
}

std::vector<Op> add_block_ops(const Range& range, const std::string& start_token,
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
        return remove_block_ops(
            Range::of({start_line, static_cast<std::size_t>(start_index) + start_token.size() + 1},
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
        command.ops = remove_block_ops(
            Range::of({start.line, static_cast<std::size_t>(start_index) + start_token.size() + 1},
                      {end.line, static_cast<std::size_t>(end_index) + 1}),
            start_token, end_token);
    } else {
        command.ops =
            add_block_ops(Range::of(start, end), start_token, end_token, options.insert_space);
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
        lines[i].offset = visible > minimum ? j - 1 : j;
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
                Range::of({s.start().line, first + 1}, {s.start().line, content.size() + 1}),
                start_token, end_token, options.insert_space);
        } else {
            ops = add_block_ops(
                // Column 0, on a blank line, is kept: it counts in deciding which cursors conflict,
                // and is the first column when the edit is made.
                Range::of({s.start().line, first_non_blank_column(model.line(s.start().line))},
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
        // edit reaches into the next line, where it conflicts with the edits of other cursors.
        const std::size_t line = s.start().line;
        command.ops.push_back({{{line, model.max_column(line)}, {line + 1, 1}},
                               line == model.line_count() ? "" : "\n",
                               false});
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

// --- applying the commands ---

struct PlacedEdit {
    Edit edit;
    std::size_t command = 0;
    std::size_t op = 0;
};

class Placement {
public:
    explicit Placement(std::vector<PlacedEdit> edits) : edits_(std::move(edits)) {
        std::ranges::stable_sort(edits_, [](const PlacedEdit& a, const PlacedEdit& b) {
            return std::pair(a.edit.start, a.edit.end) < std::pair(b.edit.start, b.edit.end);
        });
    }

    const std::vector<PlacedEdit>& edits() const {
        return edits_;
    }

    // Where a position of the old text is in the new one. At an insertion, the position moves
    // after the inserted text when `moves`; inside a deletion it goes to where the deletion was.
    std::size_t map(std::size_t offset, bool moves) const {
        Signed delta = 0;
        for (const PlacedEdit& placed : edits_) {
            const Edit& e = placed.edit;
            const Signed growth =
                static_cast<Signed>(e.text.size()) - static_cast<Signed>(e.end - e.start);
            if (e.start == e.end) {
                if (e.start > offset || (e.start == offset && !moves)) {
                    break;
                }
                delta += growth;
                continue;
            }
            if (e.start >= offset) {
                break;
            }
            if (e.end <= offset) {
                delta += growth;
                continue;
            }
            return static_cast<std::size_t>(static_cast<Signed>(e.start) + delta);
        }
        return static_cast<std::size_t>(static_cast<Signed>(offset) + delta);
    }

    // Where the text that the edit `index` (in the order of edits()) puts in is in the new text.
    std::size_t placed_start(std::size_t index) const {
        Signed delta = 0;
        for (std::size_t i = 0; i < index; ++i) {
            const Edit& e = edits_[i].edit;
            delta += static_cast<Signed>(e.text.size()) - static_cast<Signed>(e.end - e.start);
        }
        return static_cast<std::size_t>(static_cast<Signed>(edits_[index].edit.start) + delta);
    }

private:
    std::vector<PlacedEdit> edits_;
};

// The cursors whose edits overlap those of a cursor with a lower number lose, as in VS Code: the
// edits of a loser are not made and its selection is dropped.
std::vector<bool> losers_of(const std::vector<Command>& commands) {
    struct Raw {
        std::size_t major;
        Range range;
    };
    std::vector<Raw> ops;
    for (std::size_t c = 0; c < commands.size(); ++c) {
        for (const Op& op : commands[c].ops) {
            if (!(op.range.empty() && op.text.empty())) {
                ops.push_back({c, op.range});
            }
        }
    }
    // The last range first: by end, then by start, descending.
    std::ranges::stable_sort(ops, [](const Raw& a, const Raw& b) {
        return std::pair(b.range.end, b.range.start) < std::pair(a.range.end, a.range.start);
    });
    std::vector<bool> loser(commands.size(), false);
    for (Signed i = 1; i < static_cast<Signed>(ops.size()); ++i) {
        const Raw previous = ops[static_cast<std::size_t>(i) - 1];
        const Raw current = ops[static_cast<std::size_t>(i)];
        if (previous.range.start < current.range.end) {
            const std::size_t major =
                previous.major > current.major ? previous.major : current.major;
            loser[major] = true;
            for (Signed j = 0; j < static_cast<Signed>(ops.size()); ++j) {
                if (ops[static_cast<std::size_t>(j)].major == major) {
                    ops.erase(ops.begin() + j);
                    if (j < i) {
                        --i;
                    }
                    --j;
                }
            }
            if (i > 0) {
                --i;
            }
        }
    }
    return loser;
}

// VS Code's merging of cursors that overlap, or that touch when one of them is a caret: the union
// of the two takes the place of the one with the lower number, in the direction of that one, or of
// the other one when it is the last cursor that was added.
void merge_cursors(std::vector<Selection>& cursors, std::size_t& primary) {
    struct Sorted {
        std::size_t index;
        Selection selection;
    };
    std::vector<Sorted> sorted;
    for (std::size_t i = 0; i < cursors.size(); ++i) {
        sorted.push_back({i, cursors[i]});
    }
    std::ranges::stable_sort(sorted, [](const Sorted& a, const Sorted& b) {
        return std::pair(a.selection.start(), a.selection.end()) <
               std::pair(b.selection.start(), b.selection.end());
    });
    std::size_t last_added = cursors.empty() ? 0 : cursors.size() - 1;
    for (std::size_t at = 0; at + 1 < sorted.size(); ++at) {
        const Sorted current = sorted[at];
        const Sorted next = sorted[at + 1];
        const bool merge = current.selection.empty() || next.selection.empty()
                               ? next.selection.start() <= current.selection.end()
                               : next.selection.start() < current.selection.end();
        if (!merge) {
            continue;
        }
        const bool current_wins = current.index < next.index;
        const std::size_t winner_at = current_wins ? at : at + 1;
        const std::size_t looser_at = current_wins ? at + 1 : at;
        const std::size_t winner_index = sorted[winner_at].index;
        const std::size_t looser_index = sorted[looser_at].index;
        const Selection looser = sorted[looser_at].selection;
        const Selection winner = sorted[winner_at].selection;
        if (!(looser == winner)) {
            const std::size_t start = std::min(looser.start(), winner.start());
            const std::size_t end = std::max(looser.end(), winner.end());
            bool forward = !winner.reversed();
            if (looser_index == last_added) {
                forward = !looser.reversed();
                last_added = winner_index;
            }
            const Selection merged = forward ? Selection{start, end} : Selection{end, start};
            sorted[winner_at].selection = merged;
            cursors[winner_index] = merged;
        }
        for (Sorted& other : sorted) {
            if (other.index > looser_index) {
                --other.index;
            }
        }
        if (primary > looser_index) {
            --primary;
        } else if (primary == looser_index) {
            primary = winner_index > looser_index ? winner_index - 1 : winner_index;
        }
        cursors.erase(cursors.begin() + static_cast<Signed>(looser_index));
        sorted.erase(sorted.begin() + static_cast<Signed>(looser_at));
        if (last_added >= looser_index && last_added > 0) {
            --last_added;
        }
        --at;
    }
}

struct Finished {
    std::vector<Edit> edits;
    std::vector<Selection> selections;
    std::size_t primary = 0;
    // VS Code drops every command when the first cursor loses.
    bool cancelled = false;
};

// The edits of all the commands, as one batch, and the selection each command leaves.
Finished finish(const Model& model, const std::vector<Command>& commands, std::size_t primary) {
    Finished finished;
    const bool any = std::ranges::any_of(commands, [](const Command& command) {
        return std::ranges::any_of(
            command.ops, [](const Op& op) { return !(op.range.empty() && op.text.empty()); });
    });
    if (!any) {
        finished.cancelled = true;
        return finished;
    }
    const std::vector<bool> loser = losers_of(commands);
    if (!loser.empty() && loser[0]) {
        finished.cancelled = true;
        return finished;
    }

    std::vector<PlacedEdit> placed;
    for (std::size_t c = 0; c < commands.size(); ++c) {
        if (loser[c]) {
            continue;
        }
        for (std::size_t k = 0; k < commands[c].ops.size(); ++k) {
            const Op& op = commands[c].ops[k];
            if (op.applies && !(op.range.empty() && op.text.empty())) {
                placed.push_back(
                    {Edit{model.offset(op.range.start), model.offset(op.range.end), op.text}, c,
                     k});
            }
        }
    }
    const Placement placement(std::move(placed));
    const auto edit_index = [&](std::size_t command, std::size_t op) {
        const auto& edits = placement.edits();
        for (std::size_t i = 0; i < edits.size(); ++i) {
            if (edits[i].command == command && edits[i].op == op) {
                return i;
            }
        }
        return edits.size();
    };

    std::size_t kept_primary = primary;
    for (std::size_t c = 0; c < commands.size(); ++c) {
        const Command& command = commands[c];
        if (loser[c]) {
            if (c < primary) {
                --kept_primary;
            }
            continue;
        }
        const bool has_ops = std::ranges::any_of(
            command.ops, [](const Op& op) { return !(op.range.empty() && op.text.empty()); });
        if (!has_ops) {
            // A command that edits nothing keeps the line and column of its selection, also when
            // other commands changed the text before it on the line or moved the line.
            const auto retained = [&](Pos position) {
                const std::size_t line_start =
                    placement.map(model.line_start_offset(position.line), false);
                const std::size_t line_end =
                    placement.map(model.line_end_offset(position.line), true);
                return std::min(line_start + position.column - 1, line_end);
            };
            finished.selections.push_back(
                Selection{retained(command.selection.anchor), retained(command.selection.active)});
            continue;
        }
        if (command.inverse) {
            const std::size_t first = edit_index(c, 0);
            const std::size_t first_end =
                placement.placed_start(first) + placement.edits()[first].edit.text.size();
            if (command.ops.size() == 2) {
                const std::size_t second = edit_index(c, 1);
                finished.selections.push_back(Selection{first_end, placement.placed_start(second)});
            } else {
                const std::size_t caret =
                    first_end - (command.used_end_token ? command.used_end_token_size + 1 : 0);
                finished.selections.push_back(Selection::caret(caret));
            }
            continue;
        }

        const Sel& tracked = command.tracked;
        bool start_moves = true;
        bool end_moves = false;
        if (tracked.empty()) {
            start_moves = end_moves =
                tracked.start().column != model.max_column(tracked.start().line);
        }
        const std::size_t start = placement.map(model.offset(tracked.start()), start_moves);
        std::size_t end = std::max(start, placement.map(model.offset(tracked.end()), end_moves));
        if (command.move_end_down) {
            end = placement.map(model.line_start_offset(command.selection.end().line), false);
        }
        const std::size_t anchor = (tracked.reversed() ? end : start) + command.delta_column;
        const std::size_t active = (tracked.reversed() ? start : end) + command.delta_column;
        finished.selections.push_back(Selection{anchor, active});
    }
    for (const PlacedEdit& edit : placement.edits()) {
        finished.edits.push_back(edit.edit);
    }
    finished.primary =
        std::min(kept_primary,
                 finished.selections.empty() ? std::size_t{0} : finished.selections.size() - 1);
    merge_cursors(finished.selections, finished.primary);
    return finished;
}

} // namespace

EditOutcome EditController::comment_lines(LineCommentType type, const CommentOptions& options) {
    const Model model(document_.text());
    const std::vector<Selection>& selections = document_.selections().all();
    const std::size_t primary = document_.selections().primary_index();

    struct Entry {
        Sel selection;
        // The place of the cursor in VS Code's order, the primary one first.
        std::size_t rank = 0;
        bool ignore_first_line = false;
    };
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < selections.size(); ++i) {
        entries.push_back(
            {model.selection(selections[i]), i == primary ? 0 : (i < primary ? i + 1 : i), false});
    }
    // Selections that share a line comment it once, for the cursor that comes first. VS Code
    // compares each selection with the first one, or with the last one that won a line.
    std::size_t previous = 0;
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[previous].selection.end().line == entries[i].selection.start().line) {
            if (entries[previous].rank < entries[i].rank) {
                entries[i].ignore_first_line = true;
            } else {
                entries[previous].ignore_first_line = true;
                previous = i;
            }
        }
    }

    std::vector<Command> commands;
    for (const Entry& entry : entries) {
        commands.push_back(
            line_comment(model, entry.selection, type, options, entry.ignore_first_line));
    }
    Finished finished = finish(model, commands, primary);
    if (finished.cancelled) {
        return {};
    }
    return apply_line_edit(std::move(finished.edits), std::move(finished.selections),
                           finished.primary);
}

EditOutcome EditController::toggle_block_comment(const CommentOptions& options) {
    const Model model(document_.text());
    std::vector<Command> commands;
    for (const Selection& selection : document_.selections().all()) {
        commands.push_back(block_comment(model, model.selection(selection), options));
    }
    Finished finished = finish(model, commands, document_.selections().primary_index());
    if (finished.cancelled) {
        return {};
    }
    return apply_line_edit(std::move(finished.edits), std::move(finished.selections),
                           finished.primary);
}

} // namespace anvil::core
