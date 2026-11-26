#include "anvil/ui/activity_bar.hpp"
#include "anvil/ui/codicons.hpp"
#include "anvil/ui/editor_view.hpp"
#include "anvil/ui/explorer_view.hpp"
#include "anvil/ui/find_bar.hpp"
#include "anvil/ui/menu_style.hpp"
#include "anvil/ui/panel.hpp"
#include "anvil/ui/quick_pick.hpp"
#include "anvil/ui/scroll_bar.hpp"
#include "anvil/ui/sidebar.hpp"
#include "anvil/ui/splitter.hpp"
#include "anvil/ui/status_bar.hpp"
#include "anvil/ui/tab_strip.hpp"
#include "anvil/ui/title_bar.hpp"
#include "anvil/ui/workbench.hpp"
#include "anvil/ui/zoom.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QStyleOptionMenuItem>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <unistd.h>

namespace {

using namespace anvil::ui;

constexpr double levels[] = {-2, 0, 3};

// The smallest rectangle that holds the pixels of `image` inside `area` that are not the color of
// the pixel at the corner of `area`: the mark that a part drew there.
QRect painted_extent(const QImage& image, const QRect& area) {
    const QColor background = image.pixelColor(area.topLeft());
    QRect extent;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const int distance = std::abs(color.red() - background.red()) +
                                 std::abs(color.green() - background.green()) +
                                 std::abs(color.blue() - background.blue());
            if (distance > 60) {
                extent = extent.united(QRect(x, y, 1, 1));
            }
        }
    }
    return extent;
}

class ZoomPartsTest : public ::testing::Test {
protected:
    WorkbenchColors colors_{QApplication::palette()};
    Zoom zoom_;
};

// --- the strip of tabs ---

class TabStripZoomTest : public ZoomPartsTest {
protected:
    void SetUp() override {
        strip_ = std::make_unique<TabStrip>(colors_);
        strip_->set_zoom(&zoom_);
        strip_->resize(900, strip_->strip_height());
        strip_->show();
        for (const char* title : {"a.txt", "b.txt", "c.txt"}) {
            strip_->add_tab({.title = title, .tooltip = {}, .dirty = false, .has_path = true});
        }
        QApplication::processEvents();
    }
    void press(const QPoint& at) {
        QMouseEvent event(QEvent::MouseButtonPress, at, strip_->mapToGlobal(at), Qt::LeftButton,
                          Qt::LeftButton, {});
        QApplication::sendEvent(strip_.get(), &event);
    }
    void release(const QPoint& at) {
        QMouseEvent event(QEvent::MouseButtonRelease, at, strip_->mapToGlobal(at), Qt::LeftButton,
                          Qt::NoButton, {});
        QApplication::sendEvent(strip_.get(), &event);
    }
    void move(const QPoint& at) {
        QMouseEvent event(QEvent::MouseMove, at, strip_->mapToGlobal(at), Qt::NoButton,
                          Qt::LeftButton, {});
        QApplication::sendEvent(strip_.get(), &event);
    }
    std::unique_ptr<TabStrip> strip_;
};

TEST_F(TabStripZoomTest, EverySizeIsTheSizeAtLevelZeroScaled) {
    for (const double level : levels) {
        zoom_.set_level(level);
        SCOPED_TRACE(level);
        EXPECT_EQ(strip_->strip_height(), zoom_.scaled(TabStrip::height_pixels));
        EXPECT_EQ(strip_->height(), zoom_.scaled(TabStrip::height_pixels));
        EXPECT_EQ(strip_->font().pixelSize(), zoom_.scaled(TabStrip::font_pixels));
        EXPECT_EQ(strip_->tab_min_width(), zoom_.scaled(TabStrip::minimum_tab_width));
        EXPECT_EQ(strip_->tab_rect(0).width(), zoom_.scaled(TabStrip::minimum_tab_width));
        EXPECT_EQ(strip_->tab_rect(0).height(), zoom_.scaled(TabStrip::height_pixels));

        const QRect button = strip_->close_button_rect(0);
        const int size = zoom_.scaled(TabStrip::close_button_size);
        const int action = zoom_.scaled(TabStrip::action_width);
        EXPECT_EQ(button.size(), QSize(size, size));
        EXPECT_EQ(button.top(), (zoom_.scaled(TabStrip::height_pixels) - size) / 2);
        EXPECT_EQ(button.left(), strip_->tab_rect(0).right() + 1 - action + (action - size) / 2);
    }
}

TEST_F(TabStripZoomTest, ALongTitleMakesATabAsWideAsItsPaddingTitleAndButtonArea) {
    strip_->add_tab({.title = QString(60, QLatin1Char('m')), .tooltip = {}, .has_path = true});
    for (const double level : levels) {
        zoom_.set_level(level);
        SCOPED_TRACE(level);
        const int title = QFontMetrics(strip_->font()).horizontalAdvance(QString(60, 'm'));
        EXPECT_EQ(strip_->tab_rect(3).width(), zoom_.scaled(TabStrip::title_padding) + title + 1 +
                                                   zoom_.scaled(TabStrip::action_width));
    }
}

TEST_F(TabStripZoomTest, TheTabsLayOutAgainWhenTheZoomChangesWithoutBeingRecreated) {
    const int before = strip_->tab_rect(1).left();
    TabStrip* same = strip_.get();
    zoom_.set_level(3);
    EXPECT_EQ(strip_.get(), same);
    EXPECT_EQ(strip_->count(), 3);
    EXPECT_EQ(strip_->tab_rect(1).left(), strip_->tab_rect(0).width());
    EXPECT_GT(strip_->tab_rect(1).left(), before);
    zoom_.set_level(0);
    EXPECT_EQ(strip_->tab_rect(1).left(), before);
}

TEST_F(TabStripZoomTest, ALevelThatLeavesTheFontAloneStillChangesTheLayout) {
    const int font = strip_->font().pixelSize();
    zoom_.set_level(0.1);
    ASSERT_EQ(strip_->font().pixelSize(), font);
    EXPECT_EQ(strip_->tab_rect(0).width(), zoom_.scaled(TabStrip::minimum_tab_width));
    EXPECT_GT(strip_->tab_rect(0).width(), TabStrip::minimum_tab_width);
    EXPECT_EQ(strip_->tab_rect(1).left(), strip_->tab_rect(0).width());
}

TEST_F(TabStripZoomTest, TheDotOfAnUnsavedTabAndTheCrossOfTheCloseButtonAreDrawnAtTheZoomedSize) {
    colors_.set_colors({{"editorGroupHeader.tabsBackground", QColor("#101010")},
                        {"tab.activeBackground", QColor("#202020")},
                        {"tab.unfocusedActiveBackground", QColor("#202020")},
                        {"tab.activeForeground", QColor("#ffffff")},
                        {"tab.unfocusedActiveForeground", QColor("#ffffff")}});
    const auto extent_of_the_mark = [&](bool dirty) {
        strip_->set_tab(0, {.title = "a.txt", .tooltip = {}, .dirty = dirty, .has_path = true});
        QApplication::processEvents();
        const QImage image = strip_->grab().toImage();
        return painted_extent(image, strip_->close_button_rect(0));
    };
    zoom_.set_level(0);
    const QRect dot = extent_of_the_mark(true);
    const QRect cross = extent_of_the_mark(false);
    ASSERT_FALSE(dot.isEmpty());
    ASSERT_FALSE(cross.isEmpty());
    EXPECT_NEAR(dot.width(), TabStrip::close_button_size / 2, 1);
    zoom_.set_level(3);
    const QRect zoomed_dot = extent_of_the_mark(true);
    const QRect zoomed_cross = extent_of_the_mark(false);
    EXPECT_NEAR(zoomed_dot.width(), zoom_.scaled(8), 2);
    EXPECT_GT(zoomed_dot.width(), dot.width() * 3 / 2);
    EXPECT_GT(zoomed_cross.width(), cross.width() * 3 / 2);
    EXPECT_GT(zoomed_cross.height(), cross.height() * 3 / 2);
}

TEST_F(TabStripZoomTest, TheTitleStartsAfterTheZoomedPadding) {
    colors_.set_colors({{"editorGroupHeader.tabsBackground", QColor("#101010")},
                        {"tab.activeBackground", QColor("#202020")},
                        {"tab.unfocusedActiveBackground", QColor("#202020")},
                        {"tab.activeForeground", QColor("#ffffff")},
                        {"tab.unfocusedActiveForeground", QColor("#ffffff")}});
    strip_->set_tab(0, {.title = "WWWW", .tooltip = {}, .has_path = true});
    for (const double level : levels) {
        zoom_.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        const QRect tab = strip_->tab_rect(0);
        const QRect text_area(tab.left(), tab.top() + 2,
                              strip_->close_button_rect(0).left() - tab.left(), tab.height() - 4);
        const QRect extent = painted_extent(strip_->grab().toImage(), text_area);
        ASSERT_FALSE(extent.isEmpty());
        EXPECT_GE(extent.left() - tab.left(), zoom_.scaled(TabStrip::title_padding));
        EXPECT_LE(extent.left() - tab.left(), zoom_.scaled(TabStrip::title_padding) + 3);
    }
}

TEST_F(TabStripZoomTest, AClickOnATabAndOnItsCloseButtonFindsThemWhereTheyAreDrawn) {
    zoom_.set_level(3);
    QSignalSpy selected(strip_.get(), &TabStrip::current_changed);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    const QPoint body = strip_->tab_rect(1).center() - QPoint(10, 0);
    press(body);
    release(body);
    ASSERT_EQ(selected.count(), 1);
    EXPECT_EQ(selected.at(0).at(0).toInt(), 1);

    const QPoint button = strip_->close_button_rect(2).center();
    press(button);
    release(button);
    ASSERT_EQ(closed.count(), 1);
    EXPECT_EQ(closed.at(0).at(0).toInt(), 2);
}

TEST_F(TabStripZoomTest, ADragMovesATabPastItsNeighborByTheZoomedWidths) {
    zoom_.set_level(3);
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    const QPoint start = strip_->tab_rect(0).center() - QPoint(10, 0);
    press(start);
    move(start + QPoint(strip_->tab_rect(0).width() / 3, 0));
    EXPECT_EQ(moved.count(), 0);
    move(start + QPoint(strip_->tab_rect(0).width() + strip_->tab_rect(0).width() / 4, 0));
    ASSERT_EQ(moved.count(), 1);
    EXPECT_EQ(moved.at(0).at(0).toInt(), 0);
    EXPECT_EQ(moved.at(0).at(1).toInt(), 1);
    release(start);
}

// --- the status bar ---

class StatusBarZoomTest : public ZoomPartsTest {
protected:
    void SetUp() override {
        bar_ = std::make_unique<StatusBar>(colors_);
        bar_->set_zoom(&zoom_);
        bar_->resize(800, bar_->bar_height());
        bar_->show();
        bar_->set_item("left", StatusBarSide::left,
                       {.text = "Ln 1, Col 1", .tooltip = {}, .command = "go.to.line"});
        bar_->set_item("right", StatusBarSide::right, {.text = "UTF-8", .tooltip = {}});
        QApplication::processEvents();
    }
    std::unique_ptr<StatusBar> bar_;
};

TEST_F(StatusBarZoomTest, EverySizeIsTheSizeAtLevelZeroScaled) {
    for (const double level : levels) {
        zoom_.set_level(level);
        SCOPED_TRACE(level);
        EXPECT_EQ(bar_->bar_height(), zoom_.scaled(StatusBar::height_pixels));
        EXPECT_EQ(bar_->height(), zoom_.scaled(StatusBar::height_pixels));
        EXPECT_EQ(bar_->font().pixelSize(), zoom_.scaled(StatusBar::font_pixels));

        const QFontMetrics metrics(bar_->font());
        const QRect left = bar_->item_rect("left");
        EXPECT_EQ(left.left(), zoom_.scaled(StatusBar::item_margin));
        EXPECT_EQ(left.width(), metrics.horizontalAdvance("Ln 1, Col 1") +
                                    2 * zoom_.scaled(StatusBar::item_padding));
        EXPECT_EQ(left.height(), zoom_.scaled(StatusBar::height_pixels));
        const QRect right = bar_->item_rect("right");
        EXPECT_EQ(right.right() + 1, bar_->width() - zoom_.scaled(StatusBar::item_margin));
    }
}

TEST_F(StatusBarZoomTest, AClickOnAnItemFindsItAtTheZoomedPlace) {
    zoom_.set_level(3);
    QSignalSpy spy(bar_.get(), &StatusBar::command_requested);
    QTest::mouseClick(bar_.get(), Qt::LeftButton, {}, bar_->item_rect("left").center());
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "go.to.line");
    QTest::mouseClick(bar_.get(), Qt::LeftButton, {}, QPoint(2, bar_->height() - 2));
    EXPECT_EQ(spy.count(), 1) << "the margin is not part of the item";
}

// --- the activity bar and the workbench around it ---

class ActivityBarZoomTest : public ZoomPartsTest {
protected:
    void SetUp() override {
        workbench_ = std::make_unique<Workbench>(colors_);
        workbench_->set_zoom(&zoom_);
        workbench_->set_editor_area(new QLabel("editor"));
        workbench_->resize(1000, 700);
        sidebar_ = std::make_unique<Sidebar>(*workbench_, registry_, colors_);
        QPixmap pixmap(24, 24);
        pixmap.fill(Qt::black);
        for (const char* id : {"one", "two"}) {
            ViewContainer container;
            container.id = id;
            container.title = id;
            container.icon = QIcon(pixmap);
            container.create = [] { return new QLabel("view"); };
            registry_.add(std::move(container));
        }
        ViewContainer round;
        round.id = "round";
        round.title = "round";
        round.icon = codicon_icon("circle-filled", Qt::white, 24);
        round.create = [] { return new QLabel("view"); };
        registry_.add(std::move(round));
        workbench_->set_visible(Part::activity_bar, true);
        workbench_->show();
        QApplication::processEvents();
    }
    ViewContainerRegistry registry_;
    std::unique_ptr<Workbench> workbench_;
    std::unique_ptr<Sidebar> sidebar_;
};

TEST_F(ActivityBarZoomTest, TheBarAndItsButtonsAreAsWideAsTheSizeAtLevelZeroScaled) {
    for (const double level : levels) {
        zoom_.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        EXPECT_EQ(workbench_->size(Part::activity_bar),
                  zoom_.scaled(Workbench::activity_bar_width));
        const QToolButton* one = sidebar_->activity_bar().button("one");
        ASSERT_NE(one, nullptr);
        EXPECT_EQ(one->size(), QSize(zoom_.scaled(ActivityBar::button_size),
                                     zoom_.scaled(ActivityBar::button_size)));
        EXPECT_EQ(sidebar_->activity_bar().button("two")->geometry().top(),
                  one->geometry().bottom() + 1);
    }
}

TEST_F(ActivityBarZoomTest, TheIconOfAButtonIsDrawnAtTheZoomedSizeAndStaysSharp) {
    QToolButton* round = sidebar_->activity_bar().button("round");
    ASSERT_NE(round, nullptr);
    const auto extent = [&] {
        QApplication::processEvents();
        return painted_extent(round->grab().toImage(), round->rect().adjusted(4, 4, -4, -4));
    };
    const QRect plain = extent();
    ASSERT_FALSE(plain.isEmpty());
    zoom_.set_level(3);
    const QRect zoomed = extent();
    EXPECT_NEAR(zoomed.width() / static_cast<double>(plain.width()), zoom_.factor(), 0.15);
    EXPECT_NEAR(zoomed.height() / static_cast<double>(plain.height()), zoom_.factor(), 0.15);
}

TEST_F(ActivityBarZoomTest, AClickOnAButtonAtLevelThreeActivatesItsContainer) {
    zoom_.set_level(3);
    QApplication::processEvents();
    QSignalSpy spy(const_cast<ActivityBar*>(&sidebar_->activity_bar()), &ActivityBar::activated);
    QToolButton* two = sidebar_->activity_bar().button("two");
    const QPoint inside = QPoint(two->width() - 3, two->height() - 3);
    QTest::mouseClick(two, Qt::LeftButton, {}, inside);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "two");
}

// --- the title bar ---

class TitleBarZoomTest : public ZoomPartsTest {
protected:
    void SetUp() override {
        bar_ = std::make_unique<TitleBar>(colors_);
        bar_->set_zoom(&zoom_);
        menu_bar_ = new QMenuBar;
        menu_bar_->addMenu("File");
        bar_->set_menu_bar(menu_bar_);
        bar_->set_command_center_visible(true);
        bar_->set_command_center_text("project");
        bar_->set_layout_controls_visible(true);
        bar_->resize(1200, bar_->height());
        bar_->show();
        QApplication::processEvents();
    }
    std::unique_ptr<TitleBar> bar_;
    QMenuBar* menu_bar_ = nullptr;
};

TEST_F(TitleBarZoomTest, EverySizeIsTheSizeAtLevelZeroScaled) {
    for (const double level : levels) {
        zoom_.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        const int height = zoom_.scaled(TitleBar::bar_height);
        EXPECT_EQ(bar_->height(), height);
        EXPECT_EQ(menu_bar_->height(), height);
        for (const QAbstractButton* button :
             {bar_->minimize_button(), bar_->maximize_button(), bar_->close_button()}) {
            EXPECT_EQ(button->size(), QSize(zoom_.scaled(TitleBar::button_width), height));
        }
        for (const QAbstractButton* button : {bar_->sidebar_button(), bar_->panel_button()}) {
            EXPECT_EQ(button->size(), QSize(zoom_.scaled(28), zoom_.scaled(28)));
        }
        EXPECT_EQ(bar_->command_center()->height(), zoom_.scaled(24));
        EXPECT_EQ(bar_->command_center()->geometry().top(), (height - zoom_.scaled(24)) / 2);
        EXPECT_EQ(bar_->close_button()->geometry().right() + 1, bar_->width());
    }
}

TEST_F(TitleBarZoomTest, TheMenuBarFontFollowsTheZoom) {
    const int base = QFontInfo(menu_bar_->font()).pixelSize();
    zoom_.set_level(3);
    EXPECT_EQ(menu_bar_->font().pixelSize(), zoom_.scaled(base));
    zoom_.set_level(0);
    EXPECT_EQ(menu_bar_->font().pixelSize(), base);
}

TEST_F(TitleBarZoomTest, TheCloseButtonIsClickedWhereItIsAtLevelThree) {
    zoom_.set_level(3);
    QApplication::processEvents();
    QSignalSpy spy(bar_.get(), &TitleBar::close_requested);
    QTest::mouseClick(
        bar_->close_button(), Qt::LeftButton, {},
        QPoint(bar_->close_button()->width() - 2, bar_->close_button()->height() - 2));
    EXPECT_EQ(spy.count(), 1);
}

// --- the side bar, the panel and the workbench around them ---

class WorkbenchZoomTest : public ZoomPartsTest {
protected:
    void SetUp() override {
        workbench_ = std::make_unique<Workbench>(colors_);
        workbench_->set_zoom(&zoom_);
        workbench_->set_editor_area(new QLabel("editor"));
        workbench_->resize(1400, 900);
        sidebar_ = std::make_unique<Sidebar>(*workbench_, registry_, colors_);
        panel_ = std::make_unique<Panel>(*workbench_, registry_, colors_);
        for (const auto& [id, location] : {std::pair{"side", ViewContainerLocation::sidebar},
                                           std::pair{"bottom", ViewContainerLocation::panel}}) {
            ViewContainer container;
            container.id = id;
            container.title = id;
            container.location = location;
            container.create = [] { return new QLabel("view"); };
            registry_.add(std::move(container));
        }
        workbench_->show();
        QApplication::processEvents();
    }
    ViewContainerRegistry registry_;
    std::unique_ptr<Workbench> workbench_;
    std::unique_ptr<Sidebar> sidebar_;
    std::unique_ptr<Panel> panel_;
};

TEST_F(WorkbenchZoomTest, TheMinimumSizesTheSashesAndTheHeadersAreTheSizesAtLevelZeroScaled) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    for (const double level : levels) {
        zoom_.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        EXPECT_EQ(workbench_->sidebar()->minimumWidth(),
                  zoom_.scaled(Workbench::sidebar_minimum_width));
        EXPECT_EQ(workbench_->panel()->minimumHeight(),
                  zoom_.scaled(Workbench::panel_minimum_height));
        workbench_->set_size(Part::sidebar, 1);
        workbench_->set_size(Part::panel, 1);
        QApplication::processEvents();
        EXPECT_EQ(workbench_->size(Part::sidebar), zoom_.scaled(Workbench::sidebar_minimum_width));
        EXPECT_EQ(workbench_->size(Part::panel), zoom_.scaled(Workbench::panel_minimum_height));
        for (const QSplitter* splitter : workbench_->findChildren<QSplitter*>()) {
            EXPECT_EQ(splitter->handleWidth(), zoom_.scaled(Splitter::sash_width));
        }
        EXPECT_EQ(sidebar_->title_height(), zoom_.scaled(35));
        const QWidget* title = sidebar_->title_actions()->parentWidget();
        EXPECT_EQ(title->height(), zoom_.scaled(35));
        EXPECT_EQ(title->layout()->contentsMargins().left(), zoom_.scaled(20));
        EXPECT_EQ(title->layout()->contentsMargins().right(), zoom_.scaled(8));
        EXPECT_EQ(panel_->header_size(), zoom_.scaled(Panel::header_height));
        for (const QToolButton* button : {panel_->maximize_button(), panel_->close_button()}) {
            EXPECT_EQ(button->size(), QSize(zoom_.scaled(28), zoom_.scaled(28)));
        }
        EXPECT_EQ(panel_->tab("bottom")->height(), zoom_.scaled(Panel::header_height));
    }
}

TEST_F(WorkbenchZoomTest, APartThatIsShownForTheFirstTimeStartsAtTheScaledDefaultSize) {
    zoom_.set_level(3);
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->size(Part::sidebar), zoom_.scaled(Workbench::default_size), 2);
    EXPECT_NEAR(workbench_->size(Part::panel), zoom_.scaled(Workbench::default_size), 2);
}

TEST_F(WorkbenchZoomTest, ALevelChangeKeepsTheSizeOfAPartAndMovesItOnlyToItsNewMinimum) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::sidebar, 200);
    workbench_->set_size(Part::panel, 400);
    QApplication::processEvents();
    ASSERT_EQ(workbench_->size(Part::sidebar), 200);
    ASSERT_EQ(workbench_->size(Part::panel), 400);

    zoom_.set_level(3);
    QApplication::processEvents();
    EXPECT_EQ(workbench_->size(Part::sidebar), zoom_.scaled(Workbench::sidebar_minimum_width))
        << "200 is under the minimum of level 3";
    EXPECT_EQ(workbench_->size(Part::panel), 400);

    zoom_.set_level(-2);
    QApplication::processEvents();
    EXPECT_EQ(workbench_->size(Part::panel), 400);
    EXPECT_GE(workbench_->size(Part::sidebar), zoom_.scaled(Workbench::sidebar_minimum_width));
}

TEST_F(WorkbenchZoomTest, ASidebarThatWasDraggedToAWidthKeepsItAcrossALevelChange) {
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    QSplitter* across = nullptr;
    for (QSplitter* splitter : workbench_->findChildren<QSplitter*>()) {
        if (splitter->orientation() == Qt::Horizontal &&
            splitter->indexOf(workbench_->sidebar()) >= 0) {
            across = splitter;
        }
    }
    ASSERT_NE(across, nullptr);
    QWidget* sash = across->handle(1);
    const QPoint start = sash->rect().center();
    QTest::mousePress(sash, Qt::LeftButton, {}, start);
    QTest::mouseMove(sash, start + QPoint(300, 0));
    QTest::mouseRelease(sash, Qt::LeftButton, {}, start + QPoint(300, 0));
    QApplication::processEvents();
    const int dragged = workbench_->sidebar()->width();
    ASSERT_GT(dragged, 500);
    zoom_.set_level(3);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), dragged, 2);
}

TEST_F(WorkbenchZoomTest, APartThatWasNeverResizedFollowsTheScaledDefaultWhenTheLevelChanges) {
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    ASSERT_NEAR(workbench_->sidebar()->width(), Workbench::default_size, 2);
    zoom_.set_level(3);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), zoom_.scaled(Workbench::default_size), 2);
}

TEST_F(WorkbenchZoomTest, AHiddenPartKeepsItsSizeAcrossALevelChangeToo) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_size(Part::sidebar, 500);
    QApplication::processEvents();
    workbench_->set_visible(Part::sidebar, false);
    zoom_.set_level(3);
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->size(Part::sidebar), 500, 2);
}

TEST_F(WorkbenchZoomTest, TheWindowAtTheEndsOfTheRangeHasNoPartWiderThanItselfAndNoOverlap) {
    workbench_->set_visible(Part::activity_bar, true);
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    for (const double level : {-8.0, 8.0}) {
        zoom_.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        const int across = workbench_->size(Part::activity_bar) + workbench_->size(Part::sidebar) +
                           workbench_->size(Part::editor);
        EXPECT_LE(across, workbench_->width());
        for (QWidget* part :
             {workbench_->activity_bar(), workbench_->sidebar(), workbench_->panel()}) {
            EXPECT_LE(part->geometry().right(), workbench_->width());
            EXPECT_LE(part->geometry().bottom(), workbench_->height());
        }
        const QRect sidebar(workbench_->sidebar()->mapTo(workbench_.get(), QPoint(0, 0)),
                            workbench_->sidebar()->size());
        const QRect activity(workbench_->activity_bar()->mapTo(workbench_.get(), QPoint(0, 0)),
                             workbench_->activity_bar()->size());
        EXPECT_FALSE(sidebar.intersects(activity));
    }
}

// --- the find bar ---

class FindBarZoomTest : public ZoomPartsTest {
protected:
    void SetUp() override {
        host_ = std::make_unique<QWidget>();
        view_ = new EditorView(host_.get());
        bar_ = new FindBar(view_, host_.get());
        base_ = QFontInfo(bar_->font()).pixelSize();
        bar_->set_zoom(&zoom_);
        host_->resize(800, 400);
        host_->show();
        bar_->open();
        QApplication::processEvents();
    }
    std::unique_ptr<QWidget> host_;
    EditorView* view_ = nullptr;
    FindBar* bar_ = nullptr;
    int base_ = 0;
};

TEST_F(FindBarZoomTest, TheFontTheMarginsAndTheSpacingAreScaled) {
    for (const double level : levels) {
        zoom_.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        EXPECT_EQ(bar_->font().pixelSize(), zoom_.scaled(base_));
        for (const QWidget* child : bar_->findChildren<QWidget*>()) {
            EXPECT_EQ(child->font().pixelSize(), zoom_.scaled(base_))
                << child->metaObject()->className();
        }
        const QMargins margins = bar_->layout()->contentsMargins();
        EXPECT_EQ(margins,
                  QMargins(zoom_.scaled(6), zoom_.scaled(4), zoom_.scaled(6), zoom_.scaled(4)));
        EXPECT_EQ(bar_->layout()->spacing(), zoom_.scaled(4));
        EXPECT_GE(bar_->sizeHint().height(),
                  bar_->fontMetrics().height() + margins.top() + margins.bottom());
    }
}

TEST_F(FindBarZoomTest, TheBarGrowsWithTheZoomAndTheFieldStaysInsideIt) {
    const int plain = bar_->sizeHint().height();
    zoom_.set_level(3);
    QApplication::processEvents();
    EXPECT_GT(bar_->sizeHint().height(), plain);
    EXPECT_TRUE(bar_->rect().contains(bar_->findChild<QLineEdit*>()->geometry()));
}

// --- the quick pick ---

class QuickPickZoomTest : public ZoomPartsTest {
protected:
    void SetUp() override {
        host_ = std::make_unique<QWidget>();
        host_->resize(1400, 900);
        pick_ = new QuickPick(host_.get(), colors_);
        base_points_ = QFontInfo(pick_->font()).pointSizeF();
        pick_->set_zoom(&zoom_);
        std::vector<QuickPickItem> items;
        for (const char* label : {"One", "Two", "Three"}) {
            items.push_back({label, {}, {}, {}});
        }
        items.push_back({"Four", {}, "detail of four", {}});
        pick_->set_items(std::move(items));
        host_->show();
        pick_->open();
        QApplication::processEvents();
    }
    std::unique_ptr<QWidget> host_;
    QuickPick* pick_ = nullptr;
    qreal base_points_ = 0;
};

TEST_F(QuickPickZoomTest, TheRowsTheWidthAndTheFontAreTheSizesAtLevelZeroScaled) {
    for (const double level : levels) {
        zoom_.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        EXPECT_EQ(pick_->row_size(), zoom_.scaled(QuickPick::row_height));
        EXPECT_EQ(pick_->list()->sizeHintForRow(0), zoom_.scaled(QuickPick::row_height));
        EXPECT_EQ(pick_->list()->sizeHintForRow(3), 2 * zoom_.scaled(QuickPick::row_height))
            << "an item with a detail has two lines";
        EXPECT_EQ(pick_->width(),
                  std::min(zoom_.scaled(600), host_->width() - 2 * zoom_.scaled(8)));
        EXPECT_NEAR(pick_->font().pointSizeF(), base_points_ * zoom_.factor(), 0.01);
        EXPECT_NEAR(pick_->filter_field()->font().pointSizeF(), base_points_ * zoom_.factor(),
                    0.01);
        EXPECT_EQ(pick_->list()->verticalScrollBar()->maximumWidth(),
                  zoom_.scaled(anvil::ui::ScrollBar::list_width));
        const int blur = zoom_.scaled(20);
        bool shadow = false;
        for (const QWidget* sibling : host_->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            shadow = shadow ||
                     (sibling != pick_ &&
                      sibling->geometry() == pick_->geometry().adjusted(-blur, -blur, blur, blur));
        }
        EXPECT_TRUE(shadow) << "the shadow is larger than the popup by the scaled blur";
        EXPECT_EQ(pick_->list()->height(), 3 * zoom_.scaled(QuickPick::row_height) +
                                               2 * zoom_.scaled(QuickPick::row_height));
    }
}

TEST_F(QuickPickZoomTest, ARowIsClickedWhereItIsDrawnAtLevelThree) {
    zoom_.set_level(3);
    QApplication::processEvents();
    QSignalSpy accepted(pick_, &QuickPick::accepted);
    const QModelIndex second = pick_->list()->model()->index(1, 0);
    const QPoint inside = pick_->list()->visualRect(second).center();
    QTest::mouseClick(pick_->list()->viewport(), Qt::LeftButton, {}, inside);
    ASSERT_EQ(accepted.count(), 1);
    EXPECT_EQ(accepted.at(0).at(0).toULongLong(), 1U);
}

TEST_F(QuickPickZoomTest, AWindowTooNarrowForTheScaledWidthGetsTheWholeWindowLessItsMargins) {
    host_->resize(700, 900);
    zoom_.set_level(3);
    QApplication::processEvents();
    EXPECT_EQ(pick_->width(), 700 - 2 * zoom_.scaled(8));
    EXPECT_GE(pick_->x(), 0);
}

// --- the menus ---

class MenuStyleZoomTest : public ZoomPartsTest {
protected:
    MenuStyleZoomTest() : style_(colors_) {
        style_.set_zoom(&zoom_);
    }
    QSize item_size(const QString& text,
                    QStyleOptionMenuItem::MenuItemType type = QStyleOptionMenuItem::Normal) {
        QStyleOptionMenuItem option;
        option.menuItemType = type;
        option.text = text;
        option.font = QApplication::font();
        option.fontMetrics = QFontMetrics(option.font);
        return style_.sizeFromContents(QStyle::CT_MenuItem, &option, QSize(0, 0), nullptr);
    }
    MenuStyle style_;
};

TEST_F(MenuStyleZoomTest, TheMeasuresOfAnItemAreTheSizesAtLevelZeroScaled) {
    const QFontMetrics metrics(QApplication::font());
    for (const double level : levels) {
        zoom_.set_level(level);
        SCOPED_TRACE(level);
        const QSize plain = item_size("Open");
        EXPECT_EQ(plain.height(), std::max(zoom_.scaled(MenuStyle::item_height),
                                           metrics.height() + zoom_.scaled(8)));
        EXPECT_EQ(plain.width(), zoom_.scaled(MenuStyle::label_left) +
                                     metrics.horizontalAdvance("Open") +
                                     zoom_.scaled(MenuStyle::shortcut_right));
        const QSize with_key = item_size("Open\tCtrl+O");
        EXPECT_EQ(with_key.width(), zoom_.scaled(MenuStyle::label_left) +
                                        metrics.horizontalAdvance("Open") +
                                        zoom_.scaled(MenuStyle::shortcut_right) +
                                        zoom_.scaled(MenuStyle::label_to_shortcut) +
                                        metrics.horizontalAdvance("Ctrl+O"));
        EXPECT_EQ(item_size("", QStyleOptionMenuItem::Separator).height(),
                  zoom_.scaled(MenuStyle::separator_height));
        EXPECT_EQ(style_.pixelMetric(QStyle::PM_MenuVMargin, nullptr, nullptr),
                  zoom_.scaled(MenuStyle::vertical_margin));
        EXPECT_EQ(style_.pixelMetric(QStyle::PM_MenuPanelWidth, nullptr, nullptr),
                  MenuStyle::frame_width);
    }
}

TEST_F(MenuStyleZoomTest, AMenuShownAtLevelThreeHasItemsOfTheScaledHeight) {
    QMenu menu;
    menu.setStyle(&style_);
    QAction* first = menu.addAction("First");
    QAction* second = menu.addAction("Second");
    menu.addSeparator();
    QAction* third = menu.addAction("Third");
    zoom_.set_level(3);
    menu.show();
    QApplication::processEvents();
    EXPECT_GE(menu.actionGeometry(first).height(), zoom_.scaled(MenuStyle::item_height));
    EXPECT_EQ(menu.actionGeometry(first).height(), menu.actionGeometry(second).height());
    EXPECT_EQ(menu.actionGeometry(second).bottom() + 1 + zoom_.scaled(MenuStyle::separator_height),
              menu.actionGeometry(third).top());
}

// --- the Explorer ---

class ExplorerZoomTest : public ZoomPartsTest {
protected:
    void SetUp() override {
        folder_ = std::filesystem::temp_directory_path() /
                  ("anvil-zoom-explorer-" + std::to_string(::getpid()));
        std::filesystem::create_directories(folder_ / "src");
        for (const char* name : {"a.txt", "b.txt"}) {
            std::ofstream(folder_ / name) << "x";
        }
        view_ = std::make_unique<ExplorerView>(colors_);
        view_->set_zoom(&zoom_);
        view_->resize(400, 600);
        view_->set_workspace(folder_, "project");
        view_->show();
        QApplication::processEvents();
    }
    void TearDown() override {
        view_.reset();
        std::filesystem::remove_all(folder_);
    }
    std::filesystem::path folder_;
    std::unique_ptr<ExplorerView> view_;
};

TEST_F(ExplorerZoomTest, TheRowsTheIndentationTheScrollBarAndTheFontAreScaled) {
    const int base_font = QFontInfo(view_->font()).pixelSize();
    const int bar = view_->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    ASSERT_TRUE(QTest::qWaitFor([&] { return view_->tree()->model()->rowCount() == 3; }));
    for (const double level : levels) {
        zoom_.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        const QModelIndex first = view_->tree()->model()->index(0, 0);
        EXPECT_EQ(view_->tree()->visualRect(first).height(), zoom_.scaled(22));
        EXPECT_EQ(view_->tree()->indentation(), zoom_.scaled(8));
        EXPECT_EQ(view_->tree()->verticalScrollBar()->width(), zoom_.scaled(bar));
        EXPECT_EQ(view_->font().pixelSize(), zoom_.scaled(base_font));
        EXPECT_EQ(view_->open_folder_button()->font().pixelSize(), zoom_.scaled(base_font));
        EXPECT_EQ(view_->message()->font().pixelSize(), zoom_.scaled(base_font));
    }
}

TEST_F(ExplorerZoomTest, TheFieldForANameAndTheReasonUnderItAreScaled) {
    const int base_font = QFontInfo(view_->font()).pixelSize();
    ASSERT_TRUE(QTest::qWaitFor([&] { return view_->tree()->model()->rowCount() == 3; }));
    zoom_.set_level(3);
    view_->new_file();
    ASSERT_TRUE(QTest::qWaitFor([&] { return view_->is_editing(); }));
    view_->name_editor()->setText("a.txt");
    const auto reason = [&] {
        const auto labels = view_->tree()->viewport()->findChildren<QLabel*>();
        return labels.isEmpty() ? nullptr : labels.last();
    };
    ASSERT_NE(reason(), nullptr);
    for (const double level : levels) {
        zoom_.set_level(level);
        QApplication::processEvents();
        SCOPED_TRACE(level);
        ASSERT_TRUE(view_->is_editing());
        EXPECT_EQ(view_->name_editor()->font().pixelSize(), zoom_.scaled(base_font));
        EXPECT_EQ(reason()->font().pixelSize(), zoom_.scaled(base_font));
    }
}

} // namespace
