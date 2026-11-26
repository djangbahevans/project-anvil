#include "anvil/ui/editor_view.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>

using namespace anvil;
using anvil::ui::EditorView;
using anvil::ui::WorkbenchColors;

namespace {

std::shared_ptr<core::Document> make_document(std::string_view text) {
    return std::make_shared<core::Document>(*core::Text::from_utf8(text));
}

class EditorViewTest : public ::testing::Test {
protected:
    void set_selections(std::vector<core::Selection> selections) {
        ASSERT_TRUE(
            document_->set_selections(core::SelectionSet::from(std::move(selections))).has_value());
        view_.document_changed_elsewhere();
    }

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

TEST_F(EditorViewTest, CopyWithSeveralCaretsPutsTheirLinesOnTheClipboardWithTheSeparatePieces) {
    open("one\ntwo\nthree");
    click(1);
    click(10, Qt::AltModifier);
    ASSERT_EQ(document_->selections().all().size(), 2u);
    view_.copy();
    EXPECT_EQ(clipboard_text(), "one\n\nthree\n");
    ASSERT_TRUE(clipboard_marks_a_line());
    const QJsonObject metadata =
        QJsonDocument::fromJson(QGuiApplication::clipboard()->mimeData()->data(line_marker_format))
            .object();
    EXPECT_FALSE(metadata.value("isFromEmptySelection").toBool());
    EXPECT_EQ(metadata.value("multicursorText").toArray(),
              (QJsonArray{QStringLiteral("one\n"), QStringLiteral("three\n")}));
    EXPECT_EQ(text(), "one\ntwo\nthree");
}

TEST_F(EditorViewTest, CopyOfSeveralSelectionsPastesOnePieceIntoEachCursor) {
    open("aa bb cc");
    set_selections({{0, 2}, {6, 8}});
    view_.copy();
    EXPECT_EQ(clipboard_text(), "aa\ncc");
    set_selections({core::Selection::caret(2), core::Selection{3, 5}});
    view_.paste();
    EXPECT_EQ(text(), "aaaa cc cc");
}

TEST_F(EditorViewTest, PiecesThatSpanLinesStillGoOneToEachCursor) {
    open("a\nb c d\ne");
    set_selections({core::Selection{0, 3}, core::Selection{8, 9}});
    view_.copy();
    EXPECT_EQ(clipboard_text(), "a\nb\ne");
    set_selections({core::Selection::caret(4), core::Selection::caret(9)});
    view_.paste();
    EXPECT_EQ(text(), "a\nb a\nbc d\nee");
}

TEST_F(EditorViewTest, CutWithSeveralCaretsRemovesTheirLinesAndCopiesThem) {
    open("one\ntwo\nthree\nfour");
    set_selections({core::Selection::caret(1), core::Selection::caret(9)});
    QSignalSpy changes(&view_, &EditorView::text_changed);
    view_.cut();
    EXPECT_EQ(text(), "two\nfour");
    EXPECT_EQ(clipboard_text(), "one\n\nthree\n");
    EXPECT_EQ(changes.count(), 1);
    view_.undo();
    EXPECT_EQ(text(), "one\ntwo\nthree\nfour");
}

TEST_F(EditorViewTest, CutWithASelectionAndACaretRemovesBoth) {
    open("one\ntwo\nthree");
    set_selections({core::Selection{0, 2}, core::Selection::caret(9)});
    view_.cut();
    EXPECT_EQ(text(), "e\ntwo");
    EXPECT_EQ(clipboard_text(), "on\nthree\n");
}

TEST_F(EditorViewTest, TextFromAnotherProgramWithAsManyLinesAsCursorsGoesOneLineToEach) {
    open("a b c");
    set_selections(
        {core::Selection::caret(1), core::Selection::caret(3), core::Selection::caret(5)});
    QGuiApplication::clipboard()->setText("1\n2\n3\n");
    view_.paste();
    EXPECT_EQ(text(), "a1 b2 c3");
}

TEST_F(EditorViewTest, TextWithAnotherNumberOfLinesGoesToEveryCursor) {
    open("a b");
    set_selections({core::Selection::caret(1), core::Selection::caret(3)});
    QGuiApplication::clipboard()->setText("x\ny\nz");
    view_.paste();
    EXPECT_EQ(text(), "ax\ny\nz bx\ny\nz");
}

TEST_F(EditorViewTest, ALoneCaretsLineStillPastesAboveEveryCaretLine) {
    open("one\ntwo\nthree");
    view_.select_range({9, 9});
    view_.copy();
    set_selections({core::Selection::caret(1), core::Selection::caret(5)});
    view_.paste();
    EXPECT_EQ(text(), "three\none\nthree\ntwo\nthree");
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
    // The area that the paints asked to redraw, together.
    QRegion painted;

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            ++paints;
            painted += static_cast<QPaintEvent*>(event)->region();
        }
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

namespace {

class RenderWhitespaceTest : public EditorViewTest {
protected:
    static constexpr const char* mark_color = "#00ff00";

    void SetUp() override {
        colors_.set_colors({{"editor.background", QColor("#000000")},
                            {"editor.foreground", QColor("#ffffff")},
                            {"editor.selectionBackground", QColor("#000080")},
                            {"editor.lineHighlightBackground", QColor("#000000")},
                            {"editorGutter.background", QColor("#000000")},
                            {"editorLineNumber.foreground", QColor("#888888")},
                            {"editorLineNumber.activeForeground", QColor("#888888")},
                            {"editorCursor.foreground", QColor("#ffffff")},
                            {"editorWhitespace.foreground", QColor(mark_color)}});
        view_.set_colors(&colors_);
        core::EditorSettings settings;
        settings.font_family = "monospace";
        settings.font_size = 14;
        view_.apply_settings(settings);
    }

    void use(core::RenderWhitespace mode, bool control_characters = true) {
        core::EditorSettings settings;
        settings.font_family = "monospace";
        settings.font_size = 14;
        settings.render_whitespace = mode;
        settings.render_control_characters = control_characters;
        view_.apply_settings(settings);
        QApplication::processEvents();
    }

    // The columns of the first line that have a green mark inside their cell.
    std::vector<std::size_t> marked_columns(std::size_t columns, std::size_t first = 0) {
        const QImage image = view_.viewport()->grab().toImage();
        std::vector<std::size_t> marked;
        for (std::size_t column = first; column < columns; ++column) {
            const QRect from = view_.cursor_rect(column);
            const QRect to = view_.cursor_rect(column + 1);
            if (count_pixels(
                    image, QRect(from.x(), 0, std::max(to.x() - from.x(), 1), view_.line_height()),
                    [](const QColor& c) {
                        return c.green() > 150 && c.red() < 100 && c.blue() < 100;
                    }) > 0) {
                marked.push_back(column);
            }
        }
        return marked;
    }

    template <class Match>
    static int count_pixels(const QImage& image, const QRect& area, Match match) {
        int count = 0;
        for (int y = area.top(); y <= area.bottom() && y < image.height(); ++y) {
            for (int x = area.left(); x <= area.right() && x < image.width(); ++x) {
                count += match(image.pixelColor(x, y)) ? 1 : 0;
            }
        }
        return count;
    }

    WorkbenchColors colors_{QApplication::palette()};
};

using Columns = std::vector<std::size_t>;

TEST_F(RenderWhitespaceTest, EachModeMarksTheSpacesAndTabsThatVSCodeMarks) {
    open("a b  c\t");
    set_selections({core::Selection{0, 5}});
    use(core::RenderWhitespace::none);
    EXPECT_EQ(marked_columns(7), Columns{});
    use(core::RenderWhitespace::all);
    EXPECT_EQ(marked_columns(7), (Columns{1, 3, 4, 6}));
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        view_.viewport()->grab().save(QString::fromUtf8(directory) + "/render_whitespace.png");
    }
    use(core::RenderWhitespace::boundary);
    EXPECT_EQ(marked_columns(7), (Columns{3, 4, 6}));
    use(core::RenderWhitespace::trailing);
    EXPECT_EQ(marked_columns(7), (Columns{6}));
    use(core::RenderWhitespace::selection);
    EXPECT_EQ(marked_columns(7), (Columns{1, 3, 4}));
}

TEST_F(RenderWhitespaceTest, SelectionMarksFollowTheSelection) {
    open("a b c d");
    use(core::RenderWhitespace::selection);
    set_selections({core::Selection{3, 3}});
    EXPECT_EQ(marked_columns(7), Columns{});
    set_selections({core::Selection{0, 4}});
    EXPECT_EQ(marked_columns(7), (Columns{1, 3}));
    set_selections({core::Selection{0, 2}, core::Selection{4, 7}});
    EXPECT_EQ(marked_columns(7), (Columns{1, 5}));
}

TEST_F(RenderWhitespaceTest, TheMarksTakeTheColorOfTheWhitespaceKey) {
    open("a b");
    use(core::RenderWhitespace::all);
    ASSERT_EQ(marked_columns(3), Columns{1});
    colors_.set_colors({{"editorWhitespace.foreground", QColor("#ff00ff")}});
    QApplication::processEvents();
    EXPECT_EQ(marked_columns(3), Columns{});
}

TEST_F(RenderWhitespaceTest, MarksAreDrawnFarToTheRightOfALongLine) {
    open(std::string(300, 'x') + "  " + std::string(300, 'y'));
    use(core::RenderWhitespace::all);
    const int character = view_.cursor_rect(1).x() - view_.cursor_rect(0).x();
    view_.horizontalScrollBar()->setValue(character * 300 - 100);
    QApplication::processEvents();
    EXPECT_EQ(marked_columns(304, 296), (Columns{300, 301}));
}

TEST_F(RenderWhitespaceTest, ControlCharactersAreDrawnAsSymbolsOnRed) {
    open(std::string("a\x01") + "b\x7f" + "\tc");
    const auto red_cells = [&] {
        const QImage image = view_.viewport()->grab().toImage();
        Columns cells;
        for (std::size_t column = 0; column < 6; ++column) {
            const QRect from = view_.cursor_rect(column);
            const QRect to = view_.cursor_rect(column + 1);
            if (count_pixels(
                    image,
                    QRect(from.x() + 2, 0, std::max(to.x() - from.x() - 4, 1), view_.line_height()),
                    [](const QColor& c) {
                        return c.red() > 120 && c.red() < 180 && c.green() < 30;
                    }) > 0) {
                cells.push_back(column);
            }
        }
        return cells;
    };
    use(core::RenderWhitespace::none, true);
    EXPECT_EQ(red_cells(), (Columns{1, 3}));
    use(core::RenderWhitespace::none, false);
    EXPECT_EQ(red_cells(), Columns{});
}

// Marking only the columns in view costs a few times what painting the text does. Marking every
// column of the line, in view or not, costs tens of times as much, so the limit sits between the
// two with room for the speed of the machine. The time is the CPU time of the process, which a
// busy machine does not stretch the way it stretches the time of a clock.
TEST_F(RenderWhitespaceTest, MarkingEveryCharacterOfLongLinesStaysCheap) {
    std::string text;
    for (int line = 0; line < 60; ++line) {
        text += std::string(4000, ' ') + "x\n";
    }
    open(text);
    view_.resize(1000, 900);
    const auto paint = [&](core::RenderWhitespace mode) {
        use(mode);
        auto best = std::numeric_limits<std::clock_t>::max();
        for (int round = 0; round < 7; ++round) {
            const std::clock_t start = std::clock();
            view_.viewport()->grab();
            best = std::min(best, std::clock() - start);
        }
        return static_cast<double>(best) / CLOCKS_PER_SEC;
    };
    view_.horizontalScrollBar()->setValue(view_.horizontalScrollBar()->maximum());
    const double without = paint(core::RenderWhitespace::none);
    const double with = paint(core::RenderWhitespace::all);
    EXPECT_LT(with, without * 12 + 0.02)
        << "CPU time without marks: " << without * 1e6 << " us, with: " << with * 1e6 << " us";
}

} // namespace

namespace {

// Gives the words of a document the scopes of a table, as a highlighter that is not a parser.
class FakeHighlighter : public core::Highlighter {
public:
    FakeHighlighter(std::shared_ptr<core::Document> document,
                    std::map<std::string, std::string> scopes)
        : document_(std::move(document)), scopes_(std::move(scopes)) {}

    std::vector<core::HighlightSpan> spans(std::size_t line) const override {
        asked_.push_back(line);
        std::vector<core::HighlightSpan> spans;
        const std::string text = document_->text().line(line);
        for (const auto& [word, scope] : scopes_) {
            for (std::size_t at = text.find(word); at != std::string::npos;
                 at = text.find(word, at + word.size())) {
                spans.push_back({at, at + word.size(), scope});
            }
        }
        std::ranges::sort(spans, {}, &core::HighlightSpan::begin);
        return spans;
    }
    std::string_view root_scope() const override {
        return "source.fake";
    }
    std::uint64_t version() const override {
        return version_;
    }
    void bump() {
        ++version_;
    }
    void set_scopes(std::map<std::string, std::string> scopes) {
        scopes_ = std::move(scopes);
    }
    // The lines that were asked for since the last `clear_asked`.
    mutable std::vector<std::size_t> asked_;
    void clear_asked() const {
        asked_.clear();
    }

private:
    std::shared_ptr<core::Document> document_;
    std::map<std::string, std::string> scopes_;
    std::uint64_t version_ = 1;
};

const char* const syntax_rules = R"j([
    {"settings": {"foreground": "#d4d4d4"}},
    {"scope": "keyword.control", "settings": {"foreground": "#ff0000"}},
    {"scope": "string", "settings": {"foreground": "#00ff00", "fontStyle": "italic"}},
    {"scope": "markup.underline", "settings": {"foreground": "#0000ff", "fontStyle": "underline"}},
    {"scope": "invalid", "settings": {"foreground": "#ffff00", "background": "#303030"}}
])j";

std::shared_ptr<const core::TokenTheme> token_theme_of(const char* rules) {
    return std::make_shared<const core::TokenTheme>(
        core::TokenTheme::from_json(nlohmann::json::parse(rules)));
}

class SyntaxHighlightingTest : public EditorViewTest {
protected:
    void SetUp() override {
        colors_.set_colors({{"editor.background", QColor("#000000")},
                            {"editor.foreground", QColor("#ffffff")},
                            {"editor.selectionBackground", QColor("#000080")},
                            {"editor.lineHighlightBackground", QColor("#000000")},
                            {"editorGutter.background", QColor("#000000")},
                            {"editorLineNumber.foreground", QColor("#888888")},
                            {"editorLineNumber.activeForeground", QColor("#888888")},
                            {"editorCursor.foreground", QColor("#000000")}});
        colors_.set_token_theme(token_theme_of(syntax_rules));
        view_.set_colors(&colors_);
        core::EditorSettings settings;
        settings.font_family = "monospace";
        settings.font_size = 14;
        view_.apply_settings(settings);
    }

    void highlight(std::map<std::string, std::string> scopes) {
        highlighter_ = std::make_shared<FakeHighlighter>(document_, std::move(scopes));
        view_.set_highlighter(highlighter_);
        QApplication::processEvents();
    }

    QImage painted() {
        QApplication::processEvents();
        return view_.viewport()->grab().toImage();
    }

    // The pixels in the cells of the columns [from, to) of the first line that match.
    template <class Match>
    int count_in(const QImage& image, std::size_t from, std::size_t to, Match match) {
        const QRect begin = view_.cursor_rect(from);
        const QRect end = view_.cursor_rect(to);
        int count = 0;
        for (int y = 0; y < view_.line_height() && y < image.height(); ++y) {
            for (int x = begin.x(); x < end.x() && x < image.width(); ++x) {
                count += match(image.pixelColor(x, y)) ? 1 : 0;
            }
        }
        return count;
    }

    static bool is_red(const QColor& c) {
        return c.red() > 200 && c.green() < 80 && c.blue() < 80;
    }
    static bool is_green(const QColor& c) {
        return c.green() > 200 && c.red() < 80 && c.blue() < 80;
    }
    static bool is_white(const QColor& c) {
        return c.red() > 200 && c.green() > 200 && c.blue() > 200;
    }

    WorkbenchColors colors_{QApplication::palette()};
    std::shared_ptr<FakeHighlighter> highlighter_;
};

} // namespace

TEST_F(SyntaxHighlightingTest, TheTextOfAScopeTakesItsColorAndTheRestKeepsTheEditorsForeground) {
    open("let name = 'hi';");
    highlight({{"let", "keyword.control.fake"}, {"'hi'", "string.quoted.fake"}});
    const QImage image = painted();
    EXPECT_GT(count_in(image, 0, 3, is_red), 10) << "the keyword";
    EXPECT_EQ(count_in(image, 0, 3, is_white), 0);
    EXPECT_GT(count_in(image, 11, 15, is_green), 10) << "the string";
    EXPECT_GT(count_in(image, 4, 8, is_white), 10) << "a word with no scope";
    EXPECT_EQ(count_in(image, 4, 8, is_red), 0);
}

TEST_F(SyntaxHighlightingTest, RendersAHighlightedSample) {
    colors_.set_token_theme(token_theme_of(R"j([
        {"settings": {"foreground": "#d4d4d4"}},
        {"scope": "keyword.control", "settings": {"foreground": "#c586c0"}},
        {"scope": "storage.type", "settings": {"foreground": "#569cd6"}},
        {"scope": "entity.name.function", "settings": {"foreground": "#dcdcaa"}},
        {"scope": "string", "settings": {"foreground": "#ce9178", "fontStyle": "italic"}},
        {"scope": "constant.numeric", "settings": {"foreground": "#b5cea8"}},
        {"scope": "markup.underline", "settings": {"foreground": "#3794ff", "fontStyle": "underline"}}])j"));
    open("int main() {\n    if (argc > 1) return 42;\n    puts(\"hello\");\n    // see docs\n}\n");
    highlight({{"int", "storage.type.fake"},
               {"main", "entity.name.function.fake"},
               {"puts", "entity.name.function.fake"},
               {"if", "keyword.control.fake"},
               {"return", "keyword.control.fake"},
               {"\"hello\"", "string.quoted.fake"},
               {"42", "constant.numeric.fake"},
               {"docs", "markup.underline.fake"}});
    const QImage image = painted();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/editor_view_syntax.png");
    }
}

TEST_F(SyntaxHighlightingTest, NoHighlighterShowsPlainText) {
    open("let name = 'hi';");
    const QImage image = painted();
    EXPECT_EQ(count_in(image, 0, 3, is_red), 0);
    EXPECT_GT(count_in(image, 0, 3, is_white), 10);
}

TEST_F(SyntaxHighlightingTest, ARuleThatHasNoColorForAScopeLeavesTheEditorsForeground) {
    open("let");
    highlight({{"let", "storage.type.fake"}});
    EXPECT_GT(count_in(painted(), 0, 3, is_white), 10);
}

TEST_F(SyntaxHighlightingTest, ItalicUnderlineAndABackgroundComeFromTheFontStyleAndTheRule) {
    open("see link and bad");
    highlight({{"link", "markup.underline.fake"}, {"bad", "invalid.fake"}});
    const QImage image = painted();
    const auto blue = [](const QColor& c) { return c.blue() > 200 && c.red() < 80; };
    // Under the text of the underlined word there is a line of its color, which plain words lack.
    int underline_rows = 0;
    for (int y = view_.line_height() / 2; y < view_.line_height(); ++y) {
        int blue_in_row = 0;
        for (int x = view_.cursor_rect(4).x(); x < view_.cursor_rect(8).x(); ++x) {
            blue_in_row += blue(image.pixelColor(x, y)) ? 1 : 0;
        }
        underline_rows +=
            blue_in_row >= 3 * (view_.cursor_rect(8).x() - view_.cursor_rect(4).x()) / 4;
    }
    EXPECT_GE(underline_rows, 1);
    // The background of a rule fills the cells of its word.
    const QRect cell = view_.cursor_rect(13);
    EXPECT_EQ(image.pixelColor(cell.x() + 1, 1), QColor("#303030"));
    EXPECT_EQ(image.pixelColor(view_.cursor_rect(9).x() + 1, 1), QColor("#000000"));
}

TEST_F(SyntaxHighlightingTest, ItalicChangesHowTheTextIsDrawn) {
    open("'hi hi hi'");
    highlight({{"'hi hi hi'", "string.quoted.fake"}});
    const QImage italic = painted();
    colors_.set_token_theme(token_theme_of(R"j([
        {"settings": {"foreground": "#d4d4d4"}},
        {"scope": "string", "settings": {"foreground": "#00ff00"}}])j"));
    const QImage upright = painted();
    EXPECT_NE(italic, upright);
    EXPECT_GT(count_in(upright, 0, 10, is_green), 10);
}

TEST_F(SyntaxHighlightingTest, ASelectionChangesTheBackgroundAndKeepsTheSyntaxColor) {
    open("let name = 'hi';");
    highlight({{"let", "keyword.control.fake"}});
    set_selections({{0, 3}});
    const QImage image = painted();
    const auto selection = [](const QColor& c) { return c.blue() > 100 && c.red() < 30; };
    EXPECT_GT(count_in(image, 0, 3, selection), 100) << "the selection background";
    EXPECT_GT(count_in(image, 0, 3, is_red), 10) << "the keyword keeps its color";
    EXPECT_EQ(count_in(image, 0, 3, is_white), 0);
}

TEST_F(SyntaxHighlightingTest, ANewTokenThemePaintsOnceAndTheNewColorShows) {
    open("let");
    highlight({{"let", "keyword.control.fake"}});
    painted();
    PaintCounter counter;
    view_.viewport()->installEventFilter(&counter);
    colors_.set_token_theme(token_theme_of(R"j([
        {"settings": {"foreground": "#d4d4d4"}},
        {"scope": "keyword.control", "settings": {"foreground": "#00ff00"}}])j"));
    QApplication::processEvents();
    EXPECT_EQ(counter.paints, 1);
    const QImage image = painted();
    EXPECT_GT(count_in(image, 0, 3, is_green), 10);
    EXPECT_EQ(count_in(image, 0, 3, is_red), 0);
}

TEST_F(SyntaxHighlightingTest, RefreshingPaintsOnlyWhenTheHighlighterHasNewSpans) {
    open("let");
    highlight({{"let", "keyword.control.fake"}});
    painted();
    PaintCounter counter;
    view_.viewport()->installEventFilter(&counter);
    view_.refresh_highlighting();
    QApplication::processEvents();
    EXPECT_EQ(counter.paints, 0);

    highlighter_->set_scopes({{"let", "string.fake"}});
    highlighter_->bump();
    view_.refresh_highlighting();
    view_.refresh_highlighting();
    QApplication::processEvents();
    EXPECT_EQ(counter.paints, 1);
}

TEST_F(SyntaxHighlightingTest, ANewVersionWithTheSameSpansPaintsNothing) {
    open("let x");
    highlight({{"let", "keyword.control.fake"}});
    painted();
    PaintCounter counter;
    view_.viewport()->installEventFilter(&counter);
    highlighter_->bump();
    view_.refresh_highlighting();
    QApplication::processEvents();
    EXPECT_EQ(counter.paints, 0);
}

TEST_F(SyntaxHighlightingTest, RefreshingPaintsOnlyTheLinesWhoseSpansChanged) {
    open("let a\nif b\nlet c\nlet d");
    highlight({{"let", "keyword.control.fake"}, {"if", "string.fake"}});
    painted();
    PaintCounter counter;
    view_.viewport()->installEventFilter(&counter);

    highlighter_->set_scopes({{"let", "keyword.control.fake"}, {"if", "invalid.fake"}});
    highlighter_->bump();
    view_.refresh_highlighting();
    QApplication::processEvents();

    ASSERT_EQ(counter.paints, 1);
    const QRect changed = counter.painted.boundingRect();
    EXPECT_EQ(changed.top(), view_.line_height());
    EXPECT_EQ(changed.height(), view_.line_height());
}

TEST_F(SyntaxHighlightingTest, TheLinesThatChangedAreThoseOfTheSpansLastLaidOut) {
    open("let a\nlet b\nlet c");
    highlight({{"let", "keyword.control.fake"}});
    painted();
    PaintCounter counter;
    view_.viewport()->installEventFilter(&counter);

    highlighter_->set_scopes({{"let", "string.fake"}});
    highlighter_->bump();
    view_.refresh_highlighting();
    QApplication::processEvents();
    EXPECT_EQ(counter.painted.boundingRect().height(), 3 * view_.line_height());
}

TEST_F(SyntaxHighlightingTest, OnlyTheLinesInViewAreAskedForSpans) {
    std::string text;
    for (int line = 0; line < 50000; ++line) {
        text += "let x = 1;\n";
    }
    open(text);
    highlight({{"let", "keyword.control.fake"}});
    painted();
    ASSERT_FALSE(highlighter_->asked_.empty());
    const std::size_t visible =
        static_cast<std::size_t>(view_.viewport()->height() / view_.line_height()) + 2;
    EXPECT_LE(*std::ranges::max_element(highlighter_->asked_), visible);
    highlighter_->clear_asked();
    view_.verticalScrollBar()->setValue(view_.line_height() * 30000);
    painted();
    EXPECT_GE(*std::ranges::min_element(highlighter_->asked_), 29999U);
    EXPECT_LE(*std::ranges::max_element(highlighter_->asked_), 30000U + visible);
}

TEST_F(SyntaxHighlightingTest,
       APaintAsksForTheSpansOfEachLineInViewAtMostTwiceWhateverTheDocument) {
    // The cost of highlighting is the spans of the lines in view, so what is counted is the lines
    // that are asked for, not the time, which depends on the machine.
    const auto asked_while_painting = [&](const std::string& text) {
        open(text);
        highlight({{"let", "keyword.control.fake"}});
        highlighter_->clear_asked();
        view_.set_highlighter(highlighter_); // drops the layouts, so the paint asks for the spans
        view_.viewport()->grab();
        std::map<std::size_t, int> times;
        for (const std::size_t line : highlighter_->asked_) {
            ++times[line];
        }
        return times;
    };

    std::string many_lines;
    for (int line = 0; line < 50000; ++line) {
        many_lines += "let x = 1;\n";
    }
    const auto lines = asked_while_painting(many_lines);
    const std::size_t visible =
        static_cast<std::size_t>(view_.viewport()->height() / view_.line_height()) + 2;
    EXPECT_LE(lines.size(), visible);
    for (const auto& [line, times] : lines) {
        EXPECT_LE(times, 2) << "line " << line;
    }

    std::string one_long_line;
    for (int word = 0; word < 12000; ++word) {
        one_long_line += "let x ";
    }
    const auto long_line = asked_while_painting(one_long_line);
    EXPECT_EQ(long_line.size(), 1U);
    EXPECT_LE(long_line.at(0), 2);
}

TEST_F(SyntaxHighlightingTest, SpansThatChangeOnOneLineLayThatLineOutAgain) {
    open("let a\nlet b\nlet c");
    highlight({{"let", "keyword.control.fake"}});
    painted();
    const std::size_t before = view_.layouts_built();

    highlighter_->set_scopes({{"let", "keyword.control.fake"}, {"b", "string.quoted.fake"}});
    highlighter_->bump();
    view_.refresh_highlighting();
    const QImage image = painted();

    EXPECT_EQ(view_.layouts_built() - before, 1U);
    const std::size_t b = document_->text().offset_at({1, 4});
    const QRect cell = view_.cursor_rect(b);
    int green = 0;
    for (int y = cell.y(); y < cell.y() + view_.line_height(); ++y) {
        for (int x = cell.x(); x < cell.x() + view_.char_width(); ++x) {
            green += is_green(image.pixelColor(x, y)) ? 1 : 0;
        }
    }
    EXPECT_GT(green, 3);
}

TEST_F(SyntaxHighlightingTest, LayoutsOutOfViewAreMadeAgainFromTheNewSpans) {
    std::string text;
    for (int line = 0; line < 100; ++line) {
        text += "let a\n";
    }
    open(text);
    highlight({{"let", "keyword.control.fake"}});
    painted();
    view_.verticalScrollBar()->setValue(view_.verticalScrollBar()->maximum());
    painted();

    highlighter_->set_scopes({{"let", "string.quoted.fake"}});
    highlighter_->bump();
    view_.refresh_highlighting();
    view_.verticalScrollBar()->setValue(0);
    const QImage image = painted();
    EXPECT_GT(count_in(image, 0, 3, is_green), 10);
    EXPECT_EQ(count_in(image, 0, 3, is_red), 0);
}

TEST_F(SyntaxHighlightingTest, ANewHighlighterColorsTheCachedLayoutsAgain) {
    open("let name = 'hi';");
    highlight({{"let", "keyword.control.fake"}});
    EXPECT_GT(count_in(painted(), 0, 3, is_red), 10);

    highlight({{"let", "string.quoted.fake"}});
    const QImage image = painted();
    EXPECT_GT(count_in(image, 0, 3, is_green), 10);
    EXPECT_EQ(count_in(image, 0, 3, is_red), 0);
}

TEST_F(SyntaxHighlightingTest, OtherColorsAreShownInsteadOfTheCachedLayouts) {
    open("let name = 'hi';");
    highlight({{"let", "keyword.control.fake"}});
    EXPECT_GT(count_in(painted(), 0, 3, is_red), 10);

    WorkbenchColors other(QApplication::palette());
    other.set_token_theme(token_theme_of(R"j([
        {"settings": {"foreground": "#d4d4d4"}},
        {"scope": "keyword.control", "settings": {"foreground": "#00ff00"}}])j"));
    view_.set_colors(&other);
    EXPECT_GT(count_in(painted(), 0, 3, is_green), 10);
    view_.set_colors(&colors_);
}

TEST_F(SyntaxHighlightingTest, ANewTokenThemeColorsTheCachedLayoutsAgain) {
    open("let name = 'hi';");
    highlight({{"let", "keyword.control.fake"}});
    EXPECT_GT(count_in(painted(), 0, 3, is_red), 10);

    colors_.set_token_theme(token_theme_of(R"j([
        {"settings": {"foreground": "#d4d4d4"}},
        {"scope": "keyword.control", "settings": {"foreground": "#00ff00"}}])j"));
    const QImage image = painted();
    EXPECT_GT(count_in(image, 0, 3, is_green), 10);
    EXPECT_EQ(count_in(image, 0, 3, is_red), 0);
}

namespace {

class MouseSelectionTest : public EditorViewTest {
protected:
    static constexpr const char* text_ = "abcdefgh\nab\nabcdefgh\nabcdefgh";

    void use(core::MultiCursorModifier modifier, bool column_selection = false) {
        core::EditorSettings settings;
        settings.font_family = "monospace";
        settings.font_size = 14;
        settings.multi_cursor_modifier = modifier;
        settings.column_selection = column_selection;
        view_.apply_settings(settings);
    }

    // The center of the cell of `column` on `line`, taking the width from the first line, which is
    // longer than any column used.
    QPoint at(int line, int column) const {
        const QRect cell = view_.cursor_rect(static_cast<std::size_t>(column));
        return {cell.x() + 1, line * view_.line_height() + view_.line_height() / 2 -
                                  view_.verticalScrollBar()->value()};
    }

    void drag(QPoint from, QPoint to, Qt::KeyboardModifiers modifiers) {
        mouse(QEvent::MouseButtonPress, from, modifiers);
        mouse(QEvent::MouseMove, to, modifiers);
        mouse(QEvent::MouseButtonRelease, to, modifiers);
    }

    std::vector<core::Selection> selections() const {
        return document_->selections().all();
    }
};

using Selections = std::vector<core::Selection>;

TEST_F(MouseSelectionTest, ShiftAltDragSelectsARectangleOneSelectionPerLine) {
    open(text_);
    use(core::MultiCursorModifier::alt);
    click(2);
    drag(at(0, 2), at(3, 5), Qt::AltModifier | Qt::ShiftModifier);
    // The line "ab" ends before the rectangle, so its selection is clamped to the end of the line.
    EXPECT_EQ(selections(), (Selections{{2, 5}, {11, 11}, {14, 17}, {23, 26}}));
    EXPECT_EQ(document_->selections().primary_index(), 3u) << "the line of the pointer";
}

TEST_F(MouseSelectionTest, TheRectangleFollowsThePointerUpAndToTheLeft) {
    open(text_);
    use(core::MultiCursorModifier::alt);
    click(18);
    drag(at(2, 6), at(0, 3), Qt::AltModifier | Qt::ShiftModifier);
    EXPECT_EQ(selections(), (Selections{{6, 3}, {11, 11}, {18, 15}}));
    EXPECT_EQ(document_->selections().primary_index(), 0u);
}

TEST_F(MouseSelectionTest, ShiftAltClickGrowsTheRectangleFromTheCaret) {
    open(text_);
    use(core::MultiCursorModifier::alt);
    click(2);
    mouse(QEvent::MouseButtonPress, at(2, 4), Qt::AltModifier | Qt::ShiftModifier);
    mouse(QEvent::MouseButtonRelease, at(2, 4), Qt::AltModifier | Qt::ShiftModifier);
    EXPECT_EQ(selections(), (Selections{{2, 4}, {11, 11}, {14, 16}}));
}

TEST_F(MouseSelectionTest, AltClickAddsACursorAndDraggingExtendsIt) {
    open(text_);
    use(core::MultiCursorModifier::alt);
    click(1);
    drag(at(2, 1), at(2, 4), Qt::AltModifier);
    EXPECT_EQ(selections(), (Selections{{1, 1}, {13, 16}}));
}

TEST_F(MouseSelectionTest, WithCtrlCmdAsTheModifierCtrlClickAddsACursorAndAltClickDoesNot) {
    open(text_);
    use(core::MultiCursorModifier::ctrl_cmd);
    click(1);
    click(14, Qt::ControlModifier);
    EXPECT_EQ(selections(), (Selections{{1, 1}, {14, 14}}));
    click(5, Qt::AltModifier);
    EXPECT_EQ(selections(), (Selections{{5, 5}})) << "a plain click";
}

TEST_F(MouseSelectionTest, WithCtrlCmdAsTheModifierAltDragSelectsARectangle) {
    open(text_);
    use(core::MultiCursorModifier::ctrl_cmd);
    drag(at(0, 2), at(2, 4), Qt::AltModifier);
    EXPECT_EQ(selections(), (Selections{{2, 4}, {11, 11}, {14, 16}}));
}

TEST_F(MouseSelectionTest, WithCtrlCmdAsTheModifierShiftAltDragSelectsARectangleToo) {
    open(text_);
    use(core::MultiCursorModifier::ctrl_cmd);
    click(2);
    drag(at(0, 2), at(2, 4), Qt::AltModifier | Qt::ShiftModifier);
    EXPECT_EQ(selections(), (Selections{{2, 4}, {11, 11}, {14, 16}}));
}

TEST_F(MouseSelectionTest, TheModifierAndAnotherModifierTogetherDoNothing) {
    open(text_);
    use(core::MultiCursorModifier::alt);
    click(3);
    click(14, Qt::AltModifier | Qt::ControlModifier);
    EXPECT_EQ(selections(), (Selections{{3, 3}}));
}

TEST_F(MouseSelectionTest, InColumnSelectionModeAPlainDragSelectsARectangle) {
    open(text_);
    use(core::MultiCursorModifier::alt, true);
    drag(at(0, 1), at(3, 4), Qt::NoModifier);
    EXPECT_EQ(selections(), (Selections{{1, 4}, {10, 11}, {13, 16}, {22, 25}}));
}

TEST_F(MouseSelectionTest, InColumnSelectionModeAClickPlacesACaretAndShiftClickGrowsTheRectangle) {
    open(text_);
    use(core::MultiCursorModifier::alt, true);
    click(2);
    EXPECT_EQ(selections(), (Selections{{2, 2}}));
    mouse(QEvent::MouseButtonPress, at(2, 5), Qt::ShiftModifier);
    mouse(QEvent::MouseButtonRelease, at(2, 5), Qt::ShiftModifier);
    EXPECT_EQ(selections(), (Selections{{2, 5}, {11, 11}, {14, 17}}));
}

TEST_F(MouseSelectionTest, WithoutColumnSelectionAPlainDragSelectsAStreamOfText) {
    open(text_);
    use(core::MultiCursorModifier::alt, false);
    drag(at(0, 2), at(2, 4), Qt::NoModifier);
    EXPECT_EQ(selections(), (Selections{{2, 16}}));
}

} // namespace

namespace {

class LayoutCacheTest : public EditorViewTest {
protected:
    void SetUp() override {
        flash_time_ = QApplication::cursorFlashTime();
        QApplication::setCursorFlashTime(0);
        settings_.font_family = "monospace";
        settings_.font_size = 14;
        view_.apply_settings(settings_);
    }
    void TearDown() override {
        QApplication::setCursorFlashTime(flash_time_);
    }

    static std::string numbered(int count) {
        std::string text;
        for (int line = 0; line < count; ++line) {
            text += std::to_string(line) + "\tend of the line\n";
        }
        return text;
    }

    QImage paint() {
        return view_.viewport()->grab().toImage();
    }
    // The view painted with nothing cached, which applying the settings again causes.
    QImage painted_afresh() {
        view_.apply_settings(settings_);
        return paint();
    }
    std::size_t built_by(const std::function<void()>& action) {
        const std::size_t before = view_.layouts_built();
        action();
        return view_.layouts_built() - before;
    }
    std::size_t offset_of_line(std::size_t line, std::size_t column = 0) {
        return document_->text().offset_at({line, column});
    }
    void caret_at(std::size_t offset) {
        set_selections({core::Selection::caret(offset)});
    }
    std::size_t lines_in_view() {
        return static_cast<std::size_t>(view_.viewport()->height() / view_.line_height()) + 1;
    }

    int flash_time_ = 0;
    core::EditorSettings settings_;
};

} // namespace

TEST_F(LayoutCacheTest, TheFirstPaintLaysOutTheLinesInViewAndAnotherPaintNoMore) {
    open(numbered(1000));
    EXPECT_GE(view_.layouts_built(), lines_in_view() - 1);
    EXPECT_LE(view_.layouts_built(), lines_in_view() + 1);
    EXPECT_EQ(built_by([&] { paint(); }), 0U);
}

TEST_F(LayoutCacheTest, TheCaretAndTheMouseUseTheLayoutsOfThePaintedLines) {
    open(numbered(1000));
    paint();
    EXPECT_EQ(built_by([&] {
                  view_.cursor_rect(offset_of_line(3, 5));
                  click(offset_of_line(5, 2));
                  view_.offset_at(QPoint(60, 3 * view_.line_height()));
              }),
              0U);
}

TEST_F(LayoutCacheTest, ScrollingByOneLineLaysOutOneLine) {
    open(numbered(1000));
    paint();
    EXPECT_EQ(built_by([&] {
                  view_.verticalScrollBar()->setValue(view_.line_height());
                  paint();
              }),
              1U);
}

TEST_F(LayoutCacheTest, TypingLaysOutTheLineTypedOnAndNoOther) {
    open(numbered(100));
    caret_at(offset_of_line(10, 4));
    paint();
    EXPECT_EQ(built_by([&] {
                  type("x");
                  paint();
              }),
              1U);
}

TEST_F(LayoutCacheTest, ALineBreakAndAJoinMoveTheLayoutsBelowTheEdit) {
    open(numbered(100));
    caret_at(offset_of_line(10, 4));
    paint();

    EXPECT_EQ(built_by([&] {
                  key(Qt::Key_Return);
                  paint();
              }),
              2U);
    EXPECT_EQ(paint(), painted_afresh());

    // The joined line, and the line that comes into view at the bottom.
    EXPECT_EQ(built_by([&] {
                  key(Qt::Key_Backspace);
                  paint();
              }),
              2U);
    EXPECT_EQ(paint(), painted_afresh());
}

TEST_F(LayoutCacheTest, CachedLayoutsPaintWhatNewOnesWould) {
    open(numbered(100));
    paint();
    const auto expect_same = [&](const char* what) {
        EXPECT_EQ(paint(), painted_afresh()) << what;
    };

    set_selections({core::Selection::caret(offset_of_line(2, 3)),
                    core::Selection::caret(offset_of_line(5, 3)),
                    core::Selection::caret(offset_of_line(9, 3))});
    paint();
    type("ab");
    expect_same("typing with several carets");

    view_.replace_ranges({{offset_of_line(4, 2), offset_of_line(4, 2), "one\ntwo\nthree"}});
    expect_same("a replacement that adds lines");

    view_.replace_ranges({{offset_of_line(1, 2), offset_of_line(6, 2), ""}});
    expect_same("a replacement that joins lines");

    view_.replace_ranges({{offset_of_line(8, 1), offset_of_line(9, 1), "x"},
                          {offset_of_line(2, 0), offset_of_line(2, 0), "a\nb\n"}});
    expect_same("two replacements");

    view_.undo();
    expect_same("undo");
    view_.undo();
    expect_same("undo again");
    view_.redo();
    expect_same("redo");
}

TEST_F(LayoutCacheTest, AnEditThroughAnotherViewOfTheDocumentIsShown) {
    open(numbered(100));
    paint();
    EditorView other;
    other.resize(640, 320);
    other.set_document(document_);
    other.replace_ranges({{offset_of_line(3, 0), offset_of_line(3, 0), "inserted\n"}});

    EXPECT_EQ(paint(), painted_afresh());
}

TEST_F(LayoutCacheTest, TheLayoutsAreMadeAgainWhenTheTabSizeOrTheFontChanges) {
    open(numbered(100));
    settings_.font_size = 12;
    view_.apply_settings(settings_);
    const QImage narrow = paint();

    settings_.tab_size = 8;
    view_.apply_settings(settings_);
    const QImage wide = paint();
    EXPECT_NE(wide, narrow);

    settings_.font_size = 16;
    view_.apply_settings(settings_);
    EXPECT_NE(paint(), wide);
    EXPECT_GE(built_by([&] {
                  view_.setFont(QFont("monospace", 9));
                  paint();
              }),
              lines_in_view() - 1);
}

TEST_F(LayoutCacheTest, TheControlCharacterSettingLaysTheLinesOutAgain) {
    open("a\x01"
         "b\nsecond");
    core::EditorSettings settings = settings_;
    settings.render_control_characters = true;
    view_.apply_settings(settings);
    const QImage shown = paint();
    settings.render_control_characters = false;
    view_.apply_settings(settings);
    EXPECT_NE(paint(), shown);
}

TEST_F(LayoutCacheTest, OnlyTheMostRecentlyUsedLayoutsAreKept) {
    open(numbered(3000));
    paint();
    for (std::size_t line = 0; line < 3000; ++line) {
        view_.cursor_rect(offset_of_line(line));
    }
    EXPECT_EQ(built_by([&] { view_.cursor_rect(offset_of_line(2999)); }), 0U);
    EXPECT_EQ(built_by([&] { view_.cursor_rect(offset_of_line(0)); }), 1U);
}

TEST_F(LayoutCacheTest, ALayoutThatIsUsedAgainOutlivesOnesThatAreNot) {
    open(numbered(3000));
    const std::size_t before = view_.layouts_built();
    for (std::size_t line = 0; line < 3000; ++line) {
        view_.cursor_rect(offset_of_line(line));
        view_.cursor_rect(offset_of_line(0));
    }
    // Each line is laid out once, the first of them included.
    EXPECT_EQ(view_.layouts_built() - before, 3000U - before);
}

TEST_F(LayoutCacheTest, AViewTallerThanTheCacheKeepsEveryLineInView) {
    open(numbered(1000));
    view_.resize(640, 400 * view_.line_height());
    paint();
    EXPECT_GT(lines_in_view(), 300U);
    EXPECT_EQ(built_by([&] { paint(); }), 0U);
}

TEST_F(LayoutCacheTest, AnotherDocumentIsShownEvenWhenItsVersionIsTheSame) {
    open(numbered(20));
    paint();
    document_ = make_document("something\nelse\nentirely");
    view_.set_document(document_);
    EXPECT_EQ(paint(), painted_afresh());
}

TEST_F(LayoutCacheTest, AnEditAfterAnEditThroughAnotherViewIsShown) {
    open(numbered(100));
    paint();
    EditorView other;
    other.resize(640, 320);
    other.set_document(document_);
    other.replace_ranges({{offset_of_line(3, 0), offset_of_line(3, 0), "inserted\n"}});
    caret_at(offset_of_line(10, 2));
    type("z");
    EXPECT_EQ(paint(), painted_afresh());
}

namespace {

class LongLineTest : public LayoutCacheTest {
protected:
    // Words, tabs and wide characters, well past the length that one layout would be made for.
    static std::string mixed_line(std::size_t words) {
        std::string line;
        for (std::size_t word = 0; word < words; ++word) {
            line += word % 7 == 3 ? "\xe4\xb8\xad\xe6\x96\x87\t" : "word ";
        }
        return line;
    }
    static std::string wide_line(std::size_t characters) {
        std::string line;
        for (std::size_t character = 0; character < characters; ++character) {
            line += "\xe4\xb8\xad";
        }
        return line;
    }

    void caret_to(std::size_t offset) {
        view_.select_range(core::Selection::caret(offset));
    }
    bool shows(std::size_t offset) {
        const QRect rect = view_.cursor_rect(offset);
        return rect.left() >= 0 && rect.right() <= view_.viewport()->width();
    }
    // The pixels that differ between two images in the cells of the columns [from, to) of the first
    // line.
    int changed_in(const QImage& before, const QImage& after, std::size_t from, std::size_t to) {
        const int left = std::max(view_.cursor_rect(from).left(), 0);
        const int right = std::min(view_.cursor_rect(to).left(), after.width());
        int changed = 0;
        for (int y = 0; y < view_.line_height(); ++y) {
            for (int x = left; x < right; ++x) {
                changed += before.pixel(x, y) != after.pixel(x, y) ? 1 : 0;
            }
        }
        return changed;
    }
    // Paints the line from its start to `offset`, so that every part up to there is measured.
    void paint_up_to(std::size_t offset) {
        for (std::size_t column = 0; column < offset; column += 20) {
            caret_to(column);
            paint();
        }
        caret_to(offset);
        paint();
    }
};

} // namespace

TEST_F(LongLineTest, ALineOfAMillionCharactersScrollsToItsEnd) {
    constexpr std::size_t length = 1'000'000;
    open(std::string(length, 'a'));
    key(Qt::Key_End);
    EXPECT_EQ(primary(), core::Selection::caret(length));
    EXPECT_TRUE(shows(length));
    paint();
    const double expected = static_cast<double>(length) *
                            QFontMetricsF(view_.font()).horizontalAdvance(QLatin1Char('a'));
    EXPECT_NEAR(view_.horizontalScrollBar()->maximum(), expected, 0.001 * expected);

    key(Qt::Key_Home);
    EXPECT_EQ(primary(), core::Selection::caret(0));
    EXPECT_EQ(view_.horizontalScrollBar()->value(), 0);
    key(Qt::Key_End, Qt::ShiftModifier);
    EXPECT_EQ(primary(), (core::Selection{0, length}));
    EXPECT_TRUE(shows(length));
}

TEST_F(LongLineTest, TypingMovingAndScrollingInALongLineLayOutAFewParts) {
    open(std::string(1'000'000, 'a'));
    caret_to(500'000);
    paint();

    EXPECT_LE(built_by([&] {
                  type("x");
                  paint();
              }),
              2U);
    EXPECT_EQ(built_by([&] {
                  key(Qt::Key_Right);
                  paint();
              }),
              0U);
    EXPECT_LE(built_by([&] {
                  view_.horizontalScrollBar()->setValue(view_.horizontalScrollBar()->value() + 300);
                  paint();
              }),
              1U);
    EXPECT_LE(built_by([&] {
                  view_.horizontalScrollBar()->setValue(view_.horizontalScrollBar()->maximum() / 4);
                  paint();
              }),
              2U);
}

TEST_F(LongLineTest, ClickingFarAlongALinePutsTheCaretWhereClicked) {
    open(mixed_line(4000));
    for (const std::size_t column : {2047U, 2048U, 2049U, 4096U, 6000U, 12000U, 19000U}) {
        // A column inside a surrogate pair or a character is not a place for a caret.
        const std::size_t offset = offset_of_line(0, column);
        caret_to(offset);
        paint();
        ASSERT_TRUE(shows(offset)) << column;
        caret_to(offset > 40 ? offset_of_line(0, column - 5) : 0);
        paint();
        const QRect rect = view_.cursor_rect(offset);
        mouse(QEvent::MouseButtonPress, QPoint(rect.left() + 1, rect.center().y()));
        mouse(QEvent::MouseButtonRelease, QPoint(rect.left() + 1, rect.center().y()));
        EXPECT_EQ(primary(), core::Selection::caret(offset)) << column;
    }
}

TEST_F(LongLineTest, AJumpToTheEndOfALongLineThatWasNeverPaintedShowsTheCaret) {
    std::string text;
    for (int line = 0; line < 100; ++line) {
        text += "short\n";
    }
    text += std::string(50'000, 'a');
    open(text);
    paint();
    const std::size_t end = document_->text().size();
    caret_to(end);
    EXPECT_TRUE(shows(end));
}

TEST_F(LongLineTest, AWideCharacterLineKeepsTheCaretInViewAtItsEnd) {
    open(wide_line(20'000));
    key(Qt::Key_End);
    paint();
    EXPECT_TRUE(shows(document_->text().size()));
}

TEST_F(LongLineTest, TypingInAWideCharacterLineDoesNotMoveTheTextBeforeTheCaret) {
    open(wide_line(6'000));
    const std::size_t offset = offset_of_line(0, 5'000);
    paint_up_to(offset);
    const QRect before = view_.cursor_rect(offset);
    const int scrolled = view_.horizontalScrollBar()->value();

    type("a");
    paint();
    // The view can scroll to keep the caret in view; the text stays where it was in the line.
    EXPECT_EQ(view_.cursor_rect(offset).left() + view_.horizontalScrollBar()->value(),
              before.left() + scrolled);
}

TEST_F(LongLineTest, SelectionsHighlightsAndMarksPaintFarAlongALine) {
    std::string line(10'000, 'a');
    line[6'000] = ' ';
    line[6'010] = '\x01';
    open(line);
    settings_.render_whitespace = core::RenderWhitespace::all;
    view_.apply_settings(settings_);

    // A selection across the end of a part.
    caret_to(4'096);
    const QImage plain = paint();
    set_selections({core::Selection{4'090, 4'102}});
    const QImage selected = paint();
    EXPECT_GT(changed_in(plain, selected, 4'090, 4'096), 50);
    EXPECT_GT(changed_in(plain, selected, 4'096, 4'102), 50);

    caret_to(8'192);
    const QImage unmarked = paint();
    view_.set_highlights({{8'186, 8'198}});
    const QImage marked = paint();
    EXPECT_GT(changed_in(unmarked, marked, 8'186, 8'192), 50);
    EXPECT_GT(changed_in(unmarked, marked, 8'192, 8'198), 50);
    view_.set_highlights({});

    // Scrolling back to the caret puts it at the left edge, with the marks to its right.
    caret_to(5'990);
    const QImage with_marks = paint();
    settings_.render_whitespace = core::RenderWhitespace::none;
    settings_.render_control_characters = false;
    view_.apply_settings(settings_);
    const QImage without = paint();
    EXPECT_GT(changed_in(without, with_marks, 6'000, 6'001), 0) << "the mark of the space";
    EXPECT_GT(changed_in(without, with_marks, 6'010, 6'011), 20) << "the control character";
}

TEST_F(SyntaxHighlightingTest, ATokenAcrossTheEndOfAPartOfALongLineIsColoredOnBothSides) {
    open(std::string(4'094, 'a') + "let" + std::string(3'000, 'a'));
    highlight({{"let", "keyword.control.fake"}});
    view_.select_range(core::Selection::caret(4'096));
    const QImage image = painted();
    EXPECT_GT(count_in(image, 4'094, 4'096, is_red), 5);
    EXPECT_GT(count_in(image, 4'096, 4'097, is_red), 3);
    EXPECT_EQ(count_in(image, 4'080, 4'094, is_red), 0);
}

TEST_F(SyntaxHighlightingTest, NewSpansKeepTheTextOfALongWideLineInPlace) {
    std::string line = "let ";
    for (int character = 0; character < 6'000; ++character) {
        line += "\xe4\xb8\xad";
    }
    open(line);
    highlight({{"let", "keyword.control.fake"}});
    for (std::size_t column = 0; column < 5'000; column += 20) {
        view_.select_range(core::Selection::caret(document_->text().offset_at({0, column})));
        painted();
    }
    const std::size_t offset = document_->text().offset_at({0, 4'990});
    const QRect before = view_.cursor_rect(offset);

    highlighter_->set_scopes({{"let", "string.quoted.fake"}});
    highlighter_->bump();
    view_.refresh_highlighting();
    painted();
    EXPECT_EQ(view_.cursor_rect(offset), before);
}
