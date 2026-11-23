#include "anvil/core/char_class.hpp"
#include "anvil/core/editing.hpp"

#include <gtest/gtest.h>

using namespace anvil::core;

namespace {

constexpr const char* astral = "\xF0\x9F\x98\x80"; // 4 bytes, 2 UTF-16 units
constexpr const char* eacute = "\xC3\xA9";         // 2 bytes

// A document with its controller, placed with carets or selections.
struct Editor {
    explicit Editor(std::string_view text,
                    std::vector<Selection> selections = {Selection::caret(0)})
        : document(*Text::from_utf8(text)), controller(document) {
        EXPECT_TRUE(document.set_selections(SelectionSet::from(std::move(selections))).has_value());
    }

    std::string text() const {
        return document.text().to_string();
    }
    std::vector<Selection> selections() const {
        return document.selections().all();
    }
    Selection primary() const {
        return document.selections().primary();
    }

    Document document;
    EditController controller;
};

using Carets = std::vector<Selection>;

} // namespace

// --- typing ---

TEST(EditControllerTyping, InsertsAtTheCaretAndAdvancesIt) {
    Editor editor("ac", {Selection::caret(1)});
    const EditOutcome outcome = editor.controller.type("b");
    EXPECT_EQ(editor.text(), "abc");
    EXPECT_EQ(editor.primary(), Selection::caret(2));
    ASSERT_TRUE(outcome.change.has_value());
    EXPECT_TRUE(outcome.selections_changed);
}

TEST(EditControllerTyping, ReplacesTheSelection) {
    Editor editor("hello world", {Selection{6, 11}});
    editor.controller.type("there");
    EXPECT_EQ(editor.text(), "hello there");
    EXPECT_EQ(editor.primary(), Selection::caret(11));
}

TEST(EditControllerTyping, TypesAtEveryCursor) {
    Editor editor("a b", {Selection::caret(1), Selection::caret(3)});
    editor.controller.type("!");
    EXPECT_EQ(editor.text(), "a! b!");
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(2), Selection::caret(5)}));
}

TEST(EditControllerTyping, MultiCursorReplacementKeepsEachCaretAfterItsText) {
    Editor editor("one two", {Selection{0, 3}, Selection{4, 7}});
    editor.controller.type("X");
    EXPECT_EQ(editor.text(), "X X");
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(1), Selection::caret(3)}));
}

TEST(EditControllerTyping, IgnoresEmptyInputAndNormalizesLineBreaks) {
    Editor editor("ab", {Selection::caret(1)});
    EXPECT_FALSE(editor.controller.type("").changed_anything());
    editor.controller.type("x\r\ny");
    EXPECT_EQ(editor.text(), "ax\nyb");
    EXPECT_EQ(editor.primary(), Selection::caret(4));
}

TEST(EditControllerTyping, ConsecutiveTypingUndoesAsOneStep) {
    Editor editor("");
    editor.controller.type("a");
    editor.controller.type("b");
    editor.controller.type("c");
    ASSERT_EQ(editor.text(), "abc");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "");
    EXPECT_FALSE(editor.document.can_undo());
}

TEST(EditControllerTyping, MovingTheCaretStartsANewUndoStep) {
    Editor editor("");
    editor.controller.type("a");
    editor.controller.type("b");
    editor.controller.move(Motion::left);
    editor.controller.type("X");
    ASSERT_EQ(editor.text(), "aXb");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "ab");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "");
}

TEST(EditControllerTyping, ALineBreakDoesNotJoinThePrecedingTyping) {
    Editor editor("");
    editor.controller.type("a");
    editor.controller.type("\n");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "a");
}

TEST(EditControllerTyping, EditsMadeElsewhereStartANewStep) {
    Editor editor("");
    editor.controller.type("a");
    ASSERT_TRUE(editor.document.apply_edits(std::vector<Edit>{{1, 1, "!"}}).has_value());
    editor.controller.type("b");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "a!");
}

TEST(EditControllerTyping, PasteIsItsOwnUndoStep) {
    Editor editor("");
    editor.controller.type("a");
    editor.controller.paste("xyz");
    editor.controller.type("b");
    ASSERT_EQ(editor.text(), "axyzb");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "axyz");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "a");
}

// --- newline ---

TEST(EditControllerNewline, KeepsTheIndentationOfTheLine) {
    Editor editor("    foo", {Selection::caret(7)});
    editor.controller.insert_newline();
    EXPECT_EQ(editor.text(), "    foo\n    ");
    EXPECT_EQ(editor.primary(), Selection::caret(12));
}

TEST(EditControllerNewline, InsideTheIndentationKeepsOnlyWhatPrecedesTheCaret) {
    Editor editor("    foo", {Selection::caret(2)});
    editor.controller.insert_newline();
    EXPECT_EQ(editor.text(), "  \n    foo");
    EXPECT_EQ(editor.primary(), Selection::caret(5));
}

TEST(EditControllerNewline, ReplacesTheSelectionAndWorksAtEveryCursor) {
    Editor editor("\tab\n  cd", {Selection{2, 3}, Selection::caret(8)});
    editor.controller.insert_newline();
    EXPECT_EQ(editor.text(), "\ta\n\t\n  cd\n  ");
    EXPECT_EQ(editor.selections().size(), 2u);
}

TEST(EditControllerNewline, IsNeverJoinedIntoTypingSteps) {
    Editor editor("");
    editor.controller.type("a");
    editor.controller.insert_newline();
    editor.controller.type("b");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "a\n");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "a");
}

// --- deleting ---

TEST(EditControllerDelete, BackspaceRemovesTheCharacterBeforeTheCaret) {
    Editor editor("abc", {Selection::caret(3)});
    editor.controller.delete_backward();
    EXPECT_EQ(editor.text(), "ab");
    EXPECT_EQ(editor.primary(), Selection::caret(2));
}

TEST(EditControllerDelete, BackspaceRemovesWholeCharacters) {
    Editor editor(std::string("a") + astral + eacute, {Selection::caret(7)});
    editor.controller.delete_backward();
    EXPECT_EQ(editor.text(), std::string("a") + astral);
    editor.controller.delete_backward();
    EXPECT_EQ(editor.text(), "a");
}

TEST(EditControllerDelete, BackspaceAtTheStartDoesNothing) {
    Editor editor("abc", {Selection::caret(0)});
    EXPECT_FALSE(editor.controller.delete_backward().changed_anything());
    EXPECT_EQ(editor.text(), "abc");
}

TEST(EditControllerDelete, BackspaceDeletesTheSelectionInsteadOfACharacter) {
    Editor editor("hello", {Selection{1, 4}});
    editor.controller.delete_backward();
    EXPECT_EQ(editor.text(), "ho");
    EXPECT_EQ(editor.primary(), Selection::caret(1));
}

TEST(EditControllerDelete, BackspaceAtEveryCursor) {
    Editor editor("abcd", {Selection::caret(2), Selection::caret(4)});
    editor.controller.delete_backward();
    EXPECT_EQ(editor.text(), "ac");
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(1), Selection::caret(2)}));
}

TEST(EditControllerDelete, BackspaceSkipsCursorsAtTheStartButServesTheOthers) {
    Editor editor("ab", {Selection::caret(0), Selection::caret(2)});
    editor.controller.delete_backward();
    EXPECT_EQ(editor.text(), "a");
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(0), Selection::caret(1)}));
}

TEST(EditControllerDelete, DeleteForwardRemovesTheCharacterAfterTheCaret) {
    Editor editor(std::string(astral) + "bc", {Selection::caret(0)});
    editor.controller.delete_forward();
    EXPECT_EQ(editor.text(), "bc");
    EXPECT_FALSE(Editor("x", {Selection::caret(1)}).controller.delete_forward().changed_anything());
}

TEST(EditControllerDelete, ConsecutiveBackspacesUndoTogether) {
    Editor editor("abcd", {Selection::caret(4)});
    editor.controller.delete_backward();
    editor.controller.delete_backward();
    editor.controller.delete_backward();
    ASSERT_EQ(editor.text(), "a");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "abcd");
}

TEST(EditControllerDelete, BackspaceAndForwardDeleteDoNotJoinEachOther) {
    Editor editor("abcd", {Selection::caret(2)});
    editor.controller.delete_backward();
    editor.controller.delete_forward();
    ASSERT_EQ(editor.text(), "ad");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "acd");
}

TEST(EditControllerDelete, DeleteSelectionLeavesCaretsAlone) {
    Editor editor("abcdef", {Selection{0, 2}, Selection::caret(5)});
    editor.controller.delete_selection();
    EXPECT_EQ(editor.text(), "cdef");
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(0), Selection::caret(3)}));
    EXPECT_FALSE(
        Editor("abc", {Selection::caret(2)}).controller.delete_selection().changed_anything());
}

// --- moving ---

TEST(EditControllerMove, LeftAndRightStepOverWholeCharacters) {
    Editor editor(std::string("a") + astral + "b");
    for (const std::size_t expected : {1u, 5u, 6u, 6u}) {
        editor.controller.move(Motion::right);
        EXPECT_EQ(editor.primary(), Selection::caret(expected));
    }
    for (const std::size_t expected : {5u, 1u, 0u, 0u}) {
        editor.controller.move(Motion::left);
        EXPECT_EQ(editor.primary(), Selection::caret(expected));
    }
}

TEST(EditControllerMove, LeftAndRightCollapseASelection) {
    Editor left("abcdef", {Selection{4, 2}});
    left.controller.move(Motion::left);
    EXPECT_EQ(left.primary(), Selection::caret(2));
    Editor right("abcdef", {Selection{2, 4}});
    right.controller.move(Motion::right);
    EXPECT_EQ(right.primary(), Selection::caret(4));
}

TEST(EditControllerMove, ExtendingKeepsTheAnchor) {
    Editor editor("abcdef", {Selection::caret(2)});
    editor.controller.move(Motion::right, true);
    editor.controller.move(Motion::right, true);
    EXPECT_EQ(editor.primary(), (Selection{2, 4}));
    editor.controller.move(Motion::left, true);
    editor.controller.move(Motion::left, true);
    editor.controller.move(Motion::left, true);
    EXPECT_EQ(editor.primary(), (Selection{2, 1}));
}

TEST(EditControllerMove, VerticalMovementReturnsToTheOriginalColumn) {
    Editor editor("abcdef\nab\nabcdef", {Selection::caret(5)});
    editor.controller.move(Motion::down);
    EXPECT_EQ(editor.primary(), Selection::caret(9));
    editor.controller.move(Motion::down);
    EXPECT_EQ(editor.primary(), Selection::caret(15));
    editor.controller.move(Motion::up);
    EXPECT_EQ(editor.primary(), Selection::caret(9));
    editor.controller.move(Motion::up);
    EXPECT_EQ(editor.primary(), Selection::caret(5));
}

TEST(EditControllerMove, HorizontalMovementForgetsTheGoalColumn) {
    Editor editor("abcdef\nab\nabcdef", {Selection::caret(5)});
    editor.controller.move(Motion::down);
    editor.controller.move(Motion::left);
    editor.controller.move(Motion::down);
    EXPECT_EQ(editor.primary(), Selection::caret(11)); // column 1 of the last line
}

TEST(EditControllerMove, UpOnTheFirstLineGoesToTheStartAndDownOnTheLastToTheEnd) {
    Editor editor("ab\ncd", {Selection::caret(1)});
    editor.controller.move(Motion::up);
    EXPECT_EQ(editor.primary(), Selection::caret(0));
    editor.controller.move(Motion::down);
    editor.controller.move(Motion::down);
    EXPECT_EQ(editor.primary(), Selection::caret(5));
}

TEST(EditControllerMove, VerticalMovementCountsUtf16Columns) {
    Editor editor(std::string("x") + astral + "y\nabcd", {Selection::caret(5)});
    editor.controller.move(Motion::down);
    EXPECT_EQ(editor.primary(), Selection::caret(10)); // column 3 of "abcd", which starts at byte 7
}

TEST(EditControllerMove, LineAndDocumentBoundaries) {
    Editor editor("abc\ndef\nghi", {Selection::caret(5)});
    editor.controller.move(Motion::line_start);
    EXPECT_EQ(editor.primary(), Selection::caret(4));
    editor.controller.move(Motion::line_end);
    EXPECT_EQ(editor.primary(), Selection::caret(7));
    editor.controller.move(Motion::document_start);
    EXPECT_EQ(editor.primary(), Selection::caret(0));
    editor.controller.move(Motion::document_end);
    EXPECT_EQ(editor.primary(), Selection::caret(11));
}

// --- Home ---

namespace {

// Where Home goes, by the book: the column of the first character that is not a space or a tab,
// the start of the line when the caret is there already or the line has no such character.
std::size_t reference_home(const std::string& text, std::size_t offset) {
    const std::size_t newline = offset == 0 ? std::string::npos : text.rfind('\n', offset - 1);
    const std::size_t start = newline == std::string::npos ? 0 : newline + 1;
    const std::size_t end = std::min(text.find('\n', start), text.size());
    const std::size_t first = text.find_first_not_of(" \t", start);
    if (first == std::string::npos || first >= end || first == offset) {
        return start;
    }
    return first;
}

std::size_t home_from(std::string_view text, std::size_t offset) {
    Editor editor(text, {Selection::caret(offset)});
    editor.controller.move(Motion::home);
    EXPECT_TRUE(editor.primary().empty());
    return editor.primary().active;
}

} // namespace

TEST(EditControllerHome, GoesToTheFirstNonWhitespaceCharacterOfAnIndentedLine) {
    EXPECT_EQ(home_from("    abc", 7), 4U);
    EXPECT_EQ(home_from("    abc", 0), 4U);
    EXPECT_EQ(home_from("    abc", 2), 4U);
    EXPECT_EQ(home_from("\t\t abc", 6), 3U);
}

TEST(EditControllerHome, GoesToTheStartOfTheLineWhenTheCaretIsAtTheFirstCharacter) {
    EXPECT_EQ(home_from("    abc", 4), 0U);
    EXPECT_EQ(home_from("x\n  abc", 4), 2U);
}

TEST(EditControllerHome, GoesFromTheStartOfTheLineToTheFirstCharacter) {
    EXPECT_EQ(home_from("x\n  abc", 2), 4U);
}

TEST(EditControllerHome, StaysAtColumnOneOnAnUnindentedLine) {
    EXPECT_EQ(home_from("abc", 2), 0U);
    EXPECT_EQ(home_from("abc", 0), 0U);
    EXPECT_EQ(home_from("x\nabc", 4), 2U);
}

TEST(EditControllerHome, GoesToTheStartOfABlankOrWhitespaceOnlyLine) {
    EXPECT_EQ(home_from("a\n\nb", 2), 2U);
    EXPECT_EQ(home_from("a\n   \nb", 5), 2U);
    EXPECT_EQ(home_from("a\n   \nb", 2), 2U);
    EXPECT_EQ(home_from("a\n \t \nb", 4), 2U);
}

TEST(EditControllerHome, WorksOnTheLastLineAndWithMultibyteText) {
    EXPECT_EQ(home_from("a\n  b", 5), 4U);
    EXPECT_EQ(home_from(std::string("  ") + eacute + astral, 8), 2U);
    EXPECT_EQ(home_from(std::string("  ") + eacute + astral, 2), 0U);
}

TEST(EditControllerHome, FindsTheCharacterBeyondALongRunOfWhitespace) {
    const std::string indent(200, ' ');
    EXPECT_EQ(home_from(indent + "x", 201), 200U);
    EXPECT_EQ(home_from(indent + "x", 200), 0U);
    EXPECT_EQ(home_from(indent, 100), 0U);
    EXPECT_EQ(home_from(std::string(64, ' ') + "x", 0), 64U);
}

TEST(EditControllerHome, ExtendsTheSelectionFromTheAnchor) {
    Editor editor("  abc", {Selection{5, 5}});
    editor.controller.move(Motion::home, true);
    EXPECT_EQ(editor.primary(), (Selection{5, 2}));
    editor.controller.move(Motion::home, true);
    EXPECT_EQ(editor.primary(), (Selection{5, 0}));
    editor.controller.move(Motion::home, true);
    EXPECT_EQ(editor.primary(), (Selection{5, 2}));
}

TEST(EditControllerHome, MovesFromTheActiveEndAndCollapsesASelection) {
    Editor editor("  abc", {Selection{4, 3}});
    editor.controller.move(Motion::home);
    EXPECT_EQ(editor.primary(), Selection::caret(2));
}

TEST(EditControllerHome, MovesEveryCaretByItsOwnLine) {
    // Lines: "  a" at 0, "b" at 4, "    c" at 6, "  " at 12.
    Editor editor("  a\nb\n    c\n  ", {Selection::caret(3), Selection::caret(4),
                                        Selection::caret(11), Selection::caret(14)});
    editor.controller.move(Motion::home);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(2), Selection::caret(4),
                                           Selection::caret(10), Selection::caret(12)}));
    editor.controller.move(Motion::home);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(0), Selection::caret(4),
                                           Selection::caret(6), Selection::caret(12)}));
}

TEST(EditControllerHome, AgreesWithTheReferenceOnRandomText) {
    std::uint32_t seed = 12345;
    const auto next = [&seed](std::uint32_t bound) {
        seed = seed * 1664525U + 1013904223U;
        return (seed >> 8) % bound;
    };
    constexpr char alphabet[] = {' ', ' ', '\t', 'a', 'b', '\n'};
    for (int round = 0; round < 400; ++round) {
        std::string text;
        const std::uint32_t length = next(40);
        for (std::uint32_t i = 0; i < length; ++i) {
            text += alphabet[next(6)];
        }
        for (std::size_t offset = 0; offset <= text.size(); ++offset) {
            ASSERT_EQ(home_from(text, offset), reference_home(text, offset))
                << "text " << testing::PrintToString(text) << " offset " << offset;
        }
    }
}

TEST(EditControllerMove, WordMotionsSkipSpacesAndStopAtClassChanges) {
    Editor editor("foo bar_baz  +-  qux");
    for (const std::size_t expected : {3u, 11u, 15u, 20u, 20u}) {
        editor.controller.move(Motion::word_right);
        EXPECT_EQ(editor.primary(), Selection::caret(expected));
    }
    for (const std::size_t expected : {17u, 13u, 4u, 0u, 0u}) {
        editor.controller.move(Motion::word_left);
        EXPECT_EQ(editor.primary(), Selection::caret(expected));
    }
}

TEST(EditControllerMove, WordMotionsStepOverLineBreaks) {
    Editor editor("ab\ncd", {Selection::caret(2)});
    editor.controller.move(Motion::word_right);
    EXPECT_EQ(editor.primary(), Selection::caret(3));
    editor.controller.move(Motion::word_left);
    EXPECT_EQ(editor.primary(), Selection::caret(2));
}

TEST(EditControllerMove, WordMotionsTreatNonAsciiLettersAsWordCharacters) {
    Editor editor(std::string("caf") + eacute + " ok");
    editor.controller.move(Motion::word_right);
    EXPECT_EQ(editor.primary(), Selection::caret(5));
}

TEST(EditControllerMove, EveryCursorMoves) {
    Editor editor("abcd", {Selection::caret(1), Selection::caret(3)});
    editor.controller.move(Motion::right);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(2), Selection::caret(4)}));
}

TEST(EditControllerMove, CursorsThatMeetAreMerged) {
    Editor editor("abc", {Selection::caret(1), Selection::caret(2)});
    editor.controller.move(Motion::right);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(2), Selection::caret(3)}));
    editor.controller.move(Motion::line_end);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(3)}));
}

TEST(EditControllerMove, ReportsWhetherAnythingChanged) {
    Editor editor("abc", {Selection::caret(0)});
    EXPECT_FALSE(editor.controller.move(Motion::left).changed_anything());
    EXPECT_FALSE(editor.controller.move(Motion::document_start).changed_anything());
    const EditOutcome moved = editor.controller.move(Motion::right);
    EXPECT_TRUE(moved.selections_changed);
    EXPECT_FALSE(moved.change.has_value());
}

// --- selecting ---

TEST(EditControllerSelect, SelectAllCoversTheText) {
    Editor editor("abc\ndef");
    editor.controller.select_all();
    EXPECT_EQ(editor.primary(), (Selection{0, 7}));
}

TEST(EditControllerSelect, SetCaretPlacesASingleCaretOnACharacterBoundary) {
    Editor editor(std::string("a") + eacute + "b", {Selection::caret(0), Selection::caret(1)});
    editor.controller.set_caret(2);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(1)}));
    editor.controller.set_caret(999);
    EXPECT_EQ(editor.primary(), Selection::caret(4));
}

TEST(EditControllerSelect, SetCaretWithExtendMovesTheActiveEnd) {
    Editor editor("abcdef", {Selection::caret(2)});
    editor.controller.set_caret(5, true);
    EXPECT_EQ(editor.primary(), (Selection{2, 5}));
    editor.controller.set_caret(0, true);
    EXPECT_EQ(editor.primary(), (Selection{2, 0}));
}

TEST(EditControllerSelect, AddCaretKeepsTheOthersAndMakesTheNewOnePrimary) {
    Editor editor("abcdef", {Selection::caret(1)});
    editor.controller.add_caret(4);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(1), Selection::caret(4)}));
    EXPECT_EQ(editor.primary(), Selection::caret(4));
}

TEST(EditControllerSelect, KeepPrimarySelectionDropsTheOthers) {
    Editor editor("abcdef", {Selection::caret(1), Selection{3, 5}});
    editor.controller.add_caret(0);
    EXPECT_EQ(editor.selections().size(), 3u);
    editor.controller.keep_primary_selection();
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(0)}));
    EXPECT_FALSE(editor.controller.keep_primary_selection().changed_anything());
}

TEST(EditControllerSelect, SelectWordAtSelectsARunOfTheSameKind) {
    Editor editor("foo bar_baz  +-");
    editor.controller.select_word_at(5);
    EXPECT_EQ(editor.primary(), (Selection{4, 11}));
    editor.controller.select_word_at(12);
    EXPECT_EQ(editor.primary(), (Selection{11, 13}));
    editor.controller.select_word_at(14);
    EXPECT_EQ(editor.primary(), (Selection{13, 15}));
    editor.controller.select_word_at(15); // at the end, the word before it
    EXPECT_EQ(editor.primary(), (Selection{13, 15}));
}

TEST(EditControllerSelect, SelectWordAtDoesNotCrossLines) {
    Editor editor("ab\ncd");
    editor.controller.select_word_at(2);
    EXPECT_EQ(editor.primary(), (Selection{0, 2}));
    editor.controller.select_word_at(3);
    EXPECT_EQ(editor.primary(), (Selection{3, 5}));
}

TEST(EditControllerSelect, SelectLineAtIncludesTheLineBreak) {
    Editor editor("ab\ncd\nef");
    editor.controller.select_line_at(4);
    EXPECT_EQ(editor.primary(), (Selection{3, 6}));
    editor.controller.select_line_at(7);
    EXPECT_EQ(editor.primary(), (Selection{6, 8}));
}

TEST(EditControllerSelect, SelectedTextJoinsSelectionsWithLineBreaks) {
    Editor editor("abc def ghi", {Selection{0, 3}, Selection{8, 11}});
    EXPECT_EQ(editor.controller.selected_text(), "abc\nghi");
    Editor carets("abc", {Selection::caret(1)});
    EXPECT_EQ(carets.controller.selected_text(), "");
}

// --- history ---

TEST(EditControllerHistory, UndoAndRedoReturnTheChangeAndRestoreSelections) {
    Editor editor("hello", {Selection{1, 3}});
    editor.controller.type("X");
    ASSERT_EQ(editor.primary(), Selection::caret(2));

    const EditOutcome undone = editor.controller.undo();
    EXPECT_EQ(editor.text(), "hello");
    ASSERT_TRUE(undone.change.has_value());
    EXPECT_EQ(undone.change->cause, ChangeCause::undo);
    EXPECT_TRUE(undone.selections_changed);
    EXPECT_EQ(editor.primary(), (Selection{1, 3}));

    const EditOutcome redone = editor.controller.redo();
    EXPECT_EQ(editor.text(), "hXlo");
    EXPECT_EQ(redone.change->cause, ChangeCause::redo);
}

TEST(EditControllerHistory, UndoWithNothingToUndoChangesNothing) {
    Editor editor("abc");
    EXPECT_FALSE(editor.controller.undo().changed_anything());
    EXPECT_FALSE(editor.controller.redo().changed_anything());
}

TEST(EditControllerHistory, TypingAfterUndoDoesNotJoinTheUndoneStep) {
    Editor editor("");
    editor.controller.type("a");
    editor.controller.undo();
    editor.controller.type("b");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "");
    EXPECT_FALSE(editor.document.can_undo());
}

TEST(EditControllerSelect, SelectMakesTheRangeTheOnlySelection) {
    Editor editor("abcdef", {Selection::caret(1), Selection{3, 5}});
    const EditOutcome outcome = editor.controller.select({2, 4});
    EXPECT_TRUE(outcome.selections_changed);
    EXPECT_FALSE(outcome.change.has_value());
    EXPECT_EQ(editor.selections(), (Carets{Selection{2, 4}}));
    EXPECT_FALSE(editor.controller.select({2, 4}).changed_anything());

    editor.controller.select({5, 1});
    EXPECT_EQ(editor.primary(), (Selection{5, 1}));
}

TEST(EditControllerSelect, SelectClampsToTheTextAndToCharacterBoundaries) {
    const std::string text = std::string("a") + eacute + "b";
    Editor editor(text);
    editor.controller.select({2, 100});
    EXPECT_EQ(editor.primary(), (Selection{1, 4}));
}

TEST(EditControllerSelect, SelectForgetsTheColumnVerticalMovementReturnsTo) {
    Editor editor("abcdef\nab\nabcdef", {Selection::caret(5)});
    editor.controller.move(Motion::down);
    editor.controller.select({0, 0});
    editor.controller.move(Motion::down);
    editor.controller.move(Motion::down);
    EXPECT_EQ(editor.primary(), Selection::caret(10));
}

TEST(EditControllerReplaceRanges, AppliesAllEditsAsOneUndoStep) {
    Editor editor("one two one", {Selection::caret(11)});
    const EditOutcome outcome = editor.controller.replace_ranges({{0, 3, "1"}, {8, 11, "111"}});
    EXPECT_EQ(editor.text(), "1 two 111");
    ASSERT_TRUE(outcome.change.has_value());
    EXPECT_EQ(editor.primary(), Selection::caret(9));

    editor.controller.undo();
    EXPECT_EQ(editor.text(), "one two one");
    EXPECT_FALSE(editor.document.can_undo());
}

TEST(EditControllerReplaceRanges, TheGivenSelectionReplacesTheOthers) {
    Editor editor("ab", {Selection::caret(0)});
    editor.controller.replace_ranges({{0, 1, "xyz"}}, Selection{1, 3});
    EXPECT_EQ(editor.text(), "xyzb");
    EXPECT_EQ(editor.selections(), (Carets{Selection{1, 3}}));
}

TEST(EditControllerReplaceRanges, DoesNotJoinTypingBeforeOrAfter) {
    Editor editor("");
    editor.controller.type("a");
    editor.controller.replace_ranges({{0, 1, "b"}});
    editor.controller.type("c");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "b");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "a");
}

TEST(EditControllerReplaceRanges, NothingToReplaceChangesNothing) {
    Editor editor("abc");
    EXPECT_FALSE(editor.controller.replace_ranges({}).changed_anything());
}

TEST(EditControllerReplaceRanges, InvalidEditsChangeNothing) {
    Editor editor("abc");
    EXPECT_FALSE(editor.controller.replace_ranges({{2, 9, "x"}}).changed_anything());
    EXPECT_EQ(editor.text(), "abc");
}

// --- copy, cut and paste of whole lines ---

TEST(EditControllerClipboard, CopiedTextOfASelectionIsTheSelection) {
    Editor editor("hello world", {Selection{6, 11}});
    const CopiedText copied = editor.controller.copied_text();
    EXPECT_EQ(copied.text, "world");
    EXPECT_FALSE(copied.whole_line);
}

TEST(EditControllerClipboard, CopiedTextOfALoneCaretIsItsLineWithABreak) {
    Editor editor("one\ntwo\nthree", {Selection::caret(5)});
    const CopiedText copied = editor.controller.copied_text();
    EXPECT_EQ(copied.text, "two\n");
    EXPECT_TRUE(copied.whole_line);
}

TEST(EditControllerClipboard, TheLastLineWithoutABreakStillGetsOne) {
    Editor editor("one\ntwo\nthree", {Selection::caret(10)});
    EXPECT_EQ(editor.controller.copied_text().text, "three\n");
}

TEST(EditControllerClipboard, AnEmptyLineAndAnEmptyDocumentCopyALineBreak) {
    Editor empty_line("a\n\nb", {Selection::caret(2)});
    EXPECT_EQ(empty_line.controller.copied_text().text, "\n");
    EXPECT_TRUE(empty_line.controller.copied_text().whole_line);
    Editor empty("", {Selection::caret(0)});
    EXPECT_EQ(empty.controller.copied_text().text, "\n");
}

TEST(EditControllerClipboard, ASingleLineDocumentCopiesItsLine) {
    Editor editor("abc", {Selection::caret(1)});
    EXPECT_EQ(editor.controller.copied_text().text, "abc\n");
}

TEST(EditControllerClipboard, LinesAreCopiedByByteNotByCharacterCount) {
    const std::string text = std::string("a\n") + eacute + astral + "\nb";
    Editor editor(text, {Selection::caret(4)});
    EXPECT_EQ(editor.controller.copied_text().text, std::string(eacute) + astral + "\n");
}

TEST(EditControllerClipboard, SeveralCaretsCopyNothing) {
    Editor editor("one\ntwo\nthree", {Selection::caret(1), Selection::caret(5)});
    const CopiedText copied = editor.controller.copied_text();
    EXPECT_EQ(copied.text, "");
    EXPECT_FALSE(copied.whole_line);
}

TEST(EditControllerClipboard, CaretsBesideASelectionAreNotCopied) {
    Editor editor("one\ntwo\nthree", {Selection::caret(1), Selection{5, 7}});
    const CopiedText copied = editor.controller.copied_text();
    EXPECT_EQ(copied.text, "wo");
    EXPECT_FALSE(copied.whole_line);
}

TEST(EditControllerClipboard, DeletingTheCaretLineRemovesItAndItsBreak) {
    Editor editor("one\ntwo\nthree", {Selection::caret(5)});
    const EditOutcome outcome = editor.controller.delete_caret_line();
    EXPECT_EQ(editor.text(), "one\nthree");
    EXPECT_EQ(editor.primary(), Selection::caret(4));
    ASSERT_TRUE(outcome.change.has_value());
    EXPECT_TRUE(outcome.selections_changed);
}

TEST(EditControllerClipboard, DeletingTheFirstLine) {
    Editor editor("one\ntwo\nthree", {Selection::caret(1)});
    editor.controller.delete_caret_line();
    EXPECT_EQ(editor.text(), "two\nthree");
    EXPECT_EQ(editor.primary(), Selection::caret(0));
}

TEST(EditControllerClipboard, DeletingTheLastLineRemovesTheBreakBeforeIt) {
    Editor editor("one\ntwo\nthree", {Selection::caret(10)});
    editor.controller.delete_caret_line();
    EXPECT_EQ(editor.text(), "one\ntwo");
    EXPECT_EQ(editor.primary(), Selection::caret(7));
}

TEST(EditControllerClipboard, DeletingTheEmptyLineAfterATrailingBreak) {
    Editor editor("a\n", {Selection::caret(2)});
    editor.controller.delete_caret_line();
    EXPECT_EQ(editor.text(), "a");
    EXPECT_EQ(editor.primary(), Selection::caret(1));
}

TEST(EditControllerClipboard, DeletingTheOnlyLineEmptiesTheDocument) {
    Editor editor("abc", {Selection::caret(1)});
    editor.controller.delete_caret_line();
    EXPECT_EQ(editor.text(), "");
    EXPECT_EQ(editor.primary(), Selection::caret(0));
    EXPECT_FALSE(editor.controller.delete_caret_line().changed_anything());
}

TEST(EditControllerClipboard, DeletingALineOfMultiByteText) {
    const std::string text = std::string(eacute) + "\n" + astral + "\nb";
    Editor editor(text, {Selection::caret(3)});
    editor.controller.delete_caret_line();
    EXPECT_EQ(editor.text(), std::string(eacute) + "\nb");
}

TEST(EditControllerClipboard, DeletingALineNeedsOneEmptyCaret) {
    Editor selection("one\ntwo", {Selection{1, 2}});
    EXPECT_FALSE(selection.controller.delete_caret_line().changed_anything());
    Editor carets("one\ntwo", {Selection::caret(1), Selection::caret(5)});
    EXPECT_FALSE(carets.controller.delete_caret_line().changed_anything());
    EXPECT_EQ(carets.text(), "one\ntwo");
}

TEST(EditControllerClipboard, DeletingALineIsOneUndoStep) {
    Editor editor("one\ntwo\nthree", {Selection::caret(5)});
    editor.controller.delete_caret_line();
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "one\ntwo\nthree");
    EXPECT_FALSE(editor.document.can_undo());
}

TEST(EditControllerClipboard, ALinePastesAboveTheCaretLineAndTheCaretKeepsItsColumn) {
    Editor editor("one\ntwo", {Selection::caret(5)});
    const EditOutcome outcome = editor.controller.paste("new\n", true);
    EXPECT_EQ(editor.text(), "one\nnew\ntwo");
    EXPECT_EQ(editor.primary(), Selection::caret(9));
    ASSERT_TRUE(outcome.change.has_value());
}

TEST(EditControllerClipboard, ALinePastesAboveAtColumnZeroAndOnTheLastLine) {
    Editor start("one\ntwo", {Selection::caret(4)});
    start.controller.paste("new\n", true);
    EXPECT_EQ(start.text(), "one\nnew\ntwo");
    EXPECT_EQ(start.primary(), Selection::caret(8));

    Editor last("one\ntwo", {Selection::caret(7)});
    last.controller.paste("new\n", true);
    EXPECT_EQ(last.text(), "one\nnew\ntwo");
    EXPECT_EQ(last.primary(), Selection::caret(11));
}

TEST(EditControllerClipboard, ALinePastesIntoAnEmptyDocument) {
    Editor editor("", {Selection::caret(0)});
    editor.controller.paste("x\n", true);
    EXPECT_EQ(editor.text(), "x\n");
    EXPECT_EQ(editor.primary(), Selection::caret(2));
}

TEST(EditControllerClipboard, ALinePastesAboveEachCaretLine) {
    Editor editor("a\nb\nc", {Selection::caret(0), Selection::caret(4)});
    editor.controller.paste("x\n", true);
    EXPECT_EQ(editor.text(), "x\na\nb\nx\nc");
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(2), Selection::caret(8)}));
}

TEST(EditControllerClipboard, WindowsLineBreaksAreNormalizedBeforeTheCheck) {
    Editor editor("one\ntwo", {Selection::caret(5)});
    editor.controller.paste("new\r\n", true);
    EXPECT_EQ(editor.text(), "one\nnew\ntwo");
}

TEST(EditControllerClipboard, TextThatIsNotExactlyOneLinePastesAtTheCaret) {
    for (const char* text : {"new", "a\nb\n", "a\nb", "\n\n"}) {
        Editor editor("one\ntwo", {Selection::caret(5)});
        editor.controller.paste(text, true);
        std::string expected = "one\nt";
        expected += normalize_line_endings(text);
        expected += "wo";
        EXPECT_EQ(editor.text(), expected) << text;
    }
}

TEST(EditControllerClipboard, ALinePastesAtTheCaretWhenNotMarkedAsALine) {
    Editor editor("one\ntwo", {Selection::caret(5)});
    editor.controller.paste("new\n");
    EXPECT_EQ(editor.text(), "one\nt"
                             "new\n"
                             "wo");
}

TEST(EditControllerClipboard, ALineReplacesASelectionLikeOrdinaryText) {
    Editor selection("one\ntwo", {Selection{4, 7}});
    selection.controller.paste("new\n", true);
    EXPECT_EQ(selection.text(), "one\nnew\n");

    Editor mixed("one\ntwo", {Selection::caret(1), Selection{4, 7}});
    mixed.controller.paste("new\n", true);
    EXPECT_EQ(mixed.text(), "onew\nne\nnew\n");
}

TEST(EditControllerClipboard, APastedLineIsOneUndoStepThatDoesNotJoinTyping) {
    Editor editor("one\ntwo", {Selection::caret(7)});
    editor.controller.type("!");
    editor.controller.paste("new\n", true);
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "one\ntwo!");
    editor.controller.undo();
    EXPECT_EQ(editor.text(), "one\ntwo");
}

// --- the word at a caret ---

namespace {

std::optional<Selection> word_in(std::string_view text, std::size_t offset) {
    return word_at(*Text::from_utf8(text), offset);
}

} // namespace

TEST(WordAt, ACaretInsideAWordGivesTheWholeWord) {
    EXPECT_EQ(word_in("foo bar_1 baz", 6), (Selection{4, 9}));
    EXPECT_EQ(word_in("foo bar_1 baz", 4), (Selection{4, 9}));
    EXPECT_EQ(word_in("foo bar_1 baz", 9), (Selection{4, 9}));
}

TEST(WordAt, ACaretBesideAWordOnEitherSideGivesThatWord) {
    EXPECT_EQ(word_in("foo.bar", 3), (Selection{0, 3}));
    EXPECT_EQ(word_in("foo.bar", 4), (Selection{4, 7}));
    EXPECT_EQ(word_in("(foo)", 1), (Selection{1, 4}));
    EXPECT_EQ(word_in("(foo)", 4), (Selection{1, 4}));
}

TEST(WordAt, ACaretNotInOrBesideAWordGivesNothing) {
    EXPECT_EQ(word_in("foo  bar", 4), std::nullopt);
    EXPECT_EQ(word_in("a . b", 3), std::nullopt);
    EXPECT_EQ(word_in("", 0), std::nullopt);
    EXPECT_EQ(word_in("   ", 1), std::nullopt);
    EXPECT_EQ(word_in("\n\n", 1), std::nullopt);
}

TEST(WordAt, TheWordNeverCrossesALineBreak) {
    EXPECT_EQ(word_in("ab\ncd", 2), (Selection{0, 2}));
    EXPECT_EQ(word_in("ab\ncd", 3), (Selection{3, 5}));
    EXPECT_EQ(word_in("ab\n\ncd", 3), std::nullopt);
}

TEST(WordAt, NonAsciiCharactersAreWordCharacters) {
    const std::string text = std::string("x ") + eacute + "t\xE2\x82\xAC y";
    EXPECT_EQ(word_in(text, 4), (Selection{2, 8}));
    // An offset inside a character moves back to its start.
    EXPECT_EQ(word_in(text, 3), (Selection{2, 8}));
}

TEST(WordAt, AnOffsetPastTheEndIsTheEnd) {
    EXPECT_EQ(word_in("foo", 99), (Selection{0, 3}));
}

// Both find a run of one class around a character; they differ only in which character a caret
// at the edge of a word is taken to be on, so they agree wherever the character at the offset is
// a word character.
TEST(WordAt, AgreesWithSelectingAWordByDoubleClickOnAWordCharacter) {
    const std::string text = "foo bar_baz  +-  qux \xC3\xA9\xE2\x82\xAC end\nlast";
    Editor editor(text);
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        if (!editor.document.text().is_char_boundary(offset) ||
            !is_word_character(static_cast<unsigned char>(text[offset]) < 0x80
                                   ? static_cast<char32_t>(text[offset])
                                   : U'\u00e9')) {
            continue;
        }
        const auto word = word_at(editor.document.text(), offset);
        ASSERT_TRUE(word.has_value()) << offset;
        editor.controller.select_word_at(offset);
        EXPECT_EQ(editor.primary(), *word) << offset;
    }
}

// --- occurrences ---

namespace {

Search search_for(std::string pattern, bool match_case = true, bool whole_word = false,
                  bool regex = false) {
    auto search = Search::create({.pattern = std::move(pattern),
                                  .match_case = match_case,
                                  .whole_word = whole_word,
                                  .regex = regex});
    EXPECT_TRUE(search.has_value());
    return *search;
}

Search search_for(const OccurrenceQuery& occurrence) {
    auto search = Search::create(occurrence.query);
    EXPECT_TRUE(search.has_value());
    return *search;
}

} // namespace

TEST(EditControllerOccurrences, ALoneCaretTakesTheWordAtItAsAWholeCaseSensitiveSearch) {
    Editor editor("foo bar", {Selection::caret(5)});
    const auto occurrence = editor.controller.occurrence_query(false, false);
    ASSERT_TRUE(occurrence.has_value());
    EXPECT_EQ(occurrence->query.pattern, "bar");
    EXPECT_TRUE(occurrence->query.whole_word);
    EXPECT_TRUE(occurrence->query.match_case);
    EXPECT_FALSE(occurrence->query.regex);
    EXPECT_EQ(occurrence->first, (Selection{4, 7}));
}

TEST(EditControllerOccurrences, ACaretBetweenWordsHasNothingToSearchFor) {
    Editor editor("foo  bar", {Selection::caret(4)});
    EXPECT_FALSE(editor.controller.occurrence_query(false, false).has_value());
    Editor punctuation("a + b", {Selection::caret(2)});
    EXPECT_FALSE(punctuation.controller.occurrence_query(false, false).has_value());
}

TEST(EditControllerOccurrences, ASelectionSearchesItsTextWithTheGivenOptions) {
    Editor editor("foo Bar baz", {Selection{4, 7}});
    const auto plain = editor.controller.occurrence_query(false, false);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->query.pattern, "Bar");
    EXPECT_FALSE(plain->query.match_case);
    EXPECT_FALSE(plain->query.whole_word);
    EXPECT_FALSE(plain->first.has_value());

    const auto strict = editor.controller.occurrence_query(true, true);
    ASSERT_TRUE(strict.has_value());
    EXPECT_TRUE(strict->query.match_case);
    EXPECT_TRUE(strict->query.whole_word);
}

TEST(EditControllerOccurrences, AReversedMultiLineSelectionSearchesItsText) {
    Editor editor("ab\ncd ab\ncd", {Selection{5, 0}});
    const auto occurrence = editor.controller.occurrence_query(true, false);
    ASSERT_TRUE(occurrence.has_value());
    EXPECT_EQ(occurrence->query.pattern, "ab\ncd");
    Editor target("ab\ncd ab\ncd", {Selection{0, 5}});
    EXPECT_TRUE(target.controller.add_next_occurrence(search_for(*occurrence)).changed_anything());
    EXPECT_EQ(target.primary(), (Selection{6, 11}));
}

TEST(EditControllerOccurrences, ACaretThatIsNotAloneUsesTheOptionsForTheWordAtIt) {
    Editor editor("foo foo", {Selection::caret(1), Selection::caret(5)});
    const auto occurrence = editor.controller.occurrence_query(false, false);
    ASSERT_TRUE(occurrence.has_value());
    EXPECT_EQ(occurrence->query.pattern, "foo");
    EXPECT_FALSE(occurrence->query.whole_word);
    EXPECT_FALSE(occurrence->query.match_case);
}

TEST(EditControllerOccurrences, AddsTheNextMatchAfterThePrimarySelectionAndMakesItPrimary) {
    Editor editor("foo bar foo baz foo", {Selection{0, 3}});
    const Search search = search_for("foo");
    const EditOutcome first = editor.controller.add_next_occurrence(search);
    EXPECT_TRUE(first.selections_changed);
    EXPECT_FALSE(first.change.has_value());
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 3}, Selection{8, 11}}));
    EXPECT_EQ(editor.primary(), (Selection{8, 11}));

    editor.controller.add_next_occurrence(search);
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 3}, Selection{8, 11}, Selection{16, 19}}));
    EXPECT_EQ(editor.primary(), (Selection{16, 19}));
}

TEST(EditControllerOccurrences, WrapsAroundTheDocumentAndStopsWhenEveryMatchIsTaken) {
    Editor editor("foo bar foo", {Selection{8, 11}});
    const Search search = search_for("foo");
    editor.controller.add_next_occurrence(search);
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 3}, Selection{8, 11}}));
    EXPECT_EQ(editor.primary(), (Selection{0, 3}));

    const EditOutcome none = editor.controller.add_next_occurrence(search);
    EXPECT_FALSE(none.changed_anything());
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 3}, Selection{8, 11}}));
}

TEST(EditControllerOccurrences, DoesNothingWhenTheOnlyMatchIsTheSelection) {
    Editor editor("foo bar", {Selection{0, 3}});
    EXPECT_FALSE(editor.controller.add_next_occurrence(search_for("foo")).changed_anything());
    EXPECT_FALSE(editor.controller.add_next_occurrence(search_for("zzz")).changed_anything());
}

TEST(EditControllerOccurrences, SkipsMatchesThatOverlapASelection) {
    // "aa" matches at 0, 3 and 6; the second selection covers the last two.
    Editor editor("aa aa aa aa", {Selection{0, 2}, Selection{3, 8}});
    editor.controller.add_next_occurrence(search_for("aa"));
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 2}, Selection{3, 8}, Selection{9, 11}}));
    EXPECT_EQ(editor.primary(), (Selection{9, 11}));

    // With every match overlapped or taken, nothing is added.
    EXPECT_FALSE(editor.controller.add_next_occurrence(search_for("aa")).changed_anything());
}

TEST(EditControllerOccurrences, AMatchTouchingASelectionIsStillAdded) {
    Editor editor("aaaa", {Selection{0, 2}});
    editor.controller.add_next_occurrence(search_for("aa"));
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 2}, Selection{2, 4}}));
}

TEST(EditControllerOccurrences, AMatchTouchingACaretIsNotAdded) {
    Editor editor("aa aa", {Selection{0, 2}, Selection::caret(3)});
    // The caret at 3 is the start of the second match.
    EXPECT_FALSE(editor.controller.add_next_occurrence(search_for("aa")).changed_anything());
}

TEST(EditControllerOccurrences, WholeWordMatchingSkipsMatchesInsideWords) {
    Editor editor("foo foobar foo", {Selection{0, 3}});
    const auto occurrence = editor.controller.occurrence_query(true, true);
    ASSERT_TRUE(occurrence.has_value());
    editor.controller.add_next_occurrence(search_for(*occurrence));
    EXPECT_EQ(editor.primary(), (Selection{11, 14}));

    Editor loose("foo foobar foo", {Selection{0, 3}});
    loose.controller.add_next_occurrence(search_for("foo", true, false));
    EXPECT_EQ(loose.primary(), (Selection{4, 7}));
}

TEST(EditControllerOccurrences, CaseSensitivityFollowsTheSearch) {
    Editor sensitive("foo Foo foo", {Selection{0, 3}});
    sensitive.controller.add_next_occurrence(search_for("foo", true));
    EXPECT_EQ(sensitive.primary(), (Selection{8, 11}));

    Editor insensitive("foo Foo foo", {Selection{0, 3}});
    insensitive.controller.add_next_occurrence(search_for("foo", false));
    EXPECT_EQ(insensitive.primary(), (Selection{4, 7}));
}

TEST(EditControllerOccurrences, WorksOnMultiByteText) {
    const std::string text =
        std::string(eacute) + "x " + eacute + "x " + astral + " " + eacute + "x";
    Editor editor(text, {Selection{0, 3}});
    const Search search = search_for(std::string(eacute) + "x");
    editor.controller.add_next_occurrence(search);
    EXPECT_EQ(editor.primary(), (Selection{4, 7}));
    editor.controller.add_next_occurrence(search);
    EXPECT_EQ(editor.primary(), (Selection{13, 16}));
}

TEST(EditControllerOccurrences, AddSelectionMakesTheNewSelectionPrimaryAndMergesOverlaps) {
    Editor editor("foo bar", {Selection::caret(1)});
    editor.controller.add_selection(Selection{0, 3});
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 3}}));
    editor.controller.add_selection(Selection{4, 7});
    EXPECT_EQ(editor.primary(), (Selection{4, 7}));
    EXPECT_EQ(editor.selections().size(), 2U);
}

TEST(EditControllerOccurrences, SelectsEveryMatchAndKeepsTheOneAtThePrimarySelectionPrimary) {
    Editor editor("foo bar foo baz foo", {Selection::caret(9)});
    const auto result = editor.controller.select_all_occurrences(search_for("foo"), 100);
    EXPECT_TRUE(result.outcome.selections_changed);
    EXPECT_FALSE(result.outcome.change.has_value());
    EXPECT_FALSE(result.truncated);
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 3}, Selection{8, 11}, Selection{16, 19}}));
    EXPECT_EQ(editor.primary(), (Selection{8, 11}));
}

TEST(EditControllerOccurrences, SelectingAllMatchesIgnoresTheCurrentSelectionsAndHonorsTheLimit) {
    Editor editor("a a a a a", {Selection::caret(0), Selection::caret(4)});
    const auto result = editor.controller.select_all_occurrences(search_for("a"), 3);
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 1}, Selection{2, 3}, Selection{4, 5}}));
}

TEST(EditControllerOccurrences, SelectingAllMatchesTakesARegularExpression) {
    Editor editor("a1 b22 c333", {Selection::caret(0)});
    editor.controller.select_all_occurrences(search_for("\\d+", true, false, true), 100);
    EXPECT_EQ(editor.selections(), (Carets{Selection{1, 2}, Selection{4, 6}, Selection{8, 11}}));
}

TEST(EditControllerOccurrences, SelectingAllMatchesDoesNothingWithoutAMatch) {
    Editor editor("abc", {Selection::caret(1), Selection::caret(2)});
    const auto result = editor.controller.select_all_occurrences(search_for("zzz"), 100);
    EXPECT_FALSE(result.outcome.changed_anything());
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(1), Selection::caret(2)}));
}

TEST(EditControllerOccurrences, SelectionsShareTextWhenEachHasTheTextOfTheFirst) {
    Editor same("foo bar foo", {Selection{0, 3}, Selection{8, 11}});
    EXPECT_TRUE(same.controller.selections_share_text(true));
    Editor different("foo bar foo", {Selection{0, 3}, Selection{4, 7}});
    EXPECT_FALSE(different.controller.selections_share_text(true));
    Editor cased("foo Foo", {Selection{0, 3}, Selection{4, 7}});
    EXPECT_FALSE(cased.controller.selections_share_text(true));
    EXPECT_TRUE(cased.controller.selections_share_text(false));
    Editor with_caret("foo foo", {Selection{0, 3}, Selection::caret(5)});
    EXPECT_FALSE(with_caret.controller.selections_share_text(false));
    Editor alone("foo", {Selection::caret(1)});
    EXPECT_TRUE(alone.controller.selections_share_text(true));
}

TEST(EditControllerOccurrences, CaretsGrowToTheirWordsAndOtherSelectionsStay) {
    Editor editor("foo bar  baz", {Selection::caret(1), Selection::caret(8), Selection{9, 11}});
    editor.controller.expand_carets_to_words();
    EXPECT_EQ(editor.selections(),
              (Carets{Selection{0, 3}, Selection::caret(8), Selection{9, 11}}));
}

TEST(EditControllerOccurrences, NoCommandChangesTheText) {
    Editor editor("foo foo foo", {Selection{0, 3}});
    editor.controller.add_next_occurrence(search_for("foo"));
    editor.controller.select_all_occurrences(search_for("foo"), 10);
    EXPECT_EQ(editor.text(), "foo foo foo");
    EXPECT_FALSE(editor.controller.undo().changed_anything());
}
