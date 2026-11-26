#include "anvil/ui/workbench.hpp"

#include <QApplication>
#include <QEnterEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSplitterHandle>
#include <QTest>
#include <QVBoxLayout>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <vector>

using anvil::ui::Part;
using anvil::ui::Workbench;
using anvil::ui::WorkbenchColors;

namespace {

class WorkbenchTest : public ::testing::Test {
protected:
    void SetUp() override {
        workbench_ = std::make_unique<Workbench>(colors_);
        editor_ = new QLabel("editor");
        workbench_->set_editor_area(editor_);
        workbench_->resize(1000, 700);
        workbench_->show();
        QApplication::processEvents();
    }

    // The position of a widget in the workbench.
    QPoint at(const QWidget* widget) const {
        return widget->mapTo(workbench_.get(), QPoint(0, 0));
    }
    int editor_width() const {
        return editor_->width();
    }
    int editor_height() const {
        return editor_->height();
    }
    std::vector<QSplitterHandle*> sashes() const {
        std::vector<QSplitterHandle*> handles;
        for (QSplitterHandle* handle : workbench_->findChildren<QSplitterHandle*>()) {
            if (handle->isVisible()) {
                handles.push_back(handle);
            }
        }
        return handles;
    }

    WorkbenchColors colors_{QApplication::palette()};
    std::unique_ptr<Workbench> workbench_;
    QLabel* editor_ = nullptr;
};

} // namespace

TEST(WorkbenchPartsTest, EveryPartHasVsCodesId) {
    EXPECT_EQ(anvil::ui::part_id(Part::title_bar), "workbench.parts.titlebar");
    EXPECT_EQ(anvil::ui::part_id(Part::activity_bar), "workbench.parts.activitybar");
    EXPECT_EQ(anvil::ui::part_id(Part::sidebar), "workbench.parts.sidebar");
    EXPECT_EQ(anvil::ui::part_id(Part::editor), "workbench.parts.editor");
    EXPECT_EQ(anvil::ui::part_id(Part::panel), "workbench.parts.panel");
    EXPECT_EQ(anvil::ui::part_id(Part::status_bar), "workbench.parts.statusbar");
    for (const Part part : {Part::title_bar, Part::activity_bar, Part::sidebar, Part::editor,
                            Part::panel, Part::status_bar}) {
        EXPECT_EQ(anvil::ui::part_from_id(anvil::ui::part_id(part)), part);
    }
    EXPECT_FALSE(anvil::ui::part_from_id("workbench.parts.nothing").has_value());
}

TEST_F(WorkbenchTest, WithoutATitleBarThePartIsHiddenAndHasNoSize) {
    EXPECT_FALSE(workbench_->is_visible(Part::title_bar));
    EXPECT_EQ(workbench_->size(Part::title_bar), 0);
    workbench_->set_visible(Part::title_bar, true);
    EXPECT_FALSE(workbench_->is_visible(Part::title_bar));
}

TEST_F(WorkbenchTest, TheTitleBarIsTheTopRowAndHidingItGivesItsSpaceToTheEditorArea) {
    auto* title_bar = new QLabel("title");
    title_bar->setFixedHeight(35);
    workbench_->set_title_bar(title_bar);
    QApplication::processEvents();

    EXPECT_TRUE(workbench_->is_visible(Part::title_bar));
    EXPECT_EQ(workbench_->size(Part::title_bar), 35);
    EXPECT_EQ(at(title_bar), QPoint(0, 0));
    EXPECT_EQ(title_bar->width(), workbench_->width());
    EXPECT_EQ(at(editor_).y(), 35);

    const int height = editor_height();
    QSignalSpy spy(workbench_.get(), &Workbench::visibility_changed);
    workbench_->set_visible(Part::title_bar, false);
    QApplication::processEvents();
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<Part>(), Part::title_bar);
    EXPECT_FALSE(spy.at(0).at(1).toBool());
    EXPECT_EQ(at(editor_).y(), 0);
    EXPECT_EQ(editor_height(), height + 35);
}

TEST_F(WorkbenchTest, TheSidebarThePanelAndTheActivityBarStartHidden) {
    EXPECT_FALSE(workbench_->is_visible(Part::activity_bar));
    EXPECT_FALSE(workbench_->is_visible(Part::sidebar));
    EXPECT_FALSE(workbench_->is_visible(Part::panel));
    EXPECT_TRUE(workbench_->is_visible(Part::editor));
    EXPECT_EQ(editor_width(), workbench_->width());
    EXPECT_EQ(editor_height(), workbench_->height());
}

TEST_F(WorkbenchTest, ThePartsAreArrangedAsInVsCode) {
    workbench_->set_visible(Part::activity_bar, true);
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();

    const QWidget* activity = workbench_->activity_bar();
    const QWidget* sidebar = workbench_->sidebar();
    const QWidget* panel = workbench_->panel();

    EXPECT_EQ(at(activity), QPoint(0, 0));
    EXPECT_EQ(activity->width(), Workbench::activity_bar_width);
    EXPECT_EQ(activity->height(), workbench_->height());
    EXPECT_GE(at(sidebar).x(), activity->width());
    EXPECT_EQ(at(sidebar).y(), 0);
    EXPECT_EQ(sidebar->height(), workbench_->height());
    EXPECT_GT(at(editor_).x(), at(sidebar).x() + sidebar->width());
    EXPECT_EQ(at(editor_).y(), 0);
    EXPECT_GE(at(panel).y(), at(editor_).y() + editor_->height());
    EXPECT_EQ(at(panel).x(), at(editor_).x());
    EXPECT_EQ(panel->width(), editor_->width());
    EXPECT_EQ(at(panel).y() + panel->height(), workbench_->height());
}

TEST_F(WorkbenchTest, ShowingAPartGivesItItsDefaultSizeAndHidingReturnsTheSpace) {
    const int width = editor_width();
    const int height = editor_height();

    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), Workbench::default_size, 1);
    EXPECT_NEAR(editor_width(), width - Workbench::default_size - 4, 2);

    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), Workbench::default_size, 1);
    EXPECT_NEAR(editor_height(), height - Workbench::default_size - 4, 2);

    workbench_->set_visible(Part::sidebar, false);
    workbench_->set_visible(Part::panel, false);
    QApplication::processEvents();
    EXPECT_EQ(editor_width(), width);
    EXPECT_EQ(editor_height(), height);
}

TEST_F(WorkbenchTest, HidingAndShowingRestoresTheSizeThePartHad) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_size(Part::sidebar, 220);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), 220, 1);

    workbench_->set_visible(Part::sidebar, false);
    EXPECT_EQ(workbench_->size(Part::sidebar), workbench_->size(Part::sidebar));
    EXPECT_NEAR(workbench_->size(Part::sidebar), 220, 1);
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), 220, 1);

    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::panel, 150);
    workbench_->set_visible(Part::panel, false);
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), 150, 1);
}

TEST_F(WorkbenchTest, ASizeSetWhileHiddenAppliesWhenThePartIsShown) {
    workbench_->set_size(Part::sidebar, 260);
    EXPECT_EQ(workbench_->size(Part::sidebar), 260);
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), 260, 1);
}

TEST_F(WorkbenchTest, DraggingASashResizesThePartAndThatSizeIsRestored) {
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    const auto handles = sashes();
    ASSERT_EQ(handles.size(), 1U);
    QSplitterHandle* sash = handles.front();
    const int before = workbench_->sidebar()->width();

    QTest::mousePress(sash, Qt::LeftButton, {}, sash->rect().center());
    QTest::mouseMove(sash, sash->rect().center() + QPoint(80, 0));
    QTest::mouseRelease(sash, Qt::LeftButton, {}, sash->rect().center() + QPoint(80, 0));
    QApplication::processEvents();
    const int dragged = workbench_->sidebar()->width();
    EXPECT_NEAR(dragged, before + 80, 3);

    workbench_->set_visible(Part::sidebar, false);
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), dragged, 1);
}

TEST_F(WorkbenchTest, DraggingASashCannotShrinkAPartBelowItsMinimum) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    const auto drag = [&](QSplitterHandle* sash, QPoint by) {
        const QPoint start = sash->rect().center();
        QTest::mousePress(sash, Qt::LeftButton, {}, start);
        QTest::mouseMove(sash, start + by);
        QTest::mouseRelease(sash, Qt::LeftButton, {}, start + by);
        QApplication::processEvents();
    };
    for (QSplitterHandle* sash : sashes()) {
        if (sash->orientation() == Qt::Horizontal) {
            drag(sash, QPoint(-2000, 0));
        } else {
            drag(sash, QPoint(0, 2000));
        }
    }
    EXPECT_EQ(workbench_->sidebar()->width(), Workbench::sidebar_minimum_width);
    EXPECT_EQ(workbench_->panel()->height(), Workbench::panel_minimum_height);
}

TEST_F(WorkbenchTest, DraggingThePanelSashResizesItAndThatSizeIsRestored) {
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    ASSERT_EQ(sashes().size(), 1U);
    QSplitterHandle* sash = sashes().front();
    const int before = workbench_->panel()->height();

    const QPoint start = sash->rect().center();
    QTest::mousePress(sash, Qt::LeftButton, {}, start);
    QTest::mouseMove(sash, start + QPoint(0, -90));
    QTest::mouseRelease(sash, Qt::LeftButton, {}, start + QPoint(0, -90));
    QApplication::processEvents();
    const int dragged = workbench_->panel()->height();
    EXPECT_NEAR(dragged, before + 90, 3);

    workbench_->set_visible(Part::panel, false);
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), dragged, 1);
}

TEST_F(WorkbenchTest, ASizeBelowTheMinimumIsRaisedToIt) {
    workbench_->set_size(Part::sidebar, 10);
    workbench_->set_size(Part::panel, 5);
    EXPECT_EQ(workbench_->size(Part::sidebar), Workbench::sidebar_minimum_width);
    EXPECT_EQ(workbench_->size(Part::panel), Workbench::panel_minimum_height);
}

TEST_F(WorkbenchTest, TheMinimumSizesHold) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::sidebar, 10);
    workbench_->set_size(Part::panel, 5);
    QApplication::processEvents();
    EXPECT_GE(workbench_->sidebar()->width(), Workbench::sidebar_minimum_width);
    EXPECT_GE(workbench_->panel()->height(), Workbench::panel_minimum_height);
    EXPECT_GE(workbench_->size(Part::sidebar), Workbench::sidebar_minimum_width);
    EXPECT_GE(workbench_->size(Part::panel), Workbench::panel_minimum_height);
}

TEST_F(WorkbenchTest, NoSizeLeavesTheEditorLessThanItsMinimum) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::sidebar, 5000);
    workbench_->set_size(Part::panel, 5000);
    QApplication::processEvents();
    EXPECT_GE(editor_width(), Workbench::editor_minimum_width);
    EXPECT_GE(editor_height(), Workbench::editor_minimum_height);
}

TEST_F(WorkbenchTest, AWindowSmallerThanTheSumOfTheMinimumsDoesNotCollapseTheEditor) {
    workbench_->set_visible(Part::activity_bar, true);
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    workbench_->resize(200, 150);
    QApplication::processEvents();
    EXPECT_GE(editor_width(), Workbench::editor_minimum_width);
    EXPECT_GE(editor_height(), Workbench::editor_minimum_height);
    EXPECT_GE(workbench_->width(), Workbench::activity_bar_width +
                                       Workbench::sidebar_minimum_width +
                                       Workbench::editor_minimum_width);
}

TEST_F(WorkbenchTest, ResizingTheWindowChangesTheEditorAndKeepsTheSidebarAndPanel) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::sidebar, 250);
    workbench_->set_size(Part::panel, 180);
    QApplication::processEvents();
    const int editor_before = editor_width();

    workbench_->resize(1200, 800);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), 250, 1);
    EXPECT_NEAR(workbench_->panel()->height(), 180, 1);
    EXPECT_NEAR(editor_width(), editor_before + 200, 2);
}

TEST_F(WorkbenchTest, TogglingShowsAndHidesAndTellsWhoIsListening) {
    QSignalSpy changed(workbench_.get(), &Workbench::visibility_changed);
    workbench_->toggle(Part::sidebar);
    EXPECT_TRUE(workbench_->is_visible(Part::sidebar));
    workbench_->toggle(Part::sidebar);
    EXPECT_FALSE(workbench_->is_visible(Part::sidebar));
    ASSERT_EQ(changed.count(), 2);
    EXPECT_EQ(changed.at(0).at(0).value<Part>(), Part::sidebar);
    EXPECT_TRUE(changed.at(0).at(1).toBool());
    EXPECT_FALSE(changed.at(1).at(1).toBool());

    workbench_->set_visible(Part::sidebar, false);
    EXPECT_EQ(changed.count(), 2);
    workbench_->set_visible(Part::editor, false);
    EXPECT_TRUE(workbench_->is_visible(Part::editor));
}

TEST_F(WorkbenchTest, TheStatusBarOfTheWindowIsAPartToo) {
    EXPECT_FALSE(workbench_->is_visible(Part::status_bar));
    auto* status = new QWidget(workbench_.get());
    status->resize(300, 22);
    status->show();
    workbench_->set_status_bar(status);
    EXPECT_TRUE(workbench_->is_visible(Part::status_bar));
    EXPECT_EQ(workbench_->size(Part::status_bar), 22);

    workbench_->set_visible(Part::status_bar, false);
    EXPECT_TRUE(status->isHidden());
    workbench_->toggle(Part::status_bar);
    EXPECT_FALSE(status->isHidden());
}

TEST_F(WorkbenchTest, EachPartReportsItsSize) {
    EXPECT_EQ(workbench_->size(Part::activity_bar), 0);
    workbench_->set_visible(Part::activity_bar, true);
    EXPECT_EQ(workbench_->size(Part::activity_bar), Workbench::activity_bar_width);
    EXPECT_EQ(workbench_->size(Part::editor), editor_width());
    workbench_->set_size(Part::activity_bar, 99);
    EXPECT_EQ(workbench_->size(Part::activity_bar), Workbench::activity_bar_width);
}

TEST_F(WorkbenchTest, SashesShowTheBorderColorAndTheSashColorWhileHovered) {
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    colors_.set_colors({{"sideBar.border", QColor("#00aa00")},
                        {"sash.hoverBorder", QColor("#ff00ff")},
                        {"sideBar.background", QColor("#222222")},
                        {"editorGroupHeader.tabsBackground", QColor("#333333")}});
    QApplication::processEvents();

    QSplitterHandle* sash = sashes().front();
    EXPECT_EQ(sash->width(), 4);
    const auto grabbed = [&] { return sash->grab().toImage(); };
    EXPECT_EQ(grabbed().pixelColor(sash->width() / 2, 10), QColor("#00aa00"));
    EXPECT_NE(grabbed().pixelColor(0, 10), QColor("#00aa00"));

    QEnterEvent enter(sash->rect().center(), sash->rect().center(), sash->rect().center());
    QApplication::sendEvent(sash, &enter);
    EXPECT_EQ(grabbed().pixelColor(0, 10), QColor("#ff00ff"));
    EXPECT_EQ(grabbed().pixelColor(3, 10), QColor("#ff00ff"));

    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(sash, &leave);
    EXPECT_EQ(grabbed().pixelColor(sash->width() / 2, 10), QColor("#00aa00"));
}

TEST_F(WorkbenchTest, ThePartsDrawTheirBackgroundsFromTheWorkbenchColors) {
    workbench_->set_visible(Part::activity_bar, true);
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    colors_.set_colors({{"activityBar.background", QColor("#101010")},
                        {"sideBar.background", QColor("#202020")},
                        {"panel.background", QColor("#303030")}});
    QApplication::processEvents();
    const auto pixel = [&](QWidget* part) {
        return part->grab().toImage().pixelColor(part->width() / 2, part->height() / 2);
    };
    EXPECT_EQ(pixel(workbench_->activity_bar()), QColor("#101010"));
    EXPECT_EQ(pixel(workbench_->sidebar()), QColor("#202020"));
    EXPECT_EQ(pixel(workbench_->panel()), QColor("#303030"));
}

TEST_F(WorkbenchTest, RendersAllTheParts) {
    workbench_->set_visible(Part::activity_bar, true);
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    for (QWidget* part : {workbench_->sidebar(), workbench_->panel()}) {
        auto* layout = new QVBoxLayout(part);
        layout->addWidget(new QLabel(part == workbench_->sidebar() ? "Sidebar" : "Panel"), 0,
                          Qt::AlignTop);
    }
    editor_->setAlignment(Qt::AlignCenter);
    QApplication::processEvents();
    const QImage image = workbench_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/workbench.png");
    }
}

TEST_F(WorkbenchTest, MaximizingThePanelGivesItTheWholeEditorArea) {
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_FALSE(workbench_->is_maximized(Part::panel));

    workbench_->set_maximized(Part::panel, true);
    QApplication::processEvents();
    EXPECT_TRUE(workbench_->is_maximized(Part::panel));
    EXPECT_FALSE(workbench_->is_visible(Part::editor));
    EXPECT_EQ(workbench_->panel()->height(), workbench_->height());
    EXPECT_EQ(workbench_->panel()->y(), 0);
}

TEST_F(WorkbenchTest, RestoringThePanelBringsItBackToTheSizeItHad) {
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::panel, 210);
    QApplication::processEvents();
    const int editor_height_before = editor_height();

    workbench_->set_maximized(Part::panel, true);
    QApplication::processEvents();
    workbench_->set_maximized(Part::panel, false);
    QApplication::processEvents();
    EXPECT_FALSE(workbench_->is_maximized(Part::panel));
    EXPECT_TRUE(workbench_->is_visible(Part::editor));
    EXPECT_NEAR(workbench_->panel()->height(), 210, 1);
    EXPECT_NEAR(editor_height(), editor_height_before, 1);
}

TEST_F(WorkbenchTest, ADraggedPanelSizeSurvivesMaximizingAndRestoring) {
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    QSplitterHandle* sash = sashes().front();
    const QPoint start = sash->rect().center();
    QTest::mousePress(sash, Qt::LeftButton, {}, start);
    QTest::mouseMove(sash, start + QPoint(0, -70));
    QTest::mouseRelease(sash, Qt::LeftButton, {}, start + QPoint(0, -70));
    QApplication::processEvents();
    const int dragged = workbench_->panel()->height();

    workbench_->set_maximized(Part::panel, true);
    QApplication::processEvents();
    workbench_->set_maximized(Part::panel, false);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), dragged, 1);
}

TEST_F(WorkbenchTest, MaximizingAHiddenPanelShowsIt) {
    workbench_->set_maximized(Part::panel, true);
    QApplication::processEvents();
    EXPECT_TRUE(workbench_->is_visible(Part::panel));
    EXPECT_TRUE(workbench_->is_maximized(Part::panel));
    EXPECT_EQ(workbench_->panel()->height(), workbench_->height());
}

TEST_F(WorkbenchTest, HidingAMaximizedPanelRestoresTheEditorAreaAndTheSize) {
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::panel, 190);
    QApplication::processEvents();
    workbench_->set_maximized(Part::panel, true);
    QApplication::processEvents();

    workbench_->set_visible(Part::panel, false);
    QApplication::processEvents();
    EXPECT_FALSE(workbench_->is_maximized(Part::panel));
    EXPECT_TRUE(workbench_->is_visible(Part::editor));
    EXPECT_EQ(editor_height(), workbench_->height());

    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), 190, 1);
    EXPECT_FALSE(workbench_->is_maximized(Part::panel));
}

TEST_F(WorkbenchTest, OnlyThePanelCanBeMaximizedAndTheSignalFollowsEachChange) {
    QSignalSpy changed(workbench_.get(), &Workbench::maximized_changed);
    workbench_->set_maximized(Part::sidebar, true);
    workbench_->set_maximized(Part::editor, true);
    EXPECT_FALSE(workbench_->is_maximized(Part::sidebar));
    EXPECT_EQ(changed.count(), 0);

    workbench_->set_maximized(Part::panel, true);
    workbench_->set_maximized(Part::panel, true);
    ASSERT_EQ(changed.count(), 1);
    EXPECT_EQ(changed.at(0).at(0).value<Part>(), Part::panel);
    EXPECT_TRUE(changed.at(0).at(1).toBool());
    workbench_->set_maximized(Part::panel, false);
    ASSERT_EQ(changed.count(), 2);
    EXPECT_FALSE(changed.at(1).at(1).toBool());
}

TEST_F(WorkbenchTest, RestoringAfterTheWindowWasResizedWhileMaximizedGivesThePanelItsOldSize) {
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::panel, 210);
    QApplication::processEvents();

    workbench_->set_maximized(Part::panel, true);
    QApplication::processEvents();
    workbench_->resize(1000, 900);
    QApplication::processEvents();
    EXPECT_EQ(workbench_->panel()->height(), 900);

    workbench_->set_maximized(Part::panel, false);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), 210, 1);
    EXPECT_NEAR(editor_height(), 900 - 210 - 4, 2);
}

TEST_F(WorkbenchTest, ASizeSetWhileMaximizedAppliesWhenThePanelIsRestored) {
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::panel, 210);
    QApplication::processEvents();
    workbench_->set_maximized(Part::panel, true);
    QApplication::processEvents();

    workbench_->set_size(Part::panel, 150);
    EXPECT_EQ(workbench_->panel()->height(), workbench_->height());
    workbench_->set_maximized(Part::panel, false);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), 150, 1);
}

namespace {

using anvil::ui::PanelPosition;
using anvil::ui::Side;

constexpr PanelPosition all_positions[] = {PanelPosition::top, PanelPosition::left,
                                           PanelPosition::right, PanelPosition::bottom};

QRect rect_in(const QWidget* widget, const QWidget* ancestor) {
    return QRect(widget->mapTo(ancestor, QPoint(0, 0)), widget->size());
}

} // namespace

TEST_F(WorkbenchTest, ThePartsFollowTheSideOfTheSidebarAndThePositionOfThePanel) {
    workbench_->set_visible(Part::activity_bar, true);
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::sidebar, 200);

    for (const Side side : {Side::left, Side::right}) {
        for (const PanelPosition position : all_positions) {
            workbench_->set_sidebar_side(side);
            workbench_->set_panel_position(position);
            QApplication::processEvents();
            SCOPED_TRACE(::testing::Message() << (side == Side::left ? "left" : "right") << " "
                                              << anvil::ui::panel_position_name(position));

            const QRect whole = workbench_->rect();
            const QRect activity = rect_in(workbench_->activity_bar(), workbench_.get());
            const QRect sidebar = rect_in(workbench_->sidebar(), workbench_.get());
            const QRect panel = rect_in(workbench_->panel(), workbench_.get());
            const QRect editor = rect_in(editor_, workbench_.get());

            if (side == Side::left) {
                EXPECT_EQ(activity.left(), whole.left());
                EXPECT_EQ(sidebar.left(), activity.right() + 1);
                EXPECT_GT(editor.left(), sidebar.right());
                EXPECT_GT(panel.left(), sidebar.right());
            } else {
                EXPECT_EQ(activity.right(), whole.right());
                EXPECT_EQ(sidebar.right(), activity.left() - 1);
                EXPECT_LT(editor.right(), sidebar.left());
                EXPECT_LT(panel.right(), sidebar.left());
            }
            EXPECT_EQ(sidebar.width(), 200);
            EXPECT_EQ(activity.width(), Workbench::activity_bar_width);

            switch (position) {
            case PanelPosition::top:
                EXPECT_LT(panel.bottom(), editor.top());
                EXPECT_EQ(panel.left(), editor.left());
                EXPECT_EQ(panel.width(), editor.width());
                break;
            case PanelPosition::bottom:
                EXPECT_GT(panel.top(), editor.bottom());
                EXPECT_EQ(panel.left(), editor.left());
                EXPECT_EQ(panel.width(), editor.width());
                break;
            case PanelPosition::left:
                EXPECT_LT(panel.right(), editor.left());
                EXPECT_EQ(panel.top(), editor.top());
                EXPECT_EQ(panel.height(), editor.height());
                break;
            case PanelPosition::right:
                EXPECT_GT(panel.left(), editor.right());
                EXPECT_EQ(panel.top(), editor.top());
                EXPECT_EQ(panel.height(), editor.height());
                break;
            }
        }
    }
}

TEST_F(WorkbenchTest, TheSidebarAndThePanelStartOnTheLeftAndAtTheBottom) {
    EXPECT_EQ(workbench_->sidebar_side(), Side::left);
    EXPECT_EQ(workbench_->panel_position(), PanelPosition::bottom);
    EXPECT_EQ(anvil::ui::panel_position_name(PanelPosition::top), "top");
    EXPECT_EQ(anvil::ui::panel_position_name(PanelPosition::left), "left");
    EXPECT_EQ(anvil::ui::panel_position_name(PanelPosition::right), "right");
    EXPECT_EQ(anvil::ui::panel_position_name(PanelPosition::bottom), "bottom");
}

TEST_F(WorkbenchTest, TheSidebarKeepsItsSizeWhenItMovesToTheOtherSide) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_size(Part::sidebar, 260);
    QApplication::processEvents();

    workbench_->set_sidebar_side(Side::right);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), 260, 1);
    EXPECT_EQ(workbench_->size(Part::sidebar), workbench_->sidebar()->width());

    workbench_->set_visible(Part::sidebar, false);
    workbench_->set_sidebar_side(Side::left);
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), 260, 1);
}

TEST_F(WorkbenchTest, ADraggedSashOfASidebarOnTheRightResizesIt) {
    workbench_->set_sidebar_side(Side::right);
    workbench_->set_visible(Part::sidebar, true);
    QApplication::processEvents();
    const auto handles = sashes();
    ASSERT_EQ(handles.size(), 1U);
    QSplitterHandle* sash = handles.front();
    const int before = workbench_->sidebar()->width();

    QTest::mousePress(sash, Qt::LeftButton, {}, sash->rect().center());
    QTest::mouseMove(sash, sash->rect().center() + QPoint(-60, 0));
    QTest::mouseRelease(sash, Qt::LeftButton, {}, sash->rect().center() + QPoint(-60, 0));
    QApplication::processEvents();
    const int dragged = workbench_->sidebar()->width();
    EXPECT_NEAR(dragged, before + 60, 3);

    workbench_->set_sidebar_side(Side::left);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->sidebar()->width(), dragged, 1);
}

TEST_F(WorkbenchTest, EveryPanelPositionKeepsItsOwnSize) {
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::panel, 180);
    QApplication::processEvents();

    workbench_->set_panel_position(PanelPosition::right);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->width(), Workbench::default_size, 1);
    workbench_->set_size(Part::panel, 410);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->width(), 410, 1);

    workbench_->set_panel_position(PanelPosition::top);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), Workbench::default_size, 1);

    workbench_->set_panel_position(PanelPosition::bottom);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), 180, 1);
    workbench_->set_panel_position(PanelPosition::right);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->width(), 410, 1);
    EXPECT_EQ(workbench_->size(Part::panel), workbench_->panel()->width());
}

TEST_F(WorkbenchTest, ASizeThatTheUserDraggedIsKeptWhenThePanelMoves) {
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    const auto handles = sashes();
    ASSERT_EQ(handles.size(), 1U);
    QSplitterHandle* sash = handles.front();
    QTest::mousePress(sash, Qt::LeftButton, {}, sash->rect().center());
    QTest::mouseMove(sash, sash->rect().center() + QPoint(0, -70));
    QTest::mouseRelease(sash, Qt::LeftButton, {}, sash->rect().center() + QPoint(0, -70));
    QApplication::processEvents();
    const int dragged = workbench_->panel()->height();
    EXPECT_NEAR(dragged, Workbench::default_size + 70, 3);

    workbench_->set_panel_position(PanelPosition::left);
    workbench_->set_panel_position(PanelPosition::bottom);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), dragged, 1);
}

TEST_F(WorkbenchTest, ThePanelBesideTheEditorHasAMinimumWidthAndNoMinimumHeight) {
    workbench_->set_visible(Part::panel, true);
    workbench_->set_panel_position(PanelPosition::left);
    workbench_->set_size(Part::panel, 50);
    QApplication::processEvents();
    EXPECT_EQ(workbench_->panel()->width(), Workbench::panel_minimum_width);
    EXPECT_EQ(workbench_->panel()->height(), editor_height());
    EXPECT_EQ(workbench_->panel()->minimumWidth(), Workbench::panel_minimum_width);
    EXPECT_EQ(workbench_->panel()->minimumHeight(), 0);

    workbench_->set_panel_position(PanelPosition::top);
    workbench_->set_size(Part::panel, 10);
    QApplication::processEvents();
    EXPECT_EQ(workbench_->panel()->height(), Workbench::panel_minimum_height);
    EXPECT_EQ(workbench_->panel()->minimumHeight(), Workbench::panel_minimum_height);
    EXPECT_EQ(workbench_->panel()->minimumWidth(), 0);
}

TEST_F(WorkbenchTest, NoPanelPositionLeavesTheEditorLessThanItsMinimum) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_visible(Part::panel, true);
    for (const PanelPosition position : all_positions) {
        workbench_->set_panel_position(position);
        workbench_->set_size(Part::panel, 5000);
        workbench_->set_size(Part::sidebar, 5000);
        QApplication::processEvents();
        EXPECT_GE(editor_width(), Workbench::editor_minimum_width);
        EXPECT_GE(editor_height(), Workbench::editor_minimum_height);
    }
}

TEST_F(WorkbenchTest, AHiddenPanelStaysHiddenWhenItMovesAndComesBackThere) {
    workbench_->set_visible(Part::panel, true);
    workbench_->set_visible(Part::panel, false);
    QApplication::processEvents();
    const int editor_before = editor_height();

    workbench_->set_panel_position(PanelPosition::right);
    QApplication::processEvents();
    EXPECT_FALSE(workbench_->is_visible(Part::panel));
    EXPECT_EQ(editor_height(), editor_before);
    EXPECT_EQ(editor_width(), workbench_->width());

    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_GT(at(workbench_->panel()).x(), at(editor_).x());
    EXPECT_NEAR(workbench_->panel()->width(), Workbench::default_size, 1);
}

TEST_F(WorkbenchTest, AMaximizedPanelTakesTheWholeEditorAreaAtEveryPosition) {
    workbench_->set_visible(Part::sidebar, true);
    workbench_->set_maximized(Part::panel, true);
    for (const PanelPosition position : all_positions) {
        workbench_->set_panel_position(position);
        QApplication::processEvents();
        SCOPED_TRACE(anvil::ui::panel_position_name(position));
        EXPECT_TRUE(workbench_->is_maximized(Part::panel));
        EXPECT_FALSE(editor_->isVisible());
        EXPECT_EQ(workbench_->panel()->width(),
                  workbench_->width() - workbench_->sidebar()->width() - 4);
        EXPECT_EQ(workbench_->panel()->height(), workbench_->height());
    }
}

TEST_F(WorkbenchTest, RestoringAPanelThatWasMovedWhileMaximizedUsesTheSizeOfItsNewPosition) {
    workbench_->set_visible(Part::panel, true);
    workbench_->set_size(Part::panel, 200);
    workbench_->set_maximized(Part::panel, true);
    workbench_->set_panel_position(PanelPosition::left);
    workbench_->set_size(Part::panel, 350);
    workbench_->set_maximized(Part::panel, false);
    QApplication::processEvents();
    EXPECT_TRUE(editor_->isVisible());
    EXPECT_NEAR(workbench_->panel()->width(), 350, 1);

    workbench_->set_panel_position(PanelPosition::bottom);
    QApplication::processEvents();
    EXPECT_NEAR(workbench_->panel()->height(), 200, 1);
}

TEST_F(WorkbenchTest, MovingThePartsTellsWhoIsListeningOnlyWhenSomethingChanges) {
    QSignalSpy side(workbench_.get(), &Workbench::sidebar_side_changed);
    QSignalSpy position(workbench_.get(), &Workbench::panel_position_changed);

    workbench_->set_sidebar_side(Side::left);
    workbench_->set_panel_position(PanelPosition::bottom);
    EXPECT_EQ(side.count(), 0);
    EXPECT_EQ(position.count(), 0);

    workbench_->set_sidebar_side(Side::right);
    workbench_->set_panel_position(PanelPosition::top);
    ASSERT_EQ(side.count(), 1);
    ASSERT_EQ(position.count(), 1);
    EXPECT_EQ(side.front().front().value<Side>(), Side::right);
    EXPECT_EQ(position.front().front().value<PanelPosition>(), PanelPosition::top);
}

TEST(WorkbenchFocusTest, MovingThePartsKeepsTheKeyboardFocusInTheEditorArea) {
    WorkbenchColors colors{QApplication::palette()};
    Workbench workbench(colors);
    auto* editor = new QLineEdit;
    workbench.set_editor_area(editor);
    auto* in_sidebar = new QLineEdit(workbench.sidebar());
    auto* in_panel = new QLineEdit(workbench.panel());
    workbench.resize(1000, 700);
    workbench.show();
    workbench.set_visible(Part::sidebar, true);
    workbench.set_visible(Part::panel, true);
    QApplication::processEvents();

    for (QLineEdit* focused : {editor, in_sidebar, in_panel}) {
        focused->setFocus();
        ASSERT_TRUE(focused->hasFocus());
        workbench.set_sidebar_side(workbench.sidebar_side() == Side::left ? Side::right
                                                                          : Side::left);
        EXPECT_TRUE(focused->hasFocus());
        for (const PanelPosition position : all_positions) {
            workbench.set_panel_position(position);
            EXPECT_TRUE(focused->hasFocus()) << anvil::ui::panel_position_name(position);
        }
    }
}

TEST(WorkbenchBeforeShownTest, APartKeepsItsSizeThoughItWasNeverOnScreen) {
    WorkbenchColors colors{QApplication::palette()};
    Workbench workbench(colors);
    workbench.set_editor_area(new QLabel("editor"));
    workbench.set_size(Part::sidebar, 260);
    workbench.set_size(Part::panel, 190);
    EXPECT_EQ(workbench.size(Part::sidebar), 260);

    workbench.set_visible(Part::sidebar, true);
    workbench.set_visible(Part::panel, true);
    EXPECT_EQ(workbench.size(Part::sidebar), 260);
    EXPECT_EQ(workbench.size(Part::panel), 190);
    workbench.set_maximized(Part::panel, true);
    workbench.set_maximized(Part::panel, false);
    workbench.set_visible(Part::sidebar, false);
    workbench.set_visible(Part::panel, false);
    EXPECT_EQ(workbench.size(Part::sidebar), 260);
    EXPECT_EQ(workbench.size(Part::panel), 190);
}

TEST_F(WorkbenchTest, ThePanelHasASizeAtEachPosition) {
    using anvil::ui::PanelPosition;
    workbench_->set_visible(Part::panel, true);
    workbench_->set_panel_size_at(PanelPosition::top, 150);
    workbench_->set_panel_size_at(PanelPosition::right, 1);
    EXPECT_EQ(workbench_->panel_size_at(PanelPosition::right), Workbench::panel_minimum_width);
    EXPECT_EQ(workbench_->panel_size_at(PanelPosition::bottom), Workbench::default_size);
    workbench_->set_panel_size_at(PanelPosition::bottom, 220);
    QApplication::processEvents();
    EXPECT_EQ(workbench_->panel()->height(), 220);
    workbench_->set_panel_position(PanelPosition::top);
    QApplication::processEvents();
    EXPECT_EQ(workbench_->panel()->height(), 150);
    EXPECT_EQ(workbench_->panel_size_at(PanelPosition::bottom), 220);
}

namespace {

class ActivityBarBorderTest : public WorkbenchTest {
protected:
    void SetUp() override {
        WorkbenchTest::SetUp();
        set_theme(line);
        workbench_->set_visible(Part::activity_bar, true);
        QApplication::processEvents();
    }

    void set_theme(std::optional<QColor> border) {
        WorkbenchColors::Colors theme{{"activityBar.background", background}};
        if (border) {
            theme.emplace("activityBar.border", *border);
        }
        colors_.set_colors(theme);
    }

    // The columns of the activity bar that are the border's color from top to bottom.
    std::vector<int> bordered_columns() {
        QWidget* bar = workbench_->activity_bar();
        const QImage image = bar->grab().toImage();
        std::vector<int> columns;
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, 0) == line && image.pixelColor(x, image.height() - 1) == line) {
                columns.push_back(x);
            } else {
                EXPECT_EQ(image.pixelColor(x, image.height() / 2), background) << x;
            }
        }
        return columns;
    }

    const QColor line{255, 0, 0};
    const QColor background{0, 0, 100};
};

TEST_F(ActivityBarBorderTest, ALineStandsOnTheEdgeThatFacesTheSidebar) {
    const int last = workbench_->activity_bar()->width() - 1;
    EXPECT_EQ(bordered_columns(), std::vector<int>{last});
    workbench_->set_visible(Part::sidebar, true);
    EXPECT_EQ(bordered_columns(), std::vector<int>{last});

    workbench_->set_sidebar_side(anvil::ui::Side::right);
    QApplication::processEvents();
    EXPECT_EQ(bordered_columns(), std::vector<int>{0});
    workbench_->set_visible(Part::sidebar, false);
    EXPECT_EQ(bordered_columns(), std::vector<int>{0});

    workbench_->set_sidebar_side(anvil::ui::Side::left);
    QApplication::processEvents();
    EXPECT_EQ(bordered_columns(), std::vector<int>{last});
}

TEST_F(ActivityBarBorderTest, TheLineFollowsTheTheme) {
    set_theme(std::nullopt);
    EXPECT_TRUE(bordered_columns().empty());
    set_theme(line);
    EXPECT_EQ(bordered_columns().size(), 1U);
}

} // namespace
