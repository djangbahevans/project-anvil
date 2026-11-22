#include "anvil/ui/workbench.hpp"

#include <QApplication>
#include <QEnterEvent>
#include <QLabel>
#include <QSignalSpy>
#include <QSplitterHandle>
#include <QTest>
#include <QVBoxLayout>
#include <gtest/gtest.h>
#include <memory>

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
    EXPECT_EQ(anvil::ui::part_id(Part::activity_bar), "workbench.parts.activitybar");
    EXPECT_EQ(anvil::ui::part_id(Part::sidebar), "workbench.parts.sidebar");
    EXPECT_EQ(anvil::ui::part_id(Part::editor), "workbench.parts.editor");
    EXPECT_EQ(anvil::ui::part_id(Part::panel), "workbench.parts.panel");
    EXPECT_EQ(anvil::ui::part_id(Part::status_bar), "workbench.parts.statusbar");
    for (const Part part :
         {Part::activity_bar, Part::sidebar, Part::editor, Part::panel, Part::status_bar}) {
        EXPECT_EQ(anvil::ui::part_from_id(anvil::ui::part_id(part)), part);
    }
    EXPECT_FALSE(anvil::ui::part_from_id("workbench.parts.titlebar").has_value());
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
