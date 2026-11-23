#include "anvil/ui/editor_view.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>

using namespace anvil;
using anvil::ui::EditorView;
using anvil::ui::WorkbenchColors;

namespace {

std::shared_ptr<core::Document> make_document(std::string_view text) {
    return std::make_shared<core::Document>(*core::Text::from_utf8(text));
}

class EditorViewTest : public ::testing::Test {
protected:
    void open(std::string_view text) {
        document_ = make_document(text);
        view_.resize(640, 320);
        view_.set_document(document_);
        view_.show();
        view_.activateWindow();
        view_.setFocus();
        QApplication::processEvents();
    }

    void key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {}) {
        QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
        QApplication::sendEvent(&view_, &press);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
        QApplication::sendEvent(&view_, &release);
    }

    void type(const QString& text) {
        for (const QChar c : text) {
            key(c.unicode(), Qt::NoModifier, QString(c));
        }
    }

    void mouse(QEvent::Type type, const QPoint& position,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, QPointF(position), view_.viewport()->mapToGlobal(QPointF(position)),
                          Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          modifiers);
        QApplication::sendEvent(view_.viewport(), &event);
    }

    void click(std::size_t offset, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QPoint point = view_.cursor_rect(offset).center();
        mouse(QEvent::MouseButtonPress, point, modifiers);
        mouse(QEvent::MouseButtonRelease, point, modifiers);
    }

    std::string text() const {
        return document_->text().to_string();
    }
    core::Selection primary() const {
        return document_->selections().primary();
    }

    EditorView view_;
    std::shared_ptr<core::Document> document_;
};

} // namespace

TEST_F(EditorViewTest, TypingInsertsTextAndReportsTheChange) {
    open("");
    QSignalSpy changes(&view_, &EditorView::text_changed);
    type("hi");
    EXPECT_EQ(text(), "hi");
    EXPECT_EQ(primary(), core::Selection::caret(2));
    EXPECT_EQ(changes.count(), 2);
}

TEST_F(EditorViewTest, EnterBackspaceDeleteAndTab) {
    open("    ab");
    key(Qt::Key_End);
    key(Qt::Key_Return);
    EXPECT_EQ(text(), "    ab\n    ");
    key(Qt::Key_Backspace);
    EXPECT_EQ(text(), "    ab\n   ");
    key(Qt::Key_Home);
    key(Qt::Key_Delete);
    EXPECT_EQ(text(), "    ab\n  ");
    key(Qt::Key_Tab); // the caret is at the start of the line, so a whole tab stop is inserted
    EXPECT_EQ(text(), "    ab\n      ");
}

TEST_F(EditorViewTest, ArrowKeysMoveAndShiftArrowsSelect) {
    open("abc\ndef");
    key(Qt::Key_Right);
    key(Qt::Key_Right);
    EXPECT_EQ(primary(), core::Selection::caret(2));
    key(Qt::Key_Down);
    EXPECT_EQ(primary(), core::Selection::caret(6));
    key(Qt::Key_Left, Qt::ShiftModifier);
    key(Qt::Key_Left, Qt::ShiftModifier);
    EXPECT_EQ(primary(), (core::Selection{6, 4}));
    key(Qt::Key_End, Qt::ControlModifier);
    EXPECT_EQ(primary(), core::Selection::caret(7));
}

TEST_F(EditorViewTest, UndoAndRedoShortcuts) {
    open("");
    type("abc");
    key(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(text(), "");
    key(Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(text(), "abc");
}

TEST_F(EditorViewTest, SelectAllAndTypingReplacesEverything) {
    open("old text");
    key(Qt::Key_A, Qt::ControlModifier);
    EXPECT_EQ(primary(), (core::Selection{0, 8}));
    type("x");
    EXPECT_EQ(text(), "x");
}

TEST_F(EditorViewTest, NonAsciiTextIsTypedAndEditedByWholeCharacters) {
    open("");
    key(0, Qt::NoModifier, QString::fromUtf8("\xC3\xA9"));
    key(0, Qt::NoModifier, QString::fromUtf8("\xF0\x9F\x98\x80"));
    EXPECT_EQ(text(), "\xC3\xA9\xF0\x9F\x98\x80");
    key(Qt::Key_Left);
    EXPECT_EQ(primary(), core::Selection::caret(2));
    key(Qt::Key_Backspace);
    EXPECT_EQ(text(), "\xF0\x9F\x98\x80");
}

TEST_F(EditorViewTest, ClicksPlaceTheCaretWhereTheTextIs) {
    open("hello world\nsecond line");
    click(5);
    EXPECT_EQ(primary(), core::Selection::caret(5));
    click(18);
    EXPECT_EQ(primary(), core::Selection::caret(18));
    click(23);
    EXPECT_EQ(primary(), core::Selection::caret(23));
}

TEST_F(EditorViewTest, ShiftClickExtendsAndAltClickAddsACaret) {
    open("hello world");
    click(2);
    click(7, Qt::ShiftModifier);
    EXPECT_EQ(primary(), (core::Selection{2, 7}));
    click(10, Qt::AltModifier);
    EXPECT_EQ(document_->selections().all().size(), 2u);
    type("!");
    EXPECT_EQ(text(), "he!orl!d");
}

TEST_F(EditorViewTest, DraggingSelectsAndDoubleClickSelectsAWord) {
    open("alpha beta gamma");
    const QPoint start = view_.cursor_rect(2).center();
    const QPoint end = view_.cursor_rect(9).center();
    mouse(QEvent::MouseButtonPress, start);
    mouse(QEvent::MouseMove, end);
    mouse(QEvent::MouseButtonRelease, end);
    EXPECT_EQ(primary(), (core::Selection{2, 9}));

    const QPoint inside_beta = view_.cursor_rect(7).center();
    mouse(QEvent::MouseButtonDblClick, inside_beta);
    EXPECT_EQ(primary(), (core::Selection{6, 10}));
}

TEST_F(EditorViewTest, TheViewScrollsToKeepTheCaretVisible) {
    std::string lines;
    for (int i = 0; i < 2000; ++i) {
        lines += "line " + std::to_string(i) + "\n";
    }
    open(lines);
    EXPECT_EQ(view_.verticalScrollBar()->value(), 0);
    key(Qt::Key_End, Qt::ControlModifier);
    EXPECT_GT(view_.verticalScrollBar()->value(), 0);
    const QRect caret = view_.cursor_rect(primary().active);
    EXPECT_GE(caret.top(), 0);
    EXPECT_LE(caret.bottom(), view_.viewport()->height());

    key(Qt::Key_Home, Qt::ControlModifier);
    EXPECT_EQ(view_.verticalScrollBar()->value(), 0);
    key(Qt::Key_PageDown);
    EXPECT_GT(primary().active, 10u);
}

TEST_F(EditorViewTest, HomeGoesToTheFirstCharacterThenToTheStartOfTheLine) {
    open("a\n    bc");
    key(Qt::Key_Down);
    key(Qt::Key_End);
    ASSERT_EQ(primary(), core::Selection::caret(8));
    key(Qt::Key_Home);
    EXPECT_EQ(primary(), core::Selection::caret(6));
    key(Qt::Key_Home);
    EXPECT_EQ(primary(), core::Selection::caret(2));
    key(Qt::Key_Home);
    EXPECT_EQ(primary(), core::Selection::caret(6));
}

TEST_F(EditorViewTest, ShiftHomeSelectsToTheFirstCharacterAndControlHomeToTheDocumentStart) {
    open("a\n    bc");
    key(Qt::Key_Down);
    key(Qt::Key_End);
    key(Qt::Key_Home, Qt::ShiftModifier);
    EXPECT_EQ(primary(), (core::Selection{8, 6}));
    key(Qt::Key_Home, Qt::ShiftModifier);
    EXPECT_EQ(primary(), (core::Selection{8, 2}));
    key(Qt::Key_Home, Qt::ControlModifier);
    EXPECT_EQ(primary(), core::Selection::caret(0));
}

TEST_F(EditorViewTest, SignalsAreEmittedOnlyForRealChanges) {
    open("ab");
    QSignalSpy selections(&view_, &EditorView::selections_changed);
    QSignalSpy changes(&view_, &EditorView::text_changed);
    key(Qt::Key_Left); // already at the start
    EXPECT_EQ(selections.count(), 0);
    key(Qt::Key_Right);
    EXPECT_EQ(selections.count(), 1);
    EXPECT_EQ(changes.count(), 0);
    type("x");
    EXPECT_EQ(changes.count(), 1);
    EXPECT_GE(selections.count(), 2);
}

TEST_F(EditorViewTest, EscapeKeepsOnlyThePrimaryCursor) {
    open("abc abc");
    click(1);
    click(5, Qt::AltModifier);
    ASSERT_EQ(document_->selections().all().size(), 2u);
    key(Qt::Key_Escape);
    EXPECT_EQ(document_->selections().all().size(), 1u);
}

TEST_F(EditorViewTest, ChangesMadeElsewhereAreShownAfterNotifying) {
    open("short");
    std::string lines;
    for (int i = 0; i < 500; ++i) {
        lines += "x\n";
    }
    ASSERT_TRUE(document_->apply_edits(std::vector<core::Edit>{{5, 5, lines}}).has_value());
    view_.document_changed_elsewhere();
    EXPECT_GT(view_.verticalScrollBar()->maximum(), 0);
}

TEST_F(EditorViewTest, WithoutADocumentKeysAndClicksDoNothing) {
    view_.resize(200, 100);
    view_.show();
    key(Qt::Key_A, Qt::NoModifier, "a");
    EXPECT_EQ(view_.offset_at(QPoint(10, 10)), 0u);
    view_.undo();
    SUCCEED();
}

// --- painting ---

TEST_F(EditorViewTest, PaintsTextLineNumbersAndSelections) {
    open("first line\nsecond line\nthird");
    key(Qt::Key_Right, Qt::ShiftModifier);
    key(Qt::Key_Right, Qt::ShiftModifier);
    key(Qt::Key_Right, Qt::ShiftModifier);
    QApplication::processEvents();

    const QImage image = view_.viewport()->grab().toImage();
    ASSERT_FALSE(image.isNull());
    const QColor background = view_.palette().color(QPalette::Base);
    const QColor highlight = view_.palette().color(QPalette::Highlight);

    // The selected characters have the highlight color behind them.
    const QRect selected = view_.cursor_rect(1);
    EXPECT_EQ(image.pixelColor(selected.x() + 2, selected.y() + 1), highlight);

    // Unselected text areas keep the background, and the text itself leaves ink.
    const QRect later = view_.cursor_rect(8);
    EXPECT_EQ(image.pixelColor(later.x() + 2, later.y() + 1), background);
    bool ink = false;
    for (int x = view_.cursor_rect(0).x(); x < view_.cursor_rect(10).x() && !ink; ++x) {
        for (int y = 0; y < view_.line_height() && !ink; ++y) {
            ink = image.pixelColor(x, y) != background && image.pixelColor(x, y) != highlight;
        }
    }
    EXPECT_TRUE(ink);

    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/editor_view.png");
    }
}

TEST_F(EditorViewTest, HighlightsAreShadedInAColorDifferentFromTheSelection) {
    open("find me, find me\nsecond line");
    const QColor background = view_.palette().color(QPalette::Base);
    // The line with the caret has its own shade.
    const QColor caret_line = view_.palette().color(QPalette::AlternateBase);
    const QColor selection = view_.palette().color(QPalette::Highlight);
    view_.set_highlights({{0, 4}, {9, 13}});
    QApplication::processEvents();

    QImage image = view_.viewport()->grab().toImage();
    const auto color_at = [&](std::size_t offset) {
        const QRect rect = view_.cursor_rect(offset);
        return image.pixelColor(rect.x() + 2, rect.y() + 1);
    };
    EXPECT_NE(color_at(1), caret_line);
    EXPECT_NE(color_at(1), selection);
    EXPECT_EQ(color_at(10), color_at(1));
    EXPECT_EQ(color_at(6), caret_line);
    EXPECT_EQ(color_at(20), background);

    // A selection over a highlighted match is drawn as a selection.
    view_.select_range({9, 13});
    QApplication::processEvents();
    image = view_.viewport()->grab().toImage();
    EXPECT_EQ(color_at(10), selection);
    EXPECT_NE(color_at(1), background);
    EXPECT_NE(color_at(1), caret_line);

    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/editor_view_highlights.png");
    }
}

TEST_F(EditorViewTest, HighlightsAcrossLineBreaksAndEmptyRangesArePainted) {
    open("ab\ncd");
    const QColor background = view_.palette().color(QPalette::Base);
    const QColor caret_line = view_.palette().color(QPalette::AlternateBase);
    view_.set_highlights({{1, 1}, {1, 5}});
    QApplication::processEvents();
    const QImage image = view_.viewport()->grab().toImage();
    const auto color_at = [&](std::size_t offset) {
        const QRect rect = view_.cursor_rect(offset);
        return image.pixelColor(rect.x() + 2, rect.y() + 1);
    };
    EXPECT_NE(color_at(1), caret_line);
    EXPECT_NE(color_at(3), background);
    EXPECT_NE(color_at(4), background);
    EXPECT_EQ(color_at(0), caret_line);
}

TEST_F(EditorViewTest, ShowingAnotherDocumentRemovesTheHighlights) {
    open("abc");
    view_.set_highlights({{0, 3}});
    view_.set_document(make_document("abc"));
    QApplication::processEvents();
    const QImage image = view_.viewport()->grab().toImage();
    const QRect rect = view_.cursor_rect(1);
    EXPECT_EQ(image.pixelColor(rect.x() + 2, rect.y() + 1),
              view_.palette().color(QPalette::AlternateBase));
}

TEST_F(EditorViewTest, SelectRangeSelectsAndScrollsToShowIt) {
    std::string lines;
    for (int i = 0; i < 500; ++i) {
        lines += "line " + std::to_string(i) + "\n";
    }
    open(lines);
    QSignalSpy selections(&view_, &EditorView::selections_changed);
    const std::size_t begin = text().find("line 400");
    view_.select_range({begin, begin + 8});
    EXPECT_EQ(primary(), (core::Selection{begin, begin + 8}));
    EXPECT_EQ(selections.count(), 1);
    const QRect rect = view_.cursor_rect(begin);
    EXPECT_GE(rect.y(), 0);
    EXPECT_LT(rect.y(), view_.viewport()->height());
}

TEST_F(EditorViewTest, ReplaceRangesReportsOneChangeAndUndoesAsOneStep) {
    open("one two one");
    QSignalSpy changes(&view_, &EditorView::text_changed);
    QSignalSpy content(&view_, &EditorView::content_changed);
    view_.replace_ranges({{0, 3, "1"}, {8, 11, "1"}});
    EXPECT_EQ(text(), "1 two 1");
    EXPECT_EQ(changes.count(), 1);
    EXPECT_EQ(content.count(), 1);
    view_.undo();
    EXPECT_EQ(text(), "one two one");
}

TEST_F(EditorViewTest, ContentChangedIsReportedForEditsAndForOtherDocuments) {
    open("abc");
    QSignalSpy content(&view_, &EditorView::content_changed);
    type("x");
    EXPECT_EQ(content.count(), 1);
    key(Qt::Key_Left);
    EXPECT_EQ(content.count(), 1);
    view_.document_changed_elsewhere();
    EXPECT_EQ(content.count(), 2);
    view_.set_document(make_document("other"));
    EXPECT_EQ(content.count(), 3);
}

TEST_F(EditorViewTest, TheGutterWidensForLongDocuments) {
    open("a\nb");
    const int narrow = view_.cursor_rect(0).x();
    std::string lines;
    for (int i = 0; i < 20000; ++i) {
        lines += "x\n";
    }
    open(lines);
    EXPECT_GT(view_.cursor_rect(0).x(), narrow);
}

// --- copying, cutting and pasting lines ---

namespace {

constexpr const char* line_marker_format = "application/x-anvil-editor-data";

QString clipboard_text() {
    return QGuiApplication::clipboard()->text();
}

bool clipboard_marks_a_line() {
    return QGuiApplication::clipboard()->mimeData()->hasFormat(line_marker_format);
}

} // namespace

TEST_F(EditorViewTest, CopyWithACaretPutsItsLineOnTheClipboardMarkedAsALine) {
    open("one\ntwo\nthree");
    view_.select_range({5, 5});
    view_.copy();
    EXPECT_EQ(clipboard_text(), "two\n");
    EXPECT_TRUE(clipboard_marks_a_line());
    EXPECT_EQ(text(), "one\ntwo\nthree");
}

TEST_F(EditorViewTest, CopyOfASelectionIsPlainTextWithoutTheMarker) {
    open("one\ntwo\nthree");
    view_.select_range({4, 7});
    view_.copy();
    EXPECT_EQ(clipboard_text(), "two");
    EXPECT_FALSE(clipboard_marks_a_line());
}

TEST_F(EditorViewTest, CopyWithSeveralCaretsLeavesTheClipboardAlone) {
    open("one\ntwo\nthree");
    QGuiApplication::clipboard()->setText("keep");
    click(1);
    click(5, Qt::AltModifier);
    ASSERT_EQ(document_->selections().all().size(), 2u);
    view_.copy();
    view_.cut();
    EXPECT_EQ(clipboard_text(), "keep");
    EXPECT_EQ(text(), "one\ntwo\nthree");
}

TEST_F(EditorViewTest, CutWithACaretRemovesTheLineAndCopiesIt) {
    open("one\ntwo\nthree");
    view_.select_range({5, 5});
    QSignalSpy changes(&view_, &EditorView::text_changed);
    view_.cut();
    EXPECT_EQ(text(), "one\nthree");
    EXPECT_EQ(clipboard_text(), "two\n");
    EXPECT_TRUE(clipboard_marks_a_line());
    EXPECT_EQ(changes.count(), 1);
    view_.undo();
    EXPECT_EQ(text(), "one\ntwo\nthree");
}

TEST_F(EditorViewTest, CutOfTheLastLineLeavesNoEmptyLineBehind) {
    open("one\ntwo");
    view_.select_range({6, 6});
    view_.cut();
    EXPECT_EQ(text(), "one");
    EXPECT_EQ(clipboard_text(), "two\n");
}

TEST_F(EditorViewTest, CutOfASelectionStillCutsTheSelection) {
    open("one\ntwo");
    view_.select_range({4, 7});
    view_.cut();
    EXPECT_EQ(text(), "one\n");
    EXPECT_EQ(clipboard_text(), "two");
    EXPECT_FALSE(clipboard_marks_a_line());
}

TEST_F(EditorViewTest, ALineCopiedFromACaretPastesAboveTheLineOfTheCaret) {
    open("one\ntwo\nthree");
    view_.select_range({9, 9});
    view_.copy();
    view_.select_range({5, 5});
    view_.paste();
    EXPECT_EQ(text(), "one\nthree\ntwo\nthree");
    EXPECT_EQ(primary(), core::Selection::caret(11));
}

TEST_F(EditorViewTest, CutAndPasteMoveALineInTwoUndoSteps) {
    open("one\ntwo\nthree");
    view_.select_range({1, 1});
    view_.cut();
    ASSERT_EQ(text(), "two\nthree");
    view_.select_range({5, 5});
    view_.paste();
    EXPECT_EQ(text(), "two\none\nthree");
    view_.undo();
    EXPECT_EQ(text(), "two\nthree");
    view_.undo();
    EXPECT_EQ(text(), "one\ntwo\nthree");
}

TEST_F(EditorViewTest, TheSameTextWithoutTheMarkerPastesAtTheCaret) {
    open("one\ntwo");
    QGuiApplication::clipboard()->setText("new\n");
    view_.select_range({5, 5});
    view_.paste();
    EXPECT_EQ(text(), "one\nt"
                      "new\n"
                      "wo");
}

TEST_F(EditorViewTest, CopyingSomethingElseDropsTheLineMarker) {
    open("one\ntwo");
    view_.select_range({1, 1});
    view_.copy();
    ASSERT_TRUE(clipboard_marks_a_line());
    view_.select_range({4, 7});
    view_.copy();
    view_.select_range({1, 1});
    view_.paste();
    EXPECT_EQ(text(), "otwone\ntwo");
}

TEST_F(EditorViewTest, ALineCopiedFromACaretPastesIntoAnEmptyDocument) {
    open("only");
    view_.select_range({2, 2});
    view_.copy();
    view_.select_all();
    key(Qt::Key_Backspace);
    ASSERT_EQ(text(), "");
    view_.paste();
    EXPECT_EQ(text(), "only\n");
}

namespace {

core::EditorSettings settings(int font_size = 14, int tab_size = 4, bool insert_spaces = true,
                              std::string font_family = "") {
    core::EditorSettings result;
    result.font_size = font_size;
    result.tab_size = tab_size;
    result.insert_spaces = insert_spaces;
    result.font_family = std::move(font_family);
    return result;
}

} // namespace

TEST_F(EditorViewTest, TheFontSizeSetsTheLineHeightAndTheCharacterWidth) {
    open("abc");
    view_.apply_settings(settings(12));
    const int small_height = view_.line_height();
    const int small_width = view_.char_width();
    EXPECT_EQ(view_.font().pixelSize(), 12);

    view_.apply_settings(settings(30));
    EXPECT_EQ(view_.font().pixelSize(), 30);
    EXPECT_GT(view_.line_height(), small_height);
    EXPECT_GT(view_.char_width(), small_width);
}

TEST_F(EditorViewTest, ChangingTheFontKeepsTheCaretSelectionAndTopLine) {
    std::string lines;
    for (int i = 0; i < 300; ++i) {
        lines += "line " + std::to_string(i) + (i < 299 ? "\n" : "");
    }
    open(lines);
    view_.apply_settings(settings(14));
    view_.select_range({20, 30});
    view_.verticalScrollBar()->setValue(100 * view_.line_height());
    const auto selections = document_->selections();
    const int top_line_before = view_.verticalScrollBar()->value() / view_.line_height();

    view_.apply_settings(settings(28));
    EXPECT_EQ(document_->selections(), selections);
    EXPECT_EQ(view_.verticalScrollBar()->value() / view_.line_height(), top_line_before);
    EXPECT_EQ(view_.verticalScrollBar()->value() % view_.line_height(), 0);
    EXPECT_EQ(view_.verticalScrollBar()->singleStep(), view_.line_height());
}

TEST_F(EditorViewTest, ChangingTheFontKeepsTheHorizontalScrollPosition) {
    open(std::string(400, 'x'));
    view_.apply_settings(settings(14));
    view_.horizontalScrollBar()->setValue(50 * view_.char_width());
    view_.apply_settings(settings(28));
    EXPECT_EQ(view_.horizontalScrollBar()->value() / view_.char_width(), 50);
}

TEST_F(EditorViewTest, TheScrollRangeFollowsTheLineHeight) {
    open(std::string(200, '\n'));
    view_.apply_settings(settings(14));
    const int before = view_.verticalScrollBar()->maximum();
    view_.apply_settings(settings(40));
    EXPECT_GT(view_.verticalScrollBar()->maximum(), before);
}

TEST_F(EditorViewTest, AFontFamilyThatIsNotInstalledFallsBackToTheFixedFont) {
    open("abc");
    view_.apply_settings(settings(14, 4, true, "No Such Family Anvil, 'Neither This One'"));
    EXPECT_EQ(view_.font().family(), QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
}

TEST_F(EditorViewTest, AnInstalledFamilyInTheListIsUsedInstead) {
    const QStringList installed = QFontDatabase::families();
    if (installed.isEmpty()) {
        GTEST_SKIP() << "no fonts are installed";
    }
    open("abc");
    const std::string list =
        "'No Such Family Anvil', \"" + installed.first().toLower().toStdString() + "\", monospace";
    view_.apply_settings(settings(14, 4, true, list));
    EXPECT_EQ(view_.font().family(), installed.first());
}

TEST_F(EditorViewTest, MonospaceStandsForTheFixedFont) {
    const QStringList installed = QFontDatabase::families();
    if (installed.isEmpty()) {
        GTEST_SKIP() << "no fonts are installed";
    }
    open("abc");
    view_.apply_settings(
        settings(14, 4, true, "monospace, '" + installed.first().toStdString() + "'"));
    EXPECT_EQ(view_.font().family(), QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
}

TEST_F(EditorViewTest, TheTabSizeSetsTheWidthOfATabCharacter) {
    open("\tx");
    view_.apply_settings(settings(14, 4));
    const int four = view_.cursor_rect(1).x() - view_.cursor_rect(0).x();
    EXPECT_EQ(four, 4 * view_.char_width());
    view_.apply_settings(settings(14, 8));
    EXPECT_EQ(view_.cursor_rect(1).x() - view_.cursor_rect(0).x(), 8 * view_.char_width());
}

TEST_F(EditorViewTest, TabInsertsSpacesUpToTheNextMultipleOfTheTabSize) {
    struct Case {
        int tab_size;
        std::string line;
        std::size_t caret;
        std::string expected;
    };
    for (const Case& c : std::vector<Case>{
             {4, "", 0, "    "},
             {4, "a", 1, "a   "},
             {4, "ab", 2, "ab  "},
             {4, "abc", 3, "abc "},
             {4, "abcd", 4, "abcd    "},
             {4, "abcde", 5, "abcde   "},
             {2, "a", 1, "a "},
             {2, "ab", 2, "ab  "},
             {8, "abc", 3, "abc     "},
             {1, "abc", 3, "abc "},
             {4, "\xC3\xA9", 2, "\xC3\xA9   "},
         }) {
        open(c.line);
        view_.apply_settings(settings(14, c.tab_size, true));
        view_.select_range(core::Selection::caret(c.caret));
        key(Qt::Key_Tab);
        EXPECT_EQ(text(), c.expected) << "tab size " << c.tab_size << ", line '" << c.line << "'";
    }
}

TEST_F(EditorViewTest, TabCountsATabCharacterAsAdvancingToTheNextTabStop) {
    open("\tab");
    view_.apply_settings(settings(14, 4, true));
    view_.select_range(core::Selection::caret(3));
    key(Qt::Key_Tab);
    EXPECT_EQ(text(), "\tab  ");
}

TEST_F(EditorViewTest, TabInsertsATabCharacterWhenSpacesAreTurnedOff) {
    for (const std::size_t caret : {0u, 1u, 2u, 3u}) {
        open("abc");
        view_.apply_settings(settings(14, 4, false));
        view_.select_range(core::Selection::caret(caret));
        key(Qt::Key_Tab);
        std::string expected = "abc";
        expected.insert(caret, "\t");
        EXPECT_EQ(text(), expected);
    }
}

TEST_F(EditorViewTest, TabFollowsTheLatestSettings) {
    open("");
    view_.apply_settings(settings(14, 2, false));
    view_.apply_settings(settings(14, 3, true));
    key(Qt::Key_Tab);
    EXPECT_EQ(text(), "   ");
}

TEST_F(EditorViewTest, ApplyingTheSameSettingsAgainKeepsTheExactScrollPosition) {
    std::string many;
    for (int i = 0; i < 200; ++i) {
        many += "line " + std::to_string(i) + "\n";
    }
    open(many);
    core::EditorSettings settings;
    settings.font_family = "monospace";
    settings.font_size = 14;
    view_.apply_settings(settings);
    view_.verticalScrollBar()->setValue(301);
    ASSERT_EQ(view_.verticalScrollBar()->value(), 301);

    view_.apply_settings(settings);
    EXPECT_EQ(view_.verticalScrollBar()->value(), 301);

    settings.font_size = 20;
    view_.apply_settings(settings);
    EXPECT_EQ(view_.verticalScrollBar()->value() % view_.line_height(), 0);
}

namespace {

// How far the pixel of the image nearest to the color is from it, the largest difference of a
// channel. Antialiased text may not contain the exact color.
int nearest(const QImage& image, const QRect& area, const QColor& color) {
    int best = 255;
    for (int y = std::max(0, area.top()); y <= std::min(image.height() - 1, area.bottom()); ++y) {
        for (int x = std::max(0, area.left()); x <= std::min(image.width() - 1, area.right());
             ++x) {
            const QColor pixel = image.pixelColor(x, y);
            best = std::min(best, std::max({std::abs(pixel.red() - color.red()),
                                            std::abs(pixel.green() - color.green()),
                                            std::abs(pixel.blue() - color.blue())}));
        }
    }
    return best;
}

QColor blended(const QColor& over, const QColor& under) {
    const double a = over.alphaF();
    return QColor(static_cast<int>(std::lround(over.red() * a + under.red() * (1 - a))),
                  static_cast<int>(std::lround(over.green() * a + under.green() * (1 - a))),
                  static_cast<int>(std::lround(over.blue() * a + under.blue() * (1 - a))));
}

bool close(const QColor& a, const QColor& b, int tolerance = 2) {
    return std::abs(a.red() - b.red()) <= tolerance &&
           std::abs(a.green() - b.green()) <= tolerance &&
           std::abs(a.blue() - b.blue()) <= tolerance;
}

} // namespace

TEST_F(EditorViewTest, PaintsWithTheColorsOfTheWorkbench) {
    WorkbenchColors colors(QApplication::palette());
    colors.set_colors({{"editor.background", QColor("#102030")},
                       {"editor.foreground", QColor("#ffffff")},
                       {"editorGutter.background", QColor("#0a0a0a")},
                       {"editor.lineHighlightBackground", QColor("#203040")},
                       {"editorLineNumber.foreground", QColor("#ff0000")},
                       {"editorLineNumber.activeForeground", QColor("#00ff00")},
                       {"editorCursor.foreground", QColor("#ffff00")}});
    view_.set_colors(&colors);
    open("one\ntwo\nthree");
    QApplication::processEvents();
    const QImage image = view_.viewport()->grab().toImage();

    const int right = image.width() - 5;
    const int line = view_.line_height();
    EXPECT_EQ(image.pixelColor(right, line * 2 + 2), QColor("#102030"));
    EXPECT_EQ(image.pixelColor(right, 2), QColor("#203040"));
    EXPECT_EQ(image.pixelColor(2, line + 2), QColor("#0a0a0a"));

    // The caret is drawn while the view has the focus.
    if (view_.hasFocus()) {
        EXPECT_EQ(image.pixelColor(view_.cursor_rect(0).x(), line / 2), QColor("#ffff00"));
    }

    const QRect gutter(0, 0, view_.cursor_rect(0).x() - 4, line * 3);
    EXPECT_LE(nearest(image, QRect(0, 0, gutter.width(), line), QColor("#00ff00")), 40);
    EXPECT_LE(nearest(image, QRect(0, line, gutter.width(), line), QColor("#ff0000")), 40);
    EXPECT_GT(nearest(image, QRect(0, line, gutter.width(), line), QColor("#00ff00")), 100);
    EXPECT_LE(
        nearest(image, QRect(view_.cursor_rect(0).x(), line * 2, 60, line), QColor("#ffffff")), 40);
}

TEST_F(EditorViewTest, ASelectionOfTheThemeIsDrawnWithItsAlpha) {
    WorkbenchColors colors(QApplication::palette());
    colors.set_colors({{"editor.background", QColor("#102030")},
                       {"editor.selectionBackground", QColor(0, 255, 0, 128)}});
    view_.set_colors(&colors);
    open("one\ntwo\nthree");
    key(Qt::Key_Right, Qt::ShiftModifier);
    key(Qt::Key_Right, Qt::ShiftModifier);
    key(Qt::Key_Right, Qt::ShiftModifier);
    QApplication::processEvents();
    const QImage image = view_.viewport()->grab().toImage();

    const QRect selected = view_.cursor_rect(1);
    const QColor shown = image.pixelColor(selected.x() + 2, selected.y() + 1);
    const QColor expected = blended(QColor(0, 255, 0, 128), QColor("#102030"));
    EXPECT_TRUE(close(shown, expected))
        << shown.name().toStdString() << " vs " << expected.name().toStdString();
    EXPECT_NE(shown, QColor(0, 255, 0));
}

TEST_F(
    EditorViewTest,
    TextUnderAThemedSelectionKeepsItsColorButUnderAPaletteSelectionTakesTheHighlightedTextColor) {
    QPalette palette = view_.palette();
    palette.setColor(QPalette::HighlightedText, QColor("#00ff00"));
    palette.setColor(QPalette::Highlight, QColor("#000000"));
    view_.setPalette(palette);
    WorkbenchColors colors(view_.palette());
    view_.set_colors(&colors);
    open("one two three");
    key(Qt::Key_Right, Qt::ShiftModifier);
    key(Qt::Key_Right, Qt::ShiftModifier);
    key(Qt::Key_Right, Qt::ShiftModifier);
    QApplication::processEvents();
    const QRect selected(view_.cursor_rect(0).x(), 0,
                         view_.cursor_rect(3).x() - view_.cursor_rect(0).x(), view_.line_height());

    // No theme sets the selection: the palette's highlighted text color is used.
    QImage image = view_.viewport()->grab().toImage();
    EXPECT_LE(nearest(image, selected, QColor("#00ff00")), 40);

    colors.set_colors({{"editor.foreground", QColor("#ffffff")},
                       {"editor.selectionBackground", QColor("#000000")}});
    QApplication::processEvents();
    image = view_.viewport()->grab().toImage();
    EXPECT_LE(nearest(image, selected, QColor("#ffffff")), 40);
    EXPECT_GT(nearest(image, selected, QColor("#00ff00")), 100);
}

TEST_F(EditorViewTest, FindMatchesAreDrawnWithTheThemesFindColorsAndTheCurrentOneStandsOut) {
    WorkbenchColors colors(QApplication::palette());
    colors.set_colors({{"editor.background", QColor("#102030")},
                       {"editor.selectionBackground", QColor(0, 0, 0, 0)},
                       {"editor.findMatchBackground", QColor("#ff0000")},
                       {"editor.findMatchHighlightBackground", QColor("#0000ff")}});
    view_.set_colors(&colors);
    open("aa bbb cc ddd");
    view_.set_highlights({{3, 6}, {10, 13}});
    view_.select_range({3, 6});
    QApplication::processEvents();
    const QImage image = view_.viewport()->grab().toImage();

    const QRect current = view_.cursor_rect(4);
    const QRect other = view_.cursor_rect(11);
    EXPECT_EQ(image.pixelColor(current.x() + 2, current.y() + 1), QColor("#ff0000"));
    EXPECT_EQ(image.pixelColor(other.x() + 2, other.y() + 1), QColor("#0000ff"));
}

namespace {

class PaintCounter : public QObject {
public:
    int paints = 0;

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        paints += event->type() == QEvent::Paint ? 1 : 0;
        return false;
    }
};

} // namespace

TEST_F(EditorViewTest, ChangingTheColorsSchedulesARepaint) {
    WorkbenchColors colors(QApplication::palette());
    view_.set_colors(&colors);
    open("text");
    QApplication::processEvents();
    PaintCounter counter;
    view_.viewport()->installEventFilter(&counter);

    colors.set_colors({{"editor.background", QColor("#334455")}});
    QApplication::processEvents();
    EXPECT_GE(counter.paints, 1);

    view_.set_colors(nullptr);
    QApplication::processEvents();
    counter.paints = 0;
    colors.set_colors({{"editor.background", QColor("#554433")}});
    QApplication::processEvents();
    EXPECT_EQ(counter.paints, 0);
}

TEST_F(EditorViewTest, ChangingTheColorsRepaintsAndTakingThemAwayReturnsToThePalette) {
    WorkbenchColors colors(QApplication::palette());
    view_.set_colors(&colors);
    open("text");
    colors.set_colors({{"editor.background", QColor("#334455")}});
    QApplication::processEvents();
    QImage image = view_.viewport()->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() - 5, image.height() - 5), QColor("#334455"));

    colors.set_colors({{"editor.background", QColor("#554433")}});
    QApplication::processEvents();
    image = view_.viewport()->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() - 5, image.height() - 5), QColor("#554433"));

    view_.set_colors(nullptr);
    QApplication::processEvents();
    image = view_.viewport()->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() - 5, image.height() - 5),
              view_.palette().color(QPalette::Base));
}
