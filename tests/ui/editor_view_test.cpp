#include "anvil/ui/editor_view.hpp"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <gtest/gtest.h>

using namespace anvil;
using anvil::ui::EditorView;

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
