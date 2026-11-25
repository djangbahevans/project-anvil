// Tests of the comment commands: the cases recorded from VS Code's own commands
// (tests/core/data/comment_cases.json, `node scripts/generate-comment-cases.mjs`) and cases written
// out from VS Code's unit tests of them, with the same texts, selections and results.
#include "anvil/core/editing.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

using namespace anvil::core;
using json = nlohmann::json;

namespace {

// [anchor line, anchor column, active line, active column], counted from 1, as VS Code's
// `Selection(selectionStartLineNumber, selectionStartColumn, positionLineNumber, positionColumn)`.
using Cursor = std::array<std::size_t, 4>;

enum class Command { toggle, add, remove, block };

struct Outcome {
    std::vector<std::string> lines;
    std::vector<Cursor> cursors;
};

std::string join(const std::vector<std::string>& lines) {
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text += (i ? "\n" : "") + lines[i];
    }
    return text;
}

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line, '\n')) {
        lines.push_back(line);
    }
    if (text.empty() || text.back() == '\n') {
        lines.emplace_back();
    }
    return lines;
}

std::size_t offset_of(const Text& text, std::size_t line, std::size_t column) {
    return text.offset_at(Position{line - 1, column - 1});
}

Cursor cursor_of(const Text& text, const Selection& selection) {
    const Position anchor = text.position_at(selection.anchor);
    const Position active = text.position_at(selection.active);
    return {anchor.line + 1, anchor.column + 1, active.line + 1, active.column + 1};
}

Outcome run(const LanguageComments& tokens, Command command, CommentOptions options,
            const std::vector<std::string>& lines, const std::vector<Cursor>& cursors,
            std::size_t primary = 0) {
    options.tokens = tokens;
    Document document(*Text::from_utf8(join(lines)));
    std::vector<Selection> selections;
    for (const Cursor& c : cursors) {
        selections.push_back(
            {offset_of(document.text(), c[0], c[1]), offset_of(document.text(), c[2], c[3])});
    }
    EXPECT_TRUE(document.set_selections(SelectionSet::from(selections, primary)).has_value());
    EditController controller(document);
    switch (command) {
    case Command::toggle:
        controller.comment_lines(LineCommentType::toggle, options);
        break;
    case Command::add:
        controller.comment_lines(LineCommentType::add, options);
        break;
    case Command::remove:
        controller.comment_lines(LineCommentType::remove, options);
        break;
    case Command::block:
        controller.toggle_block_comment(options);
        break;
    }
    Outcome outcome{split(document.text().to_string()), {}};
    for (const Selection& selection : document.selections().all()) {
        outcome.cursors.push_back(cursor_of(document.text(), selection));
    }
    return outcome;
}

LanguageComments tokens_of(const json& comments) {
    return read_language_comments(json{{"comments", comments}}.dump()).comments;
}

// The configuration of VS Code's tests of these commands.
LanguageComments bang_tokens() {
    return tokens_of({{"lineComment", "!@#"}, {"blockComment", {"<!@#", "#@!>"}}});
}
LanguageComments bang_tokens_in_first_column() {
    return tokens_of({{"lineComment", {{"comment", "!@#"}, {"noIndent", true}}},
                      {"blockComment", {"<!@#", "#@!>"}}});
}

std::string describe(const std::vector<std::string>& lines, const std::vector<Cursor>& cursors) {
    std::string text;
    for (const std::string& line : lines) {
        text += "  " + json(line).dump() + "\n";
    }
    for (const Cursor& c : cursors) {
        text += "  [" + std::to_string(c[0]) + "," + std::to_string(c[1]) + " -> " +
                std::to_string(c[2]) + "," + std::to_string(c[3]) + "]\n";
    }
    return text;
}

// The cursors in the order of the text, for results that VS Code lists in the order of its cursors.
std::vector<Cursor> ordered(std::vector<Cursor> cursors) {
    std::ranges::sort(cursors, [](const Cursor& a, const Cursor& b) {
        const auto key = [](const Cursor& c) {
            return std::array{std::min(std::pair(c[0], c[1]), std::pair(c[2], c[3])).first,
                              std::min(std::pair(c[0], c[1]), std::pair(c[2], c[3])).second,
                              std::max(std::pair(c[0], c[1]), std::pair(c[2], c[3])).first,
                              std::max(std::pair(c[0], c[1]), std::pair(c[2], c[3])).second};
        };
        return key(a) < key(b);
    });
    return cursors;
}

void expect_result(Command command, const LanguageComments& tokens,
                   const std::vector<std::string>& lines, Cursor cursor,
                   const std::vector<std::string>& expected_lines, Cursor expected_cursor,
                   CommentOptions options = {}) {
    const Outcome outcome = run(tokens, command, options, lines, {cursor});
    EXPECT_EQ(outcome.lines, expected_lines);
    ASSERT_EQ(outcome.cursors.size(), 1U);
    EXPECT_EQ(outcome.cursors[0], expected_cursor) << describe(outcome.lines, outcome.cursors);
}

} // namespace

// --- cases of VS Code's own unit tests ---

TEST(CommentCommands, CommentASingleLine) {
    expect_result(Command::toggle, bang_tokens(), {"some text", "\tsome more text"}, {1, 1, 1, 1},
                  {"!@# some text", "\tsome more text"}, {1, 5, 1, 5});
}

TEST(CommentCommands, TheTokenIsFoundWhateverItsCase) {
    expect_result(Command::toggle, tokens_of({{"lineComment", "rem"}}), {"REM some text"},
                  {1, 1, 1, 1}, {"some text"}, {1, 1, 1, 1});
}

TEST(CommentCommands, ATokenInTheFirstColumnGoesBeforeTheIndentation) {
    expect_result(Command::toggle, bang_tokens_in_first_column(), {"some text", "\tsome more text"},
                  {2, 1, 2, 1}, {"some text", "!@# \tsome more text"}, {2, 5, 2, 5});
}

TEST(CommentCommands, TheTokenGoesAtTheIndentationOfTheLines) {
    expect_result(Command::toggle, bang_tokens(), {"\tsome text", "\tsome more text"}, {2, 2, 1, 1},
                  {"\t!@# some text", "\t!@# some more text"}, {2, 2, 1, 1});
}

TEST(CommentCommands, TheTokensAlignAtTheSmallestIndentationOfMixedIndentation) {
    expect_result(Command::toggle, bang_tokens(), {"\tsome text", "    some more text"},
                  {2, 2, 1, 1}, {"\t!@# some text", "    !@# some more text"}, {2, 2, 1, 1});
}

TEST(CommentCommands, LinesWithNothingOnThemAreLeftOut) {
    expect_result(Command::toggle, bang_tokens(), {"\tsome text", "\t   ", "", "\tsome more text"},
                  {4, 2, 1, 1}, {"\t!@# some text", "\t   ", "", "\t!@# some more text"},
                  {4, 2, 1, 1});
}

TEST(CommentCommands, ToggleRemovesTheTokensWhenEveryLineHasOne) {
    expect_result(Command::toggle, bang_tokens(),
                  {"\t!@# some text", "\t   ", "\t\t!@# some more text"}, {3, 2, 1, 1},
                  {"\tsome text", "\t   ", "\t\tsome more text"}, {3, 2, 1, 1});
}

TEST(CommentCommands, LinesThatAreAllBlankGetTheTokenAfterAll) {
    expect_result(Command::toggle, bang_tokens(), {"\t    ", "\t", "\t\tsome more text"},
                  {3, 1, 1, 1}, {"\t!@#     ", "\t!@# ", "\t\tsome more text"}, {3, 1, 1, 1});
}

TEST(CommentCommands, WhitespaceBeforeATokenIsKept) {
    expect_result(Command::toggle, bang_tokens(), {"\t !@#first", "\tsecond line"}, {1, 1, 1, 1},
                  {"\t first", "\tsecond line"}, {1, 1, 1, 1});
}

TEST(CommentCommands, ATokenAtTheEndOfTheLineIsNotACommentAtItsStart) {
    expect_result(Command::toggle, bang_tokens(), {"first!@#", "\tsecond line"}, {1, 1, 1, 1},
                  {"!@# first!@#", "\tsecond line"}, {1, 5, 1, 5});
}

TEST(CommentCommands, ASelectionThatEndsAtTheStartOfALineLeavesThatLineOut) {
    expect_result(Command::toggle, bang_tokens(),
                  {"first", "\tsecond line", "third line", "fourth line", "fifth"}, {2, 1, 1, 1},
                  {"!@# first", "\tsecond line", "third line", "fourth line", "fifth"},
                  {2, 1, 1, 5});
}

TEST(CommentCommands, SeveralLinesAreCommentedAtTheSameColumn) {
    expect_result(Command::toggle, bang_tokens(),
                  {"first", "\tsecond line", "third line", "fourth line", "fifth"}, {2, 4, 1, 1},
                  {"!@# first", "!@# \tsecond line", "third line", "fourth line", "fifth"},
                  {2, 8, 1, 5});
}

TEST(CommentCommands, AddIgnoresTheCommentsThatAreThere) {
    expect_result(Command::add, bang_tokens(), {"!@# first", "second"}, {1, 1, 2, 7},
                  {"!@# !@# first", "!@# second"}, {1, 5, 2, 11});
}

TEST(CommentCommands, RemoveLeavesTheLinesWithoutATokenAlone) {
    expect_result(Command::remove, bang_tokens(), {"!@# first", "second", "!@# third"},
                  {1, 1, 3, 1}, {"first", "second", "!@# third"}, {1, 1, 3, 1});
}

TEST(CommentCommands, WithoutASpaceTheTokenIsAloneAndASpaceAfterItStays) {
    CommentOptions options;
    options.insert_space = false;
    expect_result(Command::toggle, bang_tokens(), {"some text"}, {1, 1, 1, 1}, {"!@#some text"},
                  {1, 4, 1, 4}, options);
    expect_result(Command::toggle, bang_tokens(), {"!@# some text"}, {1, 1, 1, 1}, {" some text"},
                  {1, 1, 1, 1}, options);
}

TEST(CommentCommands, EmptyLinesGetTheTokenWhenTheOptionIsOff) {
    CommentOptions options;
    options.ignore_empty_lines = false;
    expect_result(Command::toggle, bang_tokens(), {"a", "", "b"}, {1, 1, 3, 2},
                  {"!@# a", "!@# ", "!@# b"}, {1, 5, 3, 6}, options);
}

TEST(CommentCommands, ALanguageWithoutALineTokenGetsABlockCommentAroundTheText) {
    const LanguageComments html = tokens_of({{"blockComment", {"<!--", "-->"}}});
    expect_result(Command::toggle, html, {"  some text"}, {1, 1, 1, 1}, {"  <!-- some text -->"},
                  {1, 1, 1, 1});
    expect_result(Command::toggle, html, {"  <!-- some text -->"}, {1, 8, 1, 8}, {"  some text"},
                  {1, 3, 1, 3});
}

TEST(CommentCommands, ALanguageWithNoTokensIsLeftAlone) {
    const Outcome outcome = run({}, Command::toggle, {}, {"some text"}, {{1, 1, 1, 1}});
    EXPECT_EQ(outcome.lines, (std::vector<std::string>{"some text"}));
    EXPECT_EQ(outcome.cursors, (std::vector<Cursor>{{1, 1, 1, 1}}));
}

TEST(CommentCommands, ABlockCommentWrapsTheSelectionAndUnwrapsItAgain) {
    expect_result(Command::block, bang_tokens(), {"first line", "second line"}, {1, 3, 2, 5},
                  {"fi<!@# rst line", "seco #@!>nd line"}, {1, 8, 2, 5});
}

TEST(CommentCommands, ABlockCommentAroundACaretStartsEmptyWithTheCaretInside) {
    expect_result(Command::block, bang_tokens(), {"first line"}, {1, 3, 1, 3},
                  {"fi<!@#  #@!>rst line"}, {1, 8, 1, 8});
}

// --- a block comment for a language that has no line comment, from VS Code's tests ---

LanguageComments parentheses() {
    return tokens_of({{"lineComment", ""}, {"blockComment", {"(", ")"}}});
}

TEST(CommentCommands, ALineCommandWithoutALineTokenWrapsTheLineInTheBlockComment) {
    expect_result(Command::toggle, parentheses(),
                  {"first", "\tsecond line", "third line", "fourth line", "fifth"}, {1, 1, 1, 1},
                  {"( first )", "\tsecond line", "third line", "fourth line", "fifth"},
                  {1, 3, 1, 3});
}

TEST(CommentCommands, ALineCommandWithoutALineTokenUnwrapsTheBlockCommentOfTheSelection) {
    expect_result(Command::toggle, parentheses(),
                  {"(first)", "\tsecond line", "third line", "fourth line", "fifth"}, {1, 7, 1, 2},
                  {"first", "\tsecond line", "third line", "fourth line", "fifth"}, {1, 6, 1, 1});
}

TEST(CommentCommands, TheBlockCommentOfALineCommandReachesTheEdgesOfTheLines) {
    expect_result(Command::toggle, parentheses(),
                  {"first", "\tsecond line", "third line", "fourth line", "fifth"}, {3, 2, 1, 3},
                  {"( first", "\tsecond line", "third line )", "fourth line", "fifth"},
                  {3, 2, 1, 5});
    expect_result(Command::toggle, parentheses(),
                  {"(first", "\tsecond line", "third line)", "fourth line", "fifth"}, {3, 11, 1, 2},
                  {"first", "\tsecond line", "third line", "fourth line", "fifth"}, {3, 11, 1, 1});
}

TEST(CommentCommands, ACaretAnywhereOnALineWithABlockCommentRemovesIt) {
    const LanguageComments bang = tokens_of({{"blockComment", {"<!@#", "#@!>"}}});
    const std::vector<std::string> lines = {"\t\tfirst\t    ", "\t\tsecond line", "\tthird line",
                                            "fourth line", "\t\t<!@#fifth#@!>\t\t"};
    const std::vector<std::string> without = {"\t\tfirst\t    ", "\t\tsecond line", "\tthird line",
                                              "fourth line", "\t\tfifth\t\t"};
    expect_result(Command::toggle, bang, lines, {5, 1, 5, 1}, without, {5, 1, 5, 1});
    expect_result(Command::toggle, bang, lines, {5, 3, 5, 3}, without, {5, 3, 5, 3});
    expect_result(Command::toggle, bang, lines, {5, 4, 5, 4}, without, {5, 3, 5, 3});
    expect_result(Command::toggle, bang, lines, {5, 16, 5, 3}, without, {5, 8, 5, 3});
    expect_result(Command::toggle, bang, lines, {5, 12, 5, 7}, without, {5, 8, 5, 3});
    expect_result(Command::toggle, bang, lines, {5, 18, 5, 18}, without, {5, 10, 5, 10});
}

TEST(CommentCommands, ACaretOnALineWithoutABlockCommentWrapsTheLineFromItsIndentation) {
    const LanguageComments bang = tokens_of({{"blockComment", {"<!@#", "#@!>"}}});
    expect_result(Command::toggle, bang,
                  {"\t\tfirst\t    ", "\t\tsecond line", "\tthird line", "fourth line",
                   "\t\t<!@#fifth#@!>\t\t"},
                  {1, 1, 1, 1},
                  {"\t\t<!@# first\t     #@!>", "\t\tsecond line", "\tthird line", "fourth line",
                   "\t\t<!@#fifth#@!>\t\t"},
                  {1, 1, 1, 1});
}

TEST(CommentCommands, AddForcesTheTokenAndRemoveTheRemovalOnLinesThatAreNotAllCommented) {
    expect_result(Command::add, bang_tokens(), {"!@# a", "!@# b"}, {1, 1, 2, 5},
                  {"!@# !@# a", "!@# !@# b"}, {1, 5, 2, 9});
    expect_result(Command::remove, bang_tokens(), {"!@# a", "b"}, {1, 1, 2, 2}, {"a", "b"},
                  {1, 1, 2, 2});
}

TEST(CommentCommands, TheCursorThatComesFirstDecidesWhichLineASharedLineIsPartOf) {
    // Two selections share line 2. The primary cursor wins it: the other ignores its first line.
    const std::vector<std::string> lines = {"a", "b", "c"};
    const LanguageComments tokens = bang_tokens();
    const Outcome first = run(tokens, Command::toggle, {}, lines, {{1, 1, 2, 2}, {2, 2, 3, 2}}, 0);
    const Outcome second = run(tokens, Command::toggle, {}, lines, {{1, 1, 2, 2}, {2, 2, 3, 2}}, 1);
    EXPECT_EQ(first.lines, (std::vector<std::string>{"!@# a", "!@# b", "!@# c"}));
    EXPECT_NE(second.lines, first.lines);
}

// --- several selections ---

TEST(CommentCommands, SelectionsOnTheSameLineCommentItOnce) {
    const Outcome outcome = run(bang_tokens(), Command::toggle, {}, {"one two three"},
                                {{1, 1, 1, 1}, {1, 5, 1, 5}, {1, 9, 1, 9}});
    EXPECT_EQ(outcome.lines, (std::vector<std::string>{"!@# one two three"}));
}

TEST(CommentCommands, TheCursorThatComesFirstDecidesWhichLinesAreSharedAndTheCommentsStayWhole) {
    const std::vector<std::string> lines = {"a", "b", "c"};
    const Outcome primary_first =
        run(bang_tokens(), Command::toggle, {}, lines, {{1, 1, 2, 2}, {2, 1, 3, 2}}, 0);
    EXPECT_EQ(primary_first.lines, (std::vector<std::string>{"!@# a", "!@# b", "!@# c"}));
    const Outcome primary_last =
        run(bang_tokens(), Command::toggle, {}, lines, {{1, 1, 2, 2}, {2, 1, 3, 2}}, 1);
    EXPECT_EQ(primary_last.lines, (std::vector<std::string>{"!@# a", "!@# b", "!@# c"}));
}

TEST(CommentCommands, AllTheEditsAreOneUndoStep) {
    Document document(*Text::from_utf8("a\nb\nc"));
    ASSERT_TRUE(document.set_selections(SelectionSet::from({Selection{0, 5}, Selection::caret(5)}))
                    .has_value());
    EditController controller(document);
    CommentOptions options;
    options.tokens = bang_tokens();
    controller.comment_lines(LineCommentType::toggle, options);
    ASSERT_NE(document.text().to_string(), "a\nb\nc");
    controller.undo();
    EXPECT_EQ(document.text().to_string(), "a\nb\nc");
}

// --- the cases recorded from VS Code ---

TEST(CommentCommands, TheRecordedCasesGiveVsCodesTextAndSelections) {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/comment_cases.json");
    ASSERT_TRUE(file.is_open());
    const json recorded = json::parse(file);

    const std::map<std::string, Command> commands = {{"toggle", Command::toggle},
                                                     {"add", Command::add},
                                                     {"remove", Command::remove},
                                                     {"block", Command::block}};
    int failures = 0;
    int number = 0;
    for (const json& entry : recorded["cases"]) {
        ++number;
        const LanguageComments tokens =
            tokens_of(recorded["configurations"][entry["config"].get<std::string>()]);
        CommentOptions options;
        options.insert_space = entry["insertSpace"].get<bool>();
        options.ignore_empty_lines = entry["ignoreEmptyLines"].get<bool>();
        options.indent_size = entry["indentSize"].get<std::size_t>();
        const auto lines = entry["lines"].get<std::vector<std::string>>();
        std::vector<Cursor> cursors;
        for (const json& c : entry["selections"]) {
            cursors.push_back(c.get<Cursor>());
        }
        std::vector<Cursor> expected_cursors;
        for (const json& c : entry["expectedSelections"]) {
            expected_cursors.push_back(c.get<Cursor>());
        }
        const auto expected_lines = entry["expectedLines"].get<std::vector<std::string>>();
        // VS Code lists the primary cursor first; the selections of the document are in the order
        // of the text.
        const std::vector<Cursor> in_text_order = ordered(cursors);
        const std::size_t primary = static_cast<std::size_t>(
            std::ranges::find(in_text_order, cursors.front()) - in_text_order.begin());
        const Outcome outcome = run(tokens, commands.at(entry["command"].get<std::string>()),
                                    options, lines, in_text_order, primary);
        if (outcome.lines != expected_lines ||
            ordered(outcome.cursors) != ordered(expected_cursors)) {
            if (++failures <= 8) {
                ADD_FAILURE() << "case " << number << ": " << entry["config"] << " "
                              << entry["command"] << " insertSpace=" << entry["insertSpace"]
                              << " ignoreEmptyLines=" << entry["ignoreEmptyLines"]
                              << " indentSize=" << entry["indentSize"] << "\nfrom\n"
                              << describe(lines, cursors) << "VS Code\n"
                              << describe(expected_lines, expected_cursors) << "Anvil\n"
                              << describe(outcome.lines, outcome.cursors);
            }
        }
    }
    EXPECT_EQ(failures, 0) << failures << " of " << number << " recorded cases differ";
    EXPECT_GE(number, 1000);
}
