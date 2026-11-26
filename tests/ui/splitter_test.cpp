#include "anvil/ui/splitter.hpp"
#include "anvil/ui/zoom.hpp"

#include <QApplication>
#include <QEnterEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QSplitterHandle>
#include <gtest/gtest.h>
#include <memory>

using namespace anvil::ui;

namespace {

const QColor background("#101010");
const QColor border("#ff0000");
const QColor hover("#00ff00");
const QColor first_part("#0000aa");
const QColor second_part("#aaaa00");

// Two parts of one color each, side by side or one above the other, in a splitter of 400 by 100
// or 100 by 400 pixels.
class SplitterTest : public ::testing::Test {
protected:
    void SetUp() override {
        colors_.set_colors({{"editor.background", background},
                            {"panel.border", border},
                            {"sash.hoverBorder", hover}});
        make(Qt::Horizontal);
    }

    void make(Qt::Orientation orientation) {
        splitter_ = std::make_unique<Splitter>(orientation, colors_, "panel.border");
        for (const QColor& color : {first_part, second_part}) {
            auto* part = new QLabel;
            QPalette palette = part->palette();
            palette.setColor(QPalette::Window, color);
            part->setPalette(palette);
            part->setAutoFillBackground(true);
            splitter_->addWidget(part);
        }
        splitter_->resize(orientation == Qt::Horizontal ? QSize(400, 100) : QSize(100, 400));
        splitter_->show();
        QApplication::processEvents();
    }

    bool horizontal() const {
        return splitter_->orientation() == Qt::Horizontal;
    }
    int extent(const QWidget* widget) const {
        return horizontal() ? widget->width() : widget->height();
    }
    // Where the line is, along the splitter.
    int line() const {
        return extent(splitter_->widget(0));
    }
    QPoint along(int position) const {
        return horizontal() ? QPoint(position, 50) : QPoint(50, position);
    }
    QColor pixel(int position) {
        return splitter_->grab().toImage().pixelColor(along(position));
    }
    QSplitterHandle* handle() const {
        return splitter_->handle(1);
    }

    // Presses on whatever is at `from`, moves by `distance` and releases.
    void drag(int from, int distance) {
        QWidget* target = splitter_->childAt(along(from));
        ASSERT_EQ(target, handle()) << from;
        const QPoint start = splitter_->mapToGlobal(along(from));
        const QPoint end = splitter_->mapToGlobal(along(from + distance));
        const QPointF local = target->mapFromGlobal(start);
        QMouseEvent press(QEvent::MouseButtonPress, local, start, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(target, &press);
        QMouseEvent move(QEvent::MouseMove, target->mapFromGlobal(end), end, Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, target->mapFromGlobal(end), end,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &release);
        QApplication::processEvents();
    }

    void enter() {
        QEnterEvent event(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
        QApplication::sendEvent(handle(), &event);
    }
    void leave() {
        QEvent event(QEvent::Leave);
        QApplication::sendEvent(handle(), &event);
    }

    // The line alone separates the two parts.
    void expect_only_the_line(const QColor& color = border) {
        const int at = line();
        for (int position = at - 6; position < at; ++position) {
            EXPECT_EQ(pixel(position), first_part) << position - at;
        }
        EXPECT_EQ(pixel(at), color);
        for (int position = at + 1; position <= at + 6; ++position) {
            EXPECT_EQ(pixel(position), second_part) << position - at;
        }
    }

    WorkbenchColors colors_{QApplication::palette()};
    std::unique_ptr<Splitter> splitter_;
};

TEST_F(SplitterTest, OnlyALineOfOnePixelSeparatesThePartsWhichCannotCollapse) {
    for (const Qt::Orientation orientation : {Qt::Horizontal, Qt::Vertical}) {
        make(orientation);
        SCOPED_TRACE(horizontal());
        EXPECT_EQ(splitter_->handleWidth(), 1);
        EXPECT_EQ(extent(splitter_->widget(0)) + extent(splitter_->widget(1)), 399);
        EXPECT_FALSE(splitter_->childrenCollapsible());
        expect_only_the_line();
    }
}

TEST_F(SplitterTest, TheGrabAreaLiesOverBothParts) {
    for (const Qt::Orientation orientation : {Qt::Horizontal, Qt::Vertical}) {
        make(orientation);
        SCOPED_TRACE(horizontal());
        EXPECT_EQ(extent(handle()), 5);
        for (int offset = -2; offset <= 2; ++offset) {
            EXPECT_EQ(splitter_->childAt(along(line() + offset)), handle()) << offset;
        }
        EXPECT_EQ(splitter_->childAt(along(line() - 3)), splitter_->widget(0));
        EXPECT_EQ(splitter_->childAt(along(line() + 3)), splitter_->widget(1));
    }
}

TEST_F(SplitterTest, ADragFromEitherSideOfTheLineMovesItByTheDistance) {
    for (const Qt::Orientation orientation : {Qt::Horizontal, Qt::Vertical}) {
        for (const int offset : {-2, 0, 2}) {
            make(orientation);
            SCOPED_TRACE(testing::Message() << horizontal() << " " << offset);
            const int before = line();
            drag(before + offset, 40);
            EXPECT_EQ(line(), before + 40);
            drag(line() + offset, -25);
            EXPECT_EQ(line(), before + 15);
            leave();
            expect_only_the_line();
        }
    }
}

TEST_F(SplitterTest, AHoveredSashShowsItsColorOverTheWholeGrabArea) {
    for (const Qt::Orientation orientation : {Qt::Horizontal, Qt::Vertical}) {
        make(orientation);
        SCOPED_TRACE(horizontal());
        enter();
        for (int offset = -2; offset <= 2; ++offset) {
            EXPECT_EQ(pixel(line() + offset), hover) << offset;
        }
        EXPECT_EQ(pixel(line() - 3), first_part);
        EXPECT_EQ(pixel(line() + 3), second_part);
        leave();
        expect_only_the_line();
    }
}

TEST_F(SplitterTest, APressedSashKeepsTheColorUntilTheRelease) {
    const QPoint at = handle()->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, at, handle()->mapToGlobal(at), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle(), &press);
    EXPECT_EQ(pixel(line() - 2), hover);
    QMouseEvent release(QEvent::MouseButtonRelease, at, handle()->mapToGlobal(at), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(handle(), &release);
    expect_only_the_line();
}

TEST_F(SplitterTest, TheLineFollowsTheColorsOfTheTheme) {
    colors_.set_colors({{"editor.background", background}, {"panel.border", QColor("#0000ff")}});
    expect_only_the_line(QColor("#0000ff"));
}

TEST_F(SplitterTest, ATranslucentLineLiesOnTheEditorBackground) {
    colors_.set_colors({{"editor.background", background}, {"panel.border", QColor(255, 0, 0, 0)}});
    expect_only_the_line(background);
}

TEST_F(SplitterTest, TheZoomWidensTheGrabAreaAndNotTheLine) {
    Zoom zoom;
    splitter_->set_zoom(&zoom);
    for (const double level : {4.0, -4.0, 8.0, 0.0}) {
        zoom.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        const int reach = std::max(2, zoom.scaled(Splitter::sash_width) / 2);
        EXPECT_EQ(splitter_->handleWidth(), 1);
        EXPECT_EQ(handle()->width(), 1 + 2 * reach);
        EXPECT_EQ(splitter_->childAt(along(line() - reach)), handle());
        EXPECT_EQ(splitter_->childAt(along(line() + reach)), handle());
        expect_only_the_line();
    }
    splitter_->set_zoom(nullptr);
}

TEST_F(SplitterTest, TheSashStaysOverAPartThatIsMoved) {
    splitter_->insertWidget(0, splitter_->widget(1));
    QApplication::processEvents();
    EXPECT_EQ(splitter_->childAt(along(line() - 2)), splitter_->handle(1));
    EXPECT_EQ(splitter_->childAt(along(line() + 2)), splitter_->handle(1));
    EXPECT_EQ(pixel(line() - 1), second_part);
    EXPECT_EQ(pixel(line() + 1), first_part);
}

} // namespace
