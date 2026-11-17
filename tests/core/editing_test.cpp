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
