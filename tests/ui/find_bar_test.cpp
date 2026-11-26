#include "anvil/ui/find_bar.hpp"
#include "anvil/ui/scroll_bar.hpp"

#include <QAction>
#include <QApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTest>
#include <QThreadPool>
#include <QToolButton>
#include <gtest/gtest.h>

using namespace anvil;
using anvil::ui::EditorView;
using anvil::ui::FindBar;

namespace {

std::shared_ptr<core::Document> make_document(std::string_view text) {
    return std::make_shared<core::Document>(*core::Text::from_utf8(text));
}

class FindBarTest : public ::testing::Test {
protected:
    FindBarTest() : bar_(&view_) {}

    void open(std::string_view text, std::size_t caret = 0) {
        document_ = make_document(text);
        view_.resize(640, 320);
        view_.set_document(document_);
        view_.show();
        view_.select_range({caret, caret});
        bar_.open();
        // Opening seeds the field from the word at the caret; the tests that use this helper start
        // from an empty field, as when nothing at the caret can seed it.
        bar_.find_field()->clear();
        QApplication::processEvents();
    }

    // Shows `text` with the primary selection at `selection`, leaving the bar as it is.
    void show_text(std::string_view text, core::Selection selection) {
        document_ = make_document(text);
        view_.resize(640, 320);
        view_.set_document(document_);
        view_.show();
        view_.select_range(selection);
        // The editor has the focus, as when the user presses Ctrl+F in it.
        view_.activateWindow();
        view_.setFocus();
        QApplication::processEvents();
    }

    // The color in the middle of the top edge of a part of the bar.
    static QColor top_edge(QWidget* part) {
        const QImage image = part->grab().toImage();
        return image.pixelColor(image.width() / 2, 0);
    }
    static QColor error_color() {
        return anvil::ui::WorkbenchColors(QApplication::palette())
            .color("inputValidation.errorBorder");
    }

    void find(const QString& pattern) {
        bar_.find_field()->setText(pattern);
    }

    void press(QWidget* target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        QApplication::sendEvent(target, &event);
    }

    void toggle(QAbstractButton* button) {
        button->setChecked(!button->isChecked());
    }

    std::string text() const {
        return document_->text().to_string();
    }
    core::Selection primary() const {
        return document_->selections().primary();
    }
    std::string selected() const {
        const core::Selection selection = primary();
        return document_->text().slice(selection.start(), selection.end() - selection.start());
    }

    bool wait_until_done() {
        return QTest::qWaitFor([&] { return !bar_.is_searching(); }, 20000);
    }

    EditorView view_;
    FindBar bar_;
    std::shared_ptr<core::Document> document_;
};

std::size_t count_occurrences(const std::string& text, const std::string& pattern) {
    std::size_t count = 0;
    for (std::size_t at = text.find(pattern); at != std::string::npos;
         at = text.find(pattern, at + pattern.size())) {
        ++count;
    }
    return count;
}

} // namespace

TEST_F(FindBarTest, TheBarIsHiddenUntilOpened) {
    EXPECT_TRUE(bar_.isHidden());
    bar_.open();
    EXPECT_FALSE(bar_.isHidden());
    EXPECT_FALSE(bar_.replace_field()->isVisible());
}

TEST_F(FindBarTest, OpeningWithReplaceShowsTheReplaceField) {
    bar_.open(true);
    EXPECT_TRUE(bar_.replace_field()->isVisibleTo(&bar_));
}

TEST_F(FindBarTest, TypingSelectsTheFirstMatchFromTheCaret) {
    open("foo bar foo bar", 5);
    find("bar");
    EXPECT_EQ(primary(), (core::Selection{12, 15}));
    EXPECT_EQ(bar_.status(), "2 of 2");
}

TEST_F(FindBarTest, TypingMoreCharactersKeepsTheMatchWhereItIs) {
    open("foo bar foo bar", 0);
    find("b");
    EXPECT_EQ(primary(), (core::Selection{4, 5}));
    find("ba");
    EXPECT_EQ(primary(), (core::Selection{4, 6}));
    find("bar");
    EXPECT_EQ(primary(), (core::Selection{4, 7}));
    EXPECT_EQ(bar_.status(), "1 of 2");
}

TEST_F(FindBarTest, EnterAndShiftEnterMoveBetweenMatches) {
    open("a b a b a");
    find("a");
    EXPECT_EQ(primary(), (core::Selection{0, 1}));
    EXPECT_EQ(bar_.status(), "1 of 3");

    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), (core::Selection{4, 5}));
    EXPECT_EQ(bar_.status(), "2 of 3");
    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), (core::Selection{8, 9}));
    press(bar_.find_field(), Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(primary(), (core::Selection{4, 5}));
}

TEST_F(FindBarTest, NavigationWrapsAround) {
    open("a b a");
    find("a");
    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), (core::Selection{4, 5}));

    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), (core::Selection{0, 1}));
    EXPECT_EQ(bar_.status(), "1 of 2");

    press(bar_.find_field(), Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(primary(), (core::Selection{4, 5}));
    EXPECT_EQ(bar_.status(), "2 of 2");

    press(bar_.find_field(), Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(bar_.status(), "1 of 2");
}

TEST_F(FindBarTest, ASingleMatchIsFoundAgainAfterWrapping) {
    open("a b");
    find("b");
    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), (core::Selection{2, 3}));
    EXPECT_EQ(bar_.status(), "1 of 1");
}

TEST_F(FindBarTest, NoMatchesLeavesTheSelectionAndSaysSo) {
    open("abc", 1);
    find("zzz");
    EXPECT_EQ(primary(), core::Selection::caret(1));
    EXPECT_EQ(bar_.status(), "No results");
    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), core::Selection::caret(1));
}

TEST_F(FindBarTest, AnEmptyPatternFindsNothing) {
    open("abc");
    find("b");
    ASSERT_EQ(bar_.status(), "1 of 1");
    find("");
    EXPECT_EQ(bar_.status(), "No results");
    EXPECT_EQ(bar_.match_count(), 0u);
}

TEST_F(FindBarTest, MatchCaseWholeWordAndRegularExpressionOptions) {
    open("Cat cat concat c.t");
    find("cat");
    EXPECT_EQ(bar_.match_count(), 3u);

    toggle(bar_.match_case_button());
    EXPECT_EQ(bar_.match_count(), 2u);
    toggle(bar_.match_case_button());

    toggle(bar_.whole_word_button());
    EXPECT_EQ(bar_.match_count(), 2u);
    toggle(bar_.whole_word_button());

    find("c.t");
    EXPECT_EQ(bar_.match_count(), 1u);
    toggle(bar_.regex_button());
    EXPECT_EQ(bar_.match_count(), 4u);
}

TEST_F(FindBarTest, AnInvalidRegularExpressionShowsAnErrorStateNotACrash) {
    open("a(b", 2);
    toggle(bar_.regex_button());
    find("(b");
    EXPECT_TRUE(bar_.has_error());
    EXPECT_EQ(bar_.status(), "Invalid pattern");
    EXPECT_EQ(top_edge(bar_.find_box()), error_color());
    EXPECT_FALSE(bar_.find_field()->toolTip().isEmpty());
    EXPECT_EQ(primary(), core::Selection::caret(2));
    press(bar_.find_field(), Qt::Key_Return);
    bar_.replace_all();
    EXPECT_EQ(text(), "a(b");

    find("\\(b");
    EXPECT_FALSE(bar_.has_error());
    EXPECT_NE(top_edge(bar_.find_box()), error_color());
    EXPECT_TRUE(bar_.status().startsWith("1 of 1"));
}

TEST_F(FindBarTest, AllMatchesAreHighlightedAndTheCurrentOneIsSelected) {
    open("find me, find me, find me");
    find("find");
    QApplication::processEvents();
    const QImage image = view_.viewport()->grab().toImage();
    const QColor background = view_.palette().color(QPalette::Base);
    const QColor selection = view_.palette().color(QPalette::Highlight);
    const auto color_at = [&](std::size_t offset) {
        const QRect rect = view_.cursor_rect(offset);
        return image.pixelColor(rect.x() + 2, rect.y() + 1);
    };
    EXPECT_EQ(color_at(1), selection);
    EXPECT_NE(color_at(10), background);
    EXPECT_NE(color_at(10), selection);
    EXPECT_NE(color_at(20), background);
    EXPECT_EQ(color_at(6), background);
}

TEST_F(FindBarTest, TheCounterReadsQuestionMarkWhenTheSelectionIsNotAMatch) {
    open("ab ab ab");
    find("ab");
    ASSERT_EQ(bar_.status(), "1 of 3");
    view_.select_range({1, 2});
    EXPECT_EQ(bar_.status(), "? of 3");
    view_.select_range({3, 5});
    EXPECT_EQ(bar_.status(), "2 of 3");
}

TEST_F(FindBarTest, TheSearchRunsAgainWhenTheTextChanges) {
    open("ab ab");
    find("ab");
    ASSERT_EQ(bar_.match_count(), 2u);
    view_.select_range({5, 5});
    QKeyEvent press_event(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
    QApplication::sendEvent(&view_, &press_event);
    EXPECT_EQ(text(), "ab abx");
    view_.replace_ranges({{5, 6, " ab"}});
    EXPECT_EQ(bar_.match_count(), 3u);
    view_.undo();
    EXPECT_EQ(bar_.match_count(), 2u);
}

TEST_F(FindBarTest, ShowingAnotherDocumentSearchesIt) {
    open("ab ab");
    find("ab");
    ASSERT_EQ(bar_.match_count(), 2u);
    view_.set_document(make_document("ab ab ab ab"));
    EXPECT_EQ(bar_.match_count(), 4u);
    EXPECT_EQ(bar_.status(), "? of 4");
}

TEST_F(FindBarTest, ClosingRemovesTheHighlightsAndStopsSearching) {
    open("ab ab");
    find("ab");
    bar_.close_bar();
    EXPECT_TRUE(bar_.isHidden());
    QApplication::processEvents();
    const QImage image = view_.viewport()->grab().toImage();
    const QRect rect = view_.cursor_rect(4);
    EXPECT_EQ(image.pixelColor(rect.x() + 2, rect.y() + 1), view_.palette().color(QPalette::Base));

    view_.replace_ranges({{0, 2, "ab ab"}});
    EXPECT_EQ(bar_.match_count(), 0u);
}

TEST_F(FindBarTest, EscapeClosesTheBar) {
    open("ab");
    press(bar_.find_field(), Qt::Key_Escape);
    EXPECT_TRUE(bar_.isHidden());
}

TEST_F(FindBarTest, EscapeInTheEditorClosesTheBarAndKeepsTheSelection) {
    open("ab ab");
    find("ab");
    ASSERT_EQ(primary(), (core::Selection{0, 2}));
    press(&view_, Qt::Key_Escape);
    EXPECT_TRUE(bar_.isHidden());
    EXPECT_EQ(primary(), (core::Selection{0, 2}));
}

TEST_F(FindBarTest, EscapeInTheEditorClosesTheBarBeforeDroppingExtraCursors) {
    open("ab ab");
    ASSERT_TRUE(document_
                    ->set_selections(core::SelectionSet::from(
                        {core::Selection::caret(1), core::Selection::caret(4)}))
                    .has_value());
    press(&view_, Qt::Key_Escape);
    EXPECT_TRUE(bar_.isHidden());
    EXPECT_EQ(document_->selections().all().size(), 2u);

    press(&view_, Qt::Key_Escape);
    EXPECT_EQ(document_->selections().all().size(), 1u);
}

TEST_F(FindBarTest, EscapeInTheEditorIsLeftToAnInputMethodComposition) {
    open("ab");
    QInputMethodEvent composing("ka", {});
    QApplication::sendEvent(&view_, &composing);
    ASSERT_TRUE(view_.composing());
    press(&view_, Qt::Key_Escape);
    EXPECT_FALSE(bar_.isHidden());

    QInputMethodEvent finished;
    QApplication::sendEvent(&view_, &finished);
    ASSERT_FALSE(view_.composing());
    press(&view_, Qt::Key_Escape);
    EXPECT_TRUE(bar_.isHidden());
}

TEST_F(FindBarTest, EscapeInTheEditorStillDropsExtraCursorsWhenTheBarIsClosed) {
    open("ab ab");
    bar_.close_bar();
    ASSERT_TRUE(document_
                    ->set_selections(core::SelectionSet::from(
                        {core::Selection::caret(1), core::Selection::caret(4)}))
                    .has_value());
    press(&view_, Qt::Key_Escape);
    EXPECT_EQ(document_->selections().all().size(), 1u);
}

TEST_F(FindBarTest, ReopeningShowsTheHighlightsOfTheKeptPattern) {
    open("ab ab");
    find("ab");
    bar_.close_bar();
    bar_.open();
    EXPECT_EQ(bar_.match_count(), 2u);
    EXPECT_EQ(bar_.find_field()->text(), "ab");
}

TEST_F(FindBarTest, FindNextAndPreviousWorkWithoutFocusInTheBar) {
    open("a a a");
    find("a");
    bar_.find_next();
    EXPECT_EQ(primary(), (core::Selection{2, 3}));
    bar_.find_previous();
    EXPECT_EQ(primary(), (core::Selection{0, 1}));
    bar_.find_previous();
    EXPECT_EQ(primary(), (core::Selection{4, 5}));
}

TEST_F(FindBarTest, MultiByteTextIsSelectedByByteRange) {
    open("é€ é€");
    find("€");
    EXPECT_EQ(primary(), (core::Selection{2, 5}));
    EXPECT_EQ(selected(), "€");
    bar_.find_next();
    EXPECT_EQ(primary(), (core::Selection{8, 11}));
}

// --- replace ---

TEST_F(FindBarTest, ReplaceReplacesTheSelectedMatchAndMovesToTheNext) {
    open("one two one two one");
    find("one");
    bar_.replace_field()->setText("1");
    bar_.replace_current();
    EXPECT_EQ(text(), "1 two one two one");
    EXPECT_EQ(primary(), (core::Selection{6, 9}));
    EXPECT_EQ(bar_.status(), "1 of 2");

    press(bar_.replace_field(), Qt::Key_Return);
    EXPECT_EQ(text(), "1 two 1 two one");
    press(bar_.replace_field(), Qt::Key_Return);
    EXPECT_EQ(text(), "1 two 1 two 1");
    EXPECT_EQ(bar_.status(), "No results");
}

TEST_F(FindBarTest, ReplaceOnAnUnrelatedSelectionOnlyMovesToTheNextMatch) {
    open("one two one");
    find("one");
    view_.select_range({4, 7});
    bar_.replace_field()->setText("X");
    bar_.replace_current();
    EXPECT_EQ(text(), "one two one");
    EXPECT_EQ(primary(), (core::Selection{8, 11}));
}

TEST_F(FindBarTest, ReplaceAllIsOneUndoStep) {
    open("one two one two one");
    find("one");
    bar_.replace_field()->setText("three");
    bar_.replace_all();
    EXPECT_EQ(text(), "three two three two three");
    EXPECT_TRUE(document_->is_dirty());
    EXPECT_EQ(bar_.status(), "No results");

    view_.undo();
    EXPECT_EQ(text(), "one two one two one");
    EXPECT_FALSE(document_->is_dirty());
    EXPECT_FALSE(document_->can_undo());
    EXPECT_EQ(bar_.match_count(), 3u);
}

TEST_F(FindBarTest, ControlEnterInTheReplaceFieldReplacesAll) {
    open("a a a");
    find("a");
    bar_.replace_field()->setText("b");
    press(bar_.replace_field(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(text(), "b b b");
}

TEST_F(FindBarTest, RegularExpressionReplacementsUseGroups) {
    open("2024-05-17 and 2025-01-02");
    toggle(bar_.regex_button());
    find("(\\d+)-(\\d+)-(\\d+)");
    bar_.replace_field()->setText("$3.$2.$1");
    bar_.replace_all();
    EXPECT_EQ(text(), "17.05.2024 and 02.01.2025");
}

TEST_F(FindBarTest, ReplacingWithAnEmptyStringDeletesTheMatches) {
    open("a-b-c");
    find("-");
    bar_.replace_all();
    EXPECT_EQ(text(), "abc");
}

// --- large documents and limits ---

TEST_F(FindBarTest, TheCounterCapsTheNumberOfMatches) {
    open(std::string(FindBar::match_limit + 500, 'a'));
    find("a");
    EXPECT_EQ(bar_.match_count(), FindBar::match_limit);
    EXPECT_EQ(bar_.status(), "1 of 20000+");
    bar_.find_next();
    EXPECT_EQ(primary(), (core::Selection{1, 2}));
    EXPECT_EQ(bar_.status(), "2 of 20000+");
}

TEST_F(FindBarTest, ALargeDocumentIsSearchedOnAWorkerThread) {
    std::string big;
    while (big.size() < 3 * FindBar::synchronous_limit_bytes) {
        big += "line " + std::to_string(big.size()) + "\n";
    }
    open(big);
    find("99");
    EXPECT_TRUE(bar_.is_searching());
    EXPECT_EQ(bar_.status(), "Searching…");
    ASSERT_TRUE(wait_until_done());
    EXPECT_EQ(bar_.match_count(), count_occurrences(big, "99"));
    EXPECT_EQ(bar_.status(), QString("1 of %1").arg(bar_.match_count()));
    EXPECT_EQ(selected(), "99");

    bar_.find_next();
    EXPECT_EQ(bar_.status(), QString("2 of %1").arg(bar_.match_count()));
}

TEST_F(FindBarTest, EditingALargeDocumentWhileSearchingDiscardsTheOldResult) {
    std::string big;
    while (big.size() < 3 * FindBar::synchronous_limit_bytes) {
        big += "line " + std::to_string(big.size()) + "\n";
    }
    open(big);
    find("7777");
    ASSERT_TRUE(bar_.is_searching());
    const std::size_t first = big.find("7777");
    view_.replace_ranges({{first, first + 4, "xxxx"}});
    ASSERT_TRUE(wait_until_done());
    const std::size_t expected = count_occurrences(text(), "7777");
    ASSERT_GT(expected, 0u);
    EXPECT_EQ(bar_.match_count(), expected);
    EXPECT_LT(expected, FindBar::match_limit);
}

TEST_F(FindBarTest, EnterWhileALargeDocumentIsBeingSearchedWaitsForTheResult) {
    std::string big;
    while (big.size() < 3 * FindBar::synchronous_limit_bytes) {
        big += "line " + std::to_string(big.size()) + "\n";
    }
    open(big);
    find("line");
    ASSERT_TRUE(bar_.is_searching());
    bar_.find_next();
    ASSERT_TRUE(wait_until_done());
    const std::size_t second = big.find("line", 1);
    EXPECT_EQ(primary(), (core::Selection{second, second + 4}));
}

TEST_F(FindBarTest, ASearchThatCannotFinishReportsAnErrorInsteadOfFreezing) {
    open(std::string(60, 'a') + "!");
    toggle(bar_.regex_button());
    find("(a+)+$");
    EXPECT_NE(bar_.status().indexOf("too complex"), -1);
    // The pattern is fine; only this text is too much for it. The field is not marked invalid, and
    // the message goes away when the text changes.
    EXPECT_FALSE(bar_.has_error());
    EXPECT_NE(top_edge(bar_.find_box()), error_color());
    EXPECT_TRUE(bar_.find_field()->toolTip().isEmpty());

    view_.replace_ranges({{0, 60, "a"}});
    EXPECT_EQ(bar_.status().indexOf("too complex"), -1);
}

TEST_F(FindBarTest, EmptyMatchesAreSteppedThroughInsteadOfFoundAgain) {
    open("ab\ncd\nef");
    toggle(bar_.regex_button());
    find("^");
    EXPECT_EQ(primary(), core::Selection::caret(0));
    EXPECT_EQ(bar_.status(), "1 of 3");
    bar_.find_next();
    EXPECT_EQ(primary(), core::Selection::caret(3));
    bar_.find_next();
    EXPECT_EQ(primary(), core::Selection::caret(6));
    bar_.find_next();
    EXPECT_EQ(primary(), core::Selection::caret(0));
    EXPECT_EQ(bar_.status(), "1 of 3");

    bar_.find_previous();
    EXPECT_EQ(primary(), core::Selection::caret(6));
    bar_.find_previous();
    EXPECT_EQ(primary(), core::Selection::caret(3));
}

TEST_F(FindBarTest, EmptyMatchesBetweenCharactersAreSteppedThrough) {
    open("aé");
    toggle(bar_.regex_button());
    find("x*");
    EXPECT_EQ(primary(), core::Selection::caret(0));
    bar_.find_next();
    EXPECT_EQ(primary(), core::Selection::caret(1));
    bar_.find_next();
    EXPECT_EQ(primary(), core::Selection::caret(3));
    bar_.find_next();
    EXPECT_EQ(primary(), core::Selection::caret(0));
}

TEST_F(FindBarTest, EmptyMatchesAreSteppedThroughWhenTheListIsIncomplete) {
    open(std::string(FindBar::match_limit + 100, '\n'));
    toggle(bar_.regex_button());
    find("^");
    ASSERT_EQ(bar_.match_count(), FindBar::match_limit);
    EXPECT_EQ(primary(), core::Selection::caret(0));
    bar_.find_next();
    EXPECT_EQ(primary(), core::Selection::caret(1));
    bar_.find_next();
    EXPECT_EQ(primary(), core::Selection::caret(2));
}

TEST_F(FindBarTest, ANonEmptyMatchAtTheCaretIsSelectedByFindNext) {
    open("ab ab");
    find("ab");
    view_.select_range({3, 3});
    bar_.find_next();
    EXPECT_EQ(primary(), (core::Selection{3, 5}));
}

TEST_F(FindBarTest, ABarDestroyedWhileSearchingDoesNotReceiveTheResult) {
    std::string big;
    while (big.size() < 3 * FindBar::synchronous_limit_bytes) {
        big += "line " + std::to_string(big.size()) + "\n";
    }
    view_.set_document(make_document(big));
    view_.show();
    auto bar = std::make_unique<FindBar>(&view_);
    bar->open();
    bar->find_field()->setText("line");
    ASSERT_TRUE(bar->is_searching());
    bar.reset();
    QThreadPool::globalInstance()->waitForDone();
    QApplication::processEvents();
    SUCCEED();
}

// --- seeding the field from the editor ---

TEST_F(FindBarTest, ASelectionOnOneLineSeedsTheFieldAndStaysSelected) {
    show_text("foo bar foo", {4, 7});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "bar");
    EXPECT_EQ(bar_.find_field()->selectedText(), "bar");
    EXPECT_EQ(primary(), (core::Selection{4, 7}));
    EXPECT_EQ(bar_.status(), "1 of 1");

    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        bar_.grab().save(QString::fromUtf8(directory) + "/find_bar_seeded.png");
    }
}

TEST_F(FindBarTest, WithOnlyACaretTheWordAtTheCaretSeedsTheField) {
    show_text("foo bar foo", {5, 5});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "bar");
    EXPECT_EQ(primary(), core::Selection::caret(5));
    EXPECT_EQ(bar_.status(), "? of 1");
}

TEST_F(FindBarTest, ACaretAtEitherEdgeOfAWordSeedsIt) {
    show_text("foo.bar", {3, 3});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "foo");
    show_text("foo.bar", {4, 4});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "bar");
}

TEST_F(FindBarTest, SeedingDoesNotMoveTheSelectionSoFindNextGoesToTheNextOccurrence) {
    show_text("foo bar foo baz foo", {1, 1});
    bar_.open();
    ASSERT_EQ(bar_.find_field()->text(), "foo");
    EXPECT_EQ(primary(), core::Selection::caret(1));
    EXPECT_EQ(bar_.match_count(), 3u);
    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), (core::Selection{8, 11}));
}

TEST_F(FindBarTest, ACaretNotInOrBesideAWordLeavesTheFieldAsItWas) {
    show_text("foo  bar", {4, 4});
    bar_.open();
    find("kept");
    bar_.close_bar();
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "kept");
}

TEST_F(FindBarTest, ASelectionOverSeveralLinesLeavesTheFieldAsItWas) {
    show_text("one\ntwo", {1, 6});
    bar_.open();
    find("kept");
    bar_.close_bar();
    show_text("one\ntwo", {1, 6});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "kept");
}

TEST_F(FindBarTest, ALongSeedIsNotCutByTheLineEdit) {
    const std::string line(100000, 'a');
    show_text(line, {0, line.size()});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text().size(), 100000);
}

TEST_F(FindBarTest, ASelectionLongerThanTheSearchStringLimitLeavesTheFieldAsItWas) {
    const std::string line(524288 + 10, 'a');
    show_text(line, {0, line.size()});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "");

    show_text(line, {0, 524288 - 1});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text().size(), 524288 - 1);
}

TEST_F(FindBarTest, TheRegularExpressionSpecialCharactersOfTheSeedAreEscaped) {
    show_text("a.b axb a.b", {0, 3});
    toggle(bar_.regex_button());
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "a\\.b");
    EXPECT_FALSE(bar_.has_error());
    EXPECT_EQ(bar_.match_count(), 2u);
}

TEST_F(FindBarTest, WithoutTheRegularExpressionToggleTheSeedIsTakenAsWritten) {
    show_text("a.b axb a.b", {0, 3});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "a.b");
    EXPECT_EQ(bar_.match_count(), 2u);
}

TEST_F(FindBarTest, SeedingLeavesTheOptionTogglesAlone) {
    show_text("Foo foo", {0, 3});
    toggle(bar_.match_case_button());
    toggle(bar_.whole_word_button());
    bar_.open();
    EXPECT_TRUE(bar_.match_case_button()->isChecked());
    EXPECT_TRUE(bar_.whole_word_button()->isChecked());
    EXPECT_FALSE(bar_.regex_button()->isChecked());
    EXPECT_EQ(bar_.match_count(), 1u);
}

TEST_F(FindBarTest, MultiByteSelectionsAreSeededExactly) {
    show_text("é€ é€x", {0, 5});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), QString::fromUtf8("é€"));
    EXPECT_EQ(bar_.match_count(), 2u);
}

TEST_F(FindBarTest, OpeningWithReplaceSeedsToo) {
    show_text("foo bar", {4, 7});
    bar_.open(true);
    EXPECT_EQ(bar_.find_field()->text(), "bar");
    EXPECT_TRUE(bar_.replace_field()->isVisibleTo(&bar_));
}

TEST_F(FindBarTest, OpeningAgainFromTheEditorSeedsAgain) {
    show_text("foo bar", {0, 3});
    bar_.open();
    ASSERT_EQ(bar_.find_field()->text(), "foo");
    view_.select_range({4, 7});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "bar");
}

TEST_F(FindBarTest, OpeningWhileTheFocusIsInTheBarKeepsWhatWasTyped) {
    show_text("foo bar", {0, 3});
    bar_.open();
    bar_.activateWindow();
    QApplication::processEvents();
    ASSERT_TRUE(bar_.find_field()->hasFocus());
    find("typed");
    view_.select_range({4, 7});
    bar_.open();
    EXPECT_EQ(bar_.find_field()->text(), "typed");
}

// --- keyboard shortcuts for the options ---

namespace {

// The modifiers of VS Code's option shortcuts: Alt, and Cmd+Alt on macOS.
Qt::KeyboardModifiers option_modifiers() {
#ifdef Q_OS_MACOS
    return Qt::ControlModifier | Qt::AltModifier;
#else
    return Qt::AltModifier;
#endif
}

QKeySequence option_shortcut(Qt::Key key) {
#ifdef Q_OS_MACOS
    return QKeySequence(Qt::CTRL | Qt::ALT | key);
#else
    return QKeySequence(Qt::ALT | key);
#endif
}

} // namespace

TEST_F(FindBarTest, AltCTogglesMatchCaseFromTheFindField) {
    open("Cat cat");
    bar_.activateWindow();
    QApplication::processEvents();
    find("cat");
    ASSERT_EQ(bar_.match_count(), 2u);

    QTest::keyClick(bar_.find_field(), Qt::Key_C, option_modifiers());
    EXPECT_TRUE(bar_.match_case_button()->isChecked());
    EXPECT_EQ(bar_.match_count(), 1u);
    QTest::keyClick(bar_.find_field(), Qt::Key_C, option_modifiers());
    EXPECT_FALSE(bar_.match_case_button()->isChecked());
    EXPECT_EQ(bar_.match_count(), 2u);
}

TEST_F(FindBarTest, AltWTogglesWholeWordFromTheFindField) {
    open("cat concat");
    bar_.activateWindow();
    QApplication::processEvents();
    find("cat");
    ASSERT_EQ(bar_.match_count(), 2u);
    QTest::keyClick(bar_.find_field(), Qt::Key_W, option_modifiers());
    EXPECT_TRUE(bar_.whole_word_button()->isChecked());
    EXPECT_EQ(bar_.match_count(), 1u);
}

TEST_F(FindBarTest, AltRTogglesTheRegularExpressionFromTheFindField) {
    open("cat c.t");
    bar_.activateWindow();
    QApplication::processEvents();
    find("c.t");
    ASSERT_EQ(bar_.match_count(), 1u);
    QTest::keyClick(bar_.find_field(), Qt::Key_R, option_modifiers());
    EXPECT_TRUE(bar_.regex_button()->isChecked());
    EXPECT_EQ(bar_.match_count(), 2u);
}

namespace {

QAction* action_of(QAbstractButton* button) {
    auto* tool_button = qobject_cast<QToolButton*>(button);
    return tool_button != nullptr ? tool_button->defaultAction() : nullptr;
}

} // namespace

TEST_F(FindBarTest, TheShortcutsAreTheOnesOfVSCode) {
    open("x");
    ASSERT_NE(action_of(bar_.match_case_button()), nullptr);
    EXPECT_EQ(action_of(bar_.match_case_button())->shortcut(), option_shortcut(Qt::Key_C));
    EXPECT_EQ(action_of(bar_.whole_word_button())->shortcut(), option_shortcut(Qt::Key_W));
    EXPECT_EQ(action_of(bar_.regex_button())->shortcut(), option_shortcut(Qt::Key_R));
}

TEST_F(FindBarTest, TheTooltipsNameTheShortcuts) {
    open("x");
    for (QAbstractButton* button :
         {bar_.match_case_button(), bar_.whole_word_button(), bar_.regex_button()}) {
        ASSERT_NE(action_of(button), nullptr);
        const QString shortcut = action_of(button)->shortcut().toString(QKeySequence::NativeText);
        ASSERT_FALSE(shortcut.isEmpty());
        EXPECT_TRUE(button->toolTip().contains(shortcut)) << button->toolTip().toStdString();
    }
    EXPECT_TRUE(bar_.match_case_button()->toolTip().startsWith("Match Case"));
    EXPECT_TRUE(bar_.whole_word_button()->toolTip().startsWith("Match Whole Word"));
    EXPECT_TRUE(bar_.regex_button()->toolTip().startsWith("Use Regular Expression"));
}

TEST_F(FindBarTest, ARebindingIsReflectedInTheTooltip) {
    open("x");
    ASSERT_NE(action_of(bar_.match_case_button()), nullptr);
    const QKeySequence rebound(Qt::ALT | Qt::SHIFT | Qt::Key_X);
    action_of(bar_.match_case_button())->setShortcut(rebound);
    EXPECT_TRUE(
        bar_.match_case_button()->toolTip().contains(rebound.toString(QKeySequence::NativeText)))
        << bar_.match_case_button()->toolTip().toStdString();
}

TEST_F(FindBarTest, ClickingStillTogglesTheOptions) {
    open("Cat cat");
    find("cat");
    bar_.match_case_button()->click();
    EXPECT_TRUE(bar_.match_case_button()->isChecked());
    EXPECT_EQ(bar_.match_count(), 1u);
}

namespace {

using anvil::ui::WorkbenchColors;

// The bar over the editor, as the editor area has them: both in one parent.
class FindWidgetTest : public ::testing::Test {
protected:
    FindWidgetTest() : colors_(QApplication::palette()) {
        WorkbenchColors::Colors theme{
            {"editorWidget.background", QColor(10, 20, 30)},
            {"editorWidget.foreground", QColor(200, 200, 200)},
            {"widget.border", QColor(40, 50, 60)},
            {"widget.shadow", QColor(0, 0, 0, 200)},
            {"input.background", QColor(70, 80, 90)},
            {"input.border", QColor(100, 110, 120)},
            {"input.foreground", QColor(250, 250, 250)},
            {"focusBorder", QColor(0, 255, 255)},
            {"icon.foreground", QColor(255, 255, 0)},
            {"inputOption.activeBackground", QColor(130, 0, 130)},
            {"inputOption.activeBorder", QColor(255, 0, 255)},
            {"inputOption.activeForeground", QColor(0, 255, 0)},
            {"toolbar.hoverBackground", QColor(0, 120, 0)},
            {"errorForeground", QColor(255, 0, 0)},
        };
        colors_.set_colors(theme);
        host_.resize(700, 400);
        view_ = new EditorView(&host_);
        view_->setGeometry(0, 0, 700, 400);
        bar_ = new FindBar(view_, &host_);
        bar_->set_colors(&colors_);
        std::string text;
        for (int line = 0; line < 200; ++line) {
            text += line == 100 ? std::string(40, '.') + "needle\n" : "needle\n";
        }
        document_ = make_document(text);
        view_->set_document(document_);
        host_.show();
        host_.activateWindow();
        open();
        // Opening seeds the field from the word at the caret.
        find("");
    }

    void open(bool replace = false) {
        bar_->open(replace);
        QApplication::processEvents();
    }
    void find(const QString& pattern) {
        bar_->find_field()->setText(pattern);
        QApplication::processEvents();
    }
    // The widget keeps clear of two widths of the scroll bar, as VS Code does.
    int right_edge() const {
        return view_->geometry().right() + 1 - 2 * anvil::ui::ScrollBar::editor_vertical_width;
    }
    void view_width(int width) {
        view_->resize(width, 400);
        QApplication::processEvents();
    }
    static QColor pixel(QWidget* widget, int x, int y) {
        return widget->grab().toImage().pixelColor(x, y);
    }
    // Whether a pixel is close to the color: the thin strokes of a glyph are smoothed, and few of
    // their pixels have the color exactly.
    static bool shows(QWidget* widget, const QColor& color) {
        const QImage image = widget->grab().toImage();
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor found = image.pixelColor(x, y);
                if (std::abs(found.red() - color.red()) < 50 &&
                    std::abs(found.green() - color.green()) < 50 &&
                    std::abs(found.blue() - color.blue()) < 50) {
                    return true;
                }
            }
        }
        return false;
    }
    QColor count_color() const {
        return bar_->count_label()->palette().color(QPalette::WindowText);
    }

    WorkbenchColors colors_;
    QWidget host_;
    EditorView* view_ = nullptr;
    FindBar* bar_ = nullptr;
    std::shared_ptr<core::Document> document_;
};

} // namespace

TEST_F(FindWidgetTest, ItFloatsAtTheTopRightOfTheViewAndFollowsIt) {
    EXPECT_EQ(bar_->size(), QSize(FindBar::widget_width, FindBar::find_height));
    EXPECT_EQ(bar_->geometry().right() + 1, right_edge());
    EXPECT_EQ(bar_->geometry().top(), view_->geometry().top() + 4);
    EXPECT_EQ(view_->geometry(), QRect(0, 0, 700, 400));
    EXPECT_EQ(host_.childAt(bar_->geometry().center())->window(), &host_);
    EXPECT_TRUE(bar_->isAncestorOf(host_.childAt(bar_->geometry().center())));

    view_->setGeometry(20, 30, 600, 300);
    QApplication::processEvents();
    EXPECT_EQ(bar_->geometry().right() + 1, right_edge());
    EXPECT_EQ(bar_->geometry().top(), 34);

    open(true);
    EXPECT_EQ(bar_->size(), QSize(FindBar::widget_width, FindBar::replace_height));
    EXPECT_EQ(view_->geometry(), QRect(20, 30, 600, 300));
}

TEST_F(FindWidgetTest, ThePartsStandWhereVsCodePutsThem) {
    open(true);
    const QRect find_box = bar_->find_box()->geometry();
    const QRect replace_box = bar_->replace_box()->geometry();
    EXPECT_EQ(find_box.topLeft(), QPoint(27, 4));
    EXPECT_EQ(find_box.height(), FindBar::field_height);
    EXPECT_EQ(replace_box, find_box.translated(0, FindBar::field_height + 3));
    // The chevron is in the middle of the room left of the fields, as high as both of them.
    const QRect toggle = bar_->toggle_replace_button()->geometry();
    EXPECT_EQ(toggle, QRect(6, 5, 18, FindBar::replace_height - 10));
    EXPECT_NEAR(toggle.x() + toggle.width() / 2.0, (1 + find_box.left()) / 2.0, 1.0);
    EXPECT_EQ(toggle.y() + toggle.height() / 2.0, FindBar::replace_height / 2.0);
    // The options are in the find field, at its right end.
    for (QAbstractButton* option :
         {bar_->match_case_button(), bar_->whole_word_button(), bar_->regex_button()}) {
        EXPECT_EQ(option->parentWidget(), bar_->find_box());
        EXPECT_GE(option->geometry().left(), bar_->find_field()->geometry().right());
        EXPECT_TRUE(bar_->find_box()->rect().contains(option->geometry()));
    }
    EXPECT_LT(find_box.right(), bar_->count_label()->geometry().left());
    EXPECT_LT(bar_->count_label()->geometry().right(), bar_->previous_button()->geometry().left());
    EXPECT_LT(bar_->previous_button()->geometry().right(), bar_->next_button()->geometry().left());
    EXPECT_LT(bar_->next_button()->geometry().right(), bar_->close_button()->geometry().left());
    EXPECT_GT(bar_->replace_button()->geometry().left(), replace_box.right());
    EXPECT_GT(bar_->replace_all_button()->geometry().left(),
              bar_->replace_button()->geometry().right());
}

TEST_F(FindWidgetTest, ANarrowViewGivesUpTheCountThenTheOptionsAndArrows) {
    const auto shown = [this] {
        return std::vector<bool>{bar_->count_label()->isVisible(),
                                 bar_->match_case_button()->isVisible(),
                                 bar_->next_button()->isVisible()};
    };
    EXPECT_EQ(shown(), (std::vector<bool>{true, true, true}));
    view_width(300);
    EXPECT_EQ(shown(), (std::vector<bool>{false, true, true}));
    view_width(250);
    EXPECT_EQ(shown(), (std::vector<bool>{false, false, true}));
    view_width(150);
    EXPECT_EQ(shown(), (std::vector<bool>{false, false, false}));
    for (const int width : {500, 300, 150}) {
        view_width(width);
        EXPECT_EQ(bar_->geometry().right() + 1, right_edge()) << width;
        EXPECT_GE(bar_->geometry().left(), view_->geometry().left()) << width;
        EXPECT_TRUE(bar_->rect().contains(bar_->close_button()->geometry())) << width;
    }
    // The keys of the options work without room for their buttons.
    bar_->find_field()->setFocus();
    QTest::keyClick(bar_->find_field(), Qt::Key_C, option_modifiers());
    EXPECT_TRUE(bar_->match_case_button()->isChecked());
}

TEST_F(FindWidgetTest, ItTakesTheColorsOfTheTheme) {
    open(true);
    // Between the chevron and the fields is the widget itself.
    EXPECT_EQ(pixel(bar_, 23, 16), QColor(10, 20, 30));
    EXPECT_EQ(pixel(bar_, bar_->width() / 2, 0), QColor(40, 50, 60));
    EXPECT_EQ(pixel(bar_->find_box(), 3, 12), QColor(70, 80, 90));
    EXPECT_EQ(pixel(bar_->find_box(), bar_->find_box()->width() / 2, 0), QColor(0, 255, 255));
    EXPECT_EQ(pixel(bar_->replace_box(), bar_->replace_box()->width() / 2, 0),
              QColor(100, 110, 120));
    EXPECT_EQ(bar_->find_field()->palette().color(QPalette::Text), QColor(250, 250, 250));
    EXPECT_TRUE(shows(bar_->close_button(), QColor(255, 255, 0)));
    EXPECT_TRUE(shows(bar_->toggle_replace_button(), QColor(255, 255, 0)));

    EXPECT_FALSE(shows(bar_->match_case_button(), QColor(130, 0, 130)));
    bar_->match_case_button()->click();
    EXPECT_EQ(pixel(bar_->match_case_button(), 10, 2), QColor(130, 0, 130));
    EXPECT_TRUE(shows(bar_->match_case_button(), QColor(255, 0, 255)));
    EXPECT_TRUE(shows(bar_->match_case_button(), QColor(0, 255, 0)));

    colors_.set_colors({{"editorWidget.background", QColor(1, 2, 3)}});
    EXPECT_EQ(pixel(bar_, 23, 16), QColor(1, 2, 3));
}

TEST_F(FindWidgetTest, AShadowLiesAroundItWhileItShowsAndTheThemeHasOne) {
    const auto beside = [this] {
        QApplication::processEvents();
        return host_.grab().toImage().pixelColor(bar_->geometry().left() - 2,
                                                 bar_->geometry().bottom() - 4);
    };
    const QColor shadowed = beside();
    bar_->close_bar();
    const QColor plain = beside();
    EXPECT_LT(shadowed.lightness(), plain.lightness());
    open();
    EXPECT_EQ(beside(), shadowed);
    colors_.set_colors({});
    EXPECT_EQ(beside().lightness(), plain.lightness());
}

TEST_F(FindWidgetTest, TheCountIsAlwaysShownAndInTheErrorColorWhenNothingIsFound) {
    find("");
    EXPECT_EQ(bar_->status(), "No results");
    EXPECT_EQ(count_color(), QColor(200, 200, 200));
    find("nothing like it");
    EXPECT_EQ(bar_->status(), "No results");
    EXPECT_EQ(count_color(), QColor(255, 0, 0));
    find("needle");
    EXPECT_EQ(count_color(), QColor(200, 200, 200));
}

TEST_F(FindWidgetTest, TheButtonsAreEnabledAsVsCodeEnablesThem) {
    open(true);
    const auto enabled = [this] {
        return std::vector<bool>{
            bar_->previous_button()->isEnabled(), bar_->next_button()->isEnabled(),
            bar_->replace_button()->isEnabled(), bar_->replace_all_button()->isEnabled(),
            bar_->close_button()->isEnabled()};
    };
    find("");
    EXPECT_EQ(enabled(), (std::vector<bool>{false, false, false, false, true}));
    EXPECT_FALSE(shows(bar_->next_button(), QColor(255, 255, 0)));
    find("nothing like it");
    EXPECT_EQ(enabled(), (std::vector<bool>{false, false, true, true, true}));
    find("needle");
    EXPECT_EQ(enabled(), (std::vector<bool>{true, true, true, true, true}));
    EXPECT_TRUE(shows(bar_->next_button(), QColor(255, 255, 0)));
}

TEST_F(FindWidgetTest, AButtonUnderThePointerHasAHoverBackground) {
    QWidget* close = bar_->close_button();
    EXPECT_FALSE(shows(close, QColor(0, 120, 0)));
    close->setAttribute(Qt::WA_UnderMouse);
    EXPECT_TRUE(shows(close, QColor(0, 120, 0)));
}

TEST_F(FindWidgetTest, TabGoesBetweenTheFieldsWhileReplaceShows) {
    open(true);
    bar_->find_field()->setFocus();
    QTest::keyClick(bar_->find_field(), Qt::Key_Tab);
    EXPECT_EQ(QApplication::focusWidget(), bar_->replace_field());
    QTest::keyClick(bar_->replace_field(), Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(QApplication::focusWidget(), bar_->find_field());

    bar_->toggle_replace_button()->click();
    bar_->find_field()->setFocus();
    QTest::keyClick(bar_->find_field(), Qt::Key_Tab);
    EXPECT_EQ(QApplication::focusWidget(), bar_->match_case_button());
}

TEST_F(FindWidgetTest, AMatchThatTheWidgetWouldCoverIsScrolledFromUnderIt) {
    view_->scroll_to(QPoint(0, 100 * view_->line_height()));
    view_->select_range({700, 700});
    view_->scroll_to(QPoint(0, 100 * view_->line_height()));
    QApplication::processEvents();
    const std::size_t match = 700 + 40;
    const auto covered = [&] {
        QRect at = view_->cursor_rect(match);
        at.moveTopLeft(view_->viewport()->mapTo(&host_, at.topLeft()));
        return at.intersects(bar_->geometry());
    };
    ASSERT_TRUE(covered());
    find("needle");
    ASSERT_EQ(document_->selections().primary(), (core::Selection{match, match + 6}));
    QApplication::processEvents();
    EXPECT_FALSE(covered());
}

TEST_F(FindWidgetTest, AMatchBesideTheWidgetDoesNotScrollTheEditor) {
    view_->scroll_to(QPoint(0, 10 * view_->line_height()));
    view_->select_range({70, 70});
    view_->scroll_to(QPoint(0, 10 * view_->line_height()));
    QApplication::processEvents();
    const QPoint before = view_->scroll_target();
    find("needle");
    ASSERT_EQ(document_->selections().primary(), (core::Selection{70, 76}));
    EXPECT_EQ(view_->scroll_target(), before);
}

TEST_F(FindWidgetTest, TheFocusGoesThroughThePartsInTheOrderOfVsCode) {
    open(true);
    find("needle");
    std::vector<QWidget*> visited;
    for (QWidget* part = bar_->toggle_replace_button(); visited.size() < 10;) {
        part = part->nextInFocusChain();
        if ((part->focusPolicy() & Qt::TabFocus) && bar_->isAncestorOf(part)) {
            visited.push_back(part);
        }
    }
    EXPECT_EQ(visited, (std::vector<QWidget*>{bar_->find_field(), bar_->match_case_button(),
                                              bar_->whole_word_button(), bar_->regex_button(),
                                              bar_->replace_field(), bar_->previous_button(),
                                              bar_->next_button(), bar_->close_button(),
                                              bar_->replace_button(), bar_->replace_all_button()}));
}

TEST_F(FindWidgetTest, TheShadowGoesWithTheBar) {
    const auto children = [this] {
        return host_.findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    };
    const auto before = children().size();
    delete bar_;
    EXPECT_EQ(children().size(), before - 2);
    host_.grab();
}
