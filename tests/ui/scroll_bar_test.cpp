#include "anvil/ui/scroll_bar.hpp"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QWheelEvent>
#include <algorithm>
#include <gtest/gtest.h>
#include <random>

using anvil::ui::ScrollBar;
using anvil::ui::WorkbenchColors;

namespace {

const QColor surface("#101010");
const QColor rest("#ff0000");
const QColor hover("#00ff00");
const QColor active("#0000ff");

WorkbenchColors::Colors theme() {
    return {{"editor.background", surface},
            {"scrollbarSlider.background", rest},
            {"scrollbarSlider.hoverBackground", hover},
            {"scrollbarSlider.activeBackground", active}};
}

// A vertical bar 300 pixels long whose slider is 60 pixels long: a quarter of the content shows.
class ScrollBarTest : public ::testing::Test {
protected:
    ScrollBarTest() {
        colors_.set_colors(theme());
        bar_.set_colors(&colors_);
        bar_.resize(14, length);
        bar_.setRange(0, 900);
        bar_.setPageStep(300);
        bar_.show();
        QApplication::processEvents();
        // The pointer of the offscreen platform can be over the bar.
        leave();
    }

    void mouse(QEvent::Type type, int y, Qt::MouseButton button = Qt::LeftButton) {
        const QPointF at(7, y);
        const Qt::MouseButtons held = type == QEvent::MouseButtonRelease       ? Qt::NoButton
                                      : type == QEvent::MouseMove && !pressed_ ? Qt::NoButton
                                                                               : button;
        if (type == QEvent::MouseButtonPress) {
            pressed_ = true;
        } else if (type == QEvent::MouseButtonRelease) {
            pressed_ = false;
        }
        QMouseEvent event(type, at, bar_.mapToGlobal(at),
                          type == QEvent::MouseMove ? Qt::NoButton : button, held, Qt::NoModifier);
        QApplication::sendEvent(&bar_, &event);
    }
    void press(int y) {
        mouse(QEvent::MouseButtonPress, y);
    }
    void move(int y) {
        mouse(QEvent::MouseMove, y);
    }
    void release(int y) {
        mouse(QEvent::MouseButtonRelease, y);
    }
    void leave() {
        QEvent event(QEvent::Leave);
        QApplication::sendEvent(&bar_, &event);
    }

    QColor pixel(int y) {
        return bar_.grab().toImage().pixelColor(7, y);
    }
    QColor slider_color() {
        return pixel(bar_.slider_rect().center().y());
    }

    static constexpr int length = 300;
    WorkbenchColors colors_{QApplication::palette()};
    ScrollBar bar_{Qt::Vertical, ScrollBar::editor_vertical_width, "editor.background"};
    bool pressed_ = false;
};

TEST_F(ScrollBarTest, TheSliderCoversThePartOfTheContentThatShows) {
    EXPECT_EQ(bar_.slider_rect(), QRect(0, 0, 14, 75));
    bar_.setValue(900);
    EXPECT_EQ(bar_.slider_rect(), QRect(0, 225, 14, 75));
    bar_.setValue(450);
    EXPECT_EQ(bar_.slider_rect(), QRect(0, 113, 14, 75));
}

TEST_F(ScrollBarTest, TheSliderAgreesWithAPlainReference) {
    std::mt19937 random(241);
    std::uniform_int_distribution<int> bar_length(5, 700);
    std::uniform_int_distribution<int> page(1, 900);
    std::uniform_int_distribution<int> range(1, 100000);
    std::uniform_int_distribution<int> minimum(-50, 50);
    for (int run = 0; run < 3000; ++run) {
        const long long L = bar_length(random);
        const long long shown = page(random);
        const long long span = range(random);
        const int low = minimum(random);
        const long long at = std::uniform_int_distribution<long long>(0, span)(random);
        bar_.resize(14, static_cast<int>(L));
        bar_.setRange(low, low + static_cast<int>(span));
        bar_.setPageStep(static_cast<int>(shown));
        bar_.setValue(low + static_cast<int>(at));

        const long long size = std::min(L, std::max<long long>(20, L * shown / (span + shown)));
        // Rounded half up, in whole numbers.
        const long long start = (2 * at * (L - size) + span) / (2 * span);
        ASSERT_EQ(bar_.slider_rect(), QRect(0, static_cast<int>(start), 14, static_cast<int>(size)))
            << "length " << L << " page " << shown << " range " << span << " at " << at;
    }
}

TEST_F(ScrollBarTest, TheSliderIsNeverTooShortToTakeHoldOfAtAnyZoom) {
    bar_.setRange(0, 1000000);
    EXPECT_EQ(bar_.slider_rect().height(), 20);
    bar_.set_zoom_level(2);
    EXPECT_EQ(bar_.slider_rect().height(), 29);
    EXPECT_EQ(bar_.width(), 20);
    EXPECT_EQ(bar_.slider_rect().width(), 20);
}

TEST_F(ScrollBarTest, AHorizontalBarLiesTheOtherWay) {
    ScrollBar bar(Qt::Horizontal, ScrollBar::editor_horizontal_height, "editor.background");
    bar.resize(300, 12);
    bar.setRange(0, 900);
    bar.setPageStep(300);
    bar.setValue(900);
    EXPECT_EQ(bar.height(), 12);
    EXPECT_EQ(bar.slider_rect(), QRect(225, 0, 75, 12));

    bar.show();
    const QPointF at(37, 6);
    QMouseEvent press(QEvent::MouseButtonPress, at, bar.mapToGlobal(at), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &press);
    EXPECT_EQ(bar.value(), 0);
}

TEST_F(ScrollBarTest, WithNothingToScrollThereIsNoSlider) {
    bar_.setRange(0, 0);
    EXPECT_TRUE(bar_.slider_rect().isEmpty());
    EXPECT_EQ(pixel(10), surface);
    press(100);
    EXPECT_FALSE(bar_.isSliderDown());
}

TEST_F(ScrollBarTest, TheSliderIsColoredByWhatThePointerDoes) {
    EXPECT_EQ(slider_color(), rest);
    EXPECT_EQ(pixel(200), surface);
    move(200);
    EXPECT_EQ(slider_color(), rest) << "the pointer is on the track";
    move(30);
    EXPECT_EQ(slider_color(), hover);
    press(30);
    EXPECT_EQ(slider_color(), active);
    move(250);
    EXPECT_EQ(slider_color(), active);
    release(250);
    EXPECT_EQ(slider_color(), hover);
    leave();
    EXPECT_EQ(slider_color(), rest);
}

TEST_F(ScrollBarTest, ASliderThatScrollsUnderAStillPointerTurnsToTheHoverColor) {
    move(200);
    EXPECT_EQ(slider_color(), rest);
    bar_.setValue(600);
    EXPECT_EQ(slider_color(), hover);
}

TEST_F(ScrollBarTest, TheTrackShowsTheThemesBackgroundOverTheContentsColor) {
    EXPECT_EQ(pixel(200), surface);
    auto colors = theme();
    colors.emplace("scrollbar.background", QColor("#abcdef"));
    colors_.set_colors(colors);
    EXPECT_EQ(pixel(200), QColor("#abcdef"));
    EXPECT_EQ(slider_color(), rest);
}

TEST_F(ScrollBarTest, AChangeOfThemeRepaintsTheBarAtOnce) {
    class Paints : public QObject {
    public:
        int count = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            count += event->type() == QEvent::Paint ? 1 : 0;
            return false;
        }
    } paints;
    bar_.installEventFilter(&paints);
    auto colors = theme();
    colors["scrollbarSlider.background"] = QColor("#123456");
    colors_.set_colors(colors);
    QApplication::processEvents();
    EXPECT_GT(paints.count, 0);
    EXPECT_EQ(slider_color(), QColor("#123456"));
}

TEST_F(ScrollBarTest, WithoutAThemeTheColorsComeFromThePalette) {
    bar_.set_colors(nullptr);
    EXPECT_EQ(slider_color().rgb(), bar_.palette().color(QPalette::Mid).rgb());
    EXPECT_EQ(pixel(200).rgb(), bar_.palette().color(QPalette::Base).rgb());
}

TEST_F(ScrollBarTest, APressOnTheTrackBringsTheMiddleOfTheSliderToThePointer) {
    QSignalSpy moved(&bar_, &QScrollBar::sliderMoved);
    press(200);
    // The slider starts at 200 - 75 / 2 of the 225 pixels it can move, of 900.
    EXPECT_EQ(bar_.value(), 650);
    EXPECT_NEAR(bar_.slider_rect().center().y(), 200, 1);
    EXPECT_TRUE(bar_.isSliderDown());
    EXPECT_EQ(slider_color(), active);
    EXPECT_GT(moved.count(), 0);
    // The drag goes on from the slider's place, which is whole pixels.
    move(177);
    EXPECT_EQ(bar_.value(), 560);
    release(177);
    EXPECT_FALSE(bar_.isSliderDown());
}

TEST_F(ScrollBarTest, ADragScrollsWithThePointerAlsoPastTheEndsOfTheBar) {
    bar_.setValue(400);
    press(110);
    EXPECT_EQ(bar_.value(), 400) << "a press on the slider does not move it";
    move(155);
    EXPECT_EQ(bar_.value(), 580);
    move(1000);
    EXPECT_EQ(bar_.value(), 900);
    move(-1000);
    EXPECT_EQ(bar_.value(), 0);
    move(110);
    EXPECT_EQ(bar_.value(), 400);
    release(110);
    move(250);
    EXPECT_EQ(bar_.value(), 400) << "the button is up";
}

TEST_F(ScrollBarTest, OnlyTheLeftButtonScrolls) {
    for (const Qt::MouseButton button : {Qt::RightButton, Qt::MiddleButton}) {
        mouse(QEvent::MouseButtonPress, 200, button);
        EXPECT_EQ(bar_.value(), 0);
        EXPECT_FALSE(bar_.isSliderDown());
        mouse(QEvent::MouseButtonRelease, 200, button);
    }
}

TEST_F(ScrollBarTest, HidingTheBarEndsADrag) {
    press(30);
    ASSERT_TRUE(bar_.isSliderDown());
    bar_.hide();
    EXPECT_FALSE(bar_.isSliderDown());
    bar_.show();
    leave();
    EXPECT_EQ(slider_color(), rest);
    move(100);
    EXPECT_EQ(bar_.value(), 0);
}

TEST_F(ScrollBarTest, TheKeyboardAndTheWheelWorkAsForAnyScrollBar) {
    QScrollBar plain(Qt::Vertical);
    plain.setRange(0, 900);
    plain.setPageStep(300);
    const auto both = [&](QEvent& event) {
        QApplication::sendEvent(&plain, &event);
        QApplication::sendEvent(&bar_, &event);
        EXPECT_EQ(bar_.value(), plain.value());
    };
    for (const int key : {Qt::Key_Down, Qt::Key_PageDown, Qt::Key_Up, Qt::Key_End, Qt::Key_Home}) {
        QKeyEvent press_key(QEvent::KeyPress, key, Qt::NoModifier);
        both(press_key);
    }
    QWheelEvent wheel(QPointF(7, 10), QPointF(7, 10), QPoint(), QPoint(0, -120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    both(wheel);
    EXPECT_GT(bar_.value(), 0);
}

} // namespace
