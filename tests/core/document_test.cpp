#include "anvil/core/document.hpp"
#include "document_support.hpp"

#include <gtest/gtest.h>

using namespace anvil::core;
using anvil::ErrorCode;
using anvil::core::testing::replay_changes;

namespace {

Document document_of(std::string_view utf8) {
    return Document(*Text::from_utf8(utf8));
}

std::string text_of(const Document& document) {
    return document.text().to_string();
}

DocumentChange apply(Document& document, std::vector<Edit> edits, EditOptions options = {}) {
    auto change = document.apply_edits(edits, std::move(options));
    EXPECT_TRUE(change.has_value()) << (change ? "" : change.error().message);
    return change ? std::move(*change) : DocumentChange{};
}

DocumentChange undo(Document& document) {
    auto change = document.undo();
    EXPECT_TRUE(change.has_value() && change->has_value());
    return change && *change ? std::move(**change) : DocumentChange{};
}

DocumentChange redo(Document& document) {
    auto change = document.redo();
    EXPECT_TRUE(change.has_value() && change->has_value());
    return change && *change ? std::move(**change) : DocumentChange{};
}

} // namespace

// --- edits ---

TEST(Document, AppliesASingleEditAndDescribesIt) {
    Document document = document_of("hello world");
    EXPECT_EQ(document.version(), 1u);

    const DocumentChange change = apply(document, {{6, 11, "there"}});
    EXPECT_EQ(text_of(document), "hello there");
    EXPECT_EQ(document.version(), 2u);
    EXPECT_EQ(change.version, 2u);
    EXPECT_EQ(change.cause, ChangeCause::edit);
    ASSERT_EQ(change.changes.size(), 1u);
    EXPECT_EQ(change.changes[0].offset, 6u);
    EXPECT_EQ(change.changes[0].removed_bytes, 5u);
    EXPECT_EQ(change.changes[0].text, "there");
}

TEST(Document, BatchChangesRunFromTheEndOfTheTextToTheStart) {
    Document document = document_of("abcdef");
    const Text before = document.text();
    const DocumentChange change = apply(document, {{1, 2, "X"}, {4, 5, "YY"}});

    EXPECT_EQ(text_of(document), "aXcdYYf");
    ASSERT_EQ(change.changes.size(), 2u);
    EXPECT_EQ(change.changes[0].offset, 4u);
    EXPECT_EQ(change.changes[1].offset, 1u);
    EXPECT_EQ(replay_changes(before, change.changes).to_string(), "aXcdYYf");
}

TEST(Document, EditsMayBeGivenInAnyOrder) {
    Document document = document_of("abcdef");
    apply(document, {{4, 5, "Y"}, {1, 2, "X"}});
    EXPECT_EQ(text_of(document), "aXcdYf");
}

TEST(Document, EditsAtTheSameOffsetKeepTheOrderGiven) {
    Document document = document_of("abcd");
    apply(document, {{2, 2, "A"}, {2, 2, "B"}});
    EXPECT_EQ(text_of(document), "abABcd");
}

TEST(Document, AnInsertionMayTouchAReplacedRange) {
    Document document = document_of("abcd");
    apply(document, {{2, 2, "X"}, {2, 4, "Y"}});
    EXPECT_EQ(text_of(document), "abXY");
}

TEST(Document, RejectsOverlappingEdits) {
    Document document = document_of("abcdef");
    for (const auto& edits : {std::vector<Edit>{{1, 3, "x"}, {2, 4, "y"}},
                              std::vector<Edit>{{1, 3, "x"}, {2, 2, "y"}}}) {
        const auto result = document.apply_edits(edits);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
    }
    EXPECT_EQ(text_of(document), "abcdef");
    EXPECT_EQ(document.version(), 1u);
    EXPECT_FALSE(document.can_undo());
}

TEST(Document, RejectsInvalidEditsWithoutChangingAnything) {
    Document document = document_of("a\xE2\x82\xAC");
    const std::vector<std::vector<Edit>> bad = {
        {{3, 2, "x"}},
        {{0, 99, "x"}},
        {{2, 3, "x"}},
        {{0, 0, "\xFF"}},
        {{0, 1, "ok"}, {1, 99, "x"}},
    };
    for (const auto& edits : bad) {
        EXPECT_FALSE(document.apply_edits(edits).has_value());
    }
    EXPECT_EQ(document.text().size(), 4u);
    EXPECT_EQ(document.version(), 1u);
    EXPECT_FALSE(document.can_undo());
}

TEST(Document, AnEmptyBatchChangesNothing) {
    Document document = document_of("abc");
    const auto change = document.apply_edits({});
    ASSERT_TRUE(change.has_value());
    EXPECT_TRUE(change->changes.empty());
    EXPECT_EQ(document.version(), 1u);
    EXPECT_FALSE(document.can_undo());
}

TEST(Document, NormalizesLineBreaksInInsertedText) {
    Document document = document_of("ab");
    const DocumentChange change = apply(document, {{1, 1, "x\r\ny\rz"}});
    EXPECT_EQ(text_of(document), "ax\ny\nzb");
    EXPECT_EQ(change.changes[0].text, "x\ny\nz");
}

// --- selections ---

TEST(DocumentSelections, StartAtTheBeginningOfTheText) {
    EXPECT_EQ(document_of("abc").selections(), SelectionSet{});
}

TEST(DocumentSelections, SetSelectionsValidatesAgainstTheText) {
    Document document = document_of("a\xE2\x82\xAC");
    EXPECT_TRUE(document.set_selections(SelectionSet::from({{1, 4}})).has_value());
    EXPECT_FALSE(document.set_selections(SelectionSet::from({{0, 5}})).has_value());
    EXPECT_FALSE(document.set_selections(SelectionSet::from({{2, 2}})).has_value());
    EXPECT_EQ(document.selections(), SelectionSet::from({{1, 4}}));
}

TEST(DocumentSelections, MoveWithEditsBeforeThem) {
    Document document = document_of("hello world");
    ASSERT_TRUE(document.set_selections(SelectionSet::from({{6, 11}})).has_value());
    apply(document, {{0, 0, "XX"}});
    EXPECT_EQ(document.selections().primary(), (Selection{8, 13}));
}

TEST(DocumentSelections, ACaretFollowsTextInsertedAtIt) {
    Document document = document_of("hello");
    ASSERT_TRUE(document.set_selections(SelectionSet::from({{5, 5}})).has_value());
    apply(document, {{5, 5, "!"}});
    EXPECT_EQ(document.selections().primary(), Selection::caret(6));
}

TEST(DocumentSelections, ARangeGrowsWhenTextIsInsertedAtItsEdges) {
    Document document = document_of("hello world");
    ASSERT_TRUE(document.set_selections(SelectionSet::from({{5, 8}})).has_value());
    apply(document, {{5, 5, "A"}});
    EXPECT_EQ(document.selections().primary(), (Selection{5, 9}));
    apply(document, {{9, 9, "B"}});
    EXPECT_EQ(document.selections().primary(), (Selection{5, 10}));
}

TEST(DocumentSelections, CollapseWhenTheirTextIsDeleted) {
    Document document = document_of("0123456789");
    ASSERT_TRUE(document.set_selections(SelectionSet::from({{7, 7}})).has_value());
    apply(document, {{5, 10, ""}});
    EXPECT_EQ(document.selections().primary(), Selection::caret(5));
}

TEST(DocumentSelections, EndInsideReplacedTextMovesToTheEndOfTheReplacement) {
    Document document = document_of("0123456789");
    ASSERT_TRUE(document.set_selections(SelectionSet::from({{2, 8}})).has_value());
    apply(document, {{5, 10, "Z"}});
    EXPECT_EQ(document.selections().primary(), (Selection{2, 6}));
}

TEST(DocumentSelections, KeepTheirDirection) {
    Document document = document_of("hello world");
    ASSERT_TRUE(document.set_selections(SelectionSet::from({{8, 2}})).has_value());
    apply(document, {{0, 0, "ab"}});
    EXPECT_EQ(document.selections().primary(), (Selection{10, 4}));
}

TEST(DocumentSelections, EveryCursorMovesInAMultiCursorEdit) {
    Document document = document_of("a b c");
    ASSERT_TRUE(document.set_selections(SelectionSet::from({{1, 1}, {3, 3}, {5, 5}})).has_value());
    apply(document, {{1, 1, "!"}, {3, 3, "!"}, {5, 5, "!"}});
    EXPECT_EQ(text_of(document), "a! b! c!");
    EXPECT_EQ(
        document.selections().all(),
        (std::vector<Selection>{Selection::caret(2), Selection::caret(5), Selection::caret(8)}));
}

TEST(DocumentSelections, AnEditCanSetTheSelectionsAfterward) {
    Document document = document_of("abc");
    EditOptions options;
    options.selections_after = SelectionSet::from({{1, 3}});
    apply(document, {{0, 0, "xy"}}, options);
    EXPECT_EQ(document.selections().primary(), (Selection{1, 3}));
}

TEST(DocumentSelections, ARejectedSelectionAfterRollsTheEditBack) {
    Document document = document_of("abc");
    EditOptions options;
    options.selections_after = SelectionSet::from({{99, 99}});
    const auto result = document.apply_edits(std::vector<Edit>{{0, 0, "xy"}}, options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(text_of(document), "abc");
    EXPECT_EQ(document.version(), 1u);
    EXPECT_FALSE(document.can_undo());
}

// --- undo and redo ---

TEST(DocumentHistory, UndoAndRedoWalkTheEdits) {
    Document document = document_of("one");
    apply(document, {{3, 3, " two"}});
    apply(document, {{7, 7, " three"}});
    ASSERT_EQ(text_of(document), "one two three");

    const DocumentChange first = undo(document);
    EXPECT_EQ(text_of(document), "one two");
    EXPECT_EQ(first.cause, ChangeCause::undo);
    undo(document);
    EXPECT_EQ(text_of(document), "one");
    EXPECT_FALSE(document.can_undo());

    const DocumentChange again = redo(document);
    EXPECT_EQ(again.cause, ChangeCause::redo);
    EXPECT_EQ(text_of(document), "one two");
    redo(document);
    EXPECT_EQ(text_of(document), "one two three");
    EXPECT_FALSE(document.can_redo());
}

TEST(DocumentHistory, NothingToUndoOrRedoReturnsNoChange) {
    Document document = document_of("x");
    const auto undone = document.undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_FALSE(undone->has_value());
    const auto redone = document.redo();
    ASSERT_TRUE(redone.has_value());
    EXPECT_FALSE(redone->has_value());
    EXPECT_EQ(document.version(), 1u);
}

TEST(DocumentHistory, UndoAndRedoAreChangesWithNewVersions) {
    Document document = document_of("a");
    apply(document, {{1, 1, "b"}});
    EXPECT_EQ(document.version(), 2u);
    EXPECT_EQ(undo(document).version, 3u);
    EXPECT_EQ(redo(document).version, 4u);
}

TEST(DocumentHistory, UndoAndRedoChangesReplayOnTheTextBeforeThem) {
    Document document = document_of("abcdefgh");
    apply(document, {{1, 2, "XX"}, {4, 6, ""}, {8, 8, "!"}});
    const Text edited = document.text();

    const DocumentChange undone = undo(document);
    EXPECT_EQ(text_of(document), "abcdefgh");
    EXPECT_EQ(replay_changes(edited, undone.changes).to_string(), "abcdefgh");

    const DocumentChange redone = redo(document);
    EXPECT_EQ(text_of(document), edited.to_string());
    EXPECT_EQ(replay_changes(*Text::from_utf8("abcdefgh"), redone.changes).to_string(),
              edited.to_string());
}

TEST(DocumentHistory, UndoRestoresTheSelectionsFromBeforeTheEdit) {
    Document document = document_of("hello");
    ASSERT_TRUE(document.set_selections(SelectionSet::from({{1, 3}})).has_value());
    EditOptions options;
    options.selections_after = SelectionSet::from({{7, 7}});
    apply(document, {{0, 0, "XX"}}, options);
    EXPECT_EQ(document.selections().primary(), Selection::caret(7));

    undo(document);
    EXPECT_EQ(document.selections().primary(), (Selection{1, 3}));
    redo(document);
    EXPECT_EQ(document.selections().primary(), Selection::caret(7));
}

TEST(DocumentHistory, JoinedEditsUndoAndRedoTogether) {
    Document document = document_of("");
    apply(document, {{0, 0, "a"}});
    apply(document, {{1, 1, "b"}}, EditOptions{.join_previous = true});
    apply(document, {{2, 2, "c"}}, EditOptions{.join_previous = true});
    apply(document, {{3, 3, "!"}});
    ASSERT_EQ(text_of(document), "abc!");

    const DocumentChange first = undo(document);
    EXPECT_EQ(text_of(document), "abc");
    EXPECT_EQ(first.changes.size(), 1u);

    const DocumentChange second = undo(document);
    EXPECT_EQ(text_of(document), "");
    EXPECT_EQ(second.changes.size(), 3u);
    EXPECT_FALSE(document.can_undo());

    redo(document);
    EXPECT_EQ(text_of(document), "abc");
    redo(document);
    EXPECT_EQ(text_of(document), "abc!");
}

TEST(DocumentHistory, JoiningWithNothingBeforeStartsAGroup) {
    Document document = document_of("");
    apply(document, {{0, 0, "a"}}, EditOptions{.join_previous = true});
    undo(document);
    EXPECT_EQ(text_of(document), "");
}

TEST(DocumentHistory, ANewEditDiscardsTheRedoHistory) {
    Document document = document_of("a");
    apply(document, {{1, 1, "b"}});
    undo(document);
    ASSERT_TRUE(document.can_redo());
    apply(document, {{1, 1, "c"}});
    EXPECT_FALSE(document.can_redo());
    EXPECT_EQ(text_of(document), "ac");
}

TEST(DocumentHistory, UndoingAHugeDeletionRestoresItAndStaysBalanced) {
    std::string big;
    for (int i = 0; i < 30000; ++i) {
        big += "line " + std::to_string(i) + "\n";
    }
    Document document = document_of(big);
    apply(document, {{10, big.size() - 10, ""}});
    ASSERT_EQ(document.text().size(), 20u);
    undo(document);
    EXPECT_EQ(text_of(document), big);
    EXPECT_FALSE(document.text().check_invariants().has_value());
}

// --- dirty state ---

TEST(DocumentDirty, TracksEditsUndoAndSaves) {
    Document document = document_of("a");
    EXPECT_FALSE(document.is_dirty());

    apply(document, {{1, 1, "b"}});
    EXPECT_TRUE(document.is_dirty());
    undo(document);
    EXPECT_FALSE(document.is_dirty());
    redo(document);
    EXPECT_TRUE(document.is_dirty());

    document.mark_saved();
    EXPECT_FALSE(document.is_dirty());
    apply(document, {{2, 2, "c"}});
    EXPECT_TRUE(document.is_dirty());
    undo(document);
    EXPECT_FALSE(document.is_dirty());
}

TEST(DocumentDirty, RevertingByHandStaysDirty) {
    Document document = document_of("a");
    apply(document, {{1, 1, "b"}});
    apply(document, {{1, 2, ""}});
    EXPECT_EQ(text_of(document), "a");
    EXPECT_TRUE(document.is_dirty());
}

TEST(DocumentDirty, AnEditAfterSavingNeverJoinsTheSavedGroup) {
    Document document = document_of("");
    apply(document, {{0, 0, "a"}});
    document.mark_saved();
    apply(document, {{1, 1, "b"}}, EditOptions{.join_previous = true});
    EXPECT_TRUE(document.is_dirty());

    undo(document);
    EXPECT_EQ(text_of(document), "a");
    EXPECT_FALSE(document.is_dirty());
}

TEST(DocumentDirty, UndoingPastTheSavePointIsDirty) {
    Document document = document_of("a");
    apply(document, {{1, 1, "b"}});
    document.mark_saved();
    undo(document);
    EXPECT_TRUE(document.is_dirty());
    redo(document);
    EXPECT_FALSE(document.is_dirty());
}

// --- files ---

TEST(DocumentFile, LoadsAndSavesInTheOriginalFormat) {
    auto document = Document::from_file_bytes("\xEF\xBB\xBF"
                                              "one\r\ntwo\r\n");
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(document->line_ending(), LineEnding::crlf);
    EXPECT_TRUE(document->has_bom());
    EXPECT_EQ(document->text().to_string(), "one\ntwo\n");

    apply(*document, {{3, 3, "!"}});
    EXPECT_EQ(document->to_file_bytes(), "\xEF\xBB\xBF"
                                         "one!\r\ntwo\r\n");
}

TEST(DocumentFile, RejectsFilesThatAreNotUtf8) {
    const auto document = Document::from_file_bytes("\xFF\xFE");
    ASSERT_FALSE(document.has_value());
    EXPECT_EQ(document.error().code, ErrorCode::invalid_encoding);
}

TEST(DocumentFile, ExtensionOffsetsCountLineBreaksAtTheirFileWidth) {
    auto crlf = Document::from_file_bytes("a\r\nb\r\n\xF0\x9F\x98\x80z");
    ASSERT_TRUE(crlf.has_value());
    // text "a\nb\n😀z": the 'z' is at byte 8, UTF-16 offset 6 without line breaks counted wider
    EXPECT_EQ(crlf->extension_offset(0), 0u);
    EXPECT_EQ(crlf->extension_offset(2), 3u);
    EXPECT_EQ(crlf->extension_offset(8), 6u + 2u);

    const Document lf = document_of("a\nb\n");
    EXPECT_EQ(lf.extension_offset(2), 2u);
}
