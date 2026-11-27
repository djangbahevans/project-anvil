// Delete Word Left and Delete Word Right, ported from VS Code's `WordOperations`, with its default
// word separators and its white space rules, so that the results can be compared with the ones
// recorded from VS Code (`tests/core/data/edit_cases.json`).

#include "anvil/core/char_class.hpp"
#include "anvil/core/editing.hpp"
#include "cursor_commands.hpp"

#include <optional>

namespace anvil::core {
namespace {

using namespace cursor_commands;

// A byte of a character outside ASCII is part of a word, as the character is.
CharClass class_at(const std::string& line, std::size_t index) {
    const auto byte = static_cast<unsigned char>(line[index]);
    return byte < 0x80 ? classify(byte) : CharClass::word;
}

// A run of word characters or of separators, as the bytes [start, end) of its line.
struct Word {
    std::size_t start = 0;
    std::size_t end = 0;
};

// Whether a run of `type` stops before a character of class `next`.
bool stops(CharClass type, CharClass next) {
    return next == CharClass::space || next != type;
}

std::size_t end_of_word(const std::string& line, CharClass type, std::size_t from) {
    while (from < line.size() && !stops(type, class_at(line, from))) {
        ++from;
    }
    return from;
}

std::size_t start_of_word(const std::string& line, CharClass type, std::size_t last) {
    std::size_t start = last + 1;
    while (start > 0 && !stops(type, class_at(line, start - 1))) {
        --start;
    }
    return start;
}

// The word that holds the last character before `index` that is not white space.
std::optional<Word> previous_word(const std::string& line, std::size_t index) {
    while (index > 0 && class_at(line, index - 1) == CharClass::space) {
        --index;
    }
    if (index == 0) {
        return std::nullopt;
    }
    const CharClass type = class_at(line, index - 1);
    const std::size_t start = start_of_word(line, type, index - 1);
    return Word{start, end_of_word(line, type, start)};
}

// The word that holds the first character from `index` on that is not white space.
std::optional<Word> next_word(const std::string& line, std::size_t index) {
    while (index < line.size() && class_at(line, index) == CharClass::space) {
        ++index;
    }
    if (index >= line.size()) {
        return std::nullopt;
    }
    const CharClass type = class_at(line, index);
    const std::size_t end = end_of_word(line, type, index);
    return Word{start_of_word(line, type, end - 1), end};
}

// What Delete Word Left takes from a caret.
LineRange word_left_of(const Model& model, Pos caret) {
    if (caret.line == 1 && caret.column == 1) {
        return {caret, caret};
    }
    const std::string line = model.line(caret.line);
    const std::size_t index = caret.column - 1;
    // Two or more blanks before the caret go by themselves.
    const std::size_t text_end =
        index == 0 ? 0 : line.find_last_not_of(" \t", index - 1) + 1; // npos + 1 is 0
    if (text_end + 1 < index) {
        return {{caret.line, text_end + 1}, caret};
    }
    if (const auto word = previous_word(line, index)) {
        return {{caret.line, word->start + 1}, caret};
    }
    if (caret.column > 1) {
        return {{caret.line, 1}, caret};
    }
    return {{caret.line - 1, model.max_column(caret.line - 1)}, caret};
}

// What Delete Word Right takes from a caret.
LineRange word_right_of(const Model& model, Pos caret) {
    std::size_t number = caret.line;
    const std::size_t end_column = model.max_column(number);
    if (number == model.line_count() && caret.column == end_column) {
        return {caret, caret};
    }
    const std::string line = model.line(number);
    const std::size_t index = caret.column - 1;
    // Blanks after the caret go by themselves.
    const std::size_t text_at = std::min(line.find_first_not_of(" \t", index), line.size());
    if (index < text_at) {
        return {caret, {number, text_at + 1}};
    }
    if (const auto word = next_word(line, index)) {
        return {caret, {number, word->end + 1}};
    }
    if (caret.column < end_column || number == model.line_count()) {
        return {caret, {number, end_column}};
    }
    ++number;
    const auto word = next_word(model.line(number), 0);
    return {caret, {number, word ? word->start + 1 : model.max_column(number)}};
}

} // namespace

EditOutcome EditController::delete_word(bool right) {
    const Model model(document_.text());
    const std::vector<Sel> cursors = cursors_of(model, document_.selections());
    std::vector<Command> commands;
    for (const Sel& cursor : cursors) {
        const LineRange range = !cursor.empty() ? LineRange{cursor.start(), cursor.end()}
                                : right         ? word_right_of(model, cursor.active)
                                                : word_left_of(model, cursor.active);
        commands.push_back({.selection = cursor, .ops = {{range, {}}}, .inverse = true});
    }
    return run_commands(model, commands, cursors);
}

EditOutcome EditController::delete_word_left() {
    return delete_word(false);
}

EditOutcome EditController::delete_word_right() {
    return delete_word(true);
}

} // namespace anvil::core
