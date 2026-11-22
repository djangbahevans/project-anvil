#include "anvil/ui/status_bar.hpp"

#include <QApplication>
#include <QHelpEvent>
#include <QImage>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QToolTip>
#include <cmath>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>

using namespace anvil::ui;

namespace {

class PaintCounter : public QObject {
public:
    int count = 0;

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            ++count;
        }
        return false;
    }
};

class StatusBarTest : public ::testing::Test {
protected:
    void SetUp() override {
        bar_ = std::make_unique<StatusBar>(colors_);
        bar_->resize(600, StatusBar::height_pixels);
        bar_->show();
        QApplication::processEvents();
    }

    void add(const std::string& id, StatusBarSide side, const QString& text, int priority = 0,
             const std::string& command = {}) {
        bar_->set_item(id, side,
                       {.text = text, .tooltip = {}, .command = command, .priority = priority});
    }

    QImage grab() {
        QApplication::processEvents();
        return bar_->grab().toImage();
    }

    void hover(const QPoint& position) {
        QMouseEvent move(QEvent::MouseMove, position, bar_->mapToGlobal(position), Qt::NoButton,
                         Qt::NoButton, {});
        QApplication::sendEvent(bar_.get(), &move);
        QApplication::processEvents();
    }

    void leave() {
        QEvent event(QEvent::Leave);
        QApplication::sendEvent(bar_.get(), &event);
    }

    WorkbenchColors colors_{QApplication::palette()};
    std::unique_ptr<StatusBar> bar_;
};

} // namespace

TEST_F(StatusBarTest, IsAsHighAsVsCodesStatusBar) {
    EXPECT_EQ(bar_->height(), 22);
    EXPECT_EQ(StatusBar::height_pixels, 22);
    EXPECT_EQ(bar_->font().pixelSize(), 12);
}

TEST_F(StatusBarTest, ItemsOfAHigherPriorityAreFurtherToTheLeftOnBothSides) {
    add("low", StatusBarSide::left, "low", 1);
    add("high", StatusBarSide::left, "high", 50);
    add("middle", StatusBarSide::left, "middle", 10);
    add("r-low", StatusBarSide::right, "r-low", 1);
    add("r-high", StatusBarSide::right, "r-high", 50);

    EXPECT_EQ(bar_->item_ids(StatusBarSide::left),
              (std::vector<std::string>{"high", "middle", "low"}));
    EXPECT_EQ(bar_->item_ids(StatusBarSide::right), (std::vector<std::string>{"r-high", "r-low"}));
    EXPECT_LT(bar_->item_rect("high").left(), bar_->item_rect("middle").left());
    EXPECT_LT(bar_->item_rect("middle").left(), bar_->item_rect("low").left());
    EXPECT_LT(bar_->item_rect("r-high").left(), bar_->item_rect("r-low").left());
}

TEST_F(StatusBarTest, ItemsOfTheSamePriorityKeepTheOrderTheyWereAddedIn) {
    add("a", StatusBarSide::left, "a");
    add("b", StatusBarSide::left, "b");
    add("c", StatusBarSide::left, "c");
    EXPECT_EQ(bar_->item_ids(StatusBarSide::left), (std::vector<std::string>{"a", "b", "c"}));
    add("b", StatusBarSide::left, "b changed");
    EXPECT_EQ(bar_->item_ids(StatusBarSide::left), (std::vector<std::string>{"a", "b", "c"}));
}

TEST_F(StatusBarTest, ChangingThePriorityReordersTheGroup) {
    add("a", StatusBarSide::left, "a", 1);
    add("b", StatusBarSide::left, "b", 2);
    EXPECT_EQ(bar_->item_ids(StatusBarSide::left), (std::vector<std::string>{"b", "a"}));
    add("a", StatusBarSide::left, "a", 3);
    EXPECT_EQ(bar_->item_ids(StatusBarSide::left), (std::vector<std::string>{"a", "b"}));
    EXPECT_LT(bar_->item_rect("a").left(), bar_->item_rect("b").left());
}

TEST_F(StatusBarTest, TheGroupsSitAtTheEdgesWithAMarginAndAGapBetweenItems) {
    add("l1", StatusBarSide::left, "one", 2);
    add("l2", StatusBarSide::left, "two", 1);
    add("r1", StatusBarSide::right, "three", 2);
    add("r2", StatusBarSide::right, "four", 1);

    constexpr int margin = StatusBar::item_margin;
    EXPECT_EQ(bar_->item_rect("l1").left(), margin);
    EXPECT_EQ(bar_->item_rect("l2").left() - bar_->item_rect("l1").right() - 1, 2 * margin);
    EXPECT_EQ(bar_->item_rect("r2").right() + 1, bar_->width() - margin);
    EXPECT_EQ(bar_->item_rect("r2").left() - bar_->item_rect("r1").right() - 1, 2 * margin);
    for (const char* id : {"l1", "l2", "r1", "r2"}) {
        EXPECT_EQ(bar_->item_rect(id).top(), 0) << id;
        EXPECT_EQ(bar_->item_rect(id).height(), 22) << id;
    }
}

TEST_F(StatusBarTest, AnItemIsAsWideAsItsTextPlusPaddingOnBothSides) {
    add("item", StatusBarSide::left, "Ln 12, Col 3");
    const int text = QFontMetrics(bar_->font()).horizontalAdvance("Ln 12, Col 3");
    EXPECT_EQ(bar_->item_rect("item").width(), text + 2 * StatusBar::item_padding);
}

TEST_F(StatusBarTest, ItemsAreAddedUpdatedAndRemovedById) {
    EXPECT_FALSE(bar_->has_item("a"));
    EXPECT_EQ(bar_->item("a"), nullptr);
    add("a", StatusBarSide::left, "first");
    ASSERT_TRUE(bar_->has_item("a"));
    EXPECT_EQ(bar_->item("a")->text, "first");

    add("a", StatusBarSide::left, "second");
    EXPECT_EQ(bar_->item("a")->text, "second");
    EXPECT_EQ(bar_->item_ids(StatusBarSide::left).size(), 1U);

    EXPECT_TRUE(bar_->remove_item("a"));
    EXPECT_FALSE(bar_->has_item("a"));
    EXPECT_FALSE(bar_->remove_item("a"));
    EXPECT_TRUE(bar_->item_ids(StatusBarSide::left).empty());
    EXPECT_TRUE(bar_->item_rect("a").isEmpty());
}

TEST_F(StatusBarTest, AnItemMovesToTheOtherSideWhenSetThere) {
    add("a", StatusBarSide::left, "a");
    add("a", StatusBarSide::right, "a");
    EXPECT_TRUE(bar_->item_ids(StatusBarSide::left).empty());
    EXPECT_EQ(bar_->item_ids(StatusBarSide::right), (std::vector<std::string>{"a"}));
    EXPECT_GT(bar_->item_rect("a").left(), bar_->width() / 2);
}

TEST_F(StatusBarTest, ALongerTextPushesItsNeighborsAway) {
    add("first", StatusBarSide::left, "x", 2);
    add("second", StatusBarSide::left, "y", 1);
    add("near", StatusBarSide::right, "z", 1);
    add("far", StatusBarSide::right, "w", 2);
    const int second_before = bar_->item_rect("second").left();
    const int far_before = bar_->item_rect("far").left();

    add("first", StatusBarSide::left, "a much longer text", 2);
    add("near", StatusBarSide::right, "a much longer text", 1);
    EXPECT_GT(bar_->item_rect("second").left(), second_before);
    EXPECT_LT(bar_->item_rect("far").left(), far_before);
}

TEST_F(StatusBarTest, SettingAnItemToWhatItAlreadyIsDoesNotRepaint) {
    PaintCounter counter;
    bar_->installEventFilter(&counter);
    add("position", StatusBarSide::right, "Ln 1, Col 1", 100);
    QApplication::processEvents();
    const int painted = counter.count;
    ASSERT_GT(painted, 0);

    for (int i = 0; i < 50; ++i) {
        add("position", StatusBarSide::right, "Ln 1, Col 1", 100);
    }
    QApplication::processEvents();
    EXPECT_EQ(counter.count, painted);

    add("position", StatusBarSide::right, "Ln 1, Col 2", 100);
    QApplication::processEvents();
    EXPECT_EQ(counter.count, painted + 1);
}

TEST_F(StatusBarTest, ChangingOnlyAToolTipOrAColorRepaintsOnceAndKeepsTheGeometry) {
    add("a", StatusBarSide::left, "text", 1);
    add("b", StatusBarSide::left, "next", 0);
    const QRect before = bar_->item_rect("b");
    PaintCounter counter;
    bar_->installEventFilter(&counter);
    QApplication::processEvents();
    const int painted = counter.count;

    bar_->set_item("a", StatusBarSide::left,
                   {.text = "text",
                    .tooltip = "a tool tip",
                    .foreground = {.key = {}, .color = QColor("#ff0000")},
                    .priority = 1});
    QApplication::processEvents();
    EXPECT_EQ(counter.count, painted + 1);
    EXPECT_EQ(bar_->item_rect("b"), before);
}

TEST_F(StatusBarTest, RemovingAnItemRepaintsAndClosesTheGap) {
    add("a", StatusBarSide::left, "first", 2);
    add("b", StatusBarSide::left, "second", 1);
    const int before = bar_->item_rect("b").left();
    bar_->remove_item("a");
    EXPECT_LT(bar_->item_rect("b").left(), before);
    EXPECT_EQ(bar_->item_rect("b").left(), StatusBar::item_margin);
}

TEST_F(StatusBarTest, AResizeKeepsTheRightGroupAtTheRightEdge) {
    add("r", StatusBarSide::right, "right");
    bar_->resize(900, StatusBar::height_pixels);
    EXPECT_EQ(bar_->item_rect("r").right() + 1, 900 - StatusBar::item_margin);
}

TEST_F(StatusBarTest, AClickOnAnItemWithACommandEmitsTheCommand) {
    add("item", StatusBarSide::left, "clickable", 0, "workbench.action.showCommands");
    QSignalSpy spy(bar_.get(), &StatusBar::command_requested);
    QTest::mouseClick(bar_.get(), Qt::LeftButton, {}, bar_->item_rect("item").center());
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "workbench.action.showCommands");
}

TEST_F(StatusBarTest, AClickOnAnItemWithoutACommandOrOnNothingEmitsNothing) {
    add("plain", StatusBarSide::left, "plain");
    QSignalSpy spy(bar_.get(), &StatusBar::command_requested);
    QTest::mouseClick(bar_.get(), Qt::LeftButton, {}, bar_->item_rect("plain").center());
    QTest::mouseClick(bar_.get(), Qt::LeftButton, {}, QPoint(400, 10));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(StatusBarTest, OnlyTheLeftButtonClicks) {
    add("item", StatusBarSide::left, "clickable", 0, "a.command");
    QSignalSpy spy(bar_.get(), &StatusBar::command_requested);
    QTest::mouseClick(bar_.get(), Qt::RightButton, {}, bar_->item_rect("item").center());
    QTest::mouseClick(bar_.get(), Qt::MiddleButton, {}, bar_->item_rect("item").center());
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(StatusBarTest, ReleasingOnAnotherItemThanTheOneThatWasPressedClicksNothing) {
    add("a", StatusBarSide::left, "first", 2, "a.command");
    add("b", StatusBarSide::left, "second", 1, "b.command");
    QSignalSpy spy(bar_.get(), &StatusBar::command_requested);
    QTest::mousePress(bar_.get(), Qt::LeftButton, {}, bar_->item_rect("a").center());
    QTest::mouseRelease(bar_.get(), Qt::LeftButton, {}, bar_->item_rect("b").center());
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(StatusBarTest, TheItemUnderTheMouseIsHoveredOnlyWhenItHasACommand) {
    add("plain", StatusBarSide::left, "plain", 2);
    add("link", StatusBarSide::left, "link", 1, "a.command");
    EXPECT_EQ(bar_->hovered_item(), "");

    hover(bar_->item_rect("plain").center());
    EXPECT_EQ(bar_->hovered_item(), "");
    EXPECT_EQ(bar_->cursor().shape(), Qt::ArrowCursor);

    hover(bar_->item_rect("link").center());
    EXPECT_EQ(bar_->hovered_item(), "link");
    EXPECT_EQ(bar_->cursor().shape(), Qt::PointingHandCursor);

    hover(QPoint(400, 10));
    EXPECT_EQ(bar_->hovered_item(), "");
    EXPECT_EQ(bar_->cursor().shape(), Qt::ArrowCursor);
}

TEST_F(StatusBarTest, LeavingTheBarEndsTheHover) {
    add("link", StatusBarSide::left, "link", 0, "a.command");
    hover(bar_->item_rect("link").center());
    ASSERT_EQ(bar_->hovered_item(), "link");
    leave();
    EXPECT_EQ(bar_->hovered_item(), "");
}

TEST_F(StatusBarTest, RemovingOrDisablingTheHoveredItemEndsTheHover) {
    add("link", StatusBarSide::left, "link", 0, "a.command");
    hover(bar_->item_rect("link").center());
    ASSERT_EQ(bar_->hovered_item(), "link");
    add("link", StatusBarSide::left, "link", 0, {});
    EXPECT_EQ(bar_->hovered_item(), "");

    add("link", StatusBarSide::left, "link", 0, "a.command");
    hover(bar_->item_rect("link").center() + QPoint(1, 0));
    ASSERT_EQ(bar_->hovered_item(), "link");
    bar_->remove_item("link");
    EXPECT_EQ(bar_->hovered_item(), "");
}

TEST_F(StatusBarTest, AHoveredItemIsPaintedWithTheHoverBackground) {
    colors_.set_colors({{"statusBar.background", QColor("#202020")},
                        {"statusBarItem.hoverBackground", QColor("#336699")},
                        {"statusBarItem.activeBackground", QColor("#993366")}});
    add("link", StatusBarSide::left, "link", 0, "a.command");
    add("plain", StatusBarSide::left, "plain", -1);
    const QRect area = bar_->item_rect("link");
    const QPoint corner = area.topLeft() + QPoint(1, 2);

    EXPECT_EQ(QColor(grab().pixel(corner)), QColor("#202020"));
    hover(area.center());
    EXPECT_EQ(QColor(grab().pixel(corner)), QColor("#336699"));
    EXPECT_EQ(QColor(grab().pixel(bar_->item_rect("plain").topLeft() + QPoint(1, 2))),
              QColor("#202020"));

    QTest::mousePress(bar_.get(), Qt::LeftButton, {}, area.center());
    EXPECT_EQ(QColor(grab().pixel(corner)), QColor("#993366"));
    QTest::mouseRelease(bar_.get(), Qt::LeftButton, {}, area.center());
    EXPECT_EQ(QColor(grab().pixel(corner)), QColor("#336699"));
}

TEST_F(StatusBarTest, TheHighlightOfAnItemCoversItsPaddingButNotItsMargin) {
    colors_.set_colors({{"statusBar.background", QColor("#202020")},
                        {"statusBarItem.hoverBackground", QColor("#336699")}});
    add("link", StatusBarSide::left, "link", 0, "a.command");
    hover(bar_->item_rect("link").center());
    const QRect area = bar_->item_rect("link");
    const QImage image = grab();
    EXPECT_EQ(QColor(image.pixel(area.left(), 10)), QColor("#336699"));
    EXPECT_EQ(QColor(image.pixel(area.right(), 10)), QColor("#336699"));
    EXPECT_EQ(QColor(image.pixel(area.left() - 1, 10)), QColor("#202020"));
    EXPECT_EQ(QColor(image.pixel(area.right() + 1, 10)), QColor("#202020"));
}

TEST_F(StatusBarTest, TheBarAndItsItemsUseTheColorsOfTheTheme) {
    colors_.set_colors(
        {{"statusBar.background", QColor("#102030")}, {"statusBar.foreground", QColor("#ff0000")}});
    add("plain", StatusBarSide::left, "WWWW");
    EXPECT_EQ(QColor(grab().pixel(300, 10)), QColor("#102030"));

    // The pixel that differs most from the background is where a glyph covers a pixel fully.
    const QRect area = bar_->item_rect("plain");
    const auto strongest_text_pixel = [&] {
        const QImage image = grab();
        const QColor background("#102030");
        QColor strongest = background;
        int distance = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = area.left(); x <= area.right(); ++x) {
                const QColor pixel(image.pixel(x, y));
                const int d = std::abs(pixel.red() - background.red()) +
                              std::abs(pixel.green() - background.green()) +
                              std::abs(pixel.blue() - background.blue());
                if (d > distance) {
                    distance = d;
                    strongest = pixel;
                }
            }
        }
        return strongest;
    };
    QColor text = strongest_text_pixel();
    EXPECT_GT(text.red(), 150);
    EXPECT_LE(text.green(), 0x20);
    EXPECT_LE(text.blue(), 0x30);

    colors_.set_colors(
        {{"statusBar.background", QColor("#102030")}, {"statusBar.foreground", QColor("#ffff00")}});
    EXPECT_EQ(QColor(grab().pixel(300, 10)), QColor("#102030"));
    text = strongest_text_pixel();
    EXPECT_GT(text.red(), 150);
    EXPECT_GT(text.green(), 150);
    EXPECT_LE(text.blue(), 0x30);
}

TEST_F(StatusBarTest, AnItemCanHaveItsOwnColorsByKeyOrByValue) {
    colors_.set_colors({{"statusBar.background", QColor("#202020")},
                        {"tab.activeBackground", QColor("#3994bc")},
                        {"tab.activeForeground", QColor("#ffffff")}});
    bar_->set_item("keyed", StatusBarSide::left,
                   {.text = "keyed",
                    .foreground = {.key = "tab.activeForeground", .color = {}},
                    .background = {.key = "tab.activeBackground", .color = {}},
                    .priority = 2});
    bar_->set_item("fixed", StatusBarSide::left,
                   {.text = "fixed",
                    .foreground = {.key = {}, .color = QColor("#ff00ff")},
                    .background = {.key = {}, .color = QColor("#445566")},
                    .priority = 1});
    add("plain", StatusBarSide::left, "plain", 0);

    const QImage image = grab();
    EXPECT_EQ(QColor(image.pixel(bar_->item_rect("keyed").topLeft() + QPoint(0, 2))),
              QColor("#3994bc"));
    EXPECT_EQ(QColor(image.pixel(bar_->item_rect("fixed").topLeft() + QPoint(0, 2))),
              QColor("#445566"));
    EXPECT_EQ(QColor(image.pixel(bar_->item_rect("plain").topLeft() + QPoint(0, 2))),
              QColor("#202020"));

    colors_.set_colors(
        {{"statusBar.background", QColor("#202020")}, {"tab.activeBackground", QColor("#aa0000")}});
    EXPECT_EQ(QColor(grab().pixel(bar_->item_rect("keyed").topLeft() + QPoint(0, 2))),
              QColor("#aa0000"));
}

TEST_F(StatusBarTest, ATopBorderShowsOnlyWhenTheThemeHasOne) {
    colors_.set_colors({{"statusBar.background", QColor("#202020")}});
    EXPECT_EQ(QColor(grab().pixel(100, 0)), QColor("#202020"));
    colors_.set_colors(
        {{"statusBar.background", QColor("#202020")}, {"statusBar.border", QColor("#abcdef")}});
    const QImage image = grab();
    EXPECT_EQ(QColor(image.pixel(100, 0)), QColor("#abcdef"));
    EXPECT_EQ(QColor(image.pixel(100, 1)), QColor("#202020"));
}

TEST_F(StatusBarTest, AMessageReplacesTheLeftGroupAndLeavesTheRightOne) {
    add("left", StatusBarSide::left, "left item", 0, "a.command");
    add("right", StatusBarSide::right, "right item");
    const QRect right = bar_->item_rect("right");

    bar_->show_message("Saved");
    EXPECT_EQ(bar_->message(), "Saved");
    EXPECT_TRUE(bar_->item_rect("left").isEmpty());
    EXPECT_EQ(bar_->item_rect("right"), right);

    QSignalSpy spy(bar_.get(), &StatusBar::command_requested);
    QTest::mouseClick(bar_.get(), Qt::LeftButton, {}, QPoint(20, 10));
    EXPECT_EQ(spy.count(), 0);

    bar_->clear_message();
    EXPECT_TRUE(bar_->message().isEmpty());
    EXPECT_FALSE(bar_->item_rect("left").isEmpty());
}

TEST_F(StatusBarTest, AMessageIsPaintedInTheLeftGroupsPlace) {
    colors_.set_colors(
        {{"statusBar.background", QColor("#000000")}, {"statusBar.foreground", QColor("#ffffff")}});
    add("left", StatusBarSide::left, "left item");
    const QRect left = bar_->item_rect("left");
    const auto lit_pixels = [&](const QImage& image) {
        int lit = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = left.left(); x <= left.right(); ++x) {
                lit += QColor(image.pixel(x, y)) != QColor("#000000");
            }
        }
        return lit;
    };
    EXPECT_GT(lit_pixels(grab()), 0);
    bar_->show_message("");
    bar_->show_message("M");
    bar_->clear_message();
    EXPECT_GT(lit_pixels(grab()), 0);
    bar_->show_message("A message longer than the left item was");
    const QImage image = grab();
    int message_pixels = 0;
    for (int x = left.right() + 1; x < 200; ++x) {
        for (int y = 0; y < image.height(); ++y) {
            message_pixels += QColor(image.pixel(x, y)) != QColor("#000000");
        }
    }
    EXPECT_GT(message_pixels, 0);
}

TEST_F(StatusBarTest, AMessageWithATimeoutClearsItself) {
    bar_->show_message("Gone soon", 50);
    EXPECT_EQ(bar_->message(), "Gone soon");
    EXPECT_TRUE(QTest::qWaitFor([&] { return bar_->message().isEmpty(); }, 2000));
}

TEST_F(StatusBarTest, AMessageWithoutATimeoutStays) {
    bar_->show_message("Stays");
    QTest::qWait(100);
    EXPECT_EQ(bar_->message(), "Stays");
}

TEST_F(StatusBarTest, ANewMessageRestartsTheTimeoutOfTheOldOne) {
    bar_->show_message("first", 150);
    QTest::qWait(100);
    bar_->show_message("second", 400);
    QTest::qWait(200);
    EXPECT_EQ(bar_->message(), "second");
    EXPECT_TRUE(QTest::qWaitFor([&] { return bar_->message().isEmpty(); }, 2000));
}

TEST_F(StatusBarTest, AMessageWithoutATimeoutCancelsThePendingTimeoutOfTheOldOne) {
    bar_->show_message("timed", 100);
    bar_->show_message("permanent");
    QTest::qWait(250);
    EXPECT_EQ(bar_->message(), "permanent");
}

TEST_F(StatusBarTest, ShowingTheSameMessageAgainDoesNotRepaint) {
    bar_->show_message("same");
    QApplication::processEvents();
    PaintCounter counter;
    bar_->installEventFilter(&counter);
    bar_->show_message("same");
    QApplication::processEvents();
    EXPECT_EQ(counter.count, 0);
}

TEST_F(StatusBarTest, AToolTipShowsForTheItemUnderTheMouse) {
    bar_->set_item("a", StatusBarSide::left, {.text = "a", .tooltip = "About a"});
    add("b", StatusBarSide::left, "b", -1);
    const QRect area = bar_->item_rect("a");
    QHelpEvent help(QEvent::ToolTip, area.center(), bar_->mapToGlobal(area.center()));
    QApplication::sendEvent(bar_.get(), &help);
    EXPECT_TRUE(QToolTip::isVisible());
    EXPECT_EQ(QToolTip::text(), "About a");

    const QRect other = bar_->item_rect("b");
    QHelpEvent none(QEvent::ToolTip, other.center(), bar_->mapToGlobal(other.center()));
    QApplication::sendEvent(bar_.get(), &none);
    EXPECT_TRUE(QTest::qWaitFor([] { return !QToolTip::isVisible(); }, 2000));
}

TEST_F(StatusBarTest, IsNamedForAssistiveTechnology) {
    EXPECT_EQ(bar_->accessibleName(), "Status Bar");
}

TEST_F(StatusBarTest, RendersAStatusBarForReview) {
    colors_.set_colors({{"statusBar.background", QColor("#191A1B")},
                        {"statusBar.foreground", QColor("#8C8C8C")},
                        {"statusBar.border", QColor("#2A2B2C")},
                        {"statusBarItem.hoverBackground", QColor("#323233")},
                        {"tab.activeBackground", QColor("#3994BC")},
                        {"tab.activeForeground", QColor("#FFFFFF")}});
    bar_->resize(800, StatusBar::height_pixels);
    bar_->set_item("branch", StatusBarSide::left, {.text = "main", .command = "git.checkout"});
    bar_->set_item("problems", StatusBarSide::left,
                   {.text = "0 errors, 2 warnings", .command = "problems.focus", .priority = -1});
    bar_->set_item("remote", StatusBarSide::left,
                   {.text = "Remote",
                    .foreground = {.key = "tab.activeForeground", .color = {}},
                    .background = {.key = "tab.activeBackground", .color = {}},
                    .priority = 10});
    add("position", StatusBarSide::right, "Ln 12, Col 34", 100, "workbench.action.gotoLine");
    add("host", StatusBarSide::right, "Extension host: running", 0);
    hover(bar_->item_rect("position").center());

    const QImage image = grab();
    ASSERT_FALSE(image.isNull());
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/status_bar.png");
    }
}
