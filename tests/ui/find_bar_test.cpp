#include "anvil/ui/find_bar.hpp"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTest>
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
        QApplication::processEvents();
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

TEST_F(FindBarTest, NavigationWrapsAroundAndSaysSo) {
    open("a b a");
    find("a");
    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), (core::Selection{4, 5}));
    EXPECT_FALSE(bar_.status().contains("wrapped"));

    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), (core::Selection{0, 1}));
    EXPECT_EQ(bar_.status(), "1 of 2 (wrapped)");

    press(bar_.find_field(), Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(primary(), (core::Selection{4, 5}));
    EXPECT_EQ(bar_.status(), "2 of 2 (wrapped)");

    press(bar_.find_field(), Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(bar_.status(), "1 of 2");
}

TEST_F(FindBarTest, ASingleMatchIsFoundAgainAfterWrapping) {
    open("a b");
    find("b");
    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), (core::Selection{2, 3}));
    EXPECT_EQ(bar_.status(), "1 of 1 (wrapped)");
}

TEST_F(FindBarTest, NoMatchesLeavesTheSelectionAndSaysSo) {
    open("abc", 1);
    find("zzz");
    EXPECT_EQ(primary(), core::Selection::caret(1));
    EXPECT_EQ(bar_.status(), "No results");
    press(bar_.find_field(), Qt::Key_Return);
    EXPECT_EQ(primary(), core::Selection::caret(1));
}

TEST_F(FindBarTest, AnEmptyPatternShowsNothing) {
    open("abc");
    find("b");
    ASSERT_EQ(bar_.status(), "1 of 1");
    find("");
    EXPECT_EQ(bar_.status(), "");
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
    EXPECT_FALSE(bar_.find_field()->styleSheet().isEmpty());
    EXPECT_FALSE(bar_.find_field()->toolTip().isEmpty());
    EXPECT_EQ(primary(), core::Selection::caret(2));
    press(bar_.find_field(), Qt::Key_Return);
    bar_.replace_all();
    EXPECT_EQ(text(), "a(b");

    find("\\(b");
    EXPECT_FALSE(bar_.has_error());
    EXPECT_TRUE(bar_.find_field()->styleSheet().isEmpty());
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
}
